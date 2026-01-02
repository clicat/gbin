# gbin_c

A small, dependency‑light **C99** implementation of the **GBF / GREDBIN** container format used by the `gbin` tools.

This project provides:

- `libgbf` — a C library to **read** and **write** `.gbf` files
- `gbin` — a CLI inspector similar to the Rust/C++ versions (`header`, `tree`, `show`)
- `test_gbf` — CTest-based unit tests (roundtrips + corruption detection)
- `gbf_bench` — a small benchmark program

The library is designed for:

- **Fast header-only reads** (inspect fields without touching payload)
- **Random-access variable reads** (read only one variable/subtree)
- **Streaming-friendly writing** (payload + metadata written deterministically)

---

## Build

Dependencies:

- CMake ≥ 3.16
- A C compiler
- **zlib** (CMake: `find_package(ZLIB REQUIRED)`)

```bash
cmake -S . -B build -G Ninja
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Executables:

- `build/gbin` (CLI)
- `build/test_gbf`
- `build/gbf_bench`

---

## CLI quickstart

```bash
./build/gbin header path/to/file.gbf
./build/gbin tree   path/to/file.gbf --details
./build/gbin show   path/to/file.gbf model.weights
```

Typical workflow:

1. `header` to verify magic/version and list fields.
2. `tree` to explore the variable hierarchy.
3. `show` to preview one variable’s contents.

---

## Library overview

### Public headers

- **Core API**: `include/gbin/gbf.h`
- **Easy API**: `include/gbin/gbf_easy.h` (optional convenience wrappers)

### What is stored in a `.gbf` file?

A GBF file contains:

- A small fixed prefix (magic + header length)
- A JSON header describing:
  - format version / endianness / memory order
  - a flat list of **fields** (each field has name, kind, class, shape, offset, compression, sizes, CRC32)
- A payload section containing the variable bytes (possibly zlib-compressed per-field)

**CRC rules**:

- Per-field `crc32` is computed on the **uncompressed** payload bytes.
- Header CRC32 is computed over the header JSON bytes with the `header_crc32_hex` value zeroed (same approach as the C++ implementation).

---

## Core API (gbf.h)

The core API is explicit: you create a `gbf_value_t` tree, write it, then load it back.

### Key functions

- `gbf_read_header_only()` — parse header JSON and field metadata (fast)
- `gbf_read_var()` — random-access read of a variable (leaf or subtree)
- `gbf_read_file()` — read full file into a nested `gbf_value_t` tree
- `gbf_write_file()` — write a `gbf_value_t` tree to disk
- `gbf_value_free()` — free any value returned/created by the library

### Example: write a few arrays and a string

```c
#include <gbin/gbf.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    gbf_error_t err = {0};

    // root struct
    gbf_value_t* root = gbf_value_new_struct();

    // 3x3 float32 (column-major recommended)
    float* A = (float*)malloc(3 * 3 * sizeof(float));
    for (int i = 0; i < 9; ++i) A[i] = (float)i;

    size_t shapeA[2] = {3, 3};
    gbf_numeric_array_t* a = gbf_numeric_array_new(
        GBF_NUMERIC_SINGLE, /*complex=*/0,
        shapeA, 2,
        (uint8_t*)A, 9 * sizeof(float),
        NULL, 0,
        &err
    );
    if (!a) {
        fprintf(stderr, "numeric_array_new failed: %s\n", err.message);
        return 1;
    }
    gbf_value_t* vA = gbf_value_new_numeric(a);
    gbf_struct_set(root, "A", vA, &err);

    // A string scalar
    gbf_string_array_t* s = gbf_string_array_new_scalar_utf8("hello gbf", &err);
    gbf_value_t* vS = gbf_value_new_string(s);
    gbf_struct_set(root, "msg", vS, &err);

    // Write
    gbf_write_options_t wopt = gbf_write_options_default();
    wopt.compression = GBF_COMPRESS_AUTO; // choose zlib only if it helps
    if (gbf_write_file("out.gbf", root, &wopt, &err) != 0) {
        fprintf(stderr, "write failed: %s\n", err.message);
        gbf_value_free(root);
        return 1;
    }

    gbf_value_free(root);
    free(A);

    printf("Wrote out.gbf\n");
    return 0;
}
```

### Example: read header only (fast)

```c
#include <gbin/gbf.h>
#include <stdio.h>

int main(void) {
    gbf_error_t err = {0};
    gbf_header_t hdr = {0};

    if (gbf_read_header_only("out.gbf", &hdr, &err) != 0) {
        fprintf(stderr, "header read failed: %s\n", err.message);
        return 1;
    }

    printf("format=%s version=%u fields=%zu payload_start=%llu\n",
           hdr.format, hdr.version, hdr.field_count,
           (unsigned long long)hdr.payload_start);

    for (size_t i = 0; i < hdr.field_count; ++i) {
        const gbf_field_meta_t* f = &hdr.fields[i];
        printf("%s kind=%s class=%s\n", f->name, f->kind, f->class_name);
    }

    gbf_header_free(&hdr);
    return 0;
}
```

### Example: random-access read of a variable

```c
#include <gbin/gbf.h>
#include <stdio.h>

int main(void) {
    gbf_error_t err = {0};

    gbf_read_options_t ropt = gbf_read_options_default();
    ropt.validate_crc = 1;

    gbf_value_t* v = NULL;
    if (gbf_read_var("out.gbf", "A", &ropt, &v, &err) != 0) {
        fprintf(stderr, "read_var failed: %s\n", err.message);
        return 1;
    }

    if (v->type == GBF_VALUE_NUMERIC) {
        const gbf_numeric_array_t* a = &v->as.numeric_array;
        printf("A: ndims=%u shape=[%zu x %zu] bytes=%zu\n",
               a->ndims,
               a->shape[0], a->shape[1],
               a->real_bytes_len);
    }

    gbf_value_free(v);
    return 0;
}
```

---

## Easy API (gbf_easy.h)

If you just want to **save a set of arrays quickly** without manually building a nested tree, use the Easy API.

It provides a small “builder” style interface where you add entries by name + pointer + shape, then write.

### Example: write several matrices in one go

```c
#include <gbin/gbf_easy.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    gbf_error_t err = {0};

    // 2048x256 float32
    size_t shapeA[2] = {2048, 256};
    size_t numelA = shapeA[0] * shapeA[1];
    float* A = (float*)malloc(numelA * sizeof(float));
    for (size_t i = 0; i < numelA; ++i) A[i] = (float)i;

    // 1024x1024 float64
    size_t shapeB[2] = {1024, 1024};
    size_t numelB = shapeB[0] * shapeB[1];
    double* B = (double*)malloc(numelB * sizeof(double));
    for (size_t i = 0; i < numelB; ++i) B[i] = (double)i * 0.5;

    gbf_easy_list_t list;
    gbf_easy_list_init(&list);

    // NOTE: bytes are written as-is; the format expects column-major for MATLAB compatibility.
    if (gbf_easy_add_numeric_nd(&list, "big.A", GBF_NUMERIC_SINGLE,
                               /*complex=*/0, shapeA, 2,
                               A, numelA * sizeof(float),
                               NULL, 0, &err) != 0) {
        fprintf(stderr, "add big.A failed: %s\n", err.message);
        return 1;
    }

    if (gbf_easy_add_numeric_nd(&list, "big.B", GBF_NUMERIC_DOUBLE,
                               /*complex=*/0, shapeB, 2,
                               B, numelB * sizeof(double),
                               NULL, 0, &err) != 0) {
        fprintf(stderr, "add big.B failed: %s\n", err.message);
        return 1;
    }

    // add a char array (UTF‑16 code units; MATLAB compatible)
    uint16_t txt[] = { 'O', 'K', 0 }; // "OK" + terminator
    size_t shapeT[2] = {1, 2};
    if (gbf_easy_add_char_utf16_nd(&list, "txt", shapeT, 2, txt, 2, &err) != 0) {
        fprintf(stderr, "add txt failed: %s\n", err.message);
        return 1;
    }

    gbf_easy_write_options_t wopt = gbf_easy_write_options_default();
    wopt.compression = GBF_COMPRESS_AUTO;

    if (gbf_easy_write_file("easy_out.gbf", &list, &wopt, &err) != 0) {
        fprintf(stderr, "easy write failed: %s\n", err.message);
        gbf_easy_list_free(&list);
        return 1;
    }

    gbf_easy_list_free(&list);
    free(A);
    free(B);

    printf("Wrote easy_out.gbf\n");
    return 0;
}
```

### Example: load + access a numeric variable

```c
#include <gbin/gbf.h>
#include <stdio.h>

int main(void) {
    gbf_error_t err = {0};
    gbf_read_options_t ropt = gbf_read_options_default();
    ropt.validate_crc = 1;

    gbf_value_t* v = NULL;
    if (gbf_read_var("easy_out.gbf", "big.A", &ropt, &v, &err) != 0) {
        fprintf(stderr, "read_var failed: %s\n", err.message);
        return 1;
    }

    const gbf_numeric_array_t* a = &v->as.numeric_array;
    printf("big.A shape=[%zu x %zu] bytes=%zu\n",
           a->shape[0], a->shape[1], a->real_bytes_len);

    gbf_value_free(v);
    return 0;
}
```

---

## Practical notes (efficiency + correctness)

- **Prefer `gbf_read_header_only()`** when you just need names/types/shapes.
- Use **`gbf_read_var()`** to avoid loading large files into memory.
- For MATLAB compatibility, numeric arrays are expected to be **column‑major**.
- For best performance when writing large matrices:
  - allocate a contiguous buffer
  - fill it in column-major order
  - write once (avoid many tiny variables)
- If you store strings:
  - `string` values are UTF‑8
  - `char` values are UTF‑16 code units (MATLAB compatible)

---

## Where to look next

- `tests/test_gbf_roundtrip.c` — real tests + corruption checks
- `benches/gbf_bench.c` — benchmark patterns
- `tools/gbin_cli.c` — CLI that exercises header/tree/show

---

## License

Internal / project-specific. Add your license text here if you plan to publish.
