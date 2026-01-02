use crate::error::{GbfError, Result};
use crate::header::{
    compute_crc32, compute_header_crc32_hex_from_original_json, validate_header_crc, FieldMeta, Header,
    MAGIC_BYTES, VERSION,
};
use crate::value::{
    CalendarDurationArray, CategoricalArray, CharArray, DateTimeArray, DurationArray,
    GbfValue, LogicalArray, NumericArray, NumericClass, StringArray,
};
use flate2::read::ZlibDecoder;
use flate2::write::ZlibEncoder;
use flate2::Compression;
use std::collections::BTreeMap;
use std::fs::File;
use std::io::{BufReader, BufWriter, Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};
use tempfile::NamedTempFile;
use time::macros::format_description;
use time::OffsetDateTime;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CompressionMode {
    Auto,
    Always,
    Never,
}

#[derive(Debug, Clone)]
pub struct WriteOptions {
    pub compression: bool,
    pub compression_mode: CompressionMode,
    /// 0..=9
    pub compression_level: u32,
    pub crc: bool,
    pub pretty_header: bool,
}

impl Default for WriteOptions {
    fn default() -> Self {
        Self {
            compression: true,
            compression_mode: CompressionMode::Auto,
            compression_level: 1,
            crc: false,
            pretty_header: false,
        }
    }
}

#[derive(Debug, Clone)]
pub struct ReadOptions {
    pub validate: bool,
}

impl Default for ReadOptions {
    fn default() -> Self {
        Self { validate: false }
    }
}

// Heuristic constants similar to MATLAB's spirit (speed-first, but can compress).
const COMPRESS_THRESHOLD_BYTES: usize = 1024;
const AUTO_COMPRESS_THRESHOLD_BYTES: usize = 64 * 1024;
const AUTO_ENTROPY_SAMPLE_BYTES: usize = 4096;
const AUTO_ENTROPY_MAX_UNIQUE_RATIO: f64 = 0.95;

// Coalesced random-access read grouping.
const READ_COALESCE_MAX_GAP_BYTES: u64 = 4096;
const READ_COALESCE_MAX_GROUP_BYTES: u64 = 8 * 1024 * 1024;

// Hardening caps. These are not format limits; they prevent accidental/hostile OOM.
// Large matrices are supported: set field limits to 16 GiB.
const MAX_HEADER_LEN: u32 = 64 * 1024 * 1024; // 64 MiB
const MAX_FIELD_USIZE: u64 = 16u64 * 1024u64 * 1024u64 * 1024u64; // 16 GiB
const MAX_FIELD_CSIZE: u64 = 16u64 * 1024u64 * 1024u64 * 1024u64; // 16 GiB

fn checked_add_u64(a: u64, b: u64) -> Result<u64> {
    a.checked_add(b)
        .ok_or_else(|| GbfError::Format("u64 addition overflow".to_string()))
}

fn mul_usize(a: usize, b: usize) -> Result<usize> {
    a.checked_mul(b)
        .ok_or_else(|| GbfError::Format("usize multiplication overflow".to_string()))
}

fn element_count_checked(shape: &[usize]) -> Result<usize> {
    if shape.is_empty() {
        return Ok(0);
    }

    // MATLAB/GBF may represent empty arrays with one or more zero-sized dimensions.
    // In that case, the element count is 0.
    if shape.iter().any(|&d| d == 0) {
        return Ok(0);
    }

    let mut n: usize = 1;
    for &d in shape {
        n = mul_usize(n, d)?;
    }
    Ok(n)
}

fn u64_to_usize(v: u64, what: &str) -> Result<usize> {
    usize::try_from(v).map_err(|_| GbfError::Unsupported(format!("{} too large for this platform", what)))
}

fn normalize_path<P: AsRef<Path>>(path: P) -> PathBuf {
    let p = path.as_ref();
    if p.extension().is_some() {
        return p.to_path_buf();
    }
    let mut out = p.to_path_buf();
    out.set_extension("gbf");
    out
}

fn read_u32_le<R: Read>(r: &mut R) -> Result<u32> {
    let mut b = [0u8; 4];
    r.read_exact(&mut b)?;
    Ok(u32::from_le_bytes(b))
}

fn write_u32_le<W: Write>(w: &mut W, v: u32) -> Result<()> {
    w.write_all(&v.to_le_bytes())?;
    Ok(())
}

/// Read and parse the GBF header (and return the raw header JSON) without decoding any payload.
///
/// This is intended for CLI/header inspection use-cases.
pub fn read_header_only<P: AsRef<Path>>(path: P, opts: ReadOptions) -> Result<(Header, u32, String)> {
    let path = normalize_path(path);
    let mut file = File::open(&path)?;
    read_header_and_json(&mut file, &opts)
}

fn should_try_compress(kind: &str, class_name: &str, raw: &[u8]) -> bool {
    if raw.len() < COMPRESS_THRESHOLD_BYTES {
        return false;
    }

    // Floats tend to be high entropy; only consider above larger threshold.
    if kind == "numeric" && (class_name == "double" || class_name == "single") {
        if raw.len() < AUTO_COMPRESS_THRESHOLD_BYTES {
            return false;
        }
    }

    let sample_len = raw.len().min(AUTO_ENTROPY_SAMPLE_BYTES);
    if sample_len == 0 {
        return false;
    }

    let mut seen = [false; 256];
    let mut unique = 0usize;
    for &b in &raw[..sample_len] {
        let idx = b as usize;
        if !seen[idx] {
            seen[idx] = true;
            unique += 1;
        }
    }
    let ratio = unique as f64 / sample_len as f64;
    ratio <= AUTO_ENTROPY_MAX_UNIQUE_RATIO
}

fn zlib_compress(raw: &[u8], level: u32) -> Result<Vec<u8>> {
    let level = level.min(9);
    let mut enc = ZlibEncoder::new(Vec::new(), Compression::new(level));
    enc.write_all(raw)?;
    let out = enc.finish()?;
    Ok(out)
}

fn zlib_decompress(comp: &[u8], max_out: u64) -> Result<Vec<u8>> {
    let max_out = max_out.min(MAX_FIELD_USIZE);
    let dec = ZlibDecoder::new(comp);
    let mut out = Vec::new();

    // Read at most max_out + 1 bytes to detect overflow.
    let mut limited = dec.take(max_out.saturating_add(1));
    limited.read_to_end(&mut out)?;
    if out.len() as u64 > max_out {
        return Err(GbfError::Format("decompressed data exceeds configured limit".to_string()));
    }
    Ok(out)
}

fn now_utc_string() -> String {
    let fmt = format_description!("[year]-[month]-[day]T[hour]:[minute]:[second]Z");
    OffsetDateTime::now_utc().format(&fmt).unwrap_or_else(|_| "".to_string())
}

fn assign_by_path(root: &mut BTreeMap<String, GbfValue>, path: &str, value: GbfValue) -> Result<()> {
    if path.is_empty() {
        return Err(GbfError::Format("empty field name".to_string()));
    }
    let parts: Vec<&str> = path.split('.').collect();
    let mut cur = root;

    for (i, part) in parts.iter().enumerate() {
        if part.is_empty() {
            return Err(GbfError::Format(format!("invalid path `{}`", path)));
        }
        if i == parts.len() - 1 {
            cur.insert(part.to_string(), value);
            return Ok(());
        }

        let entry = cur.entry(part.to_string()).or_insert_with(|| GbfValue::Struct(BTreeMap::new()));
        match entry {
            GbfValue::Struct(m) => cur = m,
            _ => {
                return Err(GbfError::Format(format!(
                    "path collision at `{}` inserting `{}`",
                    part, path
                )))
            }
        }
    }
    Ok(())
}

fn flatten_to_leaves(value: &GbfValue, prefix: &str, out: &mut Vec<(String, GbfValue)>) -> Result<()> {
    match value {
        GbfValue::Struct(map) => {
            // In MATLAB, non-empty scalar structs are expanded into leaves.
            // Empty scalar struct is a leaf (represented by EmptyStruct in Rust).
            for (k, v) in map.iter() {
                if k.contains('.') {
                    return Err(GbfError::Unsupported(format!(
                        "struct key `{}` contains '.', not supported in GBF paths",
                        k
                    )));
                }
                let name = if prefix.is_empty() {
                    k.clone()
                } else {
                    format!("{}.{}", prefix, k)
                };
                flatten_to_leaves(v, &name, out)?;
            }
            Ok(())
        }
        other => {
            let name = if prefix.is_empty() { "data".to_string() } else { prefix.to_string() };
            out.push((name, other.clone()));
            Ok(())
        }
    }
}

fn encode_leaf(name: &str, value: &GbfValue) -> Result<(Vec<u8>, String, String, Vec<u64>, bool, String)> {
    // returns: raw_bytes, kind, class_name, shape, complex, encoding
    match value {
        GbfValue::Numeric(arr) => {
            let n = element_count_checked(&arr.shape)?;
            let bpe = arr.class.bytes_per_element();
            let expected = mul_usize(n, bpe)?;

            if arr.real_le.len() != expected {
                return Err(GbfError::Format(format!(
                    "numeric `{}` real_le size mismatch: expected {} bytes, got {}",
                    name, expected, arr.real_le.len()
                )));
            }

            if !arr.complex {
                if arr.imag_le.is_some() {
                    return Err(GbfError::Format(format!(
                        "numeric `{}` is not complex but imag_le is present",
                        name
                    )));
                }
            } else {
                let imag = arr.imag_le.as_ref().ok_or_else(|| {
                    GbfError::Format(format!("numeric array `{}` is complex but imag_le is None", name))
                })?;
                if imag.len() != expected {
                    return Err(GbfError::Format(format!(
                        "numeric `{}` imag_le size mismatch: expected {} bytes, got {}",
                        name, expected, imag.len()
                    )));
                }
            }

            let mut raw = Vec::with_capacity(expected * if arr.complex { 2 } else { 1 });
            raw.extend_from_slice(&arr.real_le);
            if arr.complex {
                raw.extend_from_slice(arr.imag_le.as_ref().unwrap());
            }
            let shape_u64: Vec<u64> = arr.shape.iter().map(|&d| d as u64).collect();
            Ok((
                raw,
                "numeric".to_string(),
                arr.class.as_matlab_class().to_string(),
                shape_u64,
                arr.complex,
                "".to_string(),
            ))
        }
        GbfValue::Logical(a) => {
            let shape_u64: Vec<u64> = a.shape.iter().map(|&d| d as u64).collect();
            Ok((
                a.data.clone(),
                "logical".to_string(),
                "logical".to_string(),
                shape_u64,
                false,
                "".to_string(),
            ))
        }
        GbfValue::Char(a) => {
            let mut raw = Vec::with_capacity(a.data.len() * 2);
            for &u in &a.data {
                raw.extend_from_slice(&u.to_le_bytes());
            }
            let shape_u64: Vec<u64> = a.shape.iter().map(|&d| d as u64).collect();
            Ok((
                raw,
                "char".to_string(),
                "char".to_string(),
                shape_u64,
                false,
                "utf-16-codeunits".to_string(),
            ))
        }
        GbfValue::String(a) => {
            let n = element_count_checked(&a.shape)?;
            if a.data.len() != n {
                return Err(GbfError::Format(format!(
                    "string `{}` shape {:?} implies N={}, but data.len={}",
                    name, a.shape, n, a.data.len()
                )));
            }

            // Layout: for each element: [miss_flag u8][len u32][bytes...]
            let mut raw = Vec::new();
            for opt in &a.data {
                match opt {
                    None => {
                        raw.push(1u8);
                        raw.extend_from_slice(&0u32.to_le_bytes());
                    }
                    Some(s) => {
                        raw.push(0u8);
                        let b = s.as_bytes();
                        let len = u32::try_from(b.len()).map_err(|_| {
                            GbfError::Unsupported(format!("string too large in `{}`", name))
                        })?;
                        raw.extend_from_slice(&len.to_le_bytes());
                        raw.extend_from_slice(b);
                    }
                }
            }

            let shape_u64: Vec<u64> = a.shape.iter().map(|&d| d as u64).collect();
            Ok((
                raw,
                "string".to_string(),
                "string".to_string(),
                shape_u64,
                false,
                "utf-8".to_string(),
            ))
        }
        GbfValue::DateTime(a) => {
            let n = element_count_checked(&a.shape)?;
            if a.is_nat.len() != n
                || a.year.len() != n
                || a.month.len() != n
                || a.day.len() != n
                || a.ms_day.len() != n
            {
                return Err(GbfError::Format(format!(
                    "datetime `{}` inconsistent component lengths for shape {:?}",
                    name, a.shape
                )));
            }

            let tz = a.tz.clone().unwrap_or_default();
            let loc = a.locale.clone().unwrap_or_default();
            let fmt = a.format.clone().unwrap_or_default();

            let tz_bytes = tz.as_bytes();
            let loc_bytes = loc.as_bytes();
            let fmt_bytes = fmt.as_bytes();

            let tz_len = u32::try_from(tz_bytes.len()).unwrap_or(u32::MAX);
            let loc_len = u32::try_from(loc_bytes.len()).unwrap_or(u32::MAX);
            let fmt_len = u32::try_from(fmt_bytes.len()).unwrap_or(u32::MAX);

            let tz_present = !tz.is_empty();
            let mut flags: u8 = 0;
            if tz_present {
                flags |= 1;
            }
            if fmt_len > 0 {
                flags |= 2;
            }
            if !tz_present {
                flags |= 4; // naive_components
            }
            if loc_len > 0 {
                flags |= 8;
            }

            let mut raw = Vec::new();
            raw.push(flags);

            // tz_len + tz_bytes
            raw.extend_from_slice(&tz_len.to_le_bytes());
            raw.extend_from_slice(tz_bytes);

            // loc_len + loc_bytes
            raw.extend_from_slice(&loc_len.to_le_bytes());
            raw.extend_from_slice(loc_bytes);

            // fmt_len + fmt_bytes
            raw.extend_from_slice(&fmt_len.to_le_bytes());
            raw.extend_from_slice(fmt_bytes);

            // mask
            raw.extend_from_slice(&a.is_nat);

            // year int16 LE
            for &y in &a.year {
                raw.extend_from_slice(&y.to_le_bytes());
            }
            // month u8
            raw.extend_from_slice(&a.month);
            // day u8
            raw.extend_from_slice(&a.day);
            // ms_day int32 LE
            for &ms in &a.ms_day {
                raw.extend_from_slice(&ms.to_le_bytes());
            }

            let shape_u64: Vec<u64> = a.shape.iter().map(|&d| d as u64).collect();
            let encoding = if tz_present {
                "dt:tz-ymd+msday+nat-mask+tz+locale+format"
            } else {
                "dt:naive-ymd+msday+nat-mask+locale+format"
            };

            Ok((
                raw,
                "datetime".to_string(),
                "datetime".to_string(),
                shape_u64,
                false,
                encoding.to_string(),
            ))
        }
        GbfValue::Duration(a) => {
            let n = element_count_checked(&a.shape)?;
            if a.is_nan.len() != n || a.ms.len() != n {
                return Err(GbfError::Format(format!(
                    "duration `{}` inconsistent lengths for shape {:?}",
                    name, a.shape
                )));
            }
            let mut raw = Vec::new();
            raw.extend_from_slice(&a.is_nan);
            for &ms in &a.ms {
                raw.extend_from_slice(&ms.to_le_bytes());
            }
            let shape_u64: Vec<u64> = a.shape.iter().map(|&d| d as u64).collect();
            Ok((
                raw,
                "duration".to_string(),
                "duration".to_string(),
                shape_u64,
                false,
                "ms-i64+nan-mask".to_string(),
            ))
        }
        GbfValue::CalendarDuration(a) => {
            let n = element_count_checked(&a.shape)?;
            if a.is_missing.len() != n || a.months.len() != n || a.days.len() != n || a.time_ms.len() != n {
                return Err(GbfError::Format(format!(
                    "calendarDuration `{}` inconsistent lengths for shape {:?}",
                    name, a.shape
                )));
            }
            let mut raw = Vec::new();
            raw.extend_from_slice(&a.is_missing);
            for &m in &a.months {
                raw.extend_from_slice(&m.to_le_bytes());
            }
            for &d in &a.days {
                raw.extend_from_slice(&d.to_le_bytes());
            }
            for &t in &a.time_ms {
                raw.extend_from_slice(&t.to_le_bytes());
            }
            let shape_u64: Vec<u64> = a.shape.iter().map(|&d| d as u64).collect();
            Ok((
                raw,
                "calendarduration".to_string(),
                "calendarDuration".to_string(),
                shape_u64,
                false,
                "mask+months-i32+days-i32+time-ms-i64".to_string(),
            ))
        }
        GbfValue::Categorical(a) => {
            let n = element_count_checked(&a.shape)?;
            if a.codes.len() != n {
                return Err(GbfError::Format(format!(
                    "categorical `{}` codes.len != N for shape {:?}",
                    name, a.shape
                )));
            }
            let n_cats = u32::try_from(a.categories.len()).map_err(|_| {
                GbfError::Unsupported(format!("too many categories in `{}`", name))
            })?;

            let mut raw = Vec::new();
            raw.extend_from_slice(&n_cats.to_le_bytes());

            for cat in &a.categories {
                let b = cat.as_bytes();
                let len = u32::try_from(b.len()).map_err(|_| {
                    GbfError::Unsupported(format!("category string too large in `{}`", name))
                })?;
                raw.extend_from_slice(&len.to_le_bytes());
                raw.extend_from_slice(b);
            }

            for &c in &a.codes {
                raw.extend_from_slice(&c.to_le_bytes());
            }

            let shape_u64: Vec<u64> = a.shape.iter().map(|&d| d as u64).collect();
            Ok((
                raw,
                "categorical".to_string(),
                "categorical".to_string(),
                shape_u64,
                false,
                "cats-utf8+codes-u32".to_string(),
            ))
        }
        GbfValue::EmptyStruct => {
            Ok((
                Vec::new(),
                "struct".to_string(),
                "struct".to_string(),
                vec![1, 1],
                false,
                "empty-scalar-struct".to_string(),
            ))
        }
        GbfValue::Struct(_) => Err(GbfError::Unsupported(format!(
            "non-leaf struct encountered at `{}`; structs must be flattened before encoding",
            name
        ))),
    }
}

fn decode_leaf(field: &FieldMeta, raw: &[u8]) -> Result<GbfValue> {
    let kind = field.kind.to_ascii_lowercase();
    let shape_u64 = &field.shape;
    let shape: Vec<usize> = shape_u64
        .iter()
        .map(|&d| u64_to_usize(d, "shape dim"))
        .collect::<Result<Vec<_>>>()?;
    let n = element_count_checked(&shape)?;

    match kind.as_str() {
        "struct" => Ok(GbfValue::EmptyStruct),

        "numeric" => {
            let cls = NumericClass::from_matlab_class(&field.class_name)
                .ok_or_else(|| GbfError::Unsupported(format!("unknown numeric class `{}`", field.class_name)))?;

            let bpe = cls.bytes_per_element();
            let part_bytes = mul_usize(n, bpe)?;

            if !field.complex {
                if raw.len() != part_bytes {
                    return Err(GbfError::Format(format!(
                        "numeric `{}` size mismatch: expected {} bytes, got {}",
                        field.name, part_bytes, raw.len()
                    )));
                }
                Ok(GbfValue::Numeric(NumericArray::new_real(
                    cls,
                    shape,
                    raw.to_vec(),
                )))
            } else {
                if raw.len() != 2 * part_bytes {
                    return Err(GbfError::Format(format!(
                        "complex numeric `{}` size mismatch: expected {} bytes, got {}",
                        field.name,
                        2 * part_bytes,
                        raw.len()
                    )));
                }
                let real_le = raw[..part_bytes].to_vec();
                let imag_le = raw[part_bytes..].to_vec();
                Ok(GbfValue::Numeric(NumericArray::new_complex(
                    cls, shape, real_le, imag_le,
                )))
            }
        }

        "logical" => {
            if raw.len() != n {
                return Err(GbfError::Format(format!(
                    "logical `{}` size mismatch: expected {} bytes, got {}",
                    field.name, n, raw.len()
                )));
            }
            Ok(GbfValue::Logical(LogicalArray {
                shape,
                data: raw.to_vec(),
            }))
        }

        "char" => {
            if raw.len() != n * 2 {
                return Err(GbfError::Format(format!(
                    "char `{}` size mismatch: expected {} bytes, got {}",
                    field.name,
                    n * 2,
                    raw.len()
                )));
            }
            let mut data = Vec::with_capacity(n);
            for i in 0..n {
                let j = i * 2;
                let u = u16::from_le_bytes([raw[j], raw[j + 1]]);
                data.push(u);
            }
            Ok(GbfValue::Char(CharArray { shape, data }))
        }

        "string" => {
            let mut data: Vec<Option<String>> = Vec::with_capacity(n);
            let mut idx = 0usize;

            for _ in 0..n {
                if idx + 1 + 4 > raw.len() {
                    return Err(GbfError::Format(format!(
                        "string `{}` truncated while parsing element header",
                        field.name
                    )));
                }
                let miss_flag = raw[idx];
                idx += 1;

                let len = u32::from_le_bytes([raw[idx], raw[idx + 1], raw[idx + 2], raw[idx + 3]]) as usize;
                idx += 4;

                if idx + len > raw.len() {
                    return Err(GbfError::Format(format!(
                        "string `{}` truncated while parsing element payload",
                        field.name
                    )));
                }

                let bytes = &raw[idx..idx + len];
                idx += len;

                if miss_flag != 0 {
                    data.push(None);
                } else {
                    let s = std::str::from_utf8(bytes)
                        .map_err(|e| GbfError::Format(format!("string `{}` invalid UTF-8: {}", field.name, e)))?;
                    data.push(Some(s.to_string()));
                }
            }

            Ok(GbfValue::String(StringArray { shape, data }))
        }

        "datetime" => {
            // Layout per MATLAB:
            // [flags u8]
            // [tz_len u32][tz_bytes]
            // [loc_len u32][loc_bytes]
            // [fmt_len u32][fmt_bytes]
            // [mask N u8]
            // [Y N int16][M N u8][D N u8][ms_day N int32]
            let mut idx = 0usize;
            if raw.len() < 1 + 4 + 4 + 4 {
                return Err(GbfError::Format(format!("datetime `{}` payload too small", field.name)));
            }
            let flags = raw[idx];
            idx += 1;

            let tz_len = u32::from_le_bytes([raw[idx], raw[idx + 1], raw[idx + 2], raw[idx + 3]]) as usize;
            idx += 4;
            if idx + tz_len > raw.len() {
                return Err(GbfError::Format(format!("datetime `{}` truncated tz", field.name)));
            }
            let tz_bytes = &raw[idx..idx + tz_len];
            idx += tz_len;
            let tz = if tz_len > 0 {
                Some(std::str::from_utf8(tz_bytes).map_err(|e| {
                    GbfError::Format(format!("datetime `{}` tz invalid UTF-8: {}", field.name, e))
                })?.to_string())
            } else {
                None
            };

            let loc_len = u32::from_le_bytes([raw[idx], raw[idx + 1], raw[idx + 2], raw[idx + 3]]) as usize;
            idx += 4;
            if idx + loc_len > raw.len() {
                return Err(GbfError::Format(format!("datetime `{}` truncated locale", field.name)));
            }
            let loc_bytes = &raw[idx..idx + loc_len];
            idx += loc_len;
            let locale = if loc_len > 0 {
                Some(std::str::from_utf8(loc_bytes).map_err(|e| {
                    GbfError::Format(format!("datetime `{}` locale invalid UTF-8: {}", field.name, e))
                })?.to_string())
            } else {
                None
            };

            let fmt_len = u32::from_le_bytes([raw[idx], raw[idx + 1], raw[idx + 2], raw[idx + 3]]) as usize;
            idx += 4;
            if idx + fmt_len > raw.len() {
                return Err(GbfError::Format(format!("datetime `{}` truncated format", field.name)));
            }
            let fmt_bytes = &raw[idx..idx + fmt_len];
            idx += fmt_len;
            let format = if fmt_len > 0 {
                Some(std::str::from_utf8(fmt_bytes).map_err(|e| {
                    GbfError::Format(format!("datetime `{}` format invalid UTF-8: {}", field.name, e))
                })?.to_string())
            } else {
                None
            };

            let _tz_present = (flags & 1) != 0;
            let _fmt_present = (flags & 2) != 0;
            let _naive = (flags & 4) != 0;
            let _loc_present = (flags & 8) != 0;

            if idx + n > raw.len() {
                return Err(GbfError::Format(format!("datetime `{}` truncated mask", field.name)));
            }
            let is_nat = raw[idx..idx + n].to_vec();
            idx += n;

            // year int16
            let need = n * 2 + n + n + n * 4;
            if idx + need > raw.len() {
                return Err(GbfError::Format(format!(
                    "datetime `{}` truncated components",
                    field.name
                )));
            }

            let mut year = Vec::with_capacity(n);
            for _ in 0..n {
                let y = i16::from_le_bytes([raw[idx], raw[idx + 1]]);
                idx += 2;
                year.push(y);
            }

            let month = raw[idx..idx + n].to_vec();
            idx += n;

            let day = raw[idx..idx + n].to_vec();
            idx += n;

            let mut ms_day = Vec::with_capacity(n);
            for _ in 0..n {
                let ms = i32::from_le_bytes([raw[idx], raw[idx + 1], raw[idx + 2], raw[idx + 3]]);
                idx += 4;
                ms_day.push(ms);
            }

            Ok(GbfValue::DateTime(DateTimeArray {
                shape,
                tz,
                locale,
                format,
                is_nat,
                year,
                month,
                day,
                ms_day,
            }))
        }

        "duration" => {
            // [mask N u8][ms N i64 LE]
            let need = n + n * 8;
            if raw.len() != need {
                return Err(GbfError::Format(format!(
                    "duration `{}` size mismatch: expected {} bytes, got {}",
                    field.name, need, raw.len()
                )));
            }
            let is_nan = raw[..n].to_vec();
            let mut ms = Vec::with_capacity(n);
            let mut idx = n;
            for _ in 0..n {
                let v = i64::from_le_bytes([
                    raw[idx],
                    raw[idx + 1],
                    raw[idx + 2],
                    raw[idx + 3],
                    raw[idx + 4],
                    raw[idx + 5],
                    raw[idx + 6],
                    raw[idx + 7],
                ]);
                idx += 8;
                ms.push(v);
            }
            Ok(GbfValue::Duration(DurationArray { shape, is_nan, ms }))
        }

        "calendarduration" => {
            // [mask N u8][months N i32][days N i32][time_ms N i64]
            let need = n + n * 4 + n * 4 + n * 8;
            if raw.len() != need {
                return Err(GbfError::Format(format!(
                    "calendarDuration `{}` size mismatch: expected {} bytes, got {}",
                    field.name, need, raw.len()
                )));
            }
            let is_missing = raw[..n].to_vec();
            let mut idx = n;

            let mut months = Vec::with_capacity(n);
            for _ in 0..n {
                let v = i32::from_le_bytes([raw[idx], raw[idx + 1], raw[idx + 2], raw[idx + 3]]);
                idx += 4;
                months.push(v);
            }

            let mut days = Vec::with_capacity(n);
            for _ in 0..n {
                let v = i32::from_le_bytes([raw[idx], raw[idx + 1], raw[idx + 2], raw[idx + 3]]);
                idx += 4;
                days.push(v);
            }

            let mut time_ms = Vec::with_capacity(n);
            for _ in 0..n {
                let v = i64::from_le_bytes([
                    raw[idx],
                    raw[idx + 1],
                    raw[idx + 2],
                    raw[idx + 3],
                    raw[idx + 4],
                    raw[idx + 5],
                    raw[idx + 6],
                    raw[idx + 7],
                ]);
                idx += 8;
                time_ms.push(v);
            }

            Ok(GbfValue::CalendarDuration(CalendarDurationArray {
                shape,
                is_missing,
                months,
                days,
                time_ms,
            }))
        }

        "categorical" => {
            // [n_cats u32]
            // repeated: [len u32][utf8 bytes]
            // [codes N u32]
            if raw.len() < 4 {
                return Err(GbfError::Format(format!("categorical `{}` payload too small", field.name)));
            }
            let mut idx = 0usize;
            let n_cats = u32::from_le_bytes([raw[idx], raw[idx + 1], raw[idx + 2], raw[idx + 3]]) as usize;
            idx += 4;

            let mut categories = Vec::with_capacity(n_cats);
            for _ in 0..n_cats {
                if idx + 4 > raw.len() {
                    return Err(GbfError::Format(format!("categorical `{}` truncated cat len", field.name)));
                }
                let len = u32::from_le_bytes([raw[idx], raw[idx + 1], raw[idx + 2], raw[idx + 3]]) as usize;
                idx += 4;
                if idx + len > raw.len() {
                    return Err(GbfError::Format(format!("categorical `{}` truncated cat bytes", field.name)));
                }
                let b = &raw[idx..idx + len];
                idx += len;
                let s = std::str::from_utf8(b).map_err(|e| {
                    GbfError::Format(format!("categorical `{}` invalid UTF-8 cat: {}", field.name, e))
                })?;
                categories.push(s.to_string());
            }

            let codes_bytes = raw.len().saturating_sub(idx);
            if codes_bytes != n * 4 {
                return Err(GbfError::Format(format!(
                    "categorical `{}` codes size mismatch: expected {} bytes, got {}",
                    field.name,
                    n * 4,
                    codes_bytes
                )));
            }

            let mut codes = Vec::with_capacity(n);
            for _ in 0..n {
                let c = u32::from_le_bytes([raw[idx], raw[idx + 1], raw[idx + 2], raw[idx + 3]]);
                idx += 4;
                codes.push(c);
            }

            Ok(GbfValue::Categorical(CategoricalArray { shape, categories, codes }))
        }

        other => Err(GbfError::Unsupported(format!(
            "unsupported kind `{}` for field `{}`",
            other, field.name
        ))),
    }
}

fn read_header_and_json(file: &mut File, opts: &ReadOptions) -> Result<(Header, u32, String)> {
    let mut r = BufReader::new(&mut *file);

    let mut magic = [0u8; 8];
    r.read_exact(&mut magic)?;
    if magic != MAGIC_BYTES {
        return Err(GbfError::Format("bad magic; not a GBF/GREDBIN file".to_string()));
    }

    let header_len = read_u32_le(&mut r)?;
    if header_len < 2 || header_len > MAX_HEADER_LEN {
        return Err(GbfError::Format("invalid header_len".to_string()));
    }

    let mut header_bytes = vec![0u8; header_len as usize];
    r.read_exact(&mut header_bytes)?;

    let header_json = String::from_utf8(header_bytes)?;
    let header: Header = serde_json::from_str(&header_json)?;

    if opts.validate {
        validate_header_crc(&header, &header_json)?;

        if header.file_size > 0 {
            let cur_pos = r.stream_position()?;
            let fs = r.get_ref().metadata()?.len();
            r.seek(SeekFrom::Start(cur_pos))?;
            if fs != header.file_size {
                return Err(GbfError::FileSizeMismatch {
                    expected: header.file_size,
                    got: fs,
                });
            }
        }
    }

    // Move underlying file cursor to after header.
    let payload_start = 8u64 + 4u64 + header_len as u64;

    if opts.validate {
        if header.payload_start > 0 && header.payload_start != payload_start {
            return Err(GbfError::Format(format!(
                "payload_start mismatch: header={}, computed={}",
                header.payload_start, payload_start
            )));
        }
    }

    r.seek(SeekFrom::Start(payload_start))?;
    Ok((header, header_len, header_json))
}

fn field_payload_start(header_len: u32, header_payload_start: u64) -> u64 {
    if header_payload_start > 0 {
        header_payload_start
    } else {
        8u64 + 4u64 + header_len as u64
    }
}

fn read_field_raw(file: &mut File, payload_start: u64, field: &FieldMeta) -> Result<Vec<u8>> {
    if field.csize > MAX_FIELD_CSIZE {
        return Err(GbfError::Unsupported(format!(
            "field `{}` csize exceeds configured limit",
            field.name
        )));
    }
    if field.usize > MAX_FIELD_USIZE {
        return Err(GbfError::Unsupported(format!(
            "field `{}` usize exceeds configured limit",
            field.name
        )));
    }

    let fs = file.metadata()?.len();
    let pos = checked_add_u64(payload_start, field.offset)?;
    let end = checked_add_u64(pos, field.csize)?;
    if end > fs {
        return Err(GbfError::FieldOutOfBounds {
            name: field.name.clone(),
            offset: field.offset,
            csize: field.csize,
            payload_len: fs.saturating_sub(payload_start),
        });
    }

    file.seek(SeekFrom::Start(pos))?;
    let csz = u64_to_usize(field.csize, "field csize")?;
    let mut buf = vec![0u8; csz];
    file.read_exact(&mut buf)?;
    Ok(buf)
}

fn decode_field_bytes(field: &FieldMeta, comp_bytes: &[u8], validate: bool) -> Result<Vec<u8>> {
    let max_out = if field.usize > 0 { field.usize } else { MAX_FIELD_USIZE };

    let mut raw = if field.compression.eq_ignore_ascii_case("zlib") {
        zlib_decompress(comp_bytes, max_out).map_err(|e| GbfError::DecompressionFailed {
            name: field.name.clone(),
            message: e.to_string(),
        })?
    } else {
        if comp_bytes.len() as u64 > MAX_FIELD_USIZE {
            return Err(GbfError::Unsupported(format!(
                "field `{}` raw payload exceeds configured limit",
                field.name
            )));
        }
        comp_bytes.to_vec()
    };

    if validate && field.usize > 0 && raw.len() as u64 != field.usize {
        return Err(GbfError::FieldSizeMismatch {
            name: field.name.clone(),
            expected: field.usize,
            got: raw.len() as u64,
        });
    }

    if validate && field.crc32 != 0 {
        let got = compute_crc32(&raw);
        if got != field.crc32 {
            return Err(GbfError::FieldCrcMismatch {
                name: field.name.clone(),
                expected: field.crc32,
                got,
            });
        }
    }

    // MATLAB always stores little-endian in payload; raw is uncompressed bytes.
    // We keep raw as-is for decode.
    Ok(std::mem::take(&mut raw))
}

fn coalesced_read(
    file: &mut File,
    payload_start: u64,
    fields: &[&FieldMeta],
) -> Result<Vec<(String, Vec<u8>)>> {
    if fields.is_empty() {
        return Ok(Vec::new());
    }

    let mut sorted: Vec<&FieldMeta> = fields.to_vec();
    sorted.sort_by_key(|f| f.offset);

    let mut out: Vec<(String, Vec<u8>)> = Vec::with_capacity(sorted.len());

    let mut group_start = sorted[0].offset;
    let mut group_end = checked_add_u64(sorted[0].offset, sorted[0].csize)?;
    let mut group_fields: Vec<&FieldMeta> = vec![sorted[0]];

    let flush_group = |file: &mut File,
                       payload_start: u64,
                       group_start: u64,
                       group_end: u64,
                       group_fields: &[&FieldMeta]|
     -> Result<Vec<(String, Vec<u8>)>> {
        let size = group_end - group_start;
        let pos = checked_add_u64(payload_start, group_start)?;
        let fs = file.metadata()?.len();
        let end = checked_add_u64(pos, size)?;
        if end > fs {
            return Err(GbfError::FieldOutOfBounds {
                name: "<coalesced>".to_string(),
                offset: group_start,
                csize: size,
                payload_len: fs.saturating_sub(payload_start),
            });
        }
        file.seek(SeekFrom::Start(pos))?;
        let sz = u64_to_usize(size, "coalesced group size")?;
        let mut buf = vec![0u8; sz];
        file.read_exact(&mut buf)?;

        let mut res = Vec::with_capacity(group_fields.len());
        for f in group_fields {
            let rel = u64_to_usize(f.offset - group_start, "field rel offset")?;
            let csz = u64_to_usize(f.csize, "field csize")?;
            let chunk = buf[rel..rel + csz].to_vec();
            res.push((f.name.clone(), chunk));
        }
        Ok(res)
    };

    for f in sorted.iter().skip(1) {
        let f_start = f.offset;
        let f_end = checked_add_u64(f.offset, f.csize)?;

        let gap = if f_start > group_end { f_start - group_end } else { 0 };
        let new_group_size = f_end.saturating_sub(group_start);

        let can_merge = gap <= READ_COALESCE_MAX_GAP_BYTES && new_group_size <= READ_COALESCE_MAX_GROUP_BYTES;

        if can_merge {
            group_end = group_end.max(f_end);
            group_fields.push(*f);
        } else {
            out.extend(flush_group(file, payload_start, group_start, group_end, &group_fields)?);
            group_start = f_start;
            group_end = f_end;
            group_fields = vec![*f];
        }
    }

    out.extend(flush_group(file, payload_start, group_start, group_end, &group_fields)?);
    Ok(out)
}

pub fn read_file<P: AsRef<Path>>(path: P, opts: ReadOptions) -> Result<GbfValue> {
    let path = normalize_path(path);
    let mut file = File::open(&path)?;
    let (header, header_len, _header_json) = read_header_and_json(&mut file, &opts)?;

    let payload_start = field_payload_start(header_len, header.payload_start);


    // Decode fields without loading the entire payload into memory.
    let mut out = BTreeMap::<String, GbfValue>::new();

    // Coalesced IO over all fields (bounded by READ_COALESCE_MAX_GROUP_BYTES).
    let all_fields: Vec<&FieldMeta> = header.fields.iter().collect();
    let comp_chunks = coalesced_read(&mut file, payload_start, &all_fields)?;

    for (name, comp_bytes) in comp_chunks {
        let field = header
            .fields
            .iter()
            .find(|f| f.name == name)
            .ok_or_else(|| GbfError::Format("internal field lookup failure".to_string()))?;

        let raw = decode_field_bytes(field, &comp_bytes, opts.validate)?;
        let val = decode_leaf(field, &raw)?;
        assign_by_path(&mut out, &field.name, val)?;
    }

    if header.root.eq_ignore_ascii_case("single") {
        if let Some(v) = out.get("data").cloned() {
            return Ok(v);
        }
        // fallback: first field
        if let Some((_, v)) = out.iter().next() {
            return Ok(v.clone());
        }
    }

    Ok(GbfValue::Struct(out))
}

pub fn read_var<P: AsRef<Path>>(path: P, var_path: &str, opts: ReadOptions) -> Result<GbfValue> {
    let path = normalize_path(path);
    let mut file = File::open(&path)?;
    let (header, header_len, _header_json) = read_header_and_json(&mut file, &opts)?;
    let payload_start = field_payload_start(header_len, header.payload_start);

    let var_path = var_path.trim();
    if var_path.is_empty() {
        return read_file(path, opts);
    }

    // Exact leaf?
    if let Some(field) = header.fields.iter().find(|f| f.name == var_path) {
        let comp_bytes = read_field_raw(&mut file, payload_start, field)?;
        let raw = decode_field_bytes(field, &comp_bytes, opts.validate)?;
        return decode_leaf(field, &raw);
    }

    // Subtree (prefix)
    let pfx = format!("{}.", var_path);
    let subtree_fields: Vec<&FieldMeta> = header
        .fields
        .iter()
        .filter(|f| f.name.starts_with(&pfx))
        .collect();

    if subtree_fields.is_empty() {
        return Err(GbfError::VarNotFound(var_path.to_string()));
    }

    // Coalesced IO, then decode each field.
    let comp_chunks = coalesced_read(&mut file, payload_start, &subtree_fields)?;
    let mut out = BTreeMap::<String, GbfValue>::new();

    for (name, comp_bytes) in comp_chunks {
        let field = subtree_fields
            .iter()
            .find(|f| f.name == name)
            .ok_or_else(|| GbfError::Format("internal field lookup failure".to_string()))?;

        let raw = decode_field_bytes(field, &comp_bytes, opts.validate)?;
        let val = decode_leaf(field, &raw)?;

        // Insert relative path (strip "var_path.")
        let rel = &name[pfx.len()..];
        assign_by_path(&mut out, rel, val)?;
    }

    Ok(GbfValue::Struct(out))
}

pub fn write_file<P: AsRef<Path>>(path: P, value: &GbfValue, opts: WriteOptions) -> Result<()> {
    let path = normalize_path(path);

    // Flatten to leaves
    let mut leaves: Vec<(String, GbfValue)> = Vec::new();
    let root_type = match value {
        GbfValue::Struct(_) => "struct",
        _ => "single",
    };

    if root_type == "struct" {
        // Flatten struct fields with empty prefix
        if let GbfValue::Struct(map) = value {
            for (k, v) in map {
                flatten_to_leaves(v, k, &mut leaves)?;
            }
        }
    } else {
        leaves.push(("data".to_string(), value.clone()));
    }

    // Encode leaves to raw bytes, optionally compress, compute per-field CRC.
    let mut chunks: Vec<Vec<u8>> = Vec::with_capacity(leaves.len());
    let mut fields: Vec<FieldMeta> = Vec::with_capacity(leaves.len());

    for (name, v) in &leaves {
        let (raw, kind, class_name, shape, complex, encoding) = encode_leaf(name, v)?;
        let usize_u64 = raw.len() as u64;
        let crc32_u = if opts.crc { compute_crc32(&raw) } else { 0u32 };

        let mut stored = raw;
        let mut comp_tag = "none".to_string();

        let try_compress = if opts.compression {
            match opts.compression_mode {
                CompressionMode::Never => false,
                CompressionMode::Always => stored.len() >= COMPRESS_THRESHOLD_BYTES,
                CompressionMode::Auto => should_try_compress(&kind, &class_name, &stored),
            }
        } else {
            false
        };

        if try_compress {
            let comp = zlib_compress(&stored, opts.compression_level)?;
            if comp.len() < stored.len() {
                stored = comp;
                comp_tag = "zlib".to_string();
            }
        }

        let csize_u64 = stored.len() as u64;

        fields.push(FieldMeta {
            name: name.clone(),
            kind,
            class_name,
            shape,
            complex,
            encoding,
            compression: comp_tag,
            offset: 0,
            csize: csize_u64,
            usize: usize_u64,
            crc32: crc32_u,
        });

        chunks.push(stored);
    }

    // Compute offsets relative to payload start
    let mut off = 0u64;
    for f in fields.iter_mut() {
        f.offset = off;
        off = off.saturating_add(f.csize);
    }
    let payload_bytes_total = off;

    // Build header (payload_start/file_size/header_crc computed iteratively)
    let mut header = Header {
        format: "GBF".to_string(),
        magic: "GREDBIN".to_string(),
        version: VERSION,
        endianness: "little".to_string(),
        order: "column-major".to_string(),
        root: root_type.to_string(),
        created_utc: now_utc_string(),
        matlab_version: format!("rust-gbin {}", env!("CARGO_PKG_VERSION")),
        fields,
        payload_start: 0,
        file_size: 0,
        header_crc32_hex: "00000000".to_string(),
    };

    // Iterate until header_len/payload_start/file_size stabilize, then CRC stabilizes.
    let mut header_json_final = String::new();
    let mut header_bytes_final: Vec<u8> = Vec::new();
    let mut header_len_final: u32 = 0;

    for _ in 0..10 {
        // 1) encode with placeholder CRC
        header.header_crc32_hex = "00000000".to_string();

        let json_for_crc = if opts.pretty_header {
            serde_json::to_string_pretty(&header)?
        } else {
            serde_json::to_string(&header)?
        };
        let mut json_for_crc_nl = json_for_crc.clone();
        json_for_crc_nl.push('\n');

        // 2) compute CRC hex over the bytes with placeholder
        let crc_hex = compute_header_crc32_hex_from_original_json(&json_for_crc_nl)?;
        header.header_crc32_hex = crc_hex;

        // 3) encode final header (with CRC)
        let json_final = if opts.pretty_header {
            serde_json::to_string_pretty(&header)?
        } else {
            serde_json::to_string(&header)?
        };
        let mut json_final_nl = json_final;
        json_final_nl.push('\n');

        let bytes = json_final_nl.as_bytes().to_vec();
        let header_len = bytes.len() as u32;
        let payload_start = 8u64 + 4u64 + header_len as u64;
        let file_size = payload_start + payload_bytes_total;

        // If stable, stop.
        let stable = header.payload_start == payload_start
            && header.file_size == file_size
            && header_len_final == header_len
            && header_json_final == json_final_nl;

        header.payload_start = payload_start;
        header.file_size = file_size;

        header_json_final = json_final_nl;
        header_bytes_final = bytes;
        header_len_final = header_len;

        if stable {
            break;
        }
    }

    // Atomic write in same dir
    let dir = path.parent().unwrap_or_else(|| Path::new("."));
    std::fs::create_dir_all(dir)?;

    let mut tmp = NamedTempFile::new_in(dir)?;
    {
        let mut w = BufWriter::new(tmp.as_file_mut());

        // magic
        w.write_all(&MAGIC_BYTES)?;
        // header_len (u32 LE)
        write_u32_le(&mut w, header_len_final)?;
        // header bytes
        w.write_all(&header_bytes_final)?;
        // payload chunks
        for ck in &chunks {
            w.write_all(ck)?;
        }
        w.flush()?;
    }
    tmp.as_file().sync_all()?;

    if path.exists() {
        std::fs::remove_file(&path)?;
    }
    tmp.persist(&path)
        .map_err(|e| GbfError::Io(e.error))?;

    Ok(())
}