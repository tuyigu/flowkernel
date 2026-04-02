//! # FlowKernel Core
//!
//! High-performance robot dataflow middleware powered by Zenoh.
//!
//! ## Features
//!
//! - **Multi-priority channels**: CRITICAL (E-Stop), NORMAL (Telemetry), BACKGROUND (Costmap)
//! - **Low-latency wakeup**: CRITICAL data triggers instant Reactor wakeup
//! - **Zero-copy**: `Bytes` zero-copy view to handler callbacks
//! - **Memory pool**: `bytes::Bytes` arena pool, batch release per cycle
//! - **Native Rust**: Uses Zenoh Rust SDK, no C API binding hassle
//!
//! ## Quick Start
//!
//! ```rust,no_run
//! use flowkernel_core::{DataFlowReactor, TopicPriority, HandlerConfig};
//! use zenoh::prelude::rust::*;
//!
//! #[tokio::main]
//! async fn main() -> anyhow::Result<()> {
//!     let mut reactor = DataFlowReactor::new().await?;
//!     
//!     reactor.register_handler(HandlerConfig {
//!         path: "robot/*/cmd/estop".to_string(),
//!         priority: TopicPriority::Critical,
//!         callback: |data| {
//!             println!("E-Stop received: {} bytes", data.len());
//!         },
//!     }).await?;
//!     
//!     reactor.run().await?;
//!     Ok(())
//! }
//! ```

mod reactor;
mod error;

pub use reactor::{DataFlowReactor, TopicPriority, HandlerConfig, ChannelStats};
pub use error::ReactorError;