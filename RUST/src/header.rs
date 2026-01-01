use crate::error::{GbfError, Result};
use crc32fast::Hasher;
use once_cell::sync::Lazy;
use regex::Regex;
use serde::{Deserialize, Deserializer, Serialize};
use serde_json::Value;

pub const MAGIC_BYTES: [u8; 8] = [71, 82, 69, 68, 66, 73, 78, 0]; // "GREDBIN\0"
pub const VERSION: u32 = 1;

static CRC_RE: Lazy<Regex> = Lazy::new(|| {
    // Match: "header_crc32_hex" <ws> : <ws> "DEADBEEF" and capture the whitespace around ':'
    Regex::new(r#""header_crc32_hex"(\s*:\s*)"([0-9A-Fa-f]{8})""#).expect("regex")
});
static CRC_FALLBACK_RE: Lazy<Regex> = Lazy::new(|| {
    Regex::new(r#""header_crc32_hex"\s*:\s*"[^"]+""#).expect("regex")
});

fn de_u64<'de, D>(deserializer: D) -> std::result::Result<u64, D::Error>
where
    D: Deserializer<'de>,
{
    let v = Value::deserialize(deserializer)?;
    match v {
        Value::Number(n) => {
            if let Some(u) = n.as_u64() {
                Ok(u)
            } else if let Some(f) = n.as_f64() {
                Ok(f.round().max(0.0) as u64)
            } else {
                Ok(0)
            }
        }
        Value::String(s) => Ok(s.parse::<u64>().unwrap_or(0)),
        Value::Null => Ok(0),
        _ => Ok(0),
    }
}

fn de_u32<'de, D>(deserializer: D) -> std::result::Result<u32, D::Error>
where
    D: Deserializer<'de>,
{
    let v = Value::deserialize(deserializer)?;
    match v {
        Value::Number(n) => {
            if let Some(u) = n.as_u64() {
                Ok(u.min(u32::MAX as u64) as u32)
            } else if let Some(f) = n.as_f64() {
                Ok(f.round().max(0.0).min(u32::MAX as f64) as u32)
            } else {
                Ok(0)
            }
        }
        Value::String(s) => Ok(s.parse::<u32>().unwrap_or(0)),
        Value::Null => Ok(0),
        _ => Ok(0),
    }
}

fn de_vec_u64<'de, D>(deserializer: D) -> std::result::Result<Vec<u64>, D::Error>
where
    D: Deserializer<'de>,
{
    let v = Value::deserialize(deserializer)?;
    match v {
        Value::Array(arr) => {
            let mut out = Vec::with_capacity(arr.len());
            for x in arr {
                match x {
                    Value::Number(n) => {
                        if let Some(u) = n.as_u64() {
                            out.push(u);
                        } else if let Some(f) = n.as_f64() {
                            out.push(f.round().max(0.0) as u64);
                        } else {
                            out.push(0);
                        }
                    }
                    _ => out.push(0),
                }
            }
            Ok(out)
        }
        Value::Null => Ok(Vec::new()),
        _ => Ok(Vec::new()),
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct FieldMeta {
    pub name: String,
    pub kind: String,

    #[serde(rename = "class")]
    pub class_name: String,

    #[serde(default, deserialize_with = "de_vec_u64")]
    pub shape: Vec<u64>,

    #[serde(default)]
    pub complex: bool,

    #[serde(default)]
    pub encoding: String,

    #[serde(default)]
    pub compression: String,

    #[serde(default, deserialize_with = "de_u64")]
    pub offset: u64,

    #[serde(default, deserialize_with = "de_u64")]
    pub csize: u64,

    #[serde(default, deserialize_with = "de_u64")]
    pub usize: u64,

    #[serde(default, deserialize_with = "de_u32")]
    pub crc32: u32,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Header {
    pub format: String,
    pub magic: String,
    pub version: u32,

    #[serde(default)]
    pub endianness: String,

    #[serde(default)]
    pub order: String,

    #[serde(default)]
    pub root: String,

    #[serde(default)]
    pub created_utc: String,

    #[serde(default)]
    pub matlab_version: String,

    pub fields: Vec<FieldMeta>,

    #[serde(default, deserialize_with = "de_u64")]
    pub payload_start: u64,

    #[serde(default, deserialize_with = "de_u64")]
    pub file_size: u64,

    #[serde(default)]
    pub header_crc32_hex: String,
}

pub fn compute_crc32(bytes: &[u8]) -> u32 {
    let mut h = Hasher::new();
    h.update(bytes);
    h.finalize()
}

pub fn header_json_with_placeholder_crc(header_json: &str) -> String {
    // Replace ONLY the value, preserving whitespace around ':', as in MATLAB.
    let replaced = CRC_RE.replace(header_json, r#""header_crc32_hex"$1"00000000""#);
    if replaced.as_ref() == header_json {
        CRC_FALLBACK_RE
            .replace(header_json, r#""header_crc32_hex":"00000000""#)
            .to_string()
    } else {
        replaced.to_string()
    }
}

pub fn compute_header_crc32_hex_from_original_json(header_json: &str) -> Result<String> {
    let for_crc = header_json_with_placeholder_crc(header_json);
    let crc = compute_crc32(for_crc.as_bytes());
    Ok(format!("{:08X}", crc))
}

pub fn validate_header_crc(header: &Header, header_json: &str) -> Result<()> {
    if header.header_crc32_hex.trim().is_empty() {
        return Ok(());
    }
    let expected = header.header_crc32_hex.trim().to_ascii_uppercase();
    let got = compute_header_crc32_hex_from_original_json(header_json)?;
    if expected != got {
        return Err(GbfError::HeaderCrcMismatch { expected, got });
    }
    Ok(())
}