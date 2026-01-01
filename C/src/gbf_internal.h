#ifndef GBIN_GBF_INTERNAL_H
#define GBIN_GBF_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

/* forward declarations */
struct gbf_error;

#ifdef __cplusplus
extern "C" {
#endif

/* Small string builder */
typedef struct gbf_strbuf {
    char* data;
    size_t len;
    size_t cap;
} gbf_strbuf_t;

void gbf_sb_init(gbf_strbuf_t* sb);
void gbf_sb_free(gbf_strbuf_t* sb);
int  gbf_sb_reserve(gbf_strbuf_t* sb, size_t extra);
int  gbf_sb_append_byte(gbf_strbuf_t* sb, char c);
int  gbf_sb_append_mem(gbf_strbuf_t* sb, const void* p, size_t n);
int  gbf_sb_append_str(gbf_strbuf_t* sb, const char* s);
int  gbf_sb_append_fmt(gbf_strbuf_t* sb, const char* fmt, ...);

/* Memory helpers (abort on OOM) */
void* gbf_xmalloc(size_t n);
void* gbf_xcalloc(size_t n, size_t sz);
void* gbf_xrealloc(void* p, size_t n);
char* gbf_strdup(const char* s);

/* Error helper used across .c files */
void gbf_set_err(struct gbf_error* err, const char* fmt, ...);

/* Endian helpers */
uint32_t gbf_le_u32(const uint8_t b[4]);
uint64_t gbf_le_u64(const uint8_t b[8]);
int32_t  gbf_le_i32(const uint8_t b[4]);
int64_t  gbf_le_i64(const uint8_t b[8]);
void gbf_store_le_u32(uint8_t b[4], uint32_t v);
void gbf_store_le_u64(uint8_t b[8], uint64_t v);
void gbf_store_le_i32(uint8_t b[4], int32_t v);
void gbf_store_le_i64(uint8_t b[8], int64_t v);

/* Safe multiplication */
int gbf_checked_mul_size(size_t a, size_t b, size_t* out);

/* ===== Minimal JSON (internal) ===== */

typedef enum gbf_json_type {
    GBF_JSON_NULL = 0,
    GBF_JSON_BOOL,
    GBF_JSON_NUMBER,
    GBF_JSON_STRING,
    GBF_JSON_ARRAY,
    GBF_JSON_OBJECT
} gbf_json_type_t;

typedef struct gbf_json_number {
    double value;
    char* raw;
    int is_int;
} gbf_json_number_t;

typedef struct gbf_json gbf_json_t;

gbf_json_t* gbf_json_parse(const char* s, size_t n, char** out_err_string);
void gbf_json_free(gbf_json_t* j);
gbf_json_type_t gbf_json_type(const gbf_json_t* j);

const gbf_json_t* gbf_json_obj_get(const gbf_json_t* obj, const char* key);
size_t gbf_json_array_size(const gbf_json_t* arr);
const gbf_json_t* gbf_json_array_get(const gbf_json_t* arr, size_t idx);

const char* gbf_json_as_cstr(const gbf_json_t* j);
double gbf_json_as_f64(const gbf_json_t* j, double default_);
uint64_t gbf_json_as_u64(const gbf_json_t* j, uint64_t default_);
uint32_t gbf_json_as_u32(const gbf_json_t* j, uint32_t default_);
int gbf_json_as_bool(const gbf_json_t* j, int default_);

/* Header CRC helpers */
int gbf_zero_out_header_crc32_field(char* json, size_t len);
uint32_t gbf_extract_header_crc32_hex_u32(const char* json, size_t len);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* GBIN_GBF_INTERNAL_H */
