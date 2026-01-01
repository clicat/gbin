
#include "gbin/gbf.h"
#include "gbf_internal.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>

/* ===== helpers ===== */

static const char* k_magic = "GREDBIN";
static const size_t k_magic_len = 7; /* file stores 8 bytes; last is usually 0 */

/* product of shape dims */
static int shape_numel(const size_t* shape, size_t shape_len, size_t* out) {
    size_t n = 1;
    for (size_t i = 0; i < shape_len; i++) {
        size_t tmp;
        if (!gbf_checked_mul_size(n, shape[i], &tmp)) return 0;
        n = tmp;
    }
    *out = (shape_len == 0) ? 0 : n;
    return 1;
}

static size_t bytes_per_elem(gbf_numeric_class_t c) {
    switch (c) {
        case GBF_NUM_DOUBLE: return 8;
        case GBF_NUM_SINGLE: return 4;
        case GBF_NUM_INT8:   return 1;
        case GBF_NUM_UINT8:  return 1;
        case GBF_NUM_INT16:  return 2;
        case GBF_NUM_UINT16: return 2;
        case GBF_NUM_INT32:  return 4;
        case GBF_NUM_UINT32: return 4;
        case GBF_NUM_INT64:  return 8;
        case GBF_NUM_UINT64: return 8;
        default: return 1;
    }
}

static const char* numeric_class_name(gbf_numeric_class_t c) {
    switch (c) {
        case GBF_NUM_DOUBLE: return "double";
        case GBF_NUM_SINGLE: return "single";
        case GBF_NUM_INT8:   return "int8";
        case GBF_NUM_UINT8:  return "uint8";
        case GBF_NUM_INT16:  return "int16";
        case GBF_NUM_UINT16: return "uint16";
        case GBF_NUM_INT32:  return "int32";
        case GBF_NUM_UINT32: return "uint32";
        case GBF_NUM_INT64:  return "int64";
        case GBF_NUM_UINT64: return "uint64";
        default: return "uint8";
    }
}

static int parse_numeric_class(const char* s, gbf_numeric_class_t* out) {
    if (!s || !out) return 0;
    if (strcmp(s, "double") == 0) { *out = GBF_NUM_DOUBLE; return 1; }
    if (strcmp(s, "single") == 0) { *out = GBF_NUM_SINGLE; return 1; }
    if (strcmp(s, "int8") == 0)   { *out = GBF_NUM_INT8; return 1; }
    if (strcmp(s, "uint8") == 0)  { *out = GBF_NUM_UINT8; return 1; }
    if (strcmp(s, "int16") == 0)  { *out = GBF_NUM_INT16; return 1; }
    if (strcmp(s, "uint16") == 0) { *out = GBF_NUM_UINT16; return 1; }
    if (strcmp(s, "int32") == 0)  { *out = GBF_NUM_INT32; return 1; }
    if (strcmp(s, "uint32") == 0) { *out = GBF_NUM_UINT32; return 1; }
    if (strcmp(s, "int64") == 0)  { *out = GBF_NUM_INT64; return 1; }
    if (strcmp(s, "uint64") == 0) { *out = GBF_NUM_UINT64; return 1; }
    return 0;
}

static int copy_shape_u64_to_size(const uint64_t* in, size_t in_len, size_t** out_shape, size_t* out_len, gbf_error_t* err) {
    *out_shape = NULL;
    *out_len = 0;
    if (in_len == 0) return 1;
    size_t* shp = (size_t*)gbf_xcalloc(in_len, sizeof(size_t));
    for (size_t i = 0; i < in_len; i++) {
        if (in[i] > (uint64_t)SIZE_MAX) {
            free(shp);
            gbf_set_err(err, "shape dimension too large for this platform");
            return 0;
        }
        shp[i] = (size_t)in[i];
    }
    *out_shape = shp;
    *out_len = in_len;
    return 1;
}

static uint64_t* copy_shape_size_to_u64(const size_t* in, size_t in_len) {
    if (in_len == 0) return NULL;
    uint64_t* shp = (uint64_t*)gbf_xcalloc(in_len, sizeof(uint64_t));
    for (size_t i = 0; i < in_len; i++) shp[i] = (uint64_t)in[i];
    return shp;
}

/* read exactly n bytes from FILE */
static int fread_exact(FILE* f, void* buf, size_t n) {
    uint8_t* p = (uint8_t*)buf;
    size_t got = 0;
    while (got < n) {
        size_t r = fread(p + got, 1, n - got, f);
        if (r == 0) return 0;
        got += r;
    }
    return 1;
}

/* get file size (seek/tell) */

static int file_size_u64(FILE* f, uint64_t* out) {
    if (!f || !out) return 0;
    long cur = ftell(f);
    if (cur < 0) return 0;
    if (fseek(f, 0, SEEK_END) != 0) return 0;
    long end = ftell(f);
    if (end < 0) return 0;
    if (fseek(f, cur, SEEK_SET) != 0) return 0;
    *out = (uint64_t)end;
    return 1;
}

/* ===== value constructors ===== */

gbf_value_t* gbf_value_new_struct(void) {
    gbf_value_t* v = (gbf_value_t*)gbf_xcalloc(1, sizeof(gbf_value_t));
    v->kind = GBF_VALUE_STRUCT;
    v->as.s.entries = NULL;
    v->as.s.len = 0;
    v->as.s.cap = 0;
    return v;
}

gbf_value_t* gbf_value_new_empty_struct_leaf(void) {
    return gbf_value_new_struct();
}

static gbf_struct_entry_t* struct_find(gbf_struct_t* s, const char* key) {
    if (!s || !key) return NULL;
    for (size_t i = 0; i < s->len; i++) {
        if (strcmp(s->entries[i].key, key) == 0) return &s->entries[i];
    }
    return NULL;
}

int gbf_struct_set(gbf_value_t* s, const char* key, gbf_value_t* child, gbf_error_t* err) {
    if (!s || s->kind != GBF_VALUE_STRUCT) {
        gbf_set_err(err, "gbf_struct_set: target is not a struct");
        return 0;
    }
    if (!key || !*key) {
        gbf_set_err(err, "gbf_struct_set: empty key");
        return 0;
    }
    if (!child) {
        gbf_set_err(err, "gbf_struct_set: child is NULL");
        return 0;
    }

    gbf_struct_entry_t* e = struct_find(&s->as.s, key);
    if (e) {
        /* replace */
        free(e->key);
        gbf_value_free(e->value);
        e->key = gbf_strdup(key);
        e->value = child;
        return 1;
    }

    if (s->as.s.len == s->as.s.cap) {
        size_t nc = s->as.s.cap ? s->as.s.cap * 2 : 8;
        s->as.s.entries = (gbf_struct_entry_t*)gbf_xrealloc(s->as.s.entries, nc * sizeof(gbf_struct_entry_t));
        s->as.s.cap = nc;
    }

    s->as.s.entries[s->as.s.len].key = gbf_strdup(key);
    s->as.s.entries[s->as.s.len].value = child;
    s->as.s.len++;
    return 1;
}

gbf_value_t* gbf_value_new_numeric_from_bytes(
    gbf_numeric_class_t class_id,
    const size_t* shape, size_t shape_len,
    int complex,
    const void* real_le, size_t real_len,
    const void* imag_le, size_t imag_len,
    gbf_error_t* err)
{
    if (shape_len == 0) {
        gbf_set_err(err, "numeric: shape must have at least one dimension");
        return NULL;
    }
    size_t numel = 0;
    if (!shape_numel(shape, shape_len, &numel)) {
        gbf_set_err(err, "numeric: shape too large");
        return NULL;
    }
    size_t bpe = bytes_per_elem(class_id);
    size_t expected = numel * bpe;

    if (real_len != expected) {
        gbf_set_err(err, "numeric: real_len mismatch (got=%zu expected=%zu)", real_len, expected);
        return NULL;
    }
    if (complex) {
        if (imag_len != expected || !imag_le) {
            gbf_set_err(err, "numeric: imag_len mismatch for complex array");
            return NULL;
        }
    } else {
        if (imag_len != 0) {
            gbf_set_err(err, "numeric: imag_len must be 0 for non-complex array");
            return NULL;
        }
    }

    gbf_value_t* v = (gbf_value_t*)gbf_xcalloc(1, sizeof(gbf_value_t));
    v->kind = GBF_VALUE_NUMERIC;
    v->as.num.class_id = class_id;
    v->as.num.shape = (size_t*)gbf_xcalloc(shape_len, sizeof(size_t));
    v->as.num.shape_len = shape_len;
    for (size_t i = 0; i < shape_len; i++) v->as.num.shape[i] = shape[i];
    v->as.num.complex = complex ? 1 : 0;

    v->as.num.real_le = (uint8_t*)gbf_xmalloc(real_len);
    v->as.num.real_len = real_len;
    memcpy(v->as.num.real_le, real_le, real_len);

    if (complex) {
        v->as.num.imag_le = (uint8_t*)gbf_xmalloc(imag_len);
        v->as.num.imag_len = imag_len;
        memcpy(v->as.num.imag_le, imag_le, imag_len);
    }

    return v;
}

gbf_value_t* gbf_value_new_f64_matrix_rowmajor(const double* data, size_t rows, size_t cols, gbf_error_t* err) {
    if (!data) {
        gbf_set_err(err, "f64 matrix: data is NULL");
        return NULL;
    }
    size_t shape[2] = { rows, cols };
    size_t numel = rows * cols;

    /* column-major output */
    size_t real_len = numel * 8;
    uint8_t* real = (uint8_t*)gbf_xmalloc(real_len);

    for (size_t r = 0; r < rows; r++) {
        for (size_t c = 0; c < cols; c++) {
            double x = data[r * cols + c];
            uint64_t bits = 0;
            memcpy(&bits, &x, 8);

            size_t idx = r + c * rows;
            gbf_store_le_u64(real + idx * 8, bits);
        }
    }

    gbf_value_t* v = gbf_value_new_numeric_from_bytes(
        GBF_NUM_DOUBLE, shape, 2, 0,
        real, real_len,
        NULL, 0,
        err);

    free(real);
    return v;
}

gbf_value_t* gbf_value_new_logical_from_u8(
    const uint8_t* data, size_t len,
    const size_t* shape, size_t shape_len,
    gbf_error_t* err)
{
    if (!shape || shape_len == 0) {
        gbf_set_err(err, "logical: shape required");
        return NULL;
    }
    size_t numel = 0;
    if (!shape_numel(shape, shape_len, &numel)) {
        gbf_set_err(err, "logical: shape too large");
        return NULL;
    }
    if (len != numel) {
        gbf_set_err(err, "logical: len mismatch (got=%zu expected=%zu)", len, numel);
        return NULL;
    }

    gbf_value_t* v = (gbf_value_t*)gbf_xcalloc(1, sizeof(gbf_value_t));
    v->kind = GBF_VALUE_LOGICAL;
    v->as.logical.shape = (size_t*)gbf_xcalloc(shape_len, sizeof(size_t));
    v->as.logical.shape_len = shape_len;
    for (size_t i = 0; i < shape_len; i++) v->as.logical.shape[i] = shape[i];

    v->as.logical.data = (uint8_t*)gbf_xmalloc(len);
    v->as.logical.len = len;
    memcpy(v->as.logical.data, data, len);
    return v;
}

gbf_value_t* gbf_value_new_string_array(
    char** utf8_or_null, size_t n,
    const size_t* shape, size_t shape_len,
    gbf_error_t* err)
{
    if (!shape || shape_len == 0) {
        gbf_set_err(err, "string: shape required");
        return NULL;
    }
    size_t numel = 0;
    if (!shape_numel(shape, shape_len, &numel)) {
        gbf_set_err(err, "string: shape too large");
        return NULL;
    }
    if (n != numel) {
        gbf_set_err(err, "string: element count mismatch (got=%zu expected=%zu)", n, numel);
        return NULL;
    }

    gbf_value_t* v = (gbf_value_t*)gbf_xcalloc(1, sizeof(gbf_value_t));
    v->kind = GBF_VALUE_STRING;
    v->as.str.shape = (size_t*)gbf_xcalloc(shape_len, sizeof(size_t));
    v->as.str.shape_len = shape_len;
    for (size_t i = 0; i < shape_len; i++) v->as.str.shape[i] = shape[i];

    v->as.str.data = (char**)gbf_xcalloc(n, sizeof(char*));
    v->as.str.len = n;
    for (size_t i = 0; i < n; i++) {
        v->as.str.data[i] = utf8_or_null[i] ? gbf_strdup(utf8_or_null[i]) : NULL;
    }
    return v;
}

gbf_value_t* gbf_value_new_char_from_utf16(
    const uint16_t* units, size_t n,
    const size_t* shape, size_t shape_len,
    gbf_error_t* err)
{
    if (!shape || shape_len == 0) {
        gbf_set_err(err, "char: shape required");
        return NULL;
    }
    size_t numel = 0;
    if (!shape_numel(shape, shape_len, &numel)) {
        gbf_set_err(err, "char: shape too large");
        return NULL;
    }
    if (n != numel) {
        gbf_set_err(err, "char: len mismatch (got=%zu expected=%zu)", n, numel);
        return NULL;
    }

    gbf_value_t* v = (gbf_value_t*)gbf_xcalloc(1, sizeof(gbf_value_t));
    v->kind = GBF_VALUE_CHAR;
    v->as.chr.shape = (size_t*)gbf_xcalloc(shape_len, sizeof(size_t));
    v->as.chr.shape_len = shape_len;
    for (size_t i = 0; i < shape_len; i++) v->as.chr.shape[i] = shape[i];

    v->as.chr.data = (uint16_t*)gbf_xcalloc(n, sizeof(uint16_t));
    v->as.chr.len = n;
    memcpy(v->as.chr.data, units, n * sizeof(uint16_t));
    return v;
}

/* ===== free ===== */

void gbf_value_free(gbf_value_t* v) {
    if (!v) return;

    switch (v->kind) {
        case GBF_VALUE_STRUCT: {
            for (size_t i = 0; i < v->as.s.len; i++) {
                free(v->as.s.entries[i].key);
                gbf_value_free(v->as.s.entries[i].value);
            }
            free(v->as.s.entries);
        } break;

        case GBF_VALUE_NUMERIC:
            free(v->as.num.shape);
            free(v->as.num.real_le);
            free(v->as.num.imag_le);
            break;

        case GBF_VALUE_LOGICAL:
            free(v->as.logical.shape);
            free(v->as.logical.data);
            break;

        case GBF_VALUE_STRING:
            free(v->as.str.shape);
            if (v->as.str.data) {
                for (size_t i = 0; i < v->as.str.len; i++) free(v->as.str.data[i]);
            }
            free(v->as.str.data);
            break;

        case GBF_VALUE_CHAR:
            free(v->as.chr.shape);
            free(v->as.chr.data);
            break;

        case GBF_VALUE_DATETIME:
            free(v->as.dt.shape);
            free(v->as.dt.timezone);
            free(v->as.dt.locale);
            free(v->as.dt.format);
            free(v->as.dt.nat_mask);
            free(v->as.dt.ms);
            break;

        case GBF_VALUE_DURATION:
            free(v->as.dur.shape);
            free(v->as.dur.nan_mask);
            free(v->as.dur.ms);
            break;

        case GBF_VALUE_CALENDARDURATION:
            free(v->as.caldur.shape);
            free(v->as.caldur.mask);
            free(v->as.caldur.months);
            free(v->as.caldur.days);
            free(v->as.caldur.time_ms);
            break;

        case GBF_VALUE_CATEGORICAL:
            free(v->as.cat.shape);
            if (v->as.cat.categories) {
                for (size_t i = 0; i < v->as.cat.categories_len; i++) free(v->as.cat.categories[i]);
            }
            free(v->as.cat.categories);
            free(v->as.cat.codes);
            break;

        case GBF_VALUE_OPAQUE:
            free(v->as.opaque.kind);
            free(v->as.opaque.class_name);
            free(v->as.opaque.shape);
            free(v->as.opaque.encoding);
            free(v->as.opaque.bytes);
            break;

        default:
            break;
    }

    free(v);
}

/* ===== header freeing ===== */

void gbf_header_free(gbf_header_t* h) {
    if (!h) return;
    free(h->format);
    free(h->magic);
    free(h->endianness);
    free(h->order);
    free(h->root);
    free(h->header_crc32_hex);

    if (h->fields) {
        for (size_t i = 0; i < h->fields_len; i++) {
            gbf_field_meta_t* f = &h->fields[i];
            free(f->name);
            free(f->kind);
            free(f->class_name);
            free(f->shape);
            free(f->encoding);
            free(f->compression);
        }
    }
    free(h->fields);
    free(h);
}

/* ===== JSON header parsing ===== */

static int parse_header_from_json(const char* json, size_t json_len, gbf_header_t** out_hdr, gbf_error_t* err) {
    *out_hdr = NULL;

    char* jerr = NULL;
    gbf_json_t* root = gbf_json_parse(json, json_len, &jerr);
    if (!root) {
        gbf_set_err(err, "header JSON parse failed: %s", jerr ? jerr : "unknown");
        free(jerr);
        return 0;
    }
    free(jerr);

    const gbf_json_t* j_fields = gbf_json_obj_get(root, "fields");
    if (!j_fields || gbf_json_type(j_fields) != GBF_JSON_ARRAY) {
        gbf_json_free(root);
        gbf_set_err(err, "header JSON: missing 'fields' array");
        return 0;
    }

    gbf_header_t* hdr = (gbf_header_t*)gbf_xcalloc(1, sizeof(gbf_header_t));

    hdr->format = gbf_strdup(gbf_json_as_cstr(gbf_json_obj_get(root, "format")) ? gbf_json_as_cstr(gbf_json_obj_get(root, "format")) : "GBF");
    hdr->magic = gbf_strdup(gbf_json_as_cstr(gbf_json_obj_get(root, "magic")) ? gbf_json_as_cstr(gbf_json_obj_get(root, "magic")) : k_magic);
    hdr->version = (int)gbf_json_as_u64(gbf_json_obj_get(root, "version"), 1);
    hdr->endianness = gbf_strdup(gbf_json_as_cstr(gbf_json_obj_get(root, "endianness")) ? gbf_json_as_cstr(gbf_json_obj_get(root, "endianness")) : "little");
    hdr->order = gbf_strdup(gbf_json_as_cstr(gbf_json_obj_get(root, "order")) ? gbf_json_as_cstr(gbf_json_obj_get(root, "order")) : "column-major");
    hdr->root = gbf_strdup(gbf_json_as_cstr(gbf_json_obj_get(root, "root")) ? gbf_json_as_cstr(gbf_json_obj_get(root, "root")) : "struct");

    hdr->payload_start = gbf_json_as_u64(gbf_json_obj_get(root, "payload_start"), 0);
    hdr->file_size = gbf_json_as_u64(gbf_json_obj_get(root, "file_size"), 0);

    const char* hcrc = gbf_json_as_cstr(gbf_json_obj_get(root, "header_crc32_hex"));
    hdr->header_crc32_hex = hcrc ? gbf_strdup(hcrc) : gbf_strdup("00000000");

    size_t nf = gbf_json_array_size(j_fields);
    hdr->fields = (gbf_field_meta_t*)gbf_xcalloc(nf, sizeof(gbf_field_meta_t));
    hdr->fields_len = nf;

    for (size_t i = 0; i < nf; i++) {
        const gbf_json_t* jf = gbf_json_array_get(j_fields, i);
        if (!jf || gbf_json_type(jf) != GBF_JSON_OBJECT) {
            gbf_json_free(root);
            gbf_header_free(hdr);
            gbf_set_err(err, "header JSON: fields[%zu] is not an object", i);
            return 0;
        }

        gbf_field_meta_t* f = &hdr->fields[i];
        const char* name = gbf_json_as_cstr(gbf_json_obj_get(jf, "name"));
        const char* kind = gbf_json_as_cstr(gbf_json_obj_get(jf, "kind"));
        const char* class_name = gbf_json_as_cstr(gbf_json_obj_get(jf, "class"));
        const char* encoding = gbf_json_as_cstr(gbf_json_obj_get(jf, "encoding"));
        const char* comp = gbf_json_as_cstr(gbf_json_obj_get(jf, "compression"));

        if (!name || !kind || !class_name) {
            gbf_json_free(root);
            gbf_header_free(hdr);
            gbf_set_err(err, "header JSON: missing name/kind/class in fields[%zu]", i);
            return 0;
        }

        f->name = gbf_strdup(name);
        f->kind = gbf_strdup(kind);
        f->class_name = gbf_strdup(class_name);
        f->encoding = gbf_strdup(encoding ? encoding : "");
        f->compression = gbf_strdup(comp ? comp : "none");

        f->complex = gbf_json_as_bool(gbf_json_obj_get(jf, "complex"), 0);
        f->offset = gbf_json_as_u64(gbf_json_obj_get(jf, "offset"), 0);
        f->csize  = gbf_json_as_u64(gbf_json_obj_get(jf, "csize"), 0);
        f->usize  = gbf_json_as_u64(gbf_json_obj_get(jf, "usize"), 0);
        f->crc32  = gbf_json_as_u32(gbf_json_obj_get(jf, "crc32"), 0);

        const gbf_json_t* j_shape = gbf_json_obj_get(jf, "shape");
        if (!j_shape || gbf_json_type(j_shape) != GBF_JSON_ARRAY) {
            gbf_json_free(root);
            gbf_header_free(hdr);
            gbf_set_err(err, "header JSON: missing shape array in fields[%zu]", i);
            return 0;
        }
        size_t nd = gbf_json_array_size(j_shape);
        f->shape = (uint64_t*)gbf_xcalloc(nd, sizeof(uint64_t));
        f->shape_len = nd;
        for (size_t d = 0; d < nd; d++) {
            const gbf_json_t* jd = gbf_json_array_get(j_shape, d);
            f->shape[d] = gbf_json_as_u64(jd, 0);
        }
    }

    gbf_json_free(root);
    *out_hdr = hdr;
    return 1;
}

/* ===== payload decoding ===== */

static int read_u32(const uint8_t** p, const uint8_t* end, uint32_t* out) {
    if ((size_t)(end - *p) < 4) return 0;
    *out = gbf_le_u32(*p);
    *p += 4;
    return 1;
}

static int read_i32(const uint8_t** p, const uint8_t* end, int32_t* out) {
    if ((size_t)(end - *p) < 4) return 0;
    *out = gbf_le_i32(*p);
    *p += 4;
    return 1;
}

static int read_i64(const uint8_t** p, const uint8_t* end, int64_t* out) {
    if ((size_t)(end - *p) < 8) return 0;
    *out = gbf_le_i64(*p);
    *p += 8;
    return 1;
}

static int read_str_u32len(const uint8_t** p, const uint8_t* end, char** out) {
    uint32_t n = 0;
    if (!read_u32(p, end, &n)) return 0;
    if ((size_t)(end - *p) < (size_t)n) return 0;
    char* s = (char*)gbf_xmalloc((size_t)n + 1);
    memcpy(s, *p, (size_t)n);
    s[n] = 0;
    *p += n;
    *out = s;
    return 1;
}

static gbf_value_t* decode_field_value(const gbf_field_meta_t* meta, const uint8_t* bytes, size_t len, gbf_error_t* err) {
    if (!meta) return NULL;

    /* convert shape */
    size_t* shape = NULL;
    size_t shape_len = 0;
    if (!copy_shape_u64_to_size(meta->shape, meta->shape_len, &shape, &shape_len, err)) return NULL;

    size_t numel = 0;
    if (shape_len > 0 && !shape_numel(shape, shape_len, &numel)) {
        free(shape);
        gbf_set_err(err, "decode: shape too large");
        return NULL;
    }

    if (strcmp(meta->kind, "numeric") == 0) {
        gbf_numeric_class_t cls;
        if (!parse_numeric_class(meta->class_name, &cls)) {
            free(shape);
            gbf_set_err(err, "numeric: unsupported class '%s'", meta->class_name);
            return NULL;
        }
        size_t bpe = bytes_per_elem(cls);
        size_t expected = numel * bpe;
        if (!meta->complex) {
            if (len < expected) {
                free(shape);
                gbf_set_err(err, "numeric: payload too small");
                return NULL;
            }
            gbf_value_t* v = (gbf_value_t*)gbf_xcalloc(1, sizeof(gbf_value_t));
            v->kind = GBF_VALUE_NUMERIC;
            v->as.num.class_id = cls;
            v->as.num.shape = shape;
            v->as.num.shape_len = shape_len;
            v->as.num.complex = 0;
            v->as.num.real_len = expected;
            v->as.num.real_le = (uint8_t*)gbf_xmalloc(expected);
            memcpy(v->as.num.real_le, bytes, expected);
            return v;
        }

        /* complex */
        if (len < expected * 2) {
            free(shape);
            gbf_set_err(err, "numeric: complex payload too small");
            return NULL;
        }
        gbf_value_t* v = (gbf_value_t*)gbf_xcalloc(1, sizeof(gbf_value_t));
        v->kind = GBF_VALUE_NUMERIC;
        v->as.num.class_id = cls;
        v->as.num.shape = shape;
        v->as.num.shape_len = shape_len;
        v->as.num.complex = 1;

        v->as.num.real_len = expected;
        v->as.num.real_le = (uint8_t*)gbf_xmalloc(expected);
        memcpy(v->as.num.real_le, bytes, expected);

        v->as.num.imag_len = expected;
        v->as.num.imag_le = (uint8_t*)gbf_xmalloc(expected);
        memcpy(v->as.num.imag_le, bytes + expected, expected);
        return v;
    }

    if (strcmp(meta->kind, "logical") == 0) {
        if (len < numel) {
            free(shape);
            gbf_set_err(err, "logical: payload too small");
            return NULL;
        }
        gbf_value_t* v = (gbf_value_t*)gbf_xcalloc(1, sizeof(gbf_value_t));
        v->kind = GBF_VALUE_LOGICAL;
        v->as.logical.shape = shape;
        v->as.logical.shape_len = shape_len;
        v->as.logical.len = numel;
        v->as.logical.data = (uint8_t*)gbf_xmalloc(numel);
        memcpy(v->as.logical.data, bytes, numel);
        return v;
    }

    if (strcmp(meta->kind, "char") == 0) {
        size_t expected_bytes = numel * 2;
        if (len < expected_bytes) {
            free(shape);
            gbf_set_err(err, "char: payload too small");
            return NULL;
        }
        gbf_value_t* v = (gbf_value_t*)gbf_xcalloc(1, sizeof(gbf_value_t));
        v->kind = GBF_VALUE_CHAR;
        v->as.chr.shape = shape;
        v->as.chr.shape_len = shape_len;
        v->as.chr.len = numel;
        v->as.chr.data = (uint16_t*)gbf_xcalloc(numel, sizeof(uint16_t));
        for (size_t i = 0; i < numel; i++) {
            const uint8_t* b = bytes + i * 2;
            v->as.chr.data[i] = (uint16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
        }
        return v;
    }

    if (strcmp(meta->kind, "string") == 0) {
        const uint8_t* p = bytes;
        const uint8_t* end = bytes + len;

        uint32_t count = 0;
        if (!read_u32(&p, end, &count)) {
            free(shape);
            gbf_set_err(err, "string: short payload");
            return NULL;
        }
        if ((size_t)count != numel) {
            free(shape);
            gbf_set_err(err, "string: count mismatch (hdr=%zu payload=%u)", numel, count);
            return NULL;
        }

        gbf_value_t* v = (gbf_value_t*)gbf_xcalloc(1, sizeof(gbf_value_t));
        v->kind = GBF_VALUE_STRING;
        v->as.str.shape = shape;
        v->as.str.shape_len = shape_len;
        v->as.str.len = numel;
        v->as.str.data = (char**)gbf_xcalloc(numel, sizeof(char*));

        for (size_t i = 0; i < numel; i++) {
            uint32_t n = 0;
            if (!read_u32(&p, end, &n)) {
                gbf_value_free(v);
                gbf_set_err(err, "string: short element header");
                return NULL;
            }
            if (n == 0) {
                v->as.str.data[i] = NULL;
                continue;
            }
            if ((size_t)(end - p) < (size_t)n) {
                gbf_value_free(v);
                gbf_set_err(err, "string: short element bytes");
                return NULL;
            }
            char* s = (char*)gbf_xmalloc((size_t)n + 1);
            memcpy(s, p, (size_t)n);
            s[n] = 0;
            p += n;
            v->as.str.data[i] = s;
        }
        return v;
    }

    if (strcmp(meta->kind, "datetime") == 0) {
        const uint8_t* p = bytes;
        const uint8_t* end = bytes + len;

        uint32_t count = 0;
        if (!read_u32(&p, end, &count)) {
            free(shape);
            gbf_set_err(err, "datetime: short payload");
            return NULL;
        }
        if ((size_t)count != numel) {
            free(shape);
            gbf_set_err(err, "datetime: count mismatch");
            return NULL;
        }

        char* tz = NULL;
        char* loc = NULL;
        char* fmt = NULL;
        if (!read_str_u32len(&p, end, &tz) ||
            !read_str_u32len(&p, end, &loc) ||
            !read_str_u32len(&p, end, &fmt)) {
            free(shape);
            free(tz); free(loc); free(fmt);
            gbf_set_err(err, "datetime: bad strings");
            return NULL;
        }

        if ((size_t)(end - p) < numel) {
            free(shape); free(tz); free(loc); free(fmt);
            gbf_set_err(err, "datetime: short nat mask");
            return NULL;
        }
        uint8_t* mask = (uint8_t*)gbf_xmalloc(numel);
        memcpy(mask, p, numel);
        p += numel;

        if ((size_t)(end - p) < numel * 8) {
            free(shape); free(tz); free(loc); free(fmt); free(mask);
            gbf_set_err(err, "datetime: short ms array");
            return NULL;
        }
        int64_t* ms = (int64_t*)gbf_xcalloc(numel, sizeof(int64_t));
        for (size_t i = 0; i < numel; i++) {
            int64_t x = 0;
            if (!read_i64(&p, end, &x)) { /* shouldn't fail due to check above */
                free(shape); free(tz); free(loc); free(fmt); free(mask); free(ms);
                gbf_set_err(err, "datetime: bad ms");
                return NULL;
            }
            ms[i] = x;
        }

        gbf_value_t* v = (gbf_value_t*)gbf_xcalloc(1, sizeof(gbf_value_t));
        v->kind = GBF_VALUE_DATETIME;
        v->as.dt.shape = shape;
        v->as.dt.shape_len = shape_len;
        v->as.dt.timezone = tz;
        v->as.dt.locale = loc;
        v->as.dt.format = fmt;
        v->as.dt.nat_mask = mask;
        v->as.dt.mask_len = numel;
        v->as.dt.ms = ms;
        v->as.dt.n = numel;
        return v;
    }

    if (strcmp(meta->kind, "duration") == 0) {
        const uint8_t* p = bytes;
        const uint8_t* end = bytes + len;

        uint32_t count = 0;
        if (!read_u32(&p, end, &count)) {
            free(shape);
            gbf_set_err(err, "duration: short payload");
            return NULL;
        }
        if ((size_t)count != numel) {
            free(shape);
            gbf_set_err(err, "duration: count mismatch");
            return NULL;
        }

        if ((size_t)(end - p) < numel) {
            free(shape);
            gbf_set_err(err, "duration: short nan mask");
            return NULL;
        }
        uint8_t* mask = (uint8_t*)gbf_xmalloc(numel);
        memcpy(mask, p, numel);
        p += numel;

        if ((size_t)(end - p) < numel * 8) {
            free(shape); free(mask);
            gbf_set_err(err, "duration: short ms");
            return NULL;
        }
        int64_t* ms = (int64_t*)gbf_xcalloc(numel, sizeof(int64_t));
        for (size_t i = 0; i < numel; i++) {
            int64_t x = 0;
            if (!read_i64(&p, end, &x)) { free(shape); free(mask); free(ms); gbf_set_err(err, "duration: bad ms"); return NULL; }
            ms[i] = x;
        }

        gbf_value_t* v = (gbf_value_t*)gbf_xcalloc(1, sizeof(gbf_value_t));
        v->kind = GBF_VALUE_DURATION;
        v->as.dur.shape = shape;
        v->as.dur.shape_len = shape_len;
        v->as.dur.nan_mask = mask;
        v->as.dur.mask_len = numel;
        v->as.dur.ms = ms;
        v->as.dur.n = numel;
        return v;
    }

    if (strcmp(meta->kind, "calendarDuration") == 0 || strcmp(meta->kind, "calendarduration") == 0) {
        const uint8_t* p = bytes;
        const uint8_t* end = bytes + len;

        uint32_t count = 0;
        if (!read_u32(&p, end, &count)) {
            free(shape);
            gbf_set_err(err, "calendarDuration: short payload");
            return NULL;
        }
        if ((size_t)count != numel) {
            free(shape);
            gbf_set_err(err, "calendarDuration: count mismatch");
            return NULL;
        }

        if ((size_t)(end - p) < numel) {
            free(shape);
            gbf_set_err(err, "calendarDuration: short mask");
            return NULL;
        }
        uint8_t* mask = (uint8_t*)gbf_xmalloc(numel);
        memcpy(mask, p, numel);
        p += numel;

        if ((size_t)(end - p) < numel * (4 + 4 + 8)) {
            free(shape); free(mask);
            gbf_set_err(err, "calendarDuration: short arrays");
            return NULL;
        }

        int32_t* months = (int32_t*)gbf_xcalloc(numel, sizeof(int32_t));
        int32_t* days = (int32_t*)gbf_xcalloc(numel, sizeof(int32_t));
        int64_t* time_ms = (int64_t*)gbf_xcalloc(numel, sizeof(int64_t));

        for (size_t i = 0; i < numel; i++) {
            int32_t m = 0, d = 0;
            int64_t tms = 0;
            if (!read_i32(&p, end, &m) || !read_i32(&p, end, &d) || !read_i64(&p, end, &tms)) {
                free(shape); free(mask); free(months); free(days); free(time_ms);
                gbf_set_err(err, "calendarDuration: bad arrays");
                return NULL;
            }
            months[i] = m;
            days[i] = d;
            time_ms[i] = tms;
        }

        gbf_value_t* v = (gbf_value_t*)gbf_xcalloc(1, sizeof(gbf_value_t));
        v->kind = GBF_VALUE_CALENDARDURATION;
        v->as.caldur.shape = shape;
        v->as.caldur.shape_len = shape_len;
        v->as.caldur.mask = mask;
        v->as.caldur.mask_len = numel;
        v->as.caldur.months = months;
        v->as.caldur.days = days;
        v->as.caldur.time_ms = time_ms;
        v->as.caldur.n = numel;
        return v;
    }

    if (strcmp(meta->kind, "categorical") == 0) {
        const uint8_t* p = bytes;
        const uint8_t* end = bytes + len;

        uint32_t ncat = 0;
        if (!read_u32(&p, end, &ncat)) {
            free(shape);
            gbf_set_err(err, "categorical: short payload");
            return NULL;
        }

        char** cats = (char**)gbf_xcalloc(ncat, sizeof(char*));
        for (uint32_t i = 0; i < ncat; i++) {
            char* s = NULL;
            if (!read_str_u32len(&p, end, &s)) {
                for (uint32_t k = 0; k < i; k++) free(cats[k]);
                free(cats);
                free(shape);
                gbf_set_err(err, "categorical: bad category string");
                return NULL;
            }
            cats[i] = s;
        }

        if ((size_t)(end - p) < numel * 4) {
            for (uint32_t i = 0; i < ncat; i++) free(cats[i]);
            free(cats);
            free(shape);
            gbf_set_err(err, "categorical: short codes");
            return NULL;
        }

        uint32_t* codes = (uint32_t*)gbf_xcalloc(numel, sizeof(uint32_t));
        for (size_t i = 0; i < numel; i++) {
            uint32_t code = 0;
            if (!read_u32(&p, end, &code)) {
                for (uint32_t k = 0; k < ncat; k++) free(cats[k]);
                free(cats); free(shape); free(codes);
                gbf_set_err(err, "categorical: bad codes");
                return NULL;
            }
            codes[i] = code;
        }

        gbf_value_t* v = (gbf_value_t*)gbf_xcalloc(1, sizeof(gbf_value_t));
        v->kind = GBF_VALUE_CATEGORICAL;
        v->as.cat.shape = shape;
        v->as.cat.shape_len = shape_len;
        v->as.cat.categories = cats;
        v->as.cat.categories_len = (size_t)ncat;
        v->as.cat.codes = codes;
        v->as.cat.codes_len = numel;
        return v;
    }

    if (strcmp(meta->kind, "struct") == 0) {
        /* only empty scalar struct is represented as a leaf */
        free(shape);
        gbf_value_t* v = gbf_value_new_struct();
        return v;
    }

    /* unknown: opaque */
    gbf_value_t* v = (gbf_value_t*)gbf_xcalloc(1, sizeof(gbf_value_t));
    v->kind = GBF_VALUE_OPAQUE;
    v->as.opaque.kind = gbf_strdup(meta->kind);
    v->as.opaque.class_name = gbf_strdup(meta->class_name);
    v->as.opaque.shape = shape;
    v->as.opaque.shape_len = shape_len;
    v->as.opaque.complex = meta->complex ? 1 : 0;
    v->as.opaque.encoding = gbf_strdup(meta->encoding ? meta->encoding : "");
    v->as.opaque.bytes = (uint8_t*)gbf_xmalloc(len);
    v->as.opaque.bytes_len = len;
    memcpy(v->as.opaque.bytes, bytes, len);
    return v;
}

/* ===== struct insertion by path ===== */

static int split_path(const char* path, char*** out_parts, size_t* out_len, gbf_error_t* err) {
    (void)err;
    *out_parts = NULL;
    *out_len = 0;
    if (!path) return 1;

    /* count */
    size_t count = 1;
    for (const char* p = path; *p; p++) if (*p == '.') count++;

    char** parts = (char**)gbf_xcalloc(count, sizeof(char*));
    size_t idx = 0;

    const char* start = path;
    const char* p = path;
    while (1) {
        if (*p == '.' || *p == 0) {
            size_t n = (size_t)(p - start);
            char* s = (char*)gbf_xmalloc(n + 1);
            memcpy(s, start, n);
            s[n] = 0;
            parts[idx++] = s;

            if (*p == 0) break;
            start = p + 1;
        }
        p++;
    }

    *out_parts = parts;
    *out_len = idx;
    return 1;
}

static void free_parts(char** parts, size_t n) {
    if (!parts) return;
    for (size_t i = 0; i < n; i++) free(parts[i]);
    free(parts);
}

static gbf_value_t* struct_get_child(gbf_value_t* s, const char* key) {
    if (!s || s->kind != GBF_VALUE_STRUCT) return NULL;
    for (size_t i = 0; i < s->as.s.len; i++) {
        if (strcmp(s->as.s.entries[i].key, key) == 0) return s->as.s.entries[i].value;
    }
    return NULL;
}

static int struct_insert_path(gbf_value_t* root, const char* path, gbf_value_t* leaf, gbf_error_t* err) {
    if (!root || root->kind != GBF_VALUE_STRUCT) {
        gbf_set_err(err, "insert_path: root not a struct");
        return 0;
    }
    if (!path || !*path) {
        gbf_set_err(err, "insert_path: empty path");
        return 0;
    }

    char** parts = NULL;
    size_t n = 0;
    if (!split_path(path, &parts, &n, err)) return 0;
    if (n == 0) { free_parts(parts, n); gbf_set_err(err, "insert_path: bad path"); return 0; }

    gbf_value_t* cur = root;
    for (size_t i = 0; i + 1 < n; i++) {
        gbf_value_t* ch = struct_get_child(cur, parts[i]);
        if (!ch) {
            gbf_value_t* ns = gbf_value_new_struct();
            if (!gbf_struct_set(cur, parts[i], ns, err)) {
                gbf_value_free(ns);
                free_parts(parts, n);
                return 0;
            }
            ch = ns;
        }
        if (ch->kind != GBF_VALUE_STRUCT) {
            free_parts(parts, n);
            gbf_set_err(err, "insert_path: path collision at '%s'", parts[i]);
            return 0;
        }
        cur = ch;
    }

    int ok = gbf_struct_set(cur, parts[n - 1], leaf, err);
    free_parts(parts, n);
    return ok;
}

/* ===== public I/O ===== */

int gbf_read_header_only(
    const char* path,
    gbf_read_options_t opt,
    gbf_header_t** out_header,
    uint32_t* out_header_len,
    char** out_raw_json,
    gbf_error_t* err)
{
    if (out_header) *out_header = NULL;
    if (out_header_len) *out_header_len = 0;
    if (out_raw_json) *out_raw_json = NULL;

    if (!path || !*path) {
        gbf_set_err(err, "read_header_only: path is empty");
        return 0;
    }

    FILE* f = fopen(path, "rb");
    if (!f) {
        gbf_set_err(err, "failed to open '%s': %s", path, strerror(errno));
        return 0;
    }

    uint8_t magic[8];
    if (!fread_exact(f, magic, 8)) {
        fclose(f);
        gbf_set_err(err, "file too small (missing magic)");
        return 0;
    }

    /* accept "GREDBIN" (7 bytes) in the first 7, ignore last padding */
    if (memcmp(magic, k_magic, k_magic_len) != 0) {
        /* also accept legacy GRDCBIN if present */
        if (memcmp(magic, "GRDCBIN", 6) != 0) {
            fclose(f);
            gbf_set_err(err, "bad magic (expected '%s')", k_magic);
            return 0;
        }
    }

    uint8_t hlb[4];
    if (!fread_exact(f, hlb, 4)) {
        fclose(f);
        gbf_set_err(err, "file too small (missing header_len)");
        return 0;
    }
    uint32_t header_len = gbf_le_u32(hlb);

    /* sanity */
    if (header_len > (1024u * 1024u * 256u)) {
        fclose(f);
        gbf_set_err(err, "unreasonable header length: %u", header_len);
        return 0;
    }

    char* header_json = (char*)gbf_xmalloc((size_t)header_len + 1);
    if (!fread_exact(f, header_json, (size_t)header_len)) {
        fclose(f);
        free(header_json);
        gbf_set_err(err, "file too small (header truncated)");
        return 0;
    }
    header_json[header_len] = 0;

    /* validate header CRC if requested */
    if (opt.validate) {
        uint32_t expected = gbf_extract_header_crc32_hex_u32(header_json, header_len);
        char* tmp = (char*)gbf_xmalloc((size_t)header_len + 1);
        memcpy(tmp, header_json, (size_t)header_len + 1);
        int found = gbf_zero_out_header_crc32_field(tmp, header_len);
        uint32_t got = (uint32_t)crc32(0u, (const Bytef*)tmp, (uInt)header_len);
        free(tmp);

        if (found && got != expected) {
            fclose(f);
            free(header_json);
            gbf_set_err(err, "header CRC mismatch: expected=%08X got=%08X", expected, got);
            return 0;
        }
    }

    /* parse header JSON */
    gbf_header_t* hdr = NULL;
    if (!parse_header_from_json(header_json, header_len, &hdr, err)) {
        fclose(f);
        free(header_json);
        return 0;
    }

    /* validate payload_start consistency (if present) */
    uint64_t expected_payload_start = (uint64_t)(8 + 4) + (uint64_t)header_len;
    if (opt.validate && hdr->payload_start != 0 && hdr->payload_start != expected_payload_start) {
        fclose(f);
        gbf_header_free(hdr);
        free(header_json);
        gbf_set_err(err, "payload_start mismatch: header=%" PRIu64 " expected=%" PRIu64, hdr->payload_start, expected_payload_start);
        return 0;
    }
    if (hdr->payload_start == 0) hdr->payload_start = expected_payload_start;

    if (opt.validate || hdr->file_size == 0) {
        uint64_t actual = 0;
        if (file_size_u64(f, &actual)) {
            if (opt.validate && hdr->file_size != 0 && hdr->file_size != actual) {
                fclose(f);
                gbf_header_free(hdr);
                free(header_json);
                gbf_set_err(err, "file_size mismatch: header=%" PRIu64 " actual=%" PRIu64, hdr->file_size, actual);
                return 0;
            }
            hdr->file_size = actual;
        }
    }

    if (out_header) *out_header = hdr;
    else gbf_header_free(hdr);

    if (out_header_len) *out_header_len = header_len;
    if (out_raw_json) *out_raw_json = header_json;
    else free(header_json);

    fclose(f);
    return 1;
}

static int read_payload_bytes(
    FILE* f,
    uint64_t abs_offset,
    uint64_t csize,
    uint8_t** out_buf,
    size_t* out_len,
    gbf_error_t* err)
{
    *out_buf = NULL;
    *out_len = 0;

    if (csize == 0) {
        *out_buf = NULL;
        *out_len = 0;
        return 1;
    }
    if (csize > (uint64_t)SIZE_MAX) {
        gbf_set_err(err, "payload chunk too large");
        return 0;
    }
if (fseek(f, (long)abs_offset, SEEK_SET) != 0) {
        gbf_set_err(err, "seek failed");
        return 0;
    }

    size_t n = (size_t)csize;
    uint8_t* buf = (uint8_t*)gbf_xmalloc(n);
    if (!fread_exact(f, buf, n)) {
        free(buf);
        gbf_set_err(err, "payload truncated");
        return 0;
    }

    *out_buf = buf;
    *out_len = n;
    return 1;
}

static int maybe_decompress(
    const gbf_field_meta_t* meta,
    const uint8_t* cbuf,
    size_t cbuf_len,
    uint8_t** out_ubuf,
    size_t* out_ubuf_len,
    gbf_error_t* err)
{
    *out_ubuf = NULL;
    *out_ubuf_len = 0;

    if (meta->usize == 0) {
        *out_ubuf = NULL;
        *out_ubuf_len = 0;
        return 1;
    }

    if (strcmp(meta->compression, "zlib") == 0) {
        if (meta->usize > (uint64_t)SIZE_MAX) {
            gbf_set_err(err, "usize too large");
            return 0;
        }
        size_t usize = (size_t)meta->usize;
        uint8_t* ubuf = (uint8_t*)gbf_xmalloc(usize);
        uLongf dst_len = (uLongf)usize;
        int rc = uncompress((Bytef*)ubuf, &dst_len, (const Bytef*)cbuf, (uLong)cbuf_len);
        if (rc != Z_OK || (size_t)dst_len != usize) {
            free(ubuf);
            gbf_set_err(err, "zlib uncompress failed (rc=%d)", rc);
            return 0;
        }
        *out_ubuf = ubuf;
        *out_ubuf_len = usize;
        return 1;
    }

    /* none */
    if (meta->csize != meta->usize && meta->usize != 0) {
        /* still accept; but prefer to trust header */
    }
    if (cbuf_len < (size_t)meta->usize) {
        gbf_set_err(err, "uncompressed payload too small");
        return 0;
    }
    *out_ubuf = (uint8_t*)gbf_xmalloc((size_t)meta->usize);
    memcpy(*out_ubuf, cbuf, (size_t)meta->usize);
    *out_ubuf_len = (size_t)meta->usize;
    return 1;
}

int gbf_read_file(
    const char* path,
    gbf_read_options_t opt,
    gbf_value_t** out_value,
    gbf_header_t** out_header_optional,
    gbf_error_t* err)
{
    if (out_value) *out_value = NULL;
    if (out_header_optional) *out_header_optional = NULL;

    gbf_header_t* hdr = NULL;
    uint32_t header_len = 0;
    char* raw_json = NULL;

    if (!gbf_read_header_only(path, opt, &hdr, &header_len, &raw_json, err)) {
        return 0;
    }
    free(raw_json);

    FILE* f = fopen(path, "rb");
    if (!f) {
        gbf_header_free(hdr);
        gbf_set_err(err, "failed to open '%s': %s", path, strerror(errno));
        return 0;
    }

    gbf_value_t* root = gbf_value_new_struct();

    for (size_t i = 0; i < hdr->fields_len; i++) {
        const gbf_field_meta_t* meta = &hdr->fields[i];

        uint64_t abs = hdr->payload_start + meta->offset;

        uint8_t* cbuf = NULL;
        size_t cbuf_len = 0;
        if (!read_payload_bytes(f, abs, meta->csize, &cbuf, &cbuf_len, err)) {
            fclose(f);
            gbf_header_free(hdr);
            gbf_value_free(root);
            return 0;
        }

        uint8_t* ubuf = NULL;
        size_t ubuf_len = 0;
        if (!maybe_decompress(meta, cbuf, cbuf_len, &ubuf, &ubuf_len, err)) {
            free(cbuf);
            fclose(f);
            gbf_header_free(hdr);
            gbf_value_free(root);
            return 0;
        }
        free(cbuf);

        /* CRC validate on uncompressed bytes */
        if (opt.validate && ubuf_len > 0) {
            uint32_t got = (uint32_t)crc32(0u, (const Bytef*)ubuf, (uInt)ubuf_len);
            if (got != meta->crc32) {
                free(ubuf);
                fclose(f);
                gbf_header_free(hdr);
                gbf_value_free(root);
                gbf_set_err(err, "field CRC mismatch for '%s': expected=%08X got=%08X", meta->name, meta->crc32, got);
                return 0;
            }
        }

        gbf_value_t* v = decode_field_value(meta, ubuf, ubuf_len, err);
        free(ubuf);
        if (!v) {
            fclose(f);
            gbf_header_free(hdr);
            gbf_value_free(root);
            return 0;
        }

        if (!struct_insert_path(root, meta->name, v, err)) {
            gbf_value_free(v);
            fclose(f);
            gbf_header_free(hdr);
            gbf_value_free(root);
            return 0;
        }
    }

    fclose(f);

    if (out_header_optional) *out_header_optional = hdr;
    else gbf_header_free(hdr);

    if (out_value) *out_value = root;
    else gbf_value_free(root);

    return 1;
}

int gbf_read_var(
    const char* path,
    const char* var,
    gbf_read_options_t opt,
    gbf_value_t** out_value,
    gbf_error_t* err)
{
    if (out_value) *out_value = NULL;

    gbf_header_t* hdr = NULL;
    uint32_t header_len = 0;
    char* raw_json = NULL;

    if (!gbf_read_header_only(path, opt, &hdr, &header_len, &raw_json, err)) {
        return 0;
    }
    free(raw_json);

    const char* prefix = (var && *var) ? var : NULL;
    size_t prefix_len = prefix ? strlen(prefix) : 0;

    /* collect matching fields */
    size_t match_count = 0;
    for (size_t i = 0; i < hdr->fields_len; i++) {
        const char* name = hdr->fields[i].name;
        if (!prefix) {
            match_count++;
        } else if (strcmp(name, prefix) == 0) {
            match_count++;
        } else if (strncmp(name, prefix, prefix_len) == 0 && name[prefix_len] == '.') {
            match_count++;
        }
    }

    if (match_count == 0) {
        gbf_header_free(hdr);
        gbf_set_err(err, "var '%s' not found", prefix ? prefix : "<root>");
        return 0;
    }

    FILE* f = fopen(path, "rb");
    if (!f) {
        gbf_header_free(hdr);
        gbf_set_err(err, "failed to open '%s': %s", path, strerror(errno));
        return 0;
    }

    /* if exact leaf match exists and no nested children, return that leaf directly */
    int has_exact = 0;
    int has_children = 0;
    size_t exact_index = 0;
    if (prefix) {
        for (size_t i = 0; i < hdr->fields_len; i++) {
            const char* name = hdr->fields[i].name;
            if (strcmp(name, prefix) == 0) { has_exact = 1; exact_index = i; }
            else if (strncmp(name, prefix, prefix_len) == 0 && name[prefix_len] == '.') { has_children = 1; }
        }
    }

    gbf_value_t* out = NULL;

    if (prefix && has_exact && !has_children) {
        const gbf_field_meta_t* meta = &hdr->fields[exact_index];
        uint64_t abs = hdr->payload_start + meta->offset;

        uint8_t* cbuf = NULL;
        size_t cbuf_len = 0;
        if (!read_payload_bytes(f, abs, meta->csize, &cbuf, &cbuf_len, err)) goto fail;

        uint8_t* ubuf = NULL;
        size_t ubuf_len = 0;
        if (!maybe_decompress(meta, cbuf, cbuf_len, &ubuf, &ubuf_len, err)) { free(cbuf); goto fail; }
        free(cbuf);

        if (opt.validate && ubuf_len > 0) {
            uint32_t got = (uint32_t)crc32(0u, (const Bytef*)ubuf, (uInt)ubuf_len);
            if (got != meta->crc32) {
                free(ubuf);
                gbf_set_err(err, "field CRC mismatch for '%s': expected=%08X got=%08X", meta->name, meta->crc32, got);
                goto fail;
            }
        }

        gbf_value_t* v = decode_field_value(meta, ubuf, ubuf_len, err);
        free(ubuf);
        if (!v) goto fail;

        out = v;
    } else {
        /* build subtree struct (paths stripped of prefix) */
        gbf_value_t* root_sub = gbf_value_new_struct();

        for (size_t i = 0; i < hdr->fields_len; i++) {
            const gbf_field_meta_t* meta = &hdr->fields[i];
            const char* name = meta->name;

            if (!prefix) {
                /* keep as-is */
            } else if (strcmp(name, prefix) == 0) {
                /* leaf exactly at prefix: insert under "<value>" to make it reachable */
                continue; /* for simplicity */
            } else if (strncmp(name, prefix, prefix_len) == 0 && name[prefix_len] == '.') {
                name = name + prefix_len + 1; /* strip */
            } else {
                continue;
            }

            uint64_t abs = hdr->payload_start + meta->offset;

            uint8_t* cbuf = NULL;
            size_t cbuf_len = 0;
            if (!read_payload_bytes(f, abs, meta->csize, &cbuf, &cbuf_len, err)) { gbf_value_free(root_sub); goto fail; }

            uint8_t* ubuf = NULL;
            size_t ubuf_len = 0;
            if (!maybe_decompress(meta, cbuf, cbuf_len, &ubuf, &ubuf_len, err)) { free(cbuf); gbf_value_free(root_sub); goto fail; }
            free(cbuf);

            if (opt.validate && ubuf_len > 0) {
                uint32_t got = (uint32_t)crc32(0u, (const Bytef*)ubuf, (uInt)ubuf_len);
                if (got != meta->crc32) {
                    free(ubuf);
                    gbf_value_free(root_sub);
                    gbf_set_err(err, "field CRC mismatch for '%s': expected=%08X got=%08X", meta->name, meta->crc32, got);
                    goto fail;
                }
            }

            gbf_value_t* v = decode_field_value(meta, ubuf, ubuf_len, err);
            free(ubuf);
            if (!v) { gbf_value_free(root_sub); goto fail; }

            if (!struct_insert_path(root_sub, name, v, err)) {
                gbf_value_free(v);
                gbf_value_free(root_sub);
                goto fail;
            }
        }

        out = root_sub;
    }

    fclose(f);
    gbf_header_free(hdr);

    if (out_value) *out_value = out;
    else gbf_value_free(out);

    return 1;

fail:
    fclose(f);
    gbf_header_free(hdr);
    return 0;
}

/* ===== writer encoding ===== */

typedef struct writer_field {
    char* name;
    char* kind;
    char* class_name;
    uint64_t* shape;
    size_t shape_len;
    int complex;
    char* encoding;

    char* compression; /* chosen */
    uint64_t offset;
    uint64_t csize;
    uint64_t usize;
    uint32_t crc32;

    uint8_t* payload; size_t payload_len; /* compressed or uncompressed depending on compression */
} writer_field_t;

static void writer_field_free(writer_field_t* f) {
    if (!f) return;
    free(f->name);
    free(f->kind);
    free(f->class_name);
    free(f->shape);
    free(f->encoding);
    free(f->compression);
    free(f->payload);
    memset(f, 0, sizeof(*f));
}

static int push_writer_field(writer_field_t** arr, size_t* len, size_t* cap, writer_field_t* in) {
    if (*len == *cap) {
        size_t nc = *cap ? *cap * 2 : 16;
        *arr = (writer_field_t*)gbf_xrealloc(*arr, nc * sizeof(writer_field_t));
        *cap = nc;
    }
    (*arr)[*len] = *in;
    (*len)++;
    memset(in, 0, sizeof(*in));
    return 1;
}

static int encode_u32(gbf_strbuf_t* sb, uint32_t v) {
    uint8_t b[4];
    gbf_store_le_u32(b, v);
    return gbf_sb_append_mem(sb, b, 4);
}
static int encode_i32(gbf_strbuf_t* sb, int32_t v) {
    uint8_t b[4];
    gbf_store_le_i32(b, v);
    return gbf_sb_append_mem(sb, b, 4);
}
static int encode_i64(gbf_strbuf_t* sb, int64_t v) {
    uint8_t b[8];
    gbf_store_le_i64(b, v);
    return gbf_sb_append_mem(sb, b, 8);
}

static int encode_str_u32len(gbf_strbuf_t* sb, const char* s) {
    size_t n = s ? strlen(s) : 0;
    if (n > 0xFFFFFFFFu) return 0;
    if (!encode_u32(sb, (uint32_t)n)) return 0;
    if (n) return gbf_sb_append_mem(sb, s, n);
    return 1;
}

static int encode_value_bytes(const gbf_value_t* v, const char* kind, gbf_strbuf_t* out, gbf_error_t* err) {
    (void)kind;
    gbf_sb_init(out);

    if (!v) { gbf_set_err(err, "encode: null value"); return 0; }

    switch (v->kind) {
        case GBF_VALUE_STRUCT: {
            /* Only empty scalar struct is supported as a leaf payload. */
            if (v->as.s.len != 0) {
                gbf_set_err(err, "encode: non-empty struct cannot be a leaf payload");
                gbf_sb_free(out);
                return 0;
            }
            return 1; /* empty */
        }

        case GBF_VALUE_NUMERIC: {
            if (!gbf_sb_append_mem(out, v->as.num.real_le, v->as.num.real_len)) { gbf_sb_free(out); return 0; }
            if (v->as.num.complex) {
                if (!gbf_sb_append_mem(out, v->as.num.imag_le, v->as.num.imag_len)) { gbf_sb_free(out); return 0; }
            }
            return 1;
        }

        case GBF_VALUE_LOGICAL: {
            if (!gbf_sb_append_mem(out, v->as.logical.data, v->as.logical.len)) { gbf_sb_free(out); return 0; }
            return 1;
        }

        case GBF_VALUE_CHAR: {
            /* UTF-16 code units little-endian */
            for (size_t i = 0; i < v->as.chr.len; i++) {
                uint16_t u = v->as.chr.data[i];
                uint8_t b[2];
                b[0] = (uint8_t)(u & 0xFF);
                b[1] = (uint8_t)((u >> 8) & 0xFF);
                if (!gbf_sb_append_mem(out, b, 2)) { gbf_sb_free(out); return 0; }
            }
            return 1;
        }

        case GBF_VALUE_STRING: {
            if (v->as.str.len > 0xFFFFFFFFu) { gbf_set_err(err, "string: too many elements"); gbf_sb_free(out); return 0; }
            if (!encode_u32(out, (uint32_t)v->as.str.len)) { gbf_sb_free(out); return 0; }
            for (size_t i = 0; i < v->as.str.len; i++) {
                const char* s = v->as.str.data[i];
                if (!s) {
                    if (!encode_u32(out, 0)) { gbf_sb_free(out); return 0; }
                    continue;
                }
                size_t n = strlen(s);
                if (n > 0xFFFFFFFFu) { gbf_set_err(err, "string: element too large"); gbf_sb_free(out); return 0; }
                if (!encode_u32(out, (uint32_t)n)) { gbf_sb_free(out); return 0; }
                if (n && !gbf_sb_append_mem(out, s, n)) { gbf_sb_free(out); return 0; }
            }
            return 1;
        }

        case GBF_VALUE_DATETIME: {
            if (v->as.dt.n > 0xFFFFFFFFu) { gbf_set_err(err, "datetime: too many elements"); gbf_sb_free(out); return 0; }
            if (!encode_u32(out, (uint32_t)v->as.dt.n)) { gbf_sb_free(out); return 0; }
            if (!encode_str_u32len(out, v->as.dt.timezone ? v->as.dt.timezone : "")) { gbf_sb_free(out); return 0; }
            if (!encode_str_u32len(out, v->as.dt.locale ? v->as.dt.locale : "")) { gbf_sb_free(out); return 0; }
            if (!encode_str_u32len(out, v->as.dt.format ? v->as.dt.format : "")) { gbf_sb_free(out); return 0; }
            if (v->as.dt.mask_len != v->as.dt.n) { gbf_set_err(err, "datetime: nat_mask length mismatch"); gbf_sb_free(out); return 0; }
            if (!gbf_sb_append_mem(out, v->as.dt.nat_mask, v->as.dt.mask_len)) { gbf_sb_free(out); return 0; }
            for (size_t i = 0; i < v->as.dt.n; i++) {
                if (!encode_i64(out, v->as.dt.ms[i])) { gbf_sb_free(out); return 0; }
            }
            return 1;
        }

        case GBF_VALUE_DURATION: {
            if (v->as.dur.n > 0xFFFFFFFFu) { gbf_set_err(err, "duration: too many elements"); gbf_sb_free(out); return 0; }
            if (!encode_u32(out, (uint32_t)v->as.dur.n)) { gbf_sb_free(out); return 0; }
            if (v->as.dur.mask_len != v->as.dur.n) { gbf_set_err(err, "duration: nan_mask length mismatch"); gbf_sb_free(out); return 0; }
            if (!gbf_sb_append_mem(out, v->as.dur.nan_mask, v->as.dur.mask_len)) { gbf_sb_free(out); return 0; }
            for (size_t i = 0; i < v->as.dur.n; i++) {
                if (!encode_i64(out, v->as.dur.ms[i])) { gbf_sb_free(out); return 0; }
            }
            return 1;
        }

        case GBF_VALUE_CALENDARDURATION: {
            if (v->as.caldur.n > 0xFFFFFFFFu) { gbf_set_err(err, "calendarDuration: too many elements"); gbf_sb_free(out); return 0; }
            if (!encode_u32(out, (uint32_t)v->as.caldur.n)) { gbf_sb_free(out); return 0; }
            if (v->as.caldur.mask_len != v->as.caldur.n) { gbf_set_err(err, "calendarDuration: mask length mismatch"); gbf_sb_free(out); return 0; }
            if (!gbf_sb_append_mem(out, v->as.caldur.mask, v->as.caldur.mask_len)) { gbf_sb_free(out); return 0; }
            for (size_t i = 0; i < v->as.caldur.n; i++) {
                if (!encode_i32(out, v->as.caldur.months[i])) { gbf_sb_free(out); return 0; }
                if (!encode_i32(out, v->as.caldur.days[i])) { gbf_sb_free(out); return 0; }
                if (!encode_i64(out, v->as.caldur.time_ms[i])) { gbf_sb_free(out); return 0; }
            }
            return 1;
        }

        case GBF_VALUE_CATEGORICAL: {
            if (v->as.cat.categories_len > 0xFFFFFFFFu) { gbf_set_err(err, "categorical: too many categories"); gbf_sb_free(out); return 0; }
            if (!encode_u32(out, (uint32_t)v->as.cat.categories_len)) { gbf_sb_free(out); return 0; }
            for (size_t i = 0; i < v->as.cat.categories_len; i++) {
                if (!encode_str_u32len(out, v->as.cat.categories[i] ? v->as.cat.categories[i] : "")) { gbf_sb_free(out); return 0; }
            }
            for (size_t i = 0; i < v->as.cat.codes_len; i++) {
                if (!encode_u32(out, v->as.cat.codes[i])) { gbf_sb_free(out); return 0; }
            }
            return 1;
        }

        case GBF_VALUE_OPAQUE: {
            if (!gbf_sb_append_mem(out, v->as.opaque.bytes, v->as.opaque.bytes_len)) { gbf_sb_free(out); return 0; }
            return 1;
        }

        default:
            gbf_set_err(err, "encode: unsupported kind %d", (int)v->kind);
            gbf_sb_free(out);
            return 0;
    }
}

static int json_escape_append(gbf_strbuf_t* sb, const char* s) {
    if (!gbf_sb_append_byte(sb, '"')) return 0;
    if (s) {
        for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
            unsigned char c = *p;
            if (c == '"' || c == '\\') {
                if (!gbf_sb_append_byte(sb, '\\')) return 0;
                if (!gbf_sb_append_byte(sb, (char)c)) return 0;
            } else if (c < 0x20) {
                /* escape control chars as \u00XX */
                if (!gbf_sb_append_str(sb, "\\u00")) return 0;
                const char* hex = "0123456789ABCDEF";
                if (!gbf_sb_append_byte(sb, hex[(c >> 4) & 0xF])) return 0;
                if (!gbf_sb_append_byte(sb, hex[c & 0xF])) return 0;
            } else {
                if (!gbf_sb_append_byte(sb, (char)c)) return 0;
            }
        }
    }
    if (!gbf_sb_append_byte(sb, '"')) return 0;
    return 1;
}

static int json_append_u64(gbf_strbuf_t* sb, uint64_t v) {
    return gbf_sb_append_fmt(sb, "%" PRIu64, v);
}

static int json_append_u32(gbf_strbuf_t* sb, uint32_t v) {
    return gbf_sb_append_fmt(sb, "%u", (unsigned)v);
}

static int build_header_json(
    const writer_field_t* fields,
    size_t fields_len,
    uint64_t payload_start,
    uint64_t file_size,
    const char* crc_hex,
    char** out_json,
    uint32_t* out_len,
    gbf_error_t* err)
{
    *out_json = NULL;
    *out_len = 0;

    gbf_strbuf_t sb;
    gbf_sb_init(&sb);

    /* minified JSON, stable key order */
    if (!gbf_sb_append_byte(&sb, '{')) goto oom;

    if (!gbf_sb_append_str(&sb, "\"format\":\"GBF\"")) goto oom;
    if (!gbf_sb_append_str(&sb, ",\"magic\":\"GREDBIN\"")) goto oom;
    if (!gbf_sb_append_str(&sb, ",\"version\":1")) goto oom;
    if (!gbf_sb_append_str(&sb, ",\"endianness\":\"little\"")) goto oom;
    if (!gbf_sb_append_str(&sb, ",\"order\":\"column-major\"")) goto oom;
    if (!gbf_sb_append_str(&sb, ",\"root\":\"struct\"")) goto oom;

    if (!gbf_sb_append_str(&sb, ",\"fields\":[")) goto oom;
    for (size_t i = 0; i < fields_len; i++) {
        const writer_field_t* f = &fields[i];
        if (i) if (!gbf_sb_append_byte(&sb, ',')) goto oom;
        if (!gbf_sb_append_byte(&sb, '{')) goto oom;

        if (!gbf_sb_append_str(&sb, "\"name\":")) goto oom;
        if (!json_escape_append(&sb, f->name)) goto oom;

        if (!gbf_sb_append_str(&sb, ",\"kind\":")) goto oom;
        if (!json_escape_append(&sb, f->kind)) goto oom;

        if (!gbf_sb_append_str(&sb, ",\"class\":")) goto oom;
        if (!json_escape_append(&sb, f->class_name)) goto oom;

        if (!gbf_sb_append_str(&sb, ",\"shape\":[")) goto oom;
        for (size_t d = 0; d < f->shape_len; d++) {
            if (d) if (!gbf_sb_append_byte(&sb, ',')) goto oom;
            if (!json_append_u64(&sb, f->shape[d])) goto oom;
        }
        if (!gbf_sb_append_byte(&sb, ']')) goto oom;

        if (!gbf_sb_append_str(&sb, ",\"complex\":")) goto oom;
        if (!gbf_sb_append_str(&sb, f->complex ? "true" : "false")) goto oom;

        if (!gbf_sb_append_str(&sb, ",\"encoding\":")) goto oom;
        if (!json_escape_append(&sb, f->encoding ? f->encoding : "")) goto oom;

        if (!gbf_sb_append_str(&sb, ",\"compression\":")) goto oom;
        if (!json_escape_append(&sb, f->compression ? f->compression : "none")) goto oom;

        if (!gbf_sb_append_str(&sb, ",\"offset\":")) goto oom;
        if (!json_append_u64(&sb, f->offset)) goto oom;

        if (!gbf_sb_append_str(&sb, ",\"csize\":")) goto oom;
        if (!json_append_u64(&sb, f->csize)) goto oom;

        if (!gbf_sb_append_str(&sb, ",\"usize\":")) goto oom;
        if (!json_append_u64(&sb, f->usize)) goto oom;

        if (!gbf_sb_append_str(&sb, ",\"crc32\":")) goto oom;
        if (!json_append_u32(&sb, f->crc32)) goto oom;

        if (!gbf_sb_append_byte(&sb, '}')) goto oom;
    }
    if (!gbf_sb_append_byte(&sb, ']')) goto oom;

    if (!gbf_sb_append_str(&sb, ",\"payload_start\":")) goto oom;
    if (!json_append_u64(&sb, payload_start)) goto oom;

    if (!gbf_sb_append_str(&sb, ",\"file_size\":")) goto oom;
    if (!json_append_u64(&sb, file_size)) goto oom;

    if (!gbf_sb_append_str(&sb, ",\"header_crc32_hex\":")) goto oom;
    if (!json_escape_append(&sb, crc_hex ? crc_hex : "00000000")) goto oom;

    if (!gbf_sb_append_byte(&sb, '}')) goto oom;

    /* finalize */
    if (!gbf_sb_append_byte(&sb, 0)) goto oom; /* NUL */
    *out_len = (uint32_t)(sb.len - 1);
    *out_json = sb.data;
    return 1;

oom:
    gbf_sb_free(&sb);
    gbf_set_err(err, "out of memory building header JSON");
    return 0;
}

static void crc_hex8(uint32_t crc, char out[9]) {
    static const char* hex = "0123456789ABCDEF";
    out[0] = hex[(crc >> 28) & 0xF];
    out[1] = hex[(crc >> 24) & 0xF];
    out[2] = hex[(crc >> 20) & 0xF];
    out[3] = hex[(crc >> 16) & 0xF];
    out[4] = hex[(crc >> 12) & 0xF];
    out[5] = hex[(crc >> 8) & 0xF];
    out[6] = hex[(crc >> 4) & 0xF];
    out[7] = hex[(crc) & 0xF];
    out[8] = 0;
}

static int compress_maybe(
    const uint8_t* src, size_t src_len,
    gbf_write_options_t opt,
    char** out_comp_name,
    uint8_t** out_buf,
    size_t* out_len,
    gbf_error_t* err)
{
    *out_comp_name = NULL;
    *out_buf = NULL;
    *out_len = 0;

    if (src_len == 0) {
        *out_comp_name = gbf_strdup("none");
        return 1;
    }

    int level = (opt.zlib_level < 0) ? Z_DEFAULT_COMPRESSION : opt.zlib_level;

    if (opt.compression == GBF_COMP_NEVER) {
        *out_comp_name = gbf_strdup("none");
        uint8_t* b = (uint8_t*)gbf_xmalloc(src_len);
        memcpy(b, src, src_len);
        *out_buf = b;
        *out_len = src_len;
        return 1;
    }

    /* try zlib */
    uLongf bound = compressBound((uLong)src_len);
    uint8_t* comp = (uint8_t*)gbf_xmalloc((size_t)bound);
    uLongf comp_len = bound;

    int rc = compress2((Bytef*)comp, &comp_len, (const Bytef*)src, (uLong)src_len, level);
    if (rc != Z_OK) {
        free(comp);
        gbf_set_err(err, "zlib compress failed (rc=%d)", rc);
        return 0;
    }

    int use_zlib = 0;
    if (opt.compression == GBF_COMP_ALWAYS) {
        use_zlib = 1;
    } else {
        /* auto */
        use_zlib = (comp_len < src_len) ? 1 : 0;
    }

    if (use_zlib) {
        *out_comp_name = gbf_strdup("zlib");
        *out_buf = comp;
        *out_len = (size_t)comp_len;
        return 1;
    }

    /* use none */
    free(comp);
    *out_comp_name = gbf_strdup("none");
    uint8_t* b = (uint8_t*)gbf_xmalloc(src_len);
    memcpy(b, src, src_len);
    *out_buf = b;
    *out_len = src_len;
    return 1;
}

static int flatten_fields_rec(
    const gbf_value_t* v,
    const char* prefix,
    writer_field_t** out_fields,
    size_t* out_len,
    size_t* out_cap,
    gbf_write_options_t opt,
    gbf_error_t* err)
{
    if (!v) return 1;

    if (v->kind == GBF_VALUE_STRUCT) {
        if (v->as.s.len == 0 && prefix && *prefix) {
            /* empty struct leaf */
            writer_field_t f;
            memset(&f, 0, sizeof(f));
            f.name = gbf_strdup(prefix);
            f.kind = gbf_strdup("struct");
            f.class_name = gbf_strdup("struct");
            f.shape_len = 2;
            f.shape = (uint64_t*)gbf_xcalloc(2, sizeof(uint64_t));
            f.shape[0] = 1;
            f.shape[1] = 1;
            f.complex = 0;
            f.encoding = gbf_strdup("empty-scalar-struct");
            f.compression = NULL;

            gbf_strbuf_t bytes;
            if (!encode_value_bytes(v, f.kind, &bytes, err)) return 0;
            f.usize = (uint64_t)bytes.len;
            f.crc32 = 0;
            f.payload = NULL;
            f.payload_len = 0;
            gbf_sb_free(&bytes);

            return push_writer_field(out_fields, out_len, out_cap, &f);
        }

        for (size_t i = 0; i < v->as.s.len; i++) {
            const char* key = v->as.s.entries[i].key;
            const gbf_value_t* child = v->as.s.entries[i].value;

            gbf_strbuf_t p;
            gbf_sb_init(&p);
            if (prefix && *prefix) {
                if (!gbf_sb_append_fmt(&p, "%s.%s", prefix, key)) { gbf_sb_free(&p); return 0; }
            } else {
                if (!gbf_sb_append_str(&p, key)) { gbf_sb_free(&p); return 0; }
            }
            if (!gbf_sb_append_byte(&p, 0)) { gbf_sb_free(&p); return 0; }

            int ok = flatten_fields_rec(child, p.data, out_fields, out_len, out_cap, opt, err);
            gbf_sb_free(&p);
            if (!ok) return 0;
        }
        return 1;
    }

    /* leaf */
    if (!prefix || !*prefix) {
        gbf_set_err(err, "write: root must be a struct (leaves must have a name)");
        return 0;
    }

    writer_field_t f;
    memset(&f, 0, sizeof(f));
    f.name = gbf_strdup(prefix);
    f.complex = 0;
    f.encoding = gbf_strdup("");
    f.shape = NULL;
    f.shape_len = 0;

    /* fill kind/class/shape/encoding */
    switch (v->kind) {
        case GBF_VALUE_NUMERIC:
            f.kind = gbf_strdup("numeric");
            f.class_name = gbf_strdup(numeric_class_name(v->as.num.class_id));
            f.shape = copy_shape_size_to_u64(v->as.num.shape, v->as.num.shape_len);
            f.shape_len = v->as.num.shape_len;
            f.complex = v->as.num.complex ? 1 : 0;
            break;
        case GBF_VALUE_LOGICAL:
            f.kind = gbf_strdup("logical");
            f.class_name = gbf_strdup("logical");
            f.shape = copy_shape_size_to_u64(v->as.logical.shape, v->as.logical.shape_len);
            f.shape_len = v->as.logical.shape_len;
            break;
        case GBF_VALUE_STRING:
            f.kind = gbf_strdup("string");
            f.class_name = gbf_strdup("string");
            f.shape = copy_shape_size_to_u64(v->as.str.shape, v->as.str.shape_len);
            f.shape_len = v->as.str.shape_len;
            free(f.encoding);
            f.encoding = gbf_strdup("utf-8");
            break;
        case GBF_VALUE_CHAR:
            f.kind = gbf_strdup("char");
            f.class_name = gbf_strdup("char");
            f.shape = copy_shape_size_to_u64(v->as.chr.shape, v->as.chr.shape_len);
            f.shape_len = v->as.chr.shape_len;
            free(f.encoding);
            f.encoding = gbf_strdup("utf-16-codeunits");
            break;
        case GBF_VALUE_DATETIME:
            f.kind = gbf_strdup("datetime");
            f.class_name = gbf_strdup("datetime");
            f.shape = copy_shape_size_to_u64(v->as.dt.shape, v->as.dt.shape_len);
            f.shape_len = v->as.dt.shape_len;
            free(f.encoding);
            if (v->as.dt.timezone && v->as.dt.timezone[0] != 0) {
                f.encoding = gbf_strdup("dt:tz-ymd+msday+nat-mask+tz+locale+format");
            } else {
                f.encoding = gbf_strdup("dt:naive-ymd+msday+nat-mask+locale+format");
            }
            break;
        case GBF_VALUE_DURATION:
            f.kind = gbf_strdup("duration");
            f.class_name = gbf_strdup("duration");
            f.shape = copy_shape_size_to_u64(v->as.dur.shape, v->as.dur.shape_len);
            f.shape_len = v->as.dur.shape_len;
            free(f.encoding);
            f.encoding = gbf_strdup("ms-i64+nan-mask");
            break;
        case GBF_VALUE_CALENDARDURATION:
            f.kind = gbf_strdup("calendarDuration");
            f.class_name = gbf_strdup("calendarDuration");
            f.shape = copy_shape_size_to_u64(v->as.caldur.shape, v->as.caldur.shape_len);
            f.shape_len = v->as.caldur.shape_len;
            free(f.encoding);
            f.encoding = gbf_strdup("months-i32+days-i32+time-ms-i64+nan-mask");
            break;
        case GBF_VALUE_CATEGORICAL:
            f.kind = gbf_strdup("categorical");
            f.class_name = gbf_strdup("categorical");
            f.shape = copy_shape_size_to_u64(v->as.cat.shape, v->as.cat.shape_len);
            f.shape_len = v->as.cat.shape_len;
            free(f.encoding);
            f.encoding = gbf_strdup("u32-cats+u32-codes+utf8");
            break;
        case GBF_VALUE_OPAQUE:
            f.kind = gbf_strdup(v->as.opaque.kind ? v->as.opaque.kind : "opaque");
            f.class_name = gbf_strdup(v->as.opaque.class_name ? v->as.opaque.class_name : "opaque");
            f.shape = copy_shape_size_to_u64(v->as.opaque.shape, v->as.opaque.shape_len);
            f.shape_len = v->as.opaque.shape_len;
            f.complex = v->as.opaque.complex ? 1 : 0;
            free(f.encoding);
            f.encoding = gbf_strdup(v->as.opaque.encoding ? v->as.opaque.encoding : "");
            break;
        default:
            gbf_set_err(err, "write: unsupported kind %d", (int)v->kind);
            writer_field_free(&f);
            return 0;
    }

    /* encode uncompressed bytes */
    gbf_strbuf_t raw;
    if (!encode_value_bytes(v, f.kind, &raw, err)) {
        writer_field_free(&f);
        return 0;
    }

    f.usize = (uint64_t)raw.len;

    /* CRC on uncompressed bytes */
    if (f.usize > 0 && opt.include_crc32) {
        f.crc32 = (uint32_t)crc32(0u, (const Bytef*)raw.data, (uInt)raw.len);
    } else {
        f.crc32 = 0;
    }

    /* compress maybe */
    uint8_t* payload = NULL;
    size_t payload_len = 0;
    char* comp_name = NULL;

    if (!compress_maybe((const uint8_t*)raw.data, raw.len, opt, &comp_name, &payload, &payload_len, err)) {
        gbf_sb_free(&raw);
        writer_field_free(&f);
        return 0;
    }
    gbf_sb_free(&raw);

    f.compression = comp_name;
    f.payload = payload;
    f.payload_len = payload_len;
    f.csize = (uint64_t)payload_len;

    return push_writer_field(out_fields, out_len, out_cap, &f);
}

int gbf_write_file(
    const char* path,
    const gbf_value_t* root,
    gbf_write_options_t opt,
    gbf_error_t* err)
{
    if (!path || !*path) {
        gbf_set_err(err, "write: path is empty");
        return 0;
    }
    if (!root || root->kind != GBF_VALUE_STRUCT) {
        gbf_set_err(err, "write: root value must be a struct");
        return 0;
    }

    /* defaults */
    if (opt.zlib_level < -1 || opt.zlib_level > 9) opt.zlib_level = -1;

    writer_field_t* fields = NULL;
    size_t fields_len = 0;
    size_t fields_cap = 0;

    if (!flatten_fields_rec(root, "", &fields, &fields_len, &fields_cap, opt, err)) {
        if (fields) {
            for (size_t i = 0; i < fields_len; i++) writer_field_free(&fields[i]);
        }
        free(fields);
        return 0;
    }

    /* compute offsets & payload_size */
    uint64_t payload_size = 0;
    for (size_t i = 0; i < fields_len; i++) {
        fields[i].offset = payload_size;
        payload_size += fields[i].csize;
    }

    /* fixed-point for header_len/payload_start/file_size */
    uint64_t payload_start = 0;
    uint64_t file_size = 0;
    uint32_t header_len = 0;
    char* header_zero = NULL;

    for (int iter = 0; iter < 8; iter++) {
        free(header_zero);
        header_zero = NULL;
        uint32_t hl = 0;
        if (!build_header_json(fields, fields_len, payload_start, file_size, "00000000", &header_zero, &hl, err)) {
            goto fail;
        }

        header_len = hl;
        uint64_t new_payload_start = (uint64_t)(8 + 4) + (uint64_t)header_len;
        uint64_t new_file_size = new_payload_start + payload_size;

        if (new_payload_start == payload_start && new_file_size == file_size) {
            payload_start = new_payload_start;
            file_size = new_file_size;
            break;
        }

        payload_start = new_payload_start;
        file_size = new_file_size;
    }

    if (!header_zero) {
        gbf_set_err(err, "write: failed to build header");
        goto fail;
    }

    /* CRC over zeroed header JSON (already has 00000000) */
    uint32_t hcrc = (uint32_t)crc32(0u, (const Bytef*)header_zero, (uInt)header_len);
    char crc_hex[9];
    crc_hex8(hcrc, crc_hex);

    char* header_final = NULL;
    uint32_t header_final_len = 0;
    if (!build_header_json(fields, fields_len, payload_start, file_size, crc_hex, &header_final, &header_final_len, err)) {
        goto fail;
    }
    if (header_final_len != header_len) {
        /* should not happen because crc_hex is fixed width */
        free(header_final);
        gbf_set_err(err, "write: header length changed after CRC (unexpected)");
        goto fail;
    }

    /* write file */
    FILE* f = fopen(path, "wb");
    if (!f) {
        gbf_set_err(err, "failed to open '%s' for writing: %s", path, strerror(errno));
        free(header_final);
        goto fail;
    }

    uint8_t magic8[8] = {0,0,0,0,0,0,0,0};
    memcpy(magic8, k_magic, k_magic_len);
    if (fwrite(magic8, 1, 8, f) != 8) { fclose(f); gbf_set_err(err, "write failed"); free(header_final); goto fail; }

    uint8_t hlb[4];
    gbf_store_le_u32(hlb, header_len);
    if (fwrite(hlb, 1, 4, f) != 4) { fclose(f); gbf_set_err(err, "write failed"); free(header_final); goto fail; }

    if (fwrite(header_final, 1, header_len, f) != header_len) { fclose(f); gbf_set_err(err, "write failed"); free(header_final); goto fail; }

    /* payload */
    for (size_t i = 0; i < fields_len; i++) {
        if (fields[i].payload_len == 0) continue;
        if (fwrite(fields[i].payload, 1, fields[i].payload_len, f) != fields[i].payload_len) {
            fclose(f);
            gbf_set_err(err, "write failed (payload)");
            free(header_final);
            goto fail;
        }
    }

    fclose(f);
    free(header_final);
    free(header_zero);

    for (size_t i = 0; i < fields_len; i++) writer_field_free(&fields[i]);
    free(fields);
    return 1;

fail:
    free(header_zero);
    if (fields) {
        for (size_t i = 0; i < fields_len; i++) writer_field_free(&fields[i]);
    }
    free(fields);
    return 0;
}
