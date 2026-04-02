use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;

use bytes::Bytes;
use flowkernel_cache::LatestSampleCache;
use flowkernel_queue::MPSCQueue;
use tokio::sync::Notify;
use tracing::{info, warn};
use zenoh::key_expr::KeyExpr;
use zenoh::qos::Priority;
use zenoh::Session;

use crate::error::Result;

/// Cache-line aligned atomic to prevent false sharing
#[repr(align(64))]
struct CacheAlignedAtomic(AtomicU64);

impl CacheAlignedAtomic {
    fn new() -> Self {
        Self(AtomicU64::new(0))
    }
    
    fn load(&self, order: Ordering) -> u64 {
        self.0.load(order)
    }
    
    fn store(&self, val: u64, order: Ordering) {
        self.0.store(val, order);
    }
    
    fn fetch_add(&self, val: u64, order: Ordering) -> u64 {
        self.0.fetch_add(val, order)
    }
}

/// Topic priority levels
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TopicPriority {
    /// E-Stop, critical commands (highest priority, instant wakeup)
    Critical,
    /// Telemetry data (normal priority, batch processing)
    Normal,
    /// Background data like Costmap (latest-only, drop old frames)
    Background,
}

impl TopicPriority {
    /// Convert to Zenoh priority
    pub fn to_zenoh_priority(&self) -> Priority {
        match self {
            TopicPriority::Critical => Priority::RealTime,
            TopicPriority::Normal => Priority::Data,
            TopicPriority::Background => Priority::Background,
        }
    }
}

/// Handler configuration
pub struct HandlerConfig<F>
where
    F: Fn(&[u8]) + Send + Sync + 'static,
{
    /// Zenoh key expression (e.g., "robot/*/cmd/estop")
    pub path: String,
    /// Topic priority
    pub priority: TopicPriority,
    /// Callback function (receives zero-copy view of payload)
    pub callback: F,
}

/// Channel statistics
#[derive(Debug, Clone, Default)]
pub struct ChannelStats {
    pub processed: u64,
    pub dropped: u64,
    pub avg_latency_us: f64,
    pub p95_latency_us: f64,
    pub p99_latency_us: f64,
}

/// Critical sample for MPSC queue (uses Bytes for zero-copy)
/// Store handler_idx directly to avoid HashMap lookup on hot path
struct CriticalSample {
    handler_idx: usize,  // Direct index, no hash lookup needed
    payload: Bytes,
}

/// Handler entry
struct Handler {
    path: String,
    path_hash: u64,
    priority: TopicPriority,
    callback: Arc<dyn Fn(&[u8]) + Send + Sync>,
}

/// DataFlowReactor - Core reactor for high-performance robot dataflow
///
/// ## Design
///
/// - **Multi-priority channels**: CRITICAL (MPSC + instant wakeup), NORMAL (MPSC), BACKGROUND (Latest-only)
/// - **Native Rust**: Uses Zenoh Rust SDK directly, no C API binding hassle
/// - **Zero-copy**: `Vec<u8>` passed to callbacks by reference
///
/// ## Quick Start
///
/// ```rust,no_run
/// use flowkernel_core::{DataFlowReactor, TopicPriority, HandlerConfig};
///
/// #[tokio::main]
/// async fn main() -> anyhow::Result<()> {
///     let mut reactor = DataFlowReactor::new().await?;
///     
///     reactor.register_handler(HandlerConfig {
///         path: "robot/*/cmd/estop".to_string(),
///         priority: TopicPriority::Critical,
///         callback: |data| {
///             println!("E-Stop: {} bytes", data.len());
///         },
///     }).await?;
///     
///     reactor.run().await?;
///     Ok(())
/// }
/// ```
pub struct DataFlowReactor {
    session: Session,
    handlers: Vec<Handler>,
    critical_queue: Arc<MPSCQueue<CriticalSample, 512>>,
    background_cache: Arc<LatestSampleCache>,
    wakeup: Arc<Notify>,
    // Cache-line aligned atomics to prevent false sharing
    critical_processed: CacheAlignedAtomic,
    normal_processed: CacheAlignedAtomic,
    background_processed: CacheAlignedAtomic,
    critical_dropped: CacheAlignedAtomic,
    normal_dropped: CacheAlignedAtomic,
    background_dropped: CacheAlignedAtomic,
}

impl DataFlowReactor {
    /// Create a new DataFlowReactor with default config
    pub async fn new() -> Result<Self> {
        let config = zenoh::config::Config::default();
        let session = zenoh::open(config)
            .await
            .map_err(|e: zenoh::Error| crate::error::ReactorError::ZenohError(e.to_string()))?;

        Ok(Self {
            session,
            handlers: Vec::new(),
            critical_queue: Arc::new(MPSCQueue::new()),
            background_cache: Arc::new(LatestSampleCache::new()),
            wakeup: Arc::new(Notify::new()),
            critical_processed: CacheAlignedAtomic::new(),
            normal_processed: CacheAlignedAtomic::new(),
            background_processed: CacheAlignedAtomic::new(),
            critical_dropped: CacheAlignedAtomic::new(),
            normal_dropped: CacheAlignedAtomic::new(),
            background_dropped: CacheAlignedAtomic::new(),
        })
    }

    /// Create a new DataFlowReactor with custom config
    pub async fn with_config(config: zenoh::config::Config) -> Result<Self> {
        let session = zenoh::open(config)
            .await
            .map_err(|e: zenoh::Error| crate::error::ReactorError::ZenohError(e.to_string()))?;

        Ok(Self {
            session,
            handlers: Vec::new(),
            critical_queue: Arc::new(MPSCQueue::new()),
            background_cache: Arc::new(LatestSampleCache::new()),
            wakeup: Arc::new(Notify::new()),
            critical_processed: CacheAlignedAtomic::new(),
            normal_processed: CacheAlignedAtomic::new(),
            background_processed: CacheAlignedAtomic::new(),
            critical_dropped: CacheAlignedAtomic::new(),
            normal_dropped: CacheAlignedAtomic::new(),
            background_dropped: CacheAlignedAtomic::new(),
        })
    }

    /// Register a handler for a topic
    pub async fn register_handler<F>(&mut self, config: HandlerConfig<F>) -> Result<()>
    where
        F: Fn(&[u8]) + Send + Sync + 'static,
    {
        let path_hash = self.fnv1a_hash(&config.path);

        // Pre-allocate background cache slot
        if config.priority == TopicPriority::Background {
            self.background_cache.pre_allocate(path_hash);
        }

        self.handlers.push(Handler {
            path: config.path,
            path_hash,
            priority: config.priority,
            callback: Arc::new(config.callback),
        });

        Ok(())
    }

    /// Run the reactor (blocking)
    pub async fn run(&mut self) -> Result<()> {
        // Build handler index
        let handler_index: std::collections::HashMap<u64, usize> = self
            .handlers
            .iter()
            .enumerate()
            .map(|(i, h)| (h.path_hash, i))
            .collect();

        // Clone shared state for worker tasks
        let critical_queue = self.critical_queue.clone();
        let background_cache = self.background_cache.clone();
        let wakeup = self.wakeup.clone();

        info!("FlowKernel v0.1.0 — Rust + Zenoh native SDK (async workers)");
        info!("Registered {} handlers:", self.handlers.len());
        for h in &self.handlers {
            let priority_str = match h.priority {
                TopicPriority::Critical => "CRITICAL",
                TopicPriority::Normal => "NORMAL",
                TopicPriority::Background => "BACKGROUND",
            };
            info!("  [{}] {}", priority_str, h.path);
        }

        // Spawn async worker tasks for each subscriber
        let mut worker_handles = Vec::new();
        for (idx, handler) in self.handlers.iter().enumerate() {
            let key_expr = KeyExpr::try_from(handler.path.clone())
                .map_err(|e| crate::error::ReactorError::ConfigError(e.to_string()))?;

            let priority = handler.priority;
            let handler_idx = idx;  // Use index directly!
            let path_hash = handler.path_hash;
            let critical_queue = critical_queue.clone();
            let background_cache = background_cache.clone();
            let wakeup = wakeup.clone();

            let subscriber = self
                .session
                .declare_subscriber(&key_expr)
                .await
                .map_err(|e| crate::error::ReactorError::ZenohError(e.to_string()))?;

            // Spawn async worker task (correct pattern!)
            let handle = tokio::spawn(async move {
                // Use async stream, NOT try_recv loop!
                while let Ok(sample) = subscriber.recv_async().await {
                    // Zero-copy: Try to use Bytes directly
                    // If lifetime issues, fall back to to_vec()
                    let payload = Bytes::from(sample.payload().to_bytes().to_vec());

                    match priority {
                        TopicPriority::Critical | TopicPriority::Normal => {
                            let cs = CriticalSample {
                                handler_idx,  // Use index, not hash!
                                payload,
                            };
                            if !critical_queue.push(cs) {
                                warn!("MPSC FULL — frame dropped!");
                            }
                            // Instant wakeup for CRITICAL
                            if priority == TopicPriority::Critical {
                                wakeup.notify_one();
                            }
                        }
                        TopicPriority::Background => {
                            if let Some(slot) = background_cache.get_slot(path_hash) {
                                slot.put(path_hash, &payload);
                            }
                        }
                    }
                }
            });

            worker_handles.push(handle);
        }

        // Main event loop (only processes queues, no data fetching!)
        loop {
            // Process CRITICAL + NORMAL queue (O(1) lookup using handler_idx!)
            let mut critical_count = 0u64;
            let mut normal_count = 0u64;
            while let Some(sample) = self.critical_queue.pop() {
                // Direct array access - NO HashMap lookup!
                let idx = sample.handler_idx;
                let priority = self.handlers[idx].priority;
                let callback = self.handlers[idx].callback.clone();
                let payload = sample.payload.clone();

                // Critical: execute immediately (guarantee real-time)
                // Normal/Background: spawn to thread pool (isolate blocking)
                if priority == TopicPriority::Critical {
                    callback(&payload);
                    critical_count += 1;
                } else {
                    tokio::task::spawn_blocking(move || {
                        callback(&payload);
                    });
                    normal_count += 1;
                }
            }
            self.critical_processed.fetch_add(critical_count, Ordering::Release);
            self.normal_processed.fetch_add(normal_count, Ordering::Release);

            // Process BACKGROUND cache
            let mut background_count = 0u64;
            self.background_cache.drain(|hash, data| {
                if let Some(&idx) = handler_index.get(&hash) {
                    let callback = self.handlers[idx].callback.clone();
                    tokio::task::spawn_blocking(move || {
                        callback(&data);
                    });
                    background_count += 1;
                }
            });
            self.background_processed.fetch_add(background_count, Ordering::Release);

            // Wait for wakeup or timeout (1ms for 200Hz data!)
            tokio::select! {
                _ = self.wakeup.notified() => {
                    // CRITICAL data arrived, process immediately
                }
                _ = tokio::time::sleep(std::time::Duration::from_millis(1)) => {
                    // 1ms timeout for BACKGROUND (was 50ms!)
                }
            }
        }
    }

    /// Get critical channel stats
    pub fn critical_stats(&self) -> ChannelStats {
        ChannelStats {
            processed: self.critical_processed.load(Ordering::Relaxed),
            dropped: self.critical_dropped.load(Ordering::Relaxed),
            ..Default::default()
        }
    }

    /// Get normal channel stats
    pub fn normal_stats(&self) -> ChannelStats {
        ChannelStats {
            processed: self.normal_processed.load(Ordering::Relaxed),
            dropped: self.normal_dropped.load(Ordering::Relaxed),
            ..Default::default()
        }
    }

    /// Get background channel stats
    pub fn background_stats(&self) -> ChannelStats {
        ChannelStats {
            processed: self.background_processed.load(Ordering::Relaxed),
            dropped: self.background_dropped.load(Ordering::Relaxed),
            ..Default::default()
        }
    }

    /// Reset all statistics
    pub fn reset_stats(&self) {
        self.critical_processed.store(0, Ordering::Relaxed);
        self.normal_processed.store(0, Ordering::Relaxed);
        self.background_processed.store(0, Ordering::Relaxed);
        self.critical_dropped.store(0, Ordering::Relaxed);
        self.normal_dropped.store(0, Ordering::Relaxed);
        self.background_dropped.store(0, Ordering::Relaxed);
    }

    /// FNV-1a hash for key expressions
    fn fnv1a_hash(&self, s: &str) -> u64 {
        let mut hash: u64 = 0xcbf29ce484222325;
        for byte in s.bytes() {
            hash ^= byte as u64;
            hash = hash.wrapping_mul(0x100000001b3);
        }
        hash
    }
}