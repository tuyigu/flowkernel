//! Basic pub/sub example for FlowKernel
//!
//! This example demonstrates:
//! 1. Creating a DataFlowReactor
//! 2. Registering handlers for different priority topics
//! 3. Running the reactor to process messages
//!
//! Run with: `cargo run --example basic_pubsub`

use flowkernel_core::{DataFlowReactor, HandlerConfig, TopicPriority};

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    // Initialize tracing
    tracing_subscriber::fmt()
        .with_env_filter("info")
        .init();

    println!("🦀 FlowKernel Rust Example — Basic Pub/Sub");
    println!("==========================================\n");

    // Create reactor
    let mut reactor = DataFlowReactor::new().await?;

    // Register CRITICAL handler (E-Stop)
    reactor.register_handler(HandlerConfig {
        path: "robot/*/cmd/estop".to_string(),
        priority: TopicPriority::Critical,
        callback: |data| {
            println!("🚨 [CRITICAL] E-Stop received! {} bytes", data.len());
        },
    }).await?;

    // Register NORMAL handler (Telemetry)
    reactor.register_handler(HandlerConfig {
        path: "robot/uav0/telemetry".to_string(),
        priority: TopicPriority::Normal,
        callback: |data| {
            println!("📊 [NORMAL] Telemetry received: {} bytes", data.len());
        },
    }).await?;

    // Register BACKGROUND handler (Costmap)
    reactor.register_handler(HandlerConfig {
        path: "robot/*/costmap".to_string(),
        priority: TopicPriority::Background,
        callback: |data| {
            println!("🗺️  [BACKGROUND] Costmap received: {} bytes", data.len());
        },
    }).await?;

    println!("Starting reactor...\n");
    println!("Press Ctrl+C to stop\n");

    // Run reactor
    reactor.run().await?;

    Ok(())
}