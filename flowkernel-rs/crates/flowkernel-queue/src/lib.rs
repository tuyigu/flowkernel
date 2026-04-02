//! # FlowKernel Queue
//!
//! Lock-free MPSC (Multi-Producer, Single-Consumer) bounded ring buffer queue.
//!
//! ## Design
//!
//! - **Producer side**: Lightweight spinlock (only 2 Zenoh RX threads compete)
//! - **Consumer side**: Pure atomic operations, no lock (single Reactor thread)
//! - **Full queue**: Count and drop (never block Zenoh callback threads)
//!
//! ## Usage with Zenoh
//!
//! Zenoh 1.x runtime allocates 2 worker threads for RX callbacks,
//! so we need MPSC semantics (not SPSC).

mod mpsc;

pub use mpsc::MPSCQueue;