

# Gbin / GBF (GREDBIN)

A cross-language implementation of the **GBF** file format (magic: `GREDBIN`), used to store MATLAB-like structured data with a JSON header and a binary payload.

This repository is a **monorepo** containing reference implementations in multiple languages:

- **C** library + tools
- **C++** library + tools
- **Rust** library + cli
- **MATLAB** reference reader/writer

The implementations aim to be mutually compatible and to support large numeric arrays (multi‑GB payloads) with strong validation options.

---

## Repository layout

- `C/` — C implementation (library, tests, benchmarks, CLI)
- `CPP/` — C++ implementation (library, tests, benchmarks, CLI)
- `RUST/` — Rust crate (`gbin`) and CLI
- `MATLAB/` — MATLAB class and utilities

---

## Quick start

### Rust

Build the library and CLI:

```bash
cd RUST
cargo build
```

Run tests:

```bash
cd RUST
cargo test
```

Run the CLI:

```bash
cd RUST
cargo run --bin gbin -- --help
```

### C

```bash
cd C
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

### C++

```bash
cd CPP
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

### MATLAB

Add `MATLAB/` to your MATLAB path and use `Gbin`:

```matlab
addpath('MATLAB')
info = Gbin.read('file.gbf');
```

---

## Format overview

A GBF file consists of:

1. **Magic**: 8 bytes (`GREDBIN\0`)
2. **Header length**: `u32` little-endian
3. **Header JSON**: `header_len` bytes, UTF‑8 JSON
4. **Payload**: concatenated binary field payloads

The header describes each field (name/path, kind, class, shape, offsets, compressed size, uncompressed size, checksums).

---

## Documentation

Each language implementation has its own README:

- C: `C/README.md`
- C++: `CPP/README.md`
- Rust: `RUST/README.md`
- MATLAB: `MATLAB/README.md`

---

## Compatibility and validation

- On-disk numeric representation is **little-endian**.
- Readers support optional validation (CRC checks, bounds checks, header consistency).
- Writers can optionally compress payloads (e.g., zlib) depending on the implementation.

---

## Contributing

Issues and PRs are welcome. Please:

- keep changes compatible across languages when they affect the on-disk format
- add/update tests for round-trip and edge cases
- document any format changes explicitly

---

## License

Dual-licensed under **MIT*.