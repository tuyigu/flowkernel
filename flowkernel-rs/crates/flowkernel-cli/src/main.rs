use clap::Parser;
use flowkernel_core::{DataFlowReactor, HandlerConfig, TopicPriority};

#[derive(Parser)]
#[command(name = "flowkernel")]
#[command(about = "High-performance robot dataflow middleware")]
struct Cli {
    /// Zenoh config file path
    #[arg(short, long, default_value = "")]
    config: String,
}

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    tracing_subscriber::fmt()
        .with_env_filter("info")
        .init();

    let _cli = Cli::parse();

    let mut reactor = DataFlowReactor::new().await?;

    // Register handlers from config
    reactor.register_handler(HandlerConfig {
        path: "robot/*/cmd/estop".to_string(),
        priority: TopicPriority::Critical,
        callback: |data| {
            println!("[CRITICAL] E-Stop: {} bytes", data.len());
        },
    }).await?;

    reactor.register_handler(HandlerConfig {
        path: "robot/uav0/telemetry".to_string(),
        priority: TopicPriority::Normal,
        callback: |data| {
            println!("[NORMAL] Telemetry: {} bytes", data.len());
        },
    }).await?;

    reactor.register_handler(HandlerConfig {
        path: "robot/*/costmap".to_string(),
        priority: TopicPriority::Background,
        callback: |data| {
            println!("[BACKGROUND] Costmap: {} bytes", data.len());
        },
    }).await?;

    println!("FlowKernel v0.1.0 starting...");
    println!("Press Ctrl+C to stop\n");

    reactor.run().await?;

    Ok(())
}