use std::cell::UnsafeCell;
use std::mem::MaybeUninit;
use std::sync::atomic::{AtomicU64, AtomicUsize, Ordering};
use std::sync::Mutex;

/// MPSC bounded ring buffer queue for high-performance data channels.
///
/// # Design Choices
///
/// - Producer side: Lightweight spinlock (only 2 Zenoh RX threads compete)
/// - Consumer side: Pure atomic operations, no lock (single Reactor thread)
/// - Full queue: Count and drop (never block Zenoh callback threads)
///
/// # Type Parameters
///
/// - `T`: Element type (must support move semantics)
/// - `CAPACITY`: Queue capacity, must be a power of 2
pub struct MPSCQueue<T, const CAPACITY: usize> {
    buffer: Box<[UnsafeCell<MaybeUninit<T>>; CAPACITY]>,
    head: AtomicUsize,
    tail: AtomicUsize,
    dropped: AtomicU64,
    push_lock: Mutex<()>,
}

// SAFETY: MPSC queue is safe to send/sync when T is Send
unsafe impl<T: Send, const CAPACITY: usize> Send for MPSCQueue<T, CAPACITY> {}
unsafe impl<T: Send, const CAPACITY: usize> Sync for MPSCQueue<T, CAPACITY> {}

impl<T, const CAPACITY: usize> MPSCQueue<T, CAPACITY> {
    /// Create a new MPSC queue.
    ///
    /// # Panics
    ///
    /// Panics if `CAPACITY` is not a power of 2.
    pub fn new() -> Self {
        assert!(
            CAPACITY.is_power_of_two(),
            "CAPACITY must be a power of 2, got {}",
            CAPACITY
        );

        // Create uninitialized buffer
        let buffer: Box<[UnsafeCell<MaybeUninit<T>>; CAPACITY]> = {
            let mut vec: Vec<UnsafeCell<MaybeUninit<T>>> = Vec::with_capacity(CAPACITY);
            for _ in 0..CAPACITY {
                vec.push(UnsafeCell::new(MaybeUninit::uninit()));
            }
            vec.into_boxed_slice()
                .try_into()
                .ok()
                .unwrap()
        };

        Self {
            buffer,
            head: AtomicUsize::new(0),
            tail: AtomicUsize::new(0),
            dropped: AtomicU64::new(0),
            push_lock: Mutex::new(()),
        }
    }

    /// Push an item into the queue (thread-safe, multi-producer).
    ///
    /// Returns `true` if successful, `false` if the queue is full (item is dropped).
    pub fn push(&self, item: T) -> bool {
        let _lock = self.push_lock.lock().unwrap();

        let tail = self.tail.load(Ordering::Relaxed);
        let next = (tail + 1) & (CAPACITY - 1);

        if next == self.head.load(Ordering::Acquire) {
            // Queue is full: count and drop
            self.dropped.fetch_add(1, Ordering::Relaxed);
            return false;
        }

        // Write item to buffer
        unsafe {
            let ptr = self.buffer[tail].get();
            (*ptr).write(item);
        }

        self.tail.store(next, Ordering::Release);
        true
    }

    /// Pop an item from the queue (single consumer only).
    ///
    /// Returns `Some(item)` if successful, `None` if the queue is empty.
    pub fn pop(&self) -> Option<T> {
        let head = self.head.load(Ordering::Relaxed);

        if head == self.tail.load(Ordering::Acquire) {
            return None; // Queue is empty
        }

        // Read item from buffer
        let item = unsafe {
            let ptr = self.buffer[head].get();
            (*ptr).assume_init_read()
        };

        let next = (head + 1) & (CAPACITY - 1);
        self.head.store(next, Ordering::Release);

        Some(item)
    }

    /// Get the number of dropped frames (queue was full).
    pub fn dropped_count(&self) -> u64 {
        self.dropped.load(Ordering::Relaxed)
    }

    /// Get the current number of items in the queue.
    pub fn len(&self) -> usize {
        let tail = self.tail.load(Ordering::Acquire);
        let head = self.head.load(Ordering::Acquire);
        tail.wrapping_sub(head) & (CAPACITY - 1)
    }

    /// Check if the queue is empty.
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    /// Check if the queue is full.
    pub fn is_full(&self) -> bool {
        let tail = self.tail.load(Ordering::Acquire);
        let head = self.head.load(Ordering::Acquire);
        let next = (tail + 1) & (CAPACITY - 1);
        next == head
    }
}

impl<T, const CAPACITY: usize> Default for MPSCQueue<T, CAPACITY> {
    fn default() -> Self {
        Self::new()
    }
}

impl<T, const CAPACITY: usize> Drop for MPSCQueue<T, CAPACITY> {
    fn drop(&mut self) {
        // Drop all remaining items
        while self.pop().is_some() {}
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_basic_push_pop() {
        let queue = MPSCQueue::<i32, 64>::new();
        assert!(queue.push(42));
        assert_eq!(queue.pop(), Some(42));
        assert_eq!(queue.pop(), None);
    }

    #[test]
    fn test_fifo_order() {
        let queue = MPSCQueue::<i32, 64>::new();
        queue.push(1);
        queue.push(2);
        queue.push(3);

        assert_eq!(queue.pop(), Some(1));
        assert_eq!(queue.pop(), Some(2));
        assert_eq!(queue.pop(), Some(3));
        assert_eq!(queue.pop(), None);
    }

    #[test]
    fn test_full_queue() {
        let queue = MPSCQueue::<i32, 4>::new(); // Capacity 4, usable 3
        assert!(queue.push(1));
        assert!(queue.push(2));
        assert!(queue.push(3));
        assert!(!queue.push(4)); // Full!
        assert_eq!(queue.dropped_count(), 1);
    }

    #[test]
    fn test_wrap_around() {
        let queue = MPSCQueue::<i32, 4>::new();
        queue.push(1);
        queue.push(2);
        queue.push(3);
        queue.pop();
        queue.pop();
        queue.push(4);
        queue.push(5);

        assert_eq!(queue.pop(), Some(3));
        assert_eq!(queue.pop(), Some(4));
        assert_eq!(queue.pop(), Some(5));
    }

    #[test]
    fn test_len_and_empty() {
        let queue = MPSCQueue::<i32, 64>::new();
        assert!(queue.is_empty());
        assert_eq!(queue.len(), 0);

        queue.push(1);
        assert!(!queue.is_empty());
        assert_eq!(queue.len(), 1);

        queue.push(2);
        assert_eq!(queue.len(), 2);

        queue.pop();
        assert_eq!(queue.len(), 1);

        queue.pop();
        assert!(queue.is_empty());
    }
}