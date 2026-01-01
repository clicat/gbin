

# Gbin (MATLAB) – Practical Guide

This document explains how to **write**, **read**, and **inspect** `.gbf` (GBF / GREDBIN) files using the MATLAB implementation of **Gbin**.

The MATLAB code is located in:
- `Gbin.m`
- `Gbin_Interface.m`
- `Gbin_Test.m`

The format is fully compatible with the Rust implementation and is designed for **fast random access**, **large arrays**, and **metadata-rich scientific data**.

---

## 1. What is a `.gbf` file

A `.gbf` file is composed of:

1. **Binary prelude**
   - Magic string: `GREDBIN` (8 bytes)
   - Header length: `uint32` (little-endian)

2. **Header JSON**
   - UTF-8 encoded JSON
   - Describes variables, shapes, encodings, offsets, CRCs

3. **Payload**
   - Raw or compressed binary blocks
   - Stored column-major (MATLAB-native)

This design allows:
- Reading the header without touching payload
- Random-access reads of individual variables
- Strong integrity checks (CRC32)

---

## 2. Writing a GBF file (MATLAB)

### Basic example

```matlab
data = struct();
data.A = rand(4,3);
data.B = int32([1 2 3]);
data.msg = "hello";
data.t = datetime("now","TimeZone","UTC");

opts = struct();
opts.compression = "auto";   % "none" | "auto" | "always"
opts.validate = true;

Gbin.write("example.gbf", data, opts);
```

### Supported MATLAB types

| MATLAB type              | GBF kind              |
|--------------------------|-----------------------|
| double / single          | numeric               |
| int*, uint*              | numeric               |
| logical                  | logical               |
| char                     | char (UTF-16)         |
| string                   | string (UTF-8)        |
| struct                   | struct                |
| datetime                 | datetime              |
| duration                 | duration              |
| calendarDuration         | calendarduration      |
| categorical              | categorical            |

Arrays, nested structs, and empty values are all supported.

---

## 3. Inspecting a file (header only)

### Show header (no payload read)

```matlab
Gbin.showHeader("example.gbf");
```

This prints:
- File metadata
- Full JSON header
- Payload offsets and sizes
- CRC32 checksums

This operation is **O(header size)** and safe for very large files.

---

## 4. Inspecting variable tree

```matlab
Gbin.showTree("example.gbf");
```

Example output:
```
A                [4 x 3] double
B                [1 x 3] int32
msg              [1 x 1] string
t                [1 x 1] datetime
```

Nested fields are shown using dot notation:
```
model.weights
model.bias
```

---

## 5. Reading data back

### Read full file

```matlab
data = Gbin.read("example.gbf");
```

This reconstructs the full MATLAB struct exactly as written.

### Read a single variable (random access)

```matlab
A = Gbin.readVar("example.gbf", "A");
```

Only the required payload block is read and decompressed.

---

## 6. Validation and CRC checks

You can force validation:

```matlab
opts.validate = true;
data = Gbin.read("example.gbf", opts);
```

Checks performed:
- Header CRC32
- Payload CRC32 (per field)
- Truncation and corruption detection

---

## 7. Column-major guarantee

All numeric data is written **column-major**, exactly as stored in MATLAB memory.

This means:
- Zero-copy compatibility
- Identical linear indexing
- Fast I/O for large matrices

---

## 8. Compatibility notes

- MATLAB is the **reference writer** for the GBF format
- Rust implementation is bit-compatible with MATLAB output
- Header numeric values may appear as floats in JSON (MATLAB behavior); readers must accept this

---

## 9. Recommended usage patterns

- Use `.gbf` for:
  - Scientific datasets
  - GNSS / geodesy products
  - Large matrices with metadata
- Prefer `readVar` for analysis pipelines
- Keep structs shallow when possible for readability

---

## 10. Tests

Run the included test suite:

```matlab
Gbin_Test
```

This validates:
- All supported data types
- Edge cases (empty arrays, NaN, Inf)
- Cross-language compatibility with Rust

---

## 11. File list

| File              | Purpose |
|-------------------|--------|
| `Gbin.m`          | Core encoder/decoder |
| `Gbin_Interface.m`| Public API wrapper |
| `Gbin_Test.m`     | Test suite |

---

For advanced details, see inline documentation in `Gbin.m`.

--- 