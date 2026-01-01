use thiserror::Error;

#[derive(Debug, Error)]
pub enum GbfError {
    #[error("I/O error: {0}")]
    Io(#[from] std::io::Error),

    #[error("UTF-8 error: {0}")]
    Utf8(#[from] std::string::FromUtf8Error),

    #[error("JSON error: {0}")]
    Json(#[from] serde_json::Error),

    #[error("format error: {0}")]
    Format(String),

    #[error("unsupported value: {0}")]
    Unsupported(String),

    #[error("header CRC mismatch: expected {expected}, got {got}")]
    HeaderCrcMismatch { expected: String, got: String },

    #[error("file size mismatch: expected {expected} bytes, got {got} bytes")]
    FileSizeMismatch { expected: u64, got: u64 },

    #[error("variable not found: {0}")]
    VarNotFound(String),

    #[error("field `{name}` chunk out of bounds (offset {offset}, csize {csize}, payload_len {payload_len})")]
    FieldOutOfBounds {
        name: String,
        offset: u64,
        csize: u64,
        payload_len: u64,
    },

    #[error("failed to decompress field `{name}`: {message}")]
    DecompressionFailed { name: String, message: String },

    #[error("field `{name}` decoded size mismatch: expected {expected} bytes, got {got} bytes")]
    FieldSizeMismatch { name: String, expected: u64, got: u64 },

    #[error("field `{name}` CRC mismatch: expected {expected:08X}, got {got:08X}")]
    FieldCrcMismatch { name: String, expected: u32, got: u32 },
}

pub type Result<T> = std::result::Result<T, GbfError>;