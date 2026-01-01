#ifndef GBIN_GBF_EASY_H
#define GBIN_GBF_EASY_H

/*
 * gbf_easy.h
 * ----------
 * High-level convenience layer for GBF (GREDBIN) I/O in plain C.
 *
 * Rationale:
 *   In C, matrices/arrays are typically stored as a flat contiguous memory block,
 *   while shape information is carried separately (rows/cols/...) or implicit.
 *
 *   The core gbf.h API is expressive but verbose when you want to export a
 *   collection of matrices and variables. This header provides a compact and
 *   ergonomic API:
 *
 *     - One-call writing using varargs
 *     - Easy "entry" objects for numeric/logical/string/char
 *     - Support for 1D/2D/3D/ND arrays via shape arrays
 *     - Clear ownership semantics (copy vs take ownership)
 *
 * Build:
 *   This is a header-only interface; implementation lives in src/gbf_easy.c.
 *
 * Dependencies:
 *   - include/gbin/gbf.h (core GBF API)
 */

#include "gbin/gbf.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Ownership & layout
 * =============================================================================
 */

/* Memory ownership policy for buffer-based constructors. */
typedef enum gbf_easy_ownership {
    /* Library copies the provided buffers. Caller keeps ownership. */
    GBF_EASY_COPY = 0,

    /* Library takes ownership of provided buffers and will free them.
       Buffers MUST be heap allocated with malloc-compatible allocator. */
    GBF_EASY_TAKE = 1
} gbf_easy_ownership_t;

/* Numeric input layout when the caller provides typed values (not bytes). */
typedef enum gbf_easy_layout {
    /* Data given as column-major (MATLAB style). */
    GBF_EASY_COL_MAJOR = 0,

    /* Data given as row-major (C style). */
    GBF_EASY_ROW_MAJOR = 1
} gbf_easy_layout_t;

/* =============================================================================
 * Entry abstraction (varargs payload)
 * =============================================================================
 *
 * A "gbf_easy_entry_t" describes one leaf that will be placed at a dot-path
 * inside the GBF root struct.
 *
 * Example:
 *   gbf_easy_entry_t e = gbf_easy_f64_matrix("A", a_ptr, 100, 50, GBF_EASY_ROW_MAJOR);
 *
 * If you want to store it under a sub-struct:
 *   gbf_easy_entry_t e = gbf_easy_f64_matrix("model.weights", w, r, c, GBF_EASY_ROW_MAJOR);
 */

typedef struct gbf_easy_entry {
    const char* name;        /* dot-path: e.g. "A", "model.weights", "time.dt_utc" */
    gbf_value_t* value;      /* owned by the entry until written */
} gbf_easy_entry_t;

/* Sentinel macro for varargs termination. */
#define GBF_EASY_END ((gbf_easy_entry_t){ NULL, NULL })

/* =============================================================================
 * Constructors: numeric arrays
 * =============================================================================
 *
 * These constructors return an entry holding an allocated gbf_value_t*.
 * If an error occurs, entry.value == NULL and err->message is set.
 *
 * All numeric arrays stored in GBF are little-endian byte blobs in column-major
 * order (like MATLAB). These helpers will:
 *   - convert endianness if needed
 *   - convert row-major -> column-major if requested
 *
 * For complex arrays: provide both real and imag data.
 */

/* Create a numeric entry from typed input (f64). */
gbf_easy_entry_t gbf_easy_f64_nd(
    const char* name,
    const double* data,
    const size_t* shape, size_t shape_len,
    gbf_easy_layout_t layout,
    gbf_easy_ownership_t ownership, /* COPY/T TAKE: applies to a private copy only if TAKE+layout conversion needed */
    gbf_error_t* err);

/* Convenience for a 2D f64 matrix. */
gbf_easy_entry_t gbf_easy_f64_matrix(
    const char* name,
    const double* data,
    size_t rows, size_t cols,
    gbf_easy_layout_t layout,
    gbf_error_t* err);

/* Create a numeric entry from raw LE bytes (already column-major LE). */
gbf_easy_entry_t gbf_easy_numeric_bytes_nd(
    const char* name,
    gbf_numeric_class_t class_id,
    const size_t* shape, size_t shape_len,
    int complex,
    const void* real_le, size_t real_len,
    const void* imag_le, size_t imag_len,
    gbf_easy_ownership_t ownership,
    gbf_error_t* err);

/* Common integer typed constructors (row-major or col-major). */
gbf_easy_entry_t gbf_easy_i32_nd(
    const char* name,
    const int32_t* data,
    const size_t* shape, size_t shape_len,
    gbf_easy_layout_t layout,
    gbf_error_t* err);

gbf_easy_entry_t gbf_easy_u64_nd(
    const char* name,
    const uint64_t* data,
    const size_t* shape, size_t shape_len,
    gbf_easy_layout_t layout,
    gbf_error_t* err);

gbf_easy_entry_t gbf_easy_f32_nd(
    const char* name,
    const float* data,
    const size_t* shape, size_t shape_len,
    gbf_easy_layout_t layout,
    gbf_error_t* err);

/* =============================================================================
 * Constructors: logical, string, char, opaque
 * =============================================================================
 */

/* Logical array from uint8 values {0,1}. Data is treated as linear numel.
   shape_len may be 0 to represent unknown, but you should prefer providing it. */
gbf_easy_entry_t gbf_easy_logical_nd(
    const char* name,
    const uint8_t* data,
    const size_t* shape, size_t shape_len,
    gbf_easy_ownership_t ownership,
    gbf_error_t* err);

/* UTF-8 string array (each entry may be NULL => missing). */
gbf_easy_entry_t gbf_easy_string_nd(
    const char* name,
    char* const* utf8_or_null,
    size_t n,
    const size_t* shape, size_t shape_len,
    gbf_easy_ownership_t ownership,
    gbf_error_t* err);

/* UTF-16 char array (MATLAB "char"). Units are stored as UTF-16 code units. */
gbf_easy_entry_t gbf_easy_char_utf16_nd(
    const char* name,
    const uint16_t* units,
    size_t n_units,
    const size_t* shape, size_t shape_len,
    gbf_easy_ownership_t ownership,
    gbf_error_t* err);

/* Store bytes as "opaque" leaf (future-proof / debugging). */
gbf_easy_entry_t gbf_easy_opaque_bytes_nd(
    const char* name,
    const char* kind,
    const char* class_name,
    const size_t* shape, size_t shape_len,
    int complex,
    const char* encoding,
    const void* bytes, size_t bytes_len,
    gbf_easy_ownership_t ownership,
    gbf_error_t* err);

/* Empty scalar struct leaf (MATLAB empty scalar struct encoding). */
gbf_easy_entry_t gbf_easy_empty_struct_leaf(const char* name);

/* =============================================================================
 * Writer: simplest API (varargs)
 * =============================================================================
 *
 * gbf_easy_write_file() builds a root struct and inserts all entries by path.
 *
 * Usage:
 *
 *   gbf_error_t err = {0};
 *   int ok = gbf_easy_write_file(
 *       "out.gbf",
 *       (gbf_write_options_t){ .compression = GBF_COMP_AUTO, .include_crc32 = 1, .zlib_level = -1 },
 *       &err,
 *       gbf_easy_f64_matrix("A", A, rows, cols, GBF_EASY_ROW_MAJOR, &err),
 *       gbf_easy_u64_nd("ids", ids, (size_t[]){N}, 1, GBF_EASY_ROW_MAJOR, &err),
 *       GBF_EASY_END);
 *
 * If ok==0, err.message contains the reason.
 *
 * Notes:
 *   - All entry.value objects are consumed/freed by this function, even on error.
 *   - You should not reuse an entry after passing it to gbf_easy_write_file().
 */
int gbf_easy_write_file(
    const char* path,
    gbf_write_options_t opt,
    gbf_error_t* err,
    ... /* gbf_easy_entry_t entries, terminated by GBF_EASY_END */
);

/* =============================================================================
 * Reader: simplest API
 * =============================================================================
 *
 * Reads full file into a gbf_value_t root struct. Caller owns it and must free.
 */
int gbf_easy_read_file(
    const char* path,
    gbf_read_options_t opt,
    gbf_value_t** out_root,      /* required */
    gbf_header_t** out_header,   /* optional; may be NULL */
    gbf_error_t* err);

/* Read only one variable (leaf or subtree), by dot-path. */
int gbf_easy_read_var(
    const char* path,
    const char* var, /* NULL or "" => root */
    gbf_read_options_t opt,
    gbf_value_t** out_value,
    gbf_error_t* err);

/* =============================================================================
 * Helpers: navigation and typed extraction
 * =============================================================================
 *
 * These helpers are designed to make consuming GBF content easy:
 *   - gbf_easy_get(root, "a.b.c") returns gbf_value_t* pointer (borrowed).
 *   - typed helpers validate type and return pointer/shape.
 */

/* Return a borrowed pointer to a nested value inside a struct tree.
   Returns NULL if not found or if path hits non-struct. */
const gbf_value_t* gbf_easy_get(const gbf_value_t* root, const char* dot_path);

/* Typed “views” (borrowed pointers). Returns 1 on success, 0 on mismatch. */
int gbf_easy_as_numeric(
    const gbf_value_t* v,
    const gbf_numeric_array_t** out_num);

int gbf_easy_as_logical(
    const gbf_value_t* v,
    const gbf_logical_array_t** out_logical);

int gbf_easy_as_string(
    const gbf_value_t* v,
    const gbf_string_array_t** out_str);

int gbf_easy_as_char(
    const gbf_value_t* v,
    const gbf_char_array_t** out_chr);

/* Numeric helpers: return element size in bytes (1..8) for numeric class. */
size_t gbf_easy_numeric_elem_size(gbf_numeric_class_t c);

/* Convert gbf_numeric_class_t to a stable lowercase string ("double", "int32", ...). */
const char* gbf_easy_numeric_class_name(gbf_numeric_class_t c);

/* =============================================================================
 * Error utilities
 * =============================================================================
 */

/* Set err->message like printf. (Convenience wrapper that writes err->message directly). */
void gbf_easy_set_err(gbf_error_t* err, const char* fmt, ...);

#ifdef __cplusplus
} /* extern "C" */
#endif


#endif /* GBIN_GBF_EASY_H */
