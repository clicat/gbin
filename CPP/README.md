
# gbin_cpp (C++)

A small, production-oriented C++17 library + CLI for reading and writing **GBF/GREDBIN** (`.gbf`) files.

This implementation is designed to interoperate with the existing Rust and MATLAB tooling in this repository:

- File framing: `[8B magic][u32 header_len][header JSON][payload]`
- Per-field entries in header JSON with offsets/sizes and (optionally) CRC32
- Optional zlib compression for each field payload

## Build

Requirements:

- CMake >= 3.16
- A C++17 compiler
- zlib (system package)

```bash
mkdir -p build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build
```

Artifacts:

- `build/libgbin.a`
- `build/gbin` (CLI)
- `build/test_gbf`
- `build/gbf_bench`

## CLI

### Print header summary

```bash
./build/gbin header path/to/file.gbf --validate
./build/gbin header path/to/file.gbf --raw
```

### Print a variable tree

```bash
./build/gbin tree path/to/file.gbf
./build/gbin tree path/to/file.gbf --prefix meta --details
```

### Show a variable (or root)

```bash
./build/gbin show path/to/file.gbf A
./build/gbin show path/to/file.gbf            # defaults to <root>
```

## Library usage

### Read a full file

```cpp
#include "gbin/gbf.hpp"

gbin::GbfValue root = gbin::read_file("data.gbf", gbin::ReadOptions{.validate=true});
```

### Read a single variable / subtree

```cpp
gbin::GbfValue a = gbin::read_var("data.gbf", "A", gbin::ReadOptions{.validate=true});
// If "meta" is a subtree prefix, you get a struct containing only meta.*
gbin::GbfValue meta = gbin::read_var("data.gbf", "meta", gbin::ReadOptions{.validate=true});
```

### Write a file

```cpp
gbin::GbfValue::Struct root;

// A 2x3 double (column-major)
gbin::NumericArray A;
A.class_id = gbin::NumericClass::Double;
A.shape = {2,3};
A.real_le = /* bytes */;
root["A"] = gbin::GbfValue::make_numeric(A);

// A scalar char array "hello"
gbin::CharArray txt;
txt.shape = {1,5};
txt.utf16 = { 'h','e','l','l','o' };
root["txt"] = gbin::GbfValue::make_char(txt);

gbin::WriteOptions wo;
wo.compression = gbin::CompressionMode::Auto;
wo.include_crc32 = true;
wo.zlib_level = 6;

gbin::write_file("out.gbf", gbin::GbfValue::make_struct(root), wo);
```

## Notes on the in-file encodings

The library follows the encodings observed in MATLAB-generated GBF files:

- **numeric/logical**: raw element bytes (little-endian, column-major)
- **complex numeric**: real bytes followed by imaginary bytes (same type)
- **string**: for each element: `[u8 missing][u32 len][utf-8 bytes]`
- **char**: UTF-16 code units (little-endian)
- **duration**: `[nan_mask bytes (n)][i64 ms values (n)]`
- **calendarDuration**: `[mask bytes (n)][i32 months (n)][i32 days (n)][i64 time_ms (n)]`
- **categorical**: `[u32 n_cats][cats...][u32 codes (n)]` where cats are `[u32 len][utf-8 bytes]`

For unknown kinds/classes, decoding falls back to `OpaqueValue` which keeps the uncompressed payload bytes.

## License

This folder is intended as a drop-in component for the repository. If you need a specific license header policy, add it at the repository level.
