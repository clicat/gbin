fn build_edge_case_value() -> GbfValue {
    let mut root = BTreeMap::<String, GbfValue>::new();

    // Empty numeric arrays
    root.insert(
        "empty_f64_0x0".into(),
        GbfValue::Numeric(NumericArray::from_f64_column_major(vec![0, 0], vec![])),
    );
    root.insert(
        "empty_f64_0x3".into(),
        GbfValue::Numeric(NumericArray::from_f64_column_major(vec![0, 3], vec![])),
    );
    root.insert(
        "empty_f64_3x0".into(),
        GbfValue::Numeric(NumericArray::from_f64_column_major(vec![3, 0], vec![])),
    );

    // Complex numeric array 2x2 (column-major) with NaN/Inf
    // real: [1, NaN; Inf, -1]  imag: [0, 2; -3, 4]
    let real: Vec<f64> = vec![1.0, f64::NAN, f64::INFINITY, -1.0];
    let imag: Vec<f64> = vec![0.0, 2.0, -3.0, 4.0];
    root.insert(
        "cplx".into(),
        GbfValue::Numeric(NumericArray {
            shape: vec![2, 2],
            class: NumericClass::Double,
            complex: true,
            real_le: {
                let mut out = Vec::with_capacity(8 * real.len());
                for &v in &real {
                    out.extend_from_slice(&v.to_le_bytes());
                }
                out
            },
            imag_le: Some({
                let mut out = Vec::with_capacity(8 * imag.len());
                for &v in &imag {
                    out.extend_from_slice(&v.to_le_bytes());
                }
                out
            }),
        })
    );

    // Integer arrays
    root.insert(
        "i32".into(),
        GbfValue::Numeric(NumericArray {
            shape: vec![2, 2],
            class: NumericClass::Int32,
            complex: false,
            real_le: {
                let data: Vec<i32> = vec![i32::MIN, 0, 1, i32::MAX];
                let mut out = Vec::with_capacity(4 * data.len());
                for v in data {
                    out.extend_from_slice(&v.to_le_bytes());
                }
                out
            },
            imag_le: None,
        })
    );
    root.insert(
        "u64".into(),
        GbfValue::Numeric(NumericArray {
            shape: vec![1, 3],
            class: NumericClass::Uint64,
            complex: false,
            real_le: {
                let data: Vec<u64> = vec![0u64, 1, u64::MAX];
                let mut out = Vec::with_capacity(8 * data.len());
                for v in data {
                    out.extend_from_slice(&v.to_le_bytes());
                }
                out
            },
            imag_le: None,
        })
    );

    // Logical empty + non-empty
    root.insert(
        "empty_logical".into(),
        GbfValue::Logical(LogicalArray { shape: vec![0, 0], data: vec![] }),
    );
    root.insert(
        "logical".into(),
        GbfValue::Logical(LogicalArray { shape: vec![1, 4], data: vec![1, 0, 1, 1] }),
    );

    // Strings: unicode, empty, missing, newline
    root.insert(
        "str".into(),
        GbfValue::String(StringArray {
            shape: vec![2, 3],
            data: vec![
                Some("".into()),
                Some("ascii".into()),
                Some("caffè".into()),
                Some("€".into()),
                Some("line1\nline2".into()),
                None,
            ],
        }),
    );

    // Char: include newline
    root.insert("char".into(), GbfValue::Char(CharArray::from_str_row("A\nB")));

    // datetime without tz/locale/format and with NaT mask
    root.insert(
        "dt_no_meta".into(),
        GbfValue::DateTime(DateTimeArray {
            shape: vec![1, 3],
            tz: None,
            locale: None,
            format: None,
            is_nat: vec![0, 1, 0],
            year: vec![2020, 0, 2021],
            month: vec![1, 0, 12],
            day: vec![2, 0, 31],
            ms_day: vec![0, 0, 86_399_999],
        }),
    );

    // duration with negative + NaN
    root.insert(
        "duration".into(),
        GbfValue::Duration(DurationArray {
            shape: vec![1, 4],
            is_nan: vec![0, 1, 0, 0],
            ms: vec![100, 0, -1500, 3_600_000],
        }),
    );

    // calendarDuration with negative and missing
    root.insert(
        "cal".into(),
        GbfValue::CalendarDuration(CalendarDurationArray {
            shape: vec![2, 2],
            is_missing: vec![0, 1, 0, 0],
            months: vec![1, 0, -2, 0],
            days: vec![10, 0, 20, -5],
            time_ms: vec![0, 0, 60_000, -1_000],
        }),
    );

    // categorical: include undefined codes (0)
    root.insert(
        "cat".into(),
        GbfValue::Categorical(CategoricalArray {
            shape: vec![1, 5],
            categories: vec!["a".into(), "b".into(), "c".into()],
            codes: vec![0, 1, 3, 2, 0],
        }),
    );

    // Deep nesting for random-access
    let mut a = BTreeMap::<String, GbfValue>::new();
    let mut b = BTreeMap::<String, GbfValue>::new();
    let mut c = BTreeMap::<String, GbfValue>::new();
    let mut d = BTreeMap::<String, GbfValue>::new();
    d.insert(
        "leaf".into(),
        GbfValue::Numeric(NumericArray::from_f64_column_major(vec![1, 3], vec![1.0, 2.0, 3.0])),
    );
    c.insert("d".into(), GbfValue::Struct(d));
    b.insert("c".into(), GbfValue::Struct(c));
    a.insert("b".into(), GbfValue::Struct(b));
    root.insert("a".into(), GbfValue::Struct(a));

    GbfValue::Struct(root)
}

fn write_then_read(v: &GbfValue, wopts: WriteOptions, ropts: ReadOptions) -> GbfValue {
    let dir = tempdir().unwrap();
    let file = dir.path().join("rt.gbf");
    write_file(&file, v, wopts).unwrap();
    read_file(&file, ropts).unwrap()
}
use gbin::*;
use std::collections::BTreeMap;
use tempfile::tempdir;

fn build_test_value() -> GbfValue {
    // Build a nested struct similar to MATLAB tests.
    let mut root = BTreeMap::<String, GbfValue>::new();

    // A: 3x3 double, column-major bytes
    // MATLAB A = [1 2 3; 4 5 6; 7 8 9]
    // A(:) = [1 4 7 2 5 8 3 6 9]
    let a_vals = vec![1.0, 4.0, 7.0, 2.0, 5.0, 8.0, 3.0, 6.0, 9.0];
    let a = NumericArray::from_f64_column_major(vec![3, 3], a_vals);
    root.insert("A".to_string(), GbfValue::Numeric(a));

    // logical 2x2
    root.insert(
        "L".to_string(),
        GbfValue::Logical(LogicalArray {
            shape: vec![2, 2],
            data: vec![1, 0, 0, 1], // column-major 2x2
        }),
    );

    // char
    root.insert("name".to_string(), GbfValue::Char(CharArray::from_str_row("GBF")));

    // string array 2x2 (column-major flatten)
    // shape [2,2] => N=4
    // order: (1,1),(2,1),(1,2),(2,2)
    root.insert(
        "tags".to_string(),
        GbfValue::String(StringArray {
            shape: vec![2, 2],
            data: vec![
                Some("alpha".into()),
                None,
                Some("beta".into()),
                Some("".into()),
            ],
        }),
    );

    // datetime (N=2)
    root.insert(
        "t".to_string(),
        GbfValue::DateTime(DateTimeArray {
            shape: vec![1, 2],
            tz: Some("UTC".into()),
            locale: Some("en_US".into()),
            format: Some("yyyy-MM-dd HH:mm:ss.SSS Z".into()),
            is_nat: vec![0, 1],
            year: vec![2025, 0],
            month: vec![12, 0],
            day: vec![29, 0],
            // 10:15:34.000 => (10*3600+15*60+34)*1000 = 36934000
            ms_day: vec![36_934_000, 0],
        }),
    );

    // duration (N=3)
    root.insert(
        "du".to_string(),
        GbfValue::Duration(DurationArray {
            shape: vec![1, 3],
            is_nan: vec![0, 1, 0],
            ms: vec![100, 0, 4500],
        }),
    );

    // calendarDuration (N=2)
    root.insert(
        "cd".to_string(),
        GbfValue::CalendarDuration(CalendarDurationArray {
            shape: vec![1, 2],
            is_missing: vec![0, 1],
            months: vec![1, 0],
            days: vec![10, 0],
            time_ms: vec![3600_000, 0],
        }),
    );

    // categorical (shape 1x3)
    root.insert(
        "cat".to_string(),
        GbfValue::Categorical(CategoricalArray {
            shape: vec![1, 3],
            categories: vec!["a".into(), "b".into(), "c".into()],
            codes: vec![1, 0, 3],
        }),
    );

    // empty struct leaf
    root.insert("empty_struct".to_string(), GbfValue::EmptyStruct);

    // nested struct meta/
    let mut meta = BTreeMap::<String, GbfValue>::new();
    meta.insert("note".into(), GbfValue::String(StringArray {
        shape: vec![1, 1],
        data: vec![Some("hello".into())],
    }));
    root.insert("meta".into(), GbfValue::Struct(meta));

    GbfValue::Struct(root)
}

#[test]
fn roundtrip_all_types_with_crc() {
    let dir = tempdir().unwrap();
    let file = dir.path().join("test.gbf");

    let v = build_test_value();

    let mut wopts = WriteOptions::default();
    wopts.crc = true;
    wopts.compression = true;
    wopts.compression_mode = CompressionMode::Auto;
    wopts.compression_level = 1;

    write_file(&file, &v, wopts).unwrap();

    let ropts = ReadOptions { validate: true };
    let v2 = read_file(&file, ropts).unwrap();

    assert_eq!(v, v2);
}

#[test]
fn random_access_read_var() {
    let dir = tempdir().unwrap();
    let file = dir.path().join("test_var.gbf");

    let v = build_test_value();

    let mut wopts = WriteOptions::default();
    wopts.crc = true;
    wopts.compression = true;
    write_file(&file, &v, wopts).unwrap();

    let ropts = ReadOptions { validate: true };

    // read nested var meta.note
    let note = read_var(&file, "meta.note", ropts).unwrap();
    let expected = if let Some(x) = v.get_path("meta.note") { x.clone() } else { panic!("missing expected"); };
    assert_eq!(note, expected);

    // read subtree meta
    let meta = read_var(&file, "meta", ReadOptions { validate: true }).unwrap();
    match meta {
        GbfValue::Struct(m) => {
            assert!(m.contains_key("note"));
        }
        _ => panic!("expected subtree struct"),
    }
}

#[test]
fn header_crc_mismatch_is_detected() {
    let dir = tempdir().unwrap();
    let file = dir.path().join("crc_bad.gbf");

    let v = build_test_value();

    let mut wopts = WriteOptions::default();
    wopts.crc = true;
    wopts.compression = false;
    write_file(&file, &v, wopts).unwrap();

    // Corrupt one byte in the header JSON (but keep valid JSON)
    let mut bytes = std::fs::read(&file).unwrap();

    // Magic(8) + header_len(4)
    let header_len = u32::from_le_bytes([bytes[8], bytes[9], bytes[10], bytes[11]]) as usize;
    let header_start = 12;
    let header_end = header_start + header_len;

    // Find a 'G' in "GBF" and flip it to 'H' (still valid JSON)
    let mut changed = false;
    for i in header_start..header_end {
        if bytes[i] == b'G' {
            bytes[i] = b'H';
            changed = true;
            break;
        }
    }
    assert!(changed);

    let bad = dir.path().join("crc_bad_corrupt.gbf");
    std::fs::write(&bad, bytes).unwrap();

    let err = read_file(&bad, ReadOptions { validate: true }).unwrap_err();
    match err {
        GbfError::HeaderCrcMismatch { .. } => {}
        other => panic!("expected HeaderCrcMismatch, got {other:?}"),
    }
}

#[test]
fn field_crc_mismatch_is_detected() {
    let dir = tempdir().unwrap();
    let file = dir.path().join("field_crc.gbf");

    let v = build_test_value();

    let mut wopts = WriteOptions::default();
    wopts.crc = true;
    wopts.compression = false; // easier: flip raw payload byte without breaking zlib
    write_file(&file, &v, wopts).unwrap();

    let mut bytes = std::fs::read(&file).unwrap();

    let header_len = u32::from_le_bytes([bytes[8], bytes[9], bytes[10], bytes[11]]) as usize;
    let payload_start = 12 + header_len;

    // Flip one byte in payload (not in header)
    bytes[payload_start + 10] ^= 0xFF;

    let bad = dir.path().join("field_crc_corrupt.gbf");
    std::fs::write(&bad, bytes).unwrap();

    let err = read_file(&bad, ReadOptions { validate: true }).unwrap_err();
    match err {
        GbfError::FieldCrcMismatch { .. } => {}
        other => panic!("expected FieldCrcMismatch, got {other:?}"),
    }
}

#[test]
fn roundtrip_edge_cases_matrix_of_types() {
    let v = build_edge_case_value();

    // Try all compression modes for broader coverage
    for (compression, mode) in [
        (false, CompressionMode::Auto),
        (true, CompressionMode::Auto),
        (true, CompressionMode::Always),
        (true, CompressionMode::Never),
    ] {
        let mut wopts = WriteOptions::default();
        wopts.crc = true;
        wopts.compression = compression;
        wopts.compression_mode = mode;
        wopts.compression_level = 1;

        let v2 = write_then_read(&v, wopts, ReadOptions { validate: true });
        assert_eq!(v, v2);
    }
}

#[test]
fn magic_mismatch_is_detected() {
    let dir = tempdir().unwrap();
    let file = dir.path().join("magic.gbf");

    let v = build_test_value();
    write_file(&file, &v, WriteOptions::default()).unwrap();

    let mut bytes = std::fs::read(&file).unwrap();
    // Magic is first 8 bytes
    bytes[0] = b'X';
    let bad = dir.path().join("magic_bad.gbf");
    std::fs::write(&bad, bytes).unwrap();

    let err = read_file(&bad, ReadOptions { validate: true }).unwrap_err();
    // Accept either BadMagic or any error mentioning "magic"
    assert!(
        err.to_string().to_lowercase().contains("magic")
    );
}

#[test]
fn truncation_is_detected() {
    let dir = tempdir().unwrap();
    let file = dir.path().join("trunc.gbf");

    let v = build_test_value();
    let mut wopts = WriteOptions::default();
    wopts.crc = true;
    wopts.compression = true;
    write_file(&file, &v, wopts).unwrap();

    let bytes = std::fs::read(&file).unwrap();

    // Truncate in the middle of payload
    let truncated = &bytes[..bytes.len() / 2];
    let bad = dir.path().join("trunc_bad.gbf");
    std::fs::write(&bad, truncated).unwrap();

    let err = read_file(&bad, ReadOptions { validate: true }).unwrap_err();
    // Accept any of the expected IO/format errors
    assert!(
        format!("{err:?}").to_lowercase().contains("eof")
            || format!("{err:?}").to_lowercase().contains("io")
            || format!("{err:?}").to_lowercase().contains("trunc")
    );
}

#[test]
fn header_len_lie_is_detected() {
    let dir = tempdir().unwrap();
    let file = dir.path().join("hlen.gbf");

    let v = build_test_value();
    let mut wopts = WriteOptions::default();
    wopts.crc = true;
    wopts.compression = false;
    write_file(&file, &v, wopts).unwrap();

    let mut bytes = std::fs::read(&file).unwrap();

    // Overwrite header_len with a larger number than actual (will force read beyond header into payload)
    let header_len = u32::from_le_bytes([bytes[8], bytes[9], bytes[10], bytes[11]]);
    let header_len_bad = header_len.saturating_add(16);
    let bad_le = header_len_bad.to_le_bytes();
    bytes[8..12].copy_from_slice(&bad_le);

    let bad = dir.path().join("hlen_bad.gbf");
    std::fs::write(&bad, bytes).unwrap();

    let err = read_file(&bad, ReadOptions { validate: true }).unwrap_err();

    // The exact error variant/message can vary depending on how the reader fails
    // (UTF-8 decode, JSON parse, CRC, or generic invalid header length).
    // We only require that an error is raised and that it looks like a header/parse problem.
    let s = format!("{err:?}").to_lowercase();
    assert!(
        s.contains("header")
            || s.contains("json")
            || s.contains("crc")
            || s.contains("eof")
            || s.contains("utf")
            || s.contains("parse")
            || s.contains("invalid")
            || s.contains("trunc"),
        "unexpected error for header_len lie: {err:?}"
    );
}

#[test]
fn random_access_deep_leaf_and_missing_var() {
    let dir = tempdir().unwrap();
    let file = dir.path().join("deep.gbf");

    let v = build_edge_case_value();
    let mut wopts = WriteOptions::default();
    wopts.crc = true;
    wopts.compression = true;
    wopts.compression_mode = CompressionMode::Auto;
    wopts.compression_level = 1;
    write_file(&file, &v, wopts).unwrap();

    // Deep leaf
    let leaf = read_var(&file, "a.b.c.d.leaf", ReadOptions { validate: true }).unwrap();
    let expected = v.get_path("a.b.c.d.leaf").unwrap().clone();
    assert_eq!(leaf, expected);

    // Missing var
    let err = read_var(&file, "a.b.c.d.nope", ReadOptions { validate: true }).unwrap_err();
    match err {
        GbfError::VarNotFound { .. } => {}
        other => panic!("expected VarNotFound, got {other:?}"),
    }
}

#[test]
fn corrupt_compressed_payload_is_detected() {
    let dir = tempdir().unwrap();
    let file = dir.path().join("corrupt_z.gbf");

    let v = build_edge_case_value();

    let mut wopts = WriteOptions::default();
    wopts.crc = true;
    wopts.compression = true;
    wopts.compression_mode = CompressionMode::Always;
    wopts.compression_level = 1;
    write_file(&file, &v, wopts).unwrap();

    let mut bytes = std::fs::read(&file).unwrap();

    // Locate payload start from stored header_len
    let header_len = u32::from_le_bytes([bytes[8], bytes[9], bytes[10], bytes[11]]) as usize;
    let payload_start = 12 + header_len;

    // Flip a byte inside the payload (likely inside a zlib stream)
    bytes[payload_start + 32] ^= 0xFF;

    let bad = dir.path().join("corrupt_z_bad.gbf");
    std::fs::write(&bad, bytes).unwrap();

    let err = read_file(&bad, ReadOptions { validate: true }).unwrap_err();
    // Accept either a field CRC mismatch or any error mentioning "zlib", "decompress", or "crc"
    let s = format!("{err:?}").to_lowercase();
    assert!(
        s.contains("crc")
            || s.contains("zlib")
            || s.contains("decompress")
    );
}