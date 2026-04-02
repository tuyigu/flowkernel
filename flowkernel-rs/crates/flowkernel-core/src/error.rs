use thiserror::Error;

/// Reactor error types
#[derive(Error, Debug)]
pub enum ReactorError {
    #[error("Zenoh session error: {0}")]
    ZenohError(String),

    #[error("Handler registration failed: {0}")]
    HandlerRegistrationFailed(String),

    #[error("Configuration error: {0}")]
    ConfigError(String),

    #[error("Channel backpressure: {dropped} frames dropped")]
    BackpressureHigh { dropped: u64 },

    #[error("Liveliness lost for robot {robot_id}")]
    LivelinessLost { robot_id: String },
}

/// Result type for Reactor operations
pub type Result<T> = std::result::Result<T, ReactorError>;