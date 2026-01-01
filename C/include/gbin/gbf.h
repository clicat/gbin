#ifndef GBIN_GBF_H
#define GBIN_GBF_H

/*
 * GBF (GREDBIN) reader/writer in C.
 *
 * File layout:
 *   [8 bytes magic][u32 header_len LE][header JSON bytes][payload bytes...]
 *
 * The header JSON contains a flat list of leaf fields (dot-separated paths),
 * plus metadata (compression, offsets, sizes, CRC32, encoding).
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===== Error handling ===== */

typedef struct gbf_error {
    char* message; /* heap-allocated; free with gbf_free_error() */
} gbf_error_t;

void gbf_free_error(gbf_error_t* err);

/* ===== Options ===== */

typedef struct gbf_read_options {
    int validate; /* if non-zero: validate header + payload CRCs */
} gbf_read_options_t;

typedef enum gbf_compression_mode {
    GBF_COMP_AUTO   = 0,
    GBF_COMP_NEVER  = 1,
    GBF_COMP_ALWAYS = 2
} gbf_compression_mode_t;

typedef struct gbf_write_options {
    gbf_compression_mode_t compression;
    int include_crc32;
    int zlib_level; /* -1 = zlib default */
} gbf_write_options_t;

/* ===== Header model ===== */

typedef struct gbf_field_meta {
    char* name;        /* dot-separated path */
    char* kind;        /* "numeric", "logical", "string", "char", ... */
    char* class_name;  /* numeric class ("double"/"single"/...), or same as kind */
    uint64_t* shape;   /* dims */
    size_t shape_len;
    int complex;
    char* encoding;    /* optional (e.g. utf-16-codeunits) */
    char* compression; /* "none" or "zlib" */

    uint64_t offset; /* relative to payload_start */
    uint64_t csize;
    uint64_t usize;
    uint32_t crc32;  /* CRC32 of UNCOMPRESSED payload bytes */
} gbf_field_meta_t;

typedef struct gbf_header {
    char* format;
    char* magic;
    int version;
    char* endianness;
    char* order;
    char* root;

    uint64_t payload_start;
    uint64_t file_size;

    char* header_crc32_hex; /* 8 hex chars */

    gbf_field_meta_t* fields;
    size_t fields_len;
} gbf_header_t;

void gbf_header_free(gbf_header_t* h);

/* ===== Value model ===== */

typedef enum gbf_value_kind {
    GBF_VALUE_STRUCT = 0,
    GBF_VALUE_NUMERIC,
    GBF_VALUE_LOGICAL,
    GBF_VALUE_STRING,
    GBF_VALUE_CHAR,
    GBF_VALUE_DATETIME,
    GBF_VALUE_DURATION,
    GBF_VALUE_CALENDARDURATION,
    GBF_VALUE_CATEGORICAL,
    GBF_VALUE_OPAQUE
} gbf_value_kind_t;

typedef enum gbf_numeric_class {
    GBF_NUM_DOUBLE = 0,
    GBF_NUM_SINGLE,
    GBF_NUM_INT8,
    GBF_NUM_UINT8,
    GBF_NUM_INT16,
    GBF_NUM_UINT16,
    GBF_NUM_INT32,
    GBF_NUM_UINT32,
    GBF_NUM_INT64,
    GBF_NUM_UINT64
} gbf_numeric_class_t;

typedef struct gbf_numeric_array {
    gbf_numeric_class_t class_id;
    size_t* shape;
    size_t shape_len;
    int complex;

    uint8_t* real_le; size_t real_len;
    uint8_t* imag_le; size_t imag_len;
} gbf_numeric_array_t;

typedef struct gbf_logical_array {
    size_t* shape;
    size_t shape_len;
    uint8_t* data; size_t len;
} gbf_logical_array_t;

typedef struct gbf_string_array {
    size_t* shape;
    size_t shape_len;
    char** data;         /* UTF-8 strings; NULL means missing */
    size_t len;          /* number of elements */
} gbf_string_array_t;

typedef struct gbf_char_array {
    size_t* shape;
    size_t shape_len;
    uint16_t* data; size_t len; /* UTF-16 code units */
} gbf_char_array_t;

typedef struct gbf_datetime_array {
    size_t* shape;
    size_t shape_len;
    char* timezone;
    char* locale;
    char* format;
    uint8_t* nat_mask; size_t mask_len;
    int64_t* ms; size_t n;
} gbf_datetime_array_t;

typedef struct gbf_duration_array {
    size_t* shape;
    size_t shape_len;
    uint8_t* nan_mask; size_t mask_len;
    int64_t* ms; size_t n;
} gbf_duration_array_t;

typedef struct gbf_calendarduration_array {
    size_t* shape;
    size_t shape_len;
    uint8_t* mask; size_t mask_len;
    int32_t* months;
    int32_t* days;
    int64_t* time_ms;
    size_t n;
} gbf_calendarduration_array_t;

typedef struct gbf_categorical_array {
    size_t* shape;
    size_t shape_len;
    char** categories; size_t categories_len;
    uint32_t* codes; size_t codes_len;
} gbf_categorical_array_t;

struct gbf_value;

typedef struct gbf_struct_entry {
    char* key;
    struct gbf_value* value;
} gbf_struct_entry_t;

typedef struct gbf_struct {
    gbf_struct_entry_t* entries;
    size_t len;
    size_t cap;
} gbf_struct_t;

typedef struct gbf_opaque_value {
    char* kind;
    char* class_name;
    size_t* shape;
    size_t shape_len;
    int complex;
    char* encoding;
    uint8_t* bytes;
    size_t bytes_len;
} gbf_opaque_value_t;

typedef struct gbf_value {
    gbf_value_kind_t kind;
    union {
        gbf_struct_t s;
        gbf_numeric_array_t num;
        gbf_logical_array_t logical;
        gbf_string_array_t str;
        gbf_char_array_t chr;
        gbf_datetime_array_t dt;
        gbf_duration_array_t dur;
        gbf_calendarduration_array_t caldur;
        gbf_categorical_array_t cat;
        gbf_opaque_value_t opaque;
    } as;
} gbf_value_t;

void gbf_value_free(gbf_value_t* v);

/* ===== Constructors (helpers) ===== */

gbf_value_t* gbf_value_new_struct(void);
int gbf_struct_set(gbf_value_t* s, const char* key, gbf_value_t* child, gbf_error_t* err);

/* Low-level numeric: provide bytes already in little-endian. */
gbf_value_t* gbf_value_new_numeric_from_bytes(
    gbf_numeric_class_t class_id,
    const size_t* shape, size_t shape_len,
    int complex,
    const void* real_le, size_t real_len,
    const void* imag_le, size_t imag_len,
    gbf_error_t* err);

/* Convenience: build a numeric double matrix from row-major input, convert to column-major LE. */
gbf_value_t* gbf_value_new_f64_matrix_rowmajor(
    const double* data, size_t rows, size_t cols,
    gbf_error_t* err);

gbf_value_t* gbf_value_new_logical_from_u8(
    const uint8_t* data, size_t len,
    const size_t* shape, size_t shape_len,
    gbf_error_t* err);

gbf_value_t* gbf_value_new_string_array(
    char** utf8_or_null, size_t n,
    const size_t* shape, size_t shape_len,
    gbf_error_t* err);

gbf_value_t* gbf_value_new_char_from_utf16(
    const uint16_t* units, size_t n,
    const size_t* shape, size_t shape_len,
    gbf_error_t* err);

gbf_value_t* gbf_value_new_empty_struct_leaf(void);

/* ===== I/O ===== */

int gbf_read_header_only(
    const char* path,
    gbf_read_options_t opt,
    gbf_header_t** out_header,
    uint32_t* out_header_len,
    char** out_raw_json,
    gbf_error_t* err);

int gbf_read_file(
    const char* path,
    gbf_read_options_t opt,
    gbf_value_t** out_value,
    gbf_header_t** out_header_optional,
    gbf_error_t* err);

int gbf_read_var(
    const char* path,
    const char* var, /* "" or NULL => root */
    gbf_read_options_t opt,
    gbf_value_t** out_value,
    gbf_error_t* err);

int gbf_write_file(
    const char* path,
    const gbf_value_t* root, /* must be struct */
    gbf_write_options_t opt,
    gbf_error_t* err);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* GBIN_GBF_H */
