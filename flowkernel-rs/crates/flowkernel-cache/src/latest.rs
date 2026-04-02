use std::collections::HashMap;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::RwLock;

/// Latest-only sample slot for BACKGROUND topics.
///
/// Design principle:
/// - Producer always overwrites the latest frame
/// - Consumer only takes the latest frame
/// - Compared to FIFO queue, naturally prevents "latency avalanche" backlog
pub struct LatestSampleSlot {
    /// The latest payload data
    payload: RwLock<Option<Vec<u8>>>,
    /// Handler hash for routing
    handler_hash: u64,
    /// Number of frames dropped (overwritten before consumption)
    drop_count: AtomicU64,
}

impl LatestSampleSlot {
    /// Create a new empty slot
    pub fn new(handler_hash: u64) -> Self {
        Self {
            payload: RwLock::new(None),
            handler_hash,
            drop_count: AtomicU64::new(0),
        }
    }

    /// Write the latest frame (overwrite old frame, record drop if any)
    pub fn put(&self, _hash: u64, data: &[u8]) {
        let mut payload = self.payload.write().unwrap();
        
        if payload.is_some() {
            // New frame overwrites unconsumed old frame → count drop
            self.drop_count.fetch_add(1, Ordering::Relaxed);
        }
        
        *payload = Some(data.to_vec());
    }

    /// Take the latest frame and clear the slot (if any)
    pub fn take(&self) -> Option<(u64, Vec<u8>)> {
        let mut payload = self.payload.write().unwrap();
        payload.take().map(|data| (self.handler_hash, data))
    }

    /// Get the drop count
    pub fn drop_count(&self) -> u64 {
        self.drop_count.load(Ordering::Relaxed)
    }
}

/// Latest-only cache manager for BACKGROUND topics.
///
/// Key design: Two-phase access
///
/// - [Init phase] register_handler() → pre_allocate(hash)
///   Pre-allocate all BACKGROUND topic slots before startup, takes write lock.
///
/// - [Hot path] zenoh_data_callback() → get_slot(hash)
///   Map is read-only during runtime, use read lock.
pub struct LatestSampleCache {
    slots: RwLock<HashMap<u64, LatestSampleSlot>>,
}

impl LatestSampleCache {
    /// Create a new empty cache
    pub fn new() -> Self {
        Self {
            slots: RwLock::new(HashMap::new()),
        }
    }

    /// Pre-allocate a slot for the given hash (init phase)
    pub fn pre_allocate(&self, hash: u64) {
        let mut slots = self.slots.write().unwrap();
        slots.entry(hash).or_insert_with(|| LatestSampleSlot::new(hash));
    }

    /// Get a slot by hash (hot path, read lock)
    pub fn get_slot(&self, hash: u64) -> Option<impl std::ops::Deref<Target = LatestSampleSlot> + '_> {
        let slots = self.slots.read().unwrap();
        // SAFETY: Reuse the same read lock (no double lock)
        if slots.contains_key(&hash) {
            Some(SlotRef { slots, hash })
        } else {
            None
        }
    }

    /// Drain all slots, consuming the latest frame from each
    pub fn drain<F>(&self, mut consumer: F)
    where
        F: FnMut(u64, Vec<u8>),
    {
        let slots = self.slots.read().unwrap();
        for slot in slots.values() {
            // Use try_write to avoid deadlock (read lock -> write lock)
            if let Ok(mut payload) = slot.payload.try_write() {
                if let Some(data) = payload.take() {
                    consumer(slot.handler_hash, data);
                }
            }
        }
    }

    /// Print drop statistics for all slots
    pub fn print_drop_stats(&self) {
        let slots = self.slots.read().unwrap();
        for (hash, slot) in slots.iter() {
            let dropped = slot.drop_count();
            if dropped > 0 {
                eprintln!(
                    "[STAT] BACKGROUND slot 0x{:016x}: dropped={} frames",
                    hash, dropped
                );
            }
        }
    }

    /// Get total drop count across all slots
    pub fn total_drops(&self) -> u64 {
        let slots = self.slots.read().unwrap();
        slots.values().map(|s| s.drop_count()).sum()
    }
}

impl Default for LatestSampleCache {
    fn default() -> Self {
        Self::new()
    }
}

/// Helper struct to hold a reference to a slot while keeping the lock alive
struct SlotRef<'a> {
    slots: std::sync::RwLockReadGuard<'a, HashMap<u64, LatestSampleSlot>>,
    hash: u64,
}

impl<'a> std::ops::Deref for SlotRef<'a> {
    type Target = LatestSampleSlot;

    fn deref(&self) -> &Self::Target {
        self.slots.get(&self.hash).unwrap()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_pre_allocate_and_get_slot() {
        let cache = LatestSampleCache::new();
        cache.pre_allocate(100);
        
        let slot = cache.get_slot(100);
        assert!(slot.is_some());
        
        let unknown = cache.get_slot(999);
        assert!(unknown.is_none());
    }

    #[test]
    fn test_put_and_take() {
        let cache = LatestSampleCache::new();
        cache.pre_allocate(300);
        
        let slot = cache.get_slot(300).unwrap();
        slot.put(300, &[1, 2, 3, 4, 5]);
        
        let result = slot.take();
        assert!(result.is_some());
        let (hash, data) = result.unwrap();
        assert_eq!(hash, 300);
        assert_eq!(data, vec![1, 2, 3, 4, 5]);
    }

    #[test]
    fn test_overwrite_data() {
        let cache = LatestSampleCache::new();
        cache.pre_allocate(400);
        
        let slot = cache.get_slot(400).unwrap();
        slot.put(400, &[10, 20, 30]);
        slot.put(400, &[40, 50, 60]);  // Overwrite
        
        assert_eq!(slot.drop_count(), 1);  // One drop
        
        let result = slot.take().unwrap();
        assert_eq!(result.1, vec![40, 50, 60]);  // Should be latest data
    }

    #[test]
    fn test_empty_slot_take() {
        let cache = LatestSampleCache::new();
        cache.pre_allocate(500);
        
        let slot = cache.get_slot(500).unwrap();
        let result = slot.take();
        assert!(result.is_none());
    }

    #[test]
    fn test_drain_all_slots() {
        let cache = LatestSampleCache::new();
        cache.pre_allocate(100);
        cache.pre_allocate(200);
        cache.pre_allocate(300);
        
        let slot1 = cache.get_slot(100).unwrap();
        let slot2 = cache.get_slot(200).unwrap();
        let slot3 = cache.get_slot(300).unwrap();
        
        slot1.put(100, &[1]);
        slot2.put(200, &[2]);
        slot3.put(300, &[3]);
        
        let mut collected = Vec::new();
        cache.drain(|hash, data| {
            collected.push((hash, data));
        });
        
        assert_eq!(collected.len(), 3);
        
        // Verify all data was collected
        let hashes: std::collections::HashSet<_> = collected.iter().map(|(h, _)| *h).collect();
        assert!(hashes.contains(&100));
        assert!(hashes.contains(&200));
        assert!(hashes.contains(&300));
    }

    #[test]
    fn test_drain_empty_slots() {
        let cache = LatestSampleCache::new();
        cache.pre_allocate(700);
        cache.pre_allocate(800);
        
        let mut collected = Vec::new();
        cache.drain(|hash, data| {
            collected.push((hash, data));
        });
        
        assert!(collected.is_empty());  // Empty slots don't produce data
    }

    #[test]
    fn test_total_drops() {
        let cache = LatestSampleCache::new();
        cache.pre_allocate(600);
        
        let slot = cache.get_slot(600).unwrap();
        
        for i in 0..10 {
            slot.put(600, &[i]);
        }
        
        assert_eq!(cache.total_drops(), 9);  // 9 drops
    }
}