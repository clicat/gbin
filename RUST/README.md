# gbin (Rust) — Practical Guide

This crate provides a **production-grade reader/writer** for **GBF/GREDBIN (`.gbf`)** files produced by the Matlab implementation (`srcM/Gbin.m`).

A `.gbf` file contains:

- A small binary prelude (magic + header length)
- A JSON header describing variables (name, kind, class, shape, offsets, CRC, compression)
- The payload bytes (optionally zlib-compressed per-field)

The Rust library lets you:

- **Read the full file** (validated or fast)
- **Read a single variable** (random access)
- **Write `.gbf` files** from Rust values
- Validate CRCs for header and fields

---

## Add the crate to your project

In your `Cargo.toml`:

```toml
[dependencies]
gbin = { path = "../Gbin" } # adjust to your path / workspace
anyhow = "1"
```

If you published the crate or use a git dependency, replace the `path = ...` accordingly.

---

## Core types you will use

The API revolves around:

- `GbfValue`: the in-memory representation of values (struct, numeric array, string, logical, datetime, etc.)
- `NumericArray`: raw little-endian bytes + shape + numeric class
- `ReadOptions`: controls validation (CRC checks)
- `WriteOptions`: controls compression strategy

You generally create a `GbfValue::Struct(BTreeMap<_, _>)` as the root object and write it to disk.

---

## 1) Writing a `.gbf` file

### Example: write a simple struct with numeric + string + logical

```rust
use anyhow::Result;
use gbin::{
    write_file, WriteOptions,
    GbfValue, NumericArray, NumericClass,
    LogicalArray, StringArray,
};
use std::collections::BTreeMap;
use std::path::Path;

fn main() -> Result<()> {
    // Create a numeric matrix (2x3) in column-major order
    // Matlab / GBF uses column-major.
    // Matrix:
    //   [ 1 3 5
    //     2 4 6 ]
    let shape = vec![2usize, 3usize];
    let data: Vec<f64> = vec![1.0, 2.0, 3.0, 4.0, 5.0, 6.0]; // column-major

    let mut real_le = Vec::<u8>::with_capacity(data.len() * 8);
    for v in data {
        real_le.extend_from_slice(&v.to_le_bytes());
    }

    let a = GbfValue::Numeric(NumericArray {
        class: NumericClass::Double,
        shape,
        complex: false,
        real_le,
        imag_le: None,
    });

    let note = GbfValue::String(StringArray {
        shape: vec![1, 1],
        data: vec![Some("hello from rust".to_string())],
    });

    let ok = GbfValue::Logical(LogicalArray {
        shape: vec![1, 3],
        data: vec![true, false, true],
    });

    let mut root = BTreeMap::new();
    root.insert("A".to_string(), a);
    root.insert("meta.note".to_string(), note);
    root.insert("ok".to_string(), ok);

    let value = GbfValue::Struct(root);

    // Write options: auto compression (compress only if it helps)
    let wopts = WriteOptions {
        compression: gbin::CompressionMode::Auto,
    };

    let out = Path::new("example_out.gbf");
    write_file(out, &value, wopts)?;

    println!("Wrote {}", out.display());
    Ok(())
}
```

### Notes on numeric data

- **Numeric payload is raw little-endian bytes.**
- **Order is column-major**, like Matlab.
- `shape` is `[rows, cols, ...]`.

To create integer arrays, encode the scalar type to LE bytes the same way:

```rust
let v: i32 = -10;
out.extend_from_slice(&v.to_le_bytes());
```

---

## 2) Reading a `.gbf` file

### Read the full file

```rust
use anyhow::Result;
use gbin::{read_file, ReadOptions};

fn main() -> Result<()> {
    let path = "example_out.gbf";

    // validate=true checks header CRC and field CRCs
    let v = read_file(path, ReadOptions { validate: true })?;

    println!("Top-level: {v:?}");
    Ok(())
}
```

### Read one variable (random access)

```rust
use anyhow::Result;
use gbin::{read_var, ReadOptions};

fn main() -> Result<()> {
    let path = "example_out.gbf";

    let v = read_var(path, "A", ReadOptions { validate: true })?;
    println!("A = {v:?}");

    Ok(())
}
```

`read_var` is the recommended call when you want to inspect a single value without loading everything.

---

## 3) Inspecting the header without reading payload

This is the fastest way to list variables and their metadata.

```rust
use anyhow::Result;
use gbin::{read_header_only, ReadOptions};

fn main() -> Result<()> {
    let (hdr, header_len, raw_json) = read_header_only("example_out.gbf", ReadOptions { validate: true })?;

    println!("header_len={header_len}");
    println!("fields={}", hdr.fields.len());

    // raw_json contains the JSON exactly as stored in the file
    // (useful for debugging Matlab compatibility)
    println!("raw header json: {raw_json}");

    Ok(())
}
```

---

## 4) Understanding `GbfValue`

`GbfValue` is an enum representing all supported GBF data types.

Common variants:

- `GbfValue::Struct(BTreeMap<String, GbfValue>)`
- `GbfValue::Numeric(NumericArray)`
- `GbfValue::String(StringArray)`
- `GbfValue::Logical(LogicalArray)`
- `GbfValue::Char(CharArray)`
- `GbfValue::DateTime(DateTimeArray)`
- `GbfValue::Duration(DurationArray)`
- `GbfValue::CalendarDuration(CalendarDurationArray)`
- `GbfValue::Categorical(CategoricalArray)`

Tip: use `read_header_only()` to learn how Matlab encodes a type (class/kind/encoding), then mirror it in Rust.

---

## 5) Compression and validation

### Compression

`write_file()` supports per-field zlib compression.

- `CompressionMode::Never` — always store raw
- `CompressionMode::Always` — always compress
- `CompressionMode::Auto` — compress only if the compressed bytes are smaller

Matlab uses the same zlib framing.

### Validation

When reading, `ReadOptions { validate: true }` will:

- Check header CRC
- Check field CRCs for every field you read
- Reject truncated or corrupt payloads

Use `validate: false` for maximum speed when you trust the file.

---

## 6) CLI usage (optional)

This repo also contains a CLI binary (if enabled in your workspace):

```bash
# Show header
cargo run --bin gbin -- header path/to/file.gbf

# Show tree
cargo run --bin gbin -- tree path/to/file.gbf

# Interactive show/inspect
cargo run --bin gbin -- show path/to/file.gbf
```

---

## Troubleshooting

### "header CRC mismatch"

- You are likely computing CRC over the wrong bytes.
- The reference is Matlab `Gbin.m` behavior.
- Use `read_header_only(... validate=true)` to pinpoint what part differs.

### Numeric arrays look transposed

You are likely feeding row-major data.

Remember: **GBF stores numeric arrays column-major**.

### Strings and chars look wrong

- `string` is UTF-8
- `char` is stored as UTF-16 code units

Use `read_var()` and inspect the `encoding` and `class/kind` in the header.

---

## Next steps

- Add higher-level constructors (e.g. `NumericArray::from_f64_mat`) for convenience
- Add a small `gbfcat` tool to export variables to CSV/JSON
- Extend interoperability tests using Matlab reference files

If you want, we can add ready-to-use helper functions for writing common Rust numeric matrices (ndarray / nalgebra) directly to GBF.
