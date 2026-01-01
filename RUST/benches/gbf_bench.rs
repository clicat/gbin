use criterion::{criterion_group, criterion_main, BatchSize, BenchmarkId, Criterion};
use gbin::*;
use std::collections::BTreeMap;
use std::path::PathBuf;
use tempfile::tempdir;

fn make_big_numeric_f32(shape: (usize, usize), seed: u32) -> NumericArray {
    // Deterministic pseudo-random-ish payload without pulling in rand
    // (fast and stable for benchmarking).
    let (r, c) = shape;
    let n = r * c;

    let mut vals = Vec::<f32>::with_capacity(n);
    let mut x = seed as u64 + 0x9E3779B97F4A7C15u64;
    for _ in 0..n {
        // LCG
        x = x.wrapping_mul(6364136223846793005).wrapping_add(1);
        let bits = (x >> 32) as u32;
        let v = (bits as f32) / (u32::MAX as f32);
        vals.push(v);
    }

    // Column-major f32 bytes
    let mut bytes = Vec::<u8>::with_capacity(4 * vals.len());
    for v in vals {
        bytes.extend_from_slice(&v.to_le_bytes());
    }

    NumericArray {
        shape: vec![r, c],
        class: NumericClass::Single,
        complex: false,
        real_le: bytes,
        imag_le: None,
    }
}

fn make_big_numeric_f64(shape: (usize, usize), seed: u32) -> NumericArray {
    let (r, c) = shape;
    let n = r * c;

    let mut vals = Vec::<f64>::with_capacity(n);
    let mut x = seed as u64 + 0xD1B54A32D192ED03u64;
    for _ in 0..n {
        x = x.wrapping_mul(6364136223846793005).wrapping_add(1);
        let bits = (x >> 11) as u64;
        let v = (bits as f64) / ((1u64 << 53) as f64);
        vals.push(v);
    }

    let mut bytes = Vec::<u8>::with_capacity(8 * vals.len());
    for v in vals {
        bytes.extend_from_slice(&v.to_le_bytes());
    }

    NumericArray {
        shape: vec![r, c],
        class: NumericClass::Double,
        complex: false,
        real_le: bytes,
        imag_le: None,
    }
}

/// Build a nested value similar to the MATLAB benchmark test:
/// - big numeric matrices
/// - nested metadata
/// - nested model.weights
fn build_bench_value() -> GbfValue {
    let mut root = BTreeMap::<String, GbfValue>::new();

    // Similar payload sizes as MATLAB test:
    // A: 1024x1024 f64 ~ 8 MB
    // B: 1024x1024 f32 ~ 4 MB
    let a = make_big_numeric_f64((1024, 1024), 0);
    let b = make_big_numeric_f32((1024, 1024), 1);

    root.insert("A".into(), GbfValue::Numeric(a));
    root.insert("B".into(), GbfValue::Numeric(b));

    // meta subtree
    let mut meta = BTreeMap::<String, GbfValue>::new();
    meta.insert(
        "name".into(),
        GbfValue::String(StringArray {
            shape: vec![1, 1],
            data: vec![Some("GBF demo".into())],
        }),
    );
    meta.insert(
        "tag".into(),
        GbfValue::String(StringArray {
            shape: vec![1, 1],
            data: vec![Some("GReD".into())],
        }),
    );
    meta.insert(
        "note".into(),
        GbfValue::String(StringArray {
            shape: vec![1, 1],
            data: vec![Some("Hello from MATLAB -> Rust".into())],
        }),
    );
    root.insert("meta".into(), GbfValue::Struct(meta));

    // model subtree with weights/bias
    let mut model = BTreeMap::<String, GbfValue>::new();
    let weights = make_big_numeric_f32((2000, 64), 42); // ~0.5 MB
    let bias = make_big_numeric_f32((1, 64), 43);
    model.insert("weights".into(), GbfValue::Numeric(weights));
    model.insert("bias".into(), GbfValue::Numeric(bias));
    model.insert(
        "comment".into(),
        GbfValue::String(StringArray {
            shape: vec![1, 4],
            data: vec![
                Some("layer1".into()),
                Some("layer2".into()),
                None,
                Some("layer4".into()),
            ],
        }),
    );
    root.insert("model".into(), GbfValue::Struct(model));

    GbfValue::Struct(root)
}

fn temp_file(name: &str) -> (tempfile::TempDir, PathBuf) {
    let dir = tempdir().unwrap();
    let file = dir.path().join(name);
    (dir, file)
}

fn approx_payload_bytes(v: &GbfValue) -> usize {
    // Rough estimate: sum numeric bytes only, for MB/s throughput sanity
    fn rec(v: &GbfValue) -> usize {
        match v {
            GbfValue::Numeric(n) => n.real_le.len() + n.imag_le.as_ref().map(|x| x.len()).unwrap_or(0),
            GbfValue::Struct(m) => m.values().map(rec).sum(),
            _ => 0,
        }
    }
    rec(v)
}

fn read_path<'a>(v: &'a GbfValue, path: &str) -> Option<&'a GbfValue> {
    if path.is_empty() {
        return Some(v);
    }

    let mut cur = v;
    for part in path.split('.') {
        match cur {
            GbfValue::Struct(m) => {
                cur = m.get(part)?;
            }
            _ => return None,
        }
    }
    Some(cur)
}

fn bench_write_read(c: &mut Criterion) {
    let v = build_bench_value();

    // Leaf size for random-access throughput sanity: model.weights raw bytes (real + imag)
    let leaf_bytes: u64 = match read_path(&v, "model.weights") {
        Some(GbfValue::Numeric(n)) => (n.real_le.len() + n.imag_le.as_ref().map(|x| x.len()).unwrap_or(0)) as u64,
        _ => 0,
    };

    for &(compression, mode, label) in &[
        (true, CompressionMode::Auto, "compressed_auto"),
        (true, CompressionMode::Always, "compressed_always"),
        (false, CompressionMode::Auto, "uncompressed"),
    ] {
        // Use one group per mode so throughput metadata is not ambiguous.
        let mut group = c.benchmark_group(format!("gbf/{}", label));
        group.sample_size(20);
        group.warm_up_time(std::time::Duration::from_millis(500));

        // --- WRITE benchmark ---
        // Throughput is reported using the raw numeric payload estimate as a sanity check.
        // (File bytes vary depending on compression; for file-based throughput see read_full.)
        let approx_payload = approx_payload_bytes(&v) as u64;
        group.throughput(criterion::Throughput::Bytes(approx_payload));

        group.bench_with_input(BenchmarkId::new("write", label), &label, |b, _| {
            b.iter_batched(
                || {
                    let (_dir, file) = temp_file("bench_write.gbf");
                    (
                        file,
                        WriteOptions {
                            crc: true,
                            compression,
                            compression_mode: mode,
                            compression_level: 1,
                            ..WriteOptions::default()
                        },
                    )
                },
                |(file, wopts)| {
                    write_file(&file, &v, wopts).unwrap();
                },
                BatchSize::SmallInput,
            )
        });

        // --- READ benchmark (full read) ---
        // Pre-write once, then benchmark reads; throughput is based on *file bytes*.
        {
            let (_dir, file) = temp_file("bench_read.gbf");
            let wopts = WriteOptions {
                crc: true,
                compression,
                compression_mode: mode,
                compression_level: 1,
                ..WriteOptions::default()
            };
            write_file(&file, &v, wopts).unwrap();

            let file_bytes = std::fs::metadata(&file).unwrap().len();

            group.throughput(criterion::Throughput::Bytes(file_bytes));
            group.bench_with_input(BenchmarkId::new("read_full", label), &file, |b, file| {
                b.iter(|| {
                    let _ = read_file(file, ReadOptions { validate: true }).unwrap();
                })
            });
        }

        // --- READ benchmark (random-access leaf) ---
        // Pre-write once, then benchmark reading a single variable; throughput is based on the leaf raw bytes.
        {
            let (_dir, file) = temp_file("bench_read_var.gbf");
            let wopts = WriteOptions {
                crc: true,
                compression,
                compression_mode: mode,
                compression_level: 1,
                ..WriteOptions::default()
            };
            write_file(&file, &v, wopts).unwrap();

            // If we can't resolve leaf size, still run the bench without throughput metadata.
            if leaf_bytes > 0 {
                group.throughput(criterion::Throughput::Bytes(leaf_bytes));
            }

            group.bench_with_input(
                BenchmarkId::new("read_var_model.weights", label),
                &file,
                |b, file| {
                    b.iter(|| {
                        let _ = read_var(file, "model.weights", ReadOptions { validate: true }).unwrap();
                    })
                },
            );
        }

        group.finish();
    }
}

criterion_group!(benches, bench_write_read);
criterion_main!(benches);