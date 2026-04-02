//! # FlowKernel Cache
//!
//! Latest-only sample cache for high-frequency BACKGROUND topics.
//!
//! ## Design Principle
//!
//! - BACKGROUND topics (like Costmap) publish at ~200Hz
//! - Ground station only needs 10Hz rendering
//! - Latest-only strategy: producer always overwrites, consumer only takes latest
//! - Compared to FIFO queue, naturally prevents "latency avalanche" backlog
//!
//! ## Thread Safety
//!
//! - `put()`: Zenoh RX callback thread (multi-thread), needs lock
//! - `take()`: Reactor main thread (single-thread), needs lock
//! - `drop_count`: Atomic counter, lock-free

mod latest;

pub use latest::LatestSampleCache;