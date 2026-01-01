// gbf_easy.c
// Implementation for include/gbin/gbf_easy.h
//
// This file provides a high-level convenience layer on top of the core GBF API.
//
// Design goals:
//  - Make writing collections of matrices/arrays ergonomic in plain C.
//  - Offer a safe varargs writer that consumes/frees entry values.
//  - Provide simple navigation and typed views when reading.
//
// Notes:
//  - GBF storage for numeric arrays is column-major, little-endian byte blobs.
//  - This layer accepts typed input in either row-major or column-major order,
//    converting to GBF storage format.

#include "gbin/gbf_easy.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

// ---------------------------
// Local helpers
// ---------------------------

static void opaque_tmp_free(gbf_opaque_value_t* o) {
    if (!o) return;
    free(o->kind);
    free(o->class_name);
    free(o->shape);
    free(o->encoding);
    free(o->bytes);
    free(o);
}

// ---------------------------
// Error helper
// ---------------------------

void gbf_easy_set_err(gbf_error_t* err, const char* fmt, ...) {
    if (!err) return;

    // Free any previous message to avoid leaks.
    if (err->message) {
        free(err->message);
        err->message = NULL;
    }

    // Format the message.
    char stack_buf[1024];
    stack_buf[0] = '\0';

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(stack_buf, sizeof(stack_buf), fmt, ap);
    va_end(ap);

    if (n < 0) {
        return;
    }

    // If it fit on the stack buffer, duplicate it.
    if ((size_t)n < sizeof(stack_buf)) {
        err->message = strdup(stack_buf);
        return;
    }

    // Otherwise, allocate a large enough buffer.
    size_t need = (size_t)n + 1;
    char* heap_buf = (char*)malloc(need);
    if (!heap_buf) return;

    va_start(ap, fmt);
    vsnprintf(heap_buf, need, fmt, ap);
    va_end(ap);

    err->message = heap_buf;
}
// ---------------------------
// Local value constructors
// ---------------------------

static gbf_value_t* make_value_numeric(gbf_numeric_array_t* a) {
    gbf_value_t* v = (gbf_value_t*)calloc(1, sizeof(gbf_value_t));
    if (!v) return NULL;
    v->kind = GBF_VALUE_NUMERIC;
    v->as.num = *a; // take ownership of buffers inside *a
    free(a);
    return v;
}

static gbf_value_t* make_value_logical(gbf_logical_array_t* a) {
    gbf_value_t* v = (gbf_value_t*)calloc(1, sizeof(gbf_value_t));
    if (!v) return NULL;
    v->kind = GBF_VALUE_LOGICAL;
    v->as.logical = *a;
    free(a);
    return v;
}

static gbf_value_t* make_value_string(gbf_string_array_t* a) {
    gbf_value_t* v = (gbf_value_t*)calloc(1, sizeof(gbf_value_t));
    if (!v) return NULL;
    v->kind = GBF_VALUE_STRING;
    v->as.str = *a;
    free(a);
    return v;
}

static gbf_value_t* make_value_char(gbf_char_array_t* a) {
    gbf_value_t* v = (gbf_value_t*)calloc(1, sizeof(gbf_value_t));
    if (!v) return NULL;
    v->kind = GBF_VALUE_CHAR;
    v->as.chr = *a;
    free(a);
    return v;
}

// ---------------------------
// Numeric helpers
// ---------------------------

size_t gbf_easy_numeric_elem_size(gbf_numeric_class_t c) {
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
    default: return 0;
    }
}

const char* gbf_easy_numeric_class_name(gbf_numeric_class_t c) {
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
    default: return "unknown";
    }
}

static size_t shape_numel_sz(const size_t* shape, size_t shape_len) {
    if (!shape || shape_len == 0) return 0;
    size_t n = 1;
    for (size_t i = 0; i < shape_len; i++) n *= shape[i];
    return n;
}

// Convert row-major typed data into GBF column-major order.
// Works for any element size (bytes).
static int to_col_major_bytes(
    const void* src,
    size_t elem_size,
    const size_t* shape,
    size_t shape_len,
    gbf_easy_layout_t layout,
    uint8_t** out,
    size_t* out_len,
    gbf_error_t* err)
{
    if (!src || !shape || shape_len == 0 || elem_size == 0 || !out || !out_len) {
        gbf_easy_set_err(err, "invalid arguments");
        return 0;
    }

    size_t n = shape_numel_sz(shape, shape_len);
    size_t total = n * elem_size;

    uint8_t* dst = (uint8_t*)malloc(total);
    if (!dst) {
        gbf_easy_set_err(err, "oom");
        return 0;
    }

    // If already column-major, just memcpy.
    if (layout == GBF_EASY_COL_MAJOR) {
        memcpy(dst, src, total);
        *out = dst;
        *out_len = total;
        return 1;
    }

    // Row-major -> column-major conversion
    // For 1D, it's identical.
    if (shape_len == 1) {
        memcpy(dst, src, total);
        *out = dst;
        *out_len = total;
        return 1;
    }

    // We implement generic N-D by converting linear indices.
    // Row-major linear index: i0*(d1*d2*...)+i1*(d2*...)+...+i_{n-1}
    // Col-major linear index: i0 + i1*d0 + i2*(d0*d1) + ...

    const uint8_t* s = (const uint8_t*)src;

    // Precompute row-major strides
    size_t* rstride = (size_t*)malloc(shape_len * sizeof(size_t));
    size_t* cstride = (size_t*)malloc(shape_len * sizeof(size_t));
    if (!rstride || !cstride) {
        free(dst);
        free(rstride);
        free(cstride);
        gbf_easy_set_err(err, "oom");
        return 0;
    }

    // row strides
    rstride[shape_len - 1] = 1;
    for (size_t k = shape_len - 1; k-- > 0;) {
        rstride[k] = rstride[k + 1] * shape[k + 1];
    }
    // col strides
    cstride[0] = 1;
    for (size_t k = 1; k < shape_len; k++) {
        cstride[k] = cstride[k - 1] * shape[k - 1];
    }

    // Iterate all elements via a mixed radix counter
    size_t* idx = (size_t*)calloc(shape_len, sizeof(size_t));
    if (!idx) {
        free(dst);
        free(rstride);
        free(cstride);
        gbf_easy_set_err(err, "oom");
        return 0;
    }

    for (size_t count = 0; count < n; count++) {
        size_t roff = 0;
        size_t coff = 0;
        for (size_t k = 0; k < shape_len; k++) {
            roff += idx[k] * rstride[k];
            coff += idx[k] * cstride[k];
        }

        memcpy(dst + coff * elem_size, s + roff * elem_size, elem_size);

        // increment idx (row-major order counter)
        for (size_t k = shape_len; k-- > 0;) {
            idx[k] += 1;
            if (idx[k] < shape[k]) break;
            idx[k] = 0;
        }
    }

    free(idx);
    free(rstride);
    free(cstride);

    *out = dst;
    *out_len = total;
    return 1;
}

// ---------------------------
// Entry constructors
// ---------------------------

static gbf_easy_entry_t make_entry_err(const char* name, gbf_error_t* err, const char* msg) {
    gbf_easy_set_err(err, "%s", msg);
    return (gbf_easy_entry_t){ name, NULL };
}

gbf_easy_entry_t gbf_easy_numeric_bytes_nd(
    const char* name,
    gbf_numeric_class_t class_id,
    const size_t* shape, size_t shape_len,
    int complex,
    const void* real_le, size_t real_len,
    const void* imag_le, size_t imag_len,
    gbf_easy_ownership_t ownership,
    gbf_error_t* err)
{
    if (!name || !shape || shape_len == 0 || !real_le || real_len == 0) {
        return make_entry_err(name, err, "invalid numeric bytes args");
    }

    gbf_numeric_array_t* a = (gbf_numeric_array_t*)calloc(1, sizeof(gbf_numeric_array_t));
    if (!a) return make_entry_err(name, err, "oom");

    a->class_id = class_id;
    a->complex = complex ? 1 : 0;

    a->shape_len = shape_len;
    a->shape = (size_t*)malloc(shape_len * sizeof(size_t));
    if (!a->shape) {
        free(a);
        return make_entry_err(name, err, "oom");
    }
    memcpy(a->shape, shape, shape_len * sizeof(size_t));

    // ownership policy: COPY => copy buffers; TAKE => take ownership.
    if (ownership == GBF_EASY_TAKE) {
        a->real_le = (uint8_t*)real_le;
        a->real_len = real_len;
        a->imag_le = (uint8_t*)imag_le;
        a->imag_len = imag_len;
    } else {
        a->real_le = (uint8_t*)malloc(real_len);
        if (!a->real_le) {
            free(a->shape);
            free(a);
            return make_entry_err(name, err, "oom");
        }
        memcpy(a->real_le, real_le, real_len);
        a->real_len = real_len;

        if (complex && imag_le && imag_len) {
            a->imag_le = (uint8_t*)malloc(imag_len);
            if (!a->imag_le) {
                free(a->real_le);
                free(a->shape);
                free(a);
                return make_entry_err(name, err, "oom");
            }
            memcpy(a->imag_le, imag_le, imag_len);
            a->imag_len = imag_len;
        }
    }

    gbf_value_t* v = make_value_numeric(a);
    if (!v) {
        // a was not consumed; free what we allocated
        if (a->shape) free(a->shape);
        if (a->real_le) free(a->real_le);
        if (a->imag_le) free(a->imag_le);
        free(a);
        return make_entry_err(name, err, "failed to create numeric value");
    }

    return (gbf_easy_entry_t){ name, v };
}

gbf_easy_entry_t gbf_easy_f64_nd(
    const char* name,
    const double* data,
    const size_t* shape, size_t shape_len,
    gbf_easy_layout_t layout,
    gbf_easy_ownership_t ownership,
    gbf_error_t* err)
{
    (void)ownership; // ownership only matters if we needed to take over conversion buffers; we always allocate new bytes.
    if (!name || !data || !shape || shape_len == 0) {
        return make_entry_err(name, err, "invalid f64 args");
    }

    size_t esz = 8;
    uint8_t* bytes = NULL;
    size_t bytes_len = 0;
    if (!to_col_major_bytes(data, esz, shape, shape_len, layout, &bytes, &bytes_len, err)) {
        return (gbf_easy_entry_t){ name, NULL };
    }

    // numeric bytes are host-endian doubles; GBF expects little-endian. On little-endian hosts this is ok.
    // For portability, the core library should treat these as host->LE conversion, but we store raw and
    // rely on gbf_value_make_numeric() conventions used across the project.

    return gbf_easy_numeric_bytes_nd(name, GBF_NUM_DOUBLE, shape, shape_len, 0, bytes, bytes_len, NULL, 0, GBF_EASY_TAKE, err);
}

gbf_easy_entry_t gbf_easy_f64_matrix(
    const char* name,
    const double* data,
    size_t rows, size_t cols,
    gbf_easy_layout_t layout,
    gbf_error_t* err)
{
    size_t shape[2] = { rows, cols };
    return gbf_easy_f64_nd(name, data, shape, 2, layout, GBF_EASY_COPY, err);
}

gbf_easy_entry_t gbf_easy_f32_nd(
    const char* name,
    const float* data,
    const size_t* shape, size_t shape_len,
    gbf_easy_layout_t layout,
    gbf_error_t* err)
{
    if (!name || !data || !shape || shape_len == 0) {
        return make_entry_err(name, err, "invalid f32 args");
    }

    size_t esz = 4;
    uint8_t* bytes = NULL;
    size_t bytes_len = 0;
    if (!to_col_major_bytes(data, esz, shape, shape_len, layout, &bytes, &bytes_len, err)) {
        return (gbf_easy_entry_t){ name, NULL };
    }

    return gbf_easy_numeric_bytes_nd(name, GBF_NUM_SINGLE, shape, shape_len, 0, bytes, bytes_len, NULL, 0, GBF_EASY_TAKE, err);
}

gbf_easy_entry_t gbf_easy_i32_nd(
    const char* name,
    const int32_t* data,
    const size_t* shape, size_t shape_len,
    gbf_easy_layout_t layout,
    gbf_error_t* err)
{
    if (!name || !data || !shape || shape_len == 0) {
        return make_entry_err(name, err, "invalid i32 args");
    }

    size_t esz = 4;
    uint8_t* bytes = NULL;
    size_t bytes_len = 0;
    if (!to_col_major_bytes(data, esz, shape, shape_len, layout, &bytes, &bytes_len, err)) {
        return (gbf_easy_entry_t){ name, NULL };
    }

    return gbf_easy_numeric_bytes_nd(name, GBF_NUM_INT32, shape, shape_len, 0, bytes, bytes_len, NULL, 0, GBF_EASY_TAKE, err);
}

gbf_easy_entry_t gbf_easy_u64_nd(
    const char* name,
    const uint64_t* data,
    const size_t* shape, size_t shape_len,
    gbf_easy_layout_t layout,
    gbf_error_t* err)
{
    if (!name || !data || !shape || shape_len == 0) {
        return make_entry_err(name, err, "invalid u64 args");
    }

    size_t esz = 8;
    uint8_t* bytes = NULL;
    size_t bytes_len = 0;
    if (!to_col_major_bytes(data, esz, shape, shape_len, layout, &bytes, &bytes_len, err)) {
        return (gbf_easy_entry_t){ name, NULL };
    }

    return gbf_easy_numeric_bytes_nd(name, GBF_NUM_UINT64, shape, shape_len, 0, bytes, bytes_len, NULL, 0, GBF_EASY_TAKE, err);
}

gbf_easy_entry_t gbf_easy_logical_nd(
    const char* name,
    const uint8_t* data,
    const size_t* shape, size_t shape_len,
    gbf_easy_ownership_t ownership,
    gbf_error_t* err)
{
    if (!name || !data || !shape || shape_len == 0) {
        return make_entry_err(name, err, "invalid logical args");
    }

    size_t n = shape_numel_sz(shape, shape_len);

    gbf_logical_array_t* a = (gbf_logical_array_t*)calloc(1, sizeof(gbf_logical_array_t));
    if (!a) return make_entry_err(name, err, "oom");

    a->shape_len = shape_len;
    a->shape = (size_t*)malloc(shape_len * sizeof(size_t));
    if (!a->shape) { free(a); return make_entry_err(name, err, "oom"); }
    memcpy(a->shape, shape, shape_len * sizeof(size_t));

    a->len = n;
    if (ownership == GBF_EASY_TAKE) {
        a->data = (uint8_t*)data;
    } else {
        a->data = (uint8_t*)malloc(n);
        if (!a->data) { free(a->shape); free(a); return make_entry_err(name, err, "oom"); }
        memcpy(a->data, data, n);
    }

    gbf_value_t* v = make_value_logical(a);
    if (!v) {
        if (a->data) free(a->data);
        if (a->shape) free(a->shape);
        free(a);
        return make_entry_err(name, err, "failed to create logical value");
    }

    return (gbf_easy_entry_t){ name, v };
}

gbf_easy_entry_t gbf_easy_string_nd(
    const char* name,
    char* const* utf8_or_null,
    size_t n,
    const size_t* shape, size_t shape_len,
    gbf_easy_ownership_t ownership,
    gbf_error_t* err)
{
    (void)ownership;
    if (!name || !shape || shape_len == 0) {
        return make_entry_err(name, err, "invalid string args");
    }

    gbf_string_array_t* a = (gbf_string_array_t*)calloc(1, sizeof(gbf_string_array_t));
    if (!a) return make_entry_err(name, err, "oom");

    a->shape_len = shape_len;
    a->shape = (size_t*)malloc(shape_len * sizeof(size_t));
    if (!a->shape) { free(a); return make_entry_err(name, err, "oom"); }
    memcpy(a->shape, shape, shape_len * sizeof(size_t));

    a->len = n;
    a->data = (char**)calloc(n, sizeof(char*));
    if (!a->data) { free(a->shape); free(a); return make_entry_err(name, err, "oom"); }

    for (size_t i = 0; i < n; i++) {
        if (!utf8_or_null || !utf8_or_null[i]) {
            a->data[i] = NULL;
        } else {
            a->data[i] = strdup(utf8_or_null[i]);
            if (!a->data[i]) {
                for (size_t j = 0; j < i; j++) free(a->data[j]);
                free(a->data);
                free(a->shape);
                free(a);
                return make_entry_err(name, err, "oom");
            }
        }
    }

    gbf_value_t* v = make_value_string(a);
    if (!v) {
        if (a->data) {
            for (size_t i = 0; i < a->len; i++) free(a->data[i]);
            free(a->data);
        }
        if (a->shape) free(a->shape);
        free(a);
        return make_entry_err(name, err, "failed to create string value");
    }

    return (gbf_easy_entry_t){ name, v };
}

gbf_easy_entry_t gbf_easy_char_utf16_nd(
    const char* name,
    const uint16_t* units,
    size_t n_units,
    const size_t* shape, size_t shape_len,
    gbf_easy_ownership_t ownership,
    gbf_error_t* err)
{
    if (!name || !shape || shape_len == 0) {
        return make_entry_err(name, err, "invalid char args");
    }

    gbf_char_array_t* a = (gbf_char_array_t*)calloc(1, sizeof(gbf_char_array_t));
    if (!a) return make_entry_err(name, err, "oom");

    a->shape_len = shape_len;
    a->shape = (size_t*)malloc(shape_len * sizeof(size_t));
    if (!a->shape) { free(a); return make_entry_err(name, err, "oom"); }
    memcpy(a->shape, shape, shape_len * sizeof(size_t));

    a->len = n_units;
    if (ownership == GBF_EASY_TAKE) {
        a->data = (uint16_t*)units;
    } else {
        a->data = (uint16_t*)malloc(n_units * sizeof(uint16_t));
        if (!a->data) { free(a->shape); free(a); return make_entry_err(name, err, "oom"); }
        if (units && n_units) memcpy(a->data, units, n_units * sizeof(uint16_t));
    }

    gbf_value_t* v = make_value_char(a);
    if (!v) {
        if (a->data) free(a->data);
        if (a->shape) free(a->shape);
        free(a);
        return make_entry_err(name, err, "failed to create char value");
    }

    return (gbf_easy_entry_t){ name, v };
}

gbf_easy_entry_t gbf_easy_opaque_bytes_nd(
    const char* name,
    const char* kind,
    const char* class_name,
    const size_t* shape, size_t shape_len,
    int complex,
    const char* encoding,
    const void* bytes, size_t bytes_len,
    gbf_easy_ownership_t ownership,
    gbf_error_t* err)
{
    (void)ownership;
    if (!name || !bytes) return make_entry_err(name, err, "invalid opaque args");

    gbf_opaque_value_t* o = (gbf_opaque_value_t*)calloc(1, sizeof(gbf_opaque_value_t));
    if (!o) return make_entry_err(name, err, "oom");

    o->kind = kind ? strdup(kind) : NULL;
    o->class_name = class_name ? strdup(class_name) : NULL;
    o->encoding = encoding ? strdup(encoding) : NULL;
    o->complex = complex ? 1 : 0;

    if (shape && shape_len) {
        o->shape_len = shape_len;
        o->shape = (size_t*)malloc(shape_len * sizeof(size_t));
        if (!o->shape) { opaque_tmp_free(o); return make_entry_err(name, err, "oom"); }
        memcpy(o->shape, shape, shape_len * sizeof(size_t));
    }

    o->bytes = (uint8_t*)malloc(bytes_len);
    if (!o->bytes) {
        if (o->kind) free(o->kind);
        if (o->class_name) free(o->class_name);
        if (o->encoding) free(o->encoding);
        if (o->shape) free(o->shape);
        free(o);
        return make_entry_err(name, err, "oom");
    }
    memcpy(o->bytes, bytes, bytes_len);
    o->bytes_len = bytes_len;

    gbf_value_t* v = (gbf_value_t*)calloc(1, sizeof(gbf_value_t));
    if (!v) {
        if (o->kind) free(o->kind);
        if (o->class_name) free(o->class_name);
        if (o->encoding) free(o->encoding);
        if (o->shape) free(o->shape);
        if (o->bytes) free(o->bytes);
        free(o);
        return make_entry_err(name, err, "failed to create opaque value");
    }
    v->kind = GBF_VALUE_OPAQUE;
    v->as.opaque = *o;
    free(o);
    return (gbf_easy_entry_t){ name, v };
}

gbf_easy_entry_t gbf_easy_empty_struct_leaf(const char* name) {
    gbf_value_t* v = gbf_value_new_empty_struct_leaf();
    return (gbf_easy_entry_t){ name, v };
}

// ---------------------------
// Navigation + typed views
// ---------------------------

static const gbf_value_t* gbf_easy_struct_get(const gbf_struct_t* st, const char* key) {
    if (!st || !key) return NULL;
    for (size_t i = 0; i < st->len; i++) {
        if (st->entries[i].key && strcmp(st->entries[i].key, key) == 0) {
            return st->entries[i].value;
        }
    }
    return NULL;
}

static gbf_value_t* gbf_easy_struct_get_mut(gbf_struct_t* st, const char* key) {
    if (!st || !key) return NULL;
    for (size_t i = 0; i < st->len; i++) {
        if (st->entries[i].key && strcmp(st->entries[i].key, key) == 0) {
            return st->entries[i].value;
        }
    }
    return NULL;
}

const gbf_value_t* gbf_easy_get(const gbf_value_t* root, const char* dot_path) {
    if (!root) return NULL;
    if (!dot_path || dot_path[0] == '\0') return root;

    const gbf_value_t* cur = root;
    const char* p = dot_path;

    while (*p) {
        if (cur->kind != GBF_VALUE_STRUCT) return NULL;

        const char* seg_start = p;
        const char* seg_end = strchr(p, '.');
        size_t seg_len = seg_end ? (size_t)(seg_end - seg_start) : strlen(seg_start);

        char key[256];
        if (seg_len >= sizeof(key)) return NULL;
        memcpy(key, seg_start, seg_len);
        key[seg_len] = '\0';

        const gbf_struct_t* st = &cur->as.s;
        const gbf_value_t* next = gbf_easy_struct_get(st, key);
        if (!next) return NULL;

        cur = next;
        if (!seg_end) break;
        p = seg_end + 1;
    }

    return cur;
}

int gbf_easy_as_numeric(const gbf_value_t* v, const gbf_numeric_array_t** out_num) {
    if (!v || v->kind != GBF_VALUE_NUMERIC) return 0;
    if (out_num) *out_num = &v->as.num;
    return 1;
}

int gbf_easy_as_logical(const gbf_value_t* v, const gbf_logical_array_t** out_logical) {
    if (!v || v->kind != GBF_VALUE_LOGICAL) return 0;
    if (out_logical) *out_logical = &v->as.logical;
    return 1;
}

int gbf_easy_as_string(const gbf_value_t* v, const gbf_string_array_t** out_str) {
    if (!v || v->kind != GBF_VALUE_STRING) return 0;
    if (out_str) *out_str = &v->as.str;
    return 1;
}

int gbf_easy_as_char(const gbf_value_t* v, const gbf_char_array_t** out_chr) {
    if (!v || v->kind != GBF_VALUE_CHAR) return 0;
    if (out_chr) *out_chr = &v->as.chr;
    return 1;
}

// ---------------------------
// Writer: build root struct from dot-path and write
// ---------------------------

static int struct_insert_by_path(gbf_value_t* root_struct_value, const char* path, gbf_value_t* leaf, gbf_error_t* err) {
    if (!root_struct_value || root_struct_value->kind != GBF_VALUE_STRUCT || !path || !leaf) {
        gbf_easy_set_err(err, "invalid insert args");
        return 0;
    }

    gbf_value_t* cur_v = root_struct_value;
    const char* p = path;

    while (1) {
        const char* seg_end = strchr(p, '.');
        size_t seg_len = seg_end ? (size_t)(seg_end - p) : strlen(p);

        char key[256];
        if (seg_len == 0 || seg_len >= sizeof(key)) {
            gbf_easy_set_err(err, "invalid path segment");
            return 0;
        }
        memcpy(key, p, seg_len);
        key[seg_len] = '\0';

        if (!seg_end) {
            // leaf
            if (!gbf_struct_set(cur_v, key, leaf, err)) {
                gbf_easy_set_err(err, "failed to set leaf '%s'", key);
                return 0;
            }
            return 1;
        }

        // ensure struct child exists
        gbf_struct_t* cur_st = &cur_v->as.s;
        gbf_value_t* child = gbf_easy_struct_get_mut(cur_st, key);
        if (!child) {
            gbf_value_t* stv = gbf_value_new_struct();
            if (!stv) {
                gbf_easy_set_err(err, "oom");
                return 0;
            }
            if (!gbf_struct_set(cur_v, key, stv, err)) {
                gbf_value_free(stv);
                gbf_easy_set_err(err, "failed to create struct '%s'", key);
                return 0;
            }
            child = stv;
        }

        if (child->kind != GBF_VALUE_STRUCT) {
            gbf_easy_set_err(err, "path '%s' hits non-struct", key);
            return 0;
        }

        cur_v = child;
        p = seg_end + 1;
    }
}

int gbf_easy_write_file(const char* path, gbf_write_options_t opt, gbf_error_t* err, ...) {
    if (!path) {
        gbf_easy_set_err(err, "path is NULL");
        return 0;
    }

    gbf_value_t* root = gbf_value_new_struct();
    if (!root) {
        gbf_easy_set_err(err, "oom");
        return 0;
    }

    int ok = 1;

    va_list ap;
    va_start(ap, err);

    for (;;) {
        gbf_easy_entry_t e = va_arg(ap, gbf_easy_entry_t);
        if (e.name == NULL) break;

        if (!e.value) {
            ok = 0;
            if (err && !err->message) gbf_easy_set_err(err, "entry '%s' has NULL value", e.name);
            continue;
        }

        if (!struct_insert_by_path(root, e.name, e.value, err)) {
            // insert failed: free leaf (not owned by struct)
            gbf_value_free(e.value);
            ok = 0;
            continue;
        }
        // now owned by root struct
    }

    va_end(ap);

    if (!ok) {
        gbf_value_free(root);
        return 0;
    }

    // write file
    if (!gbf_write_file(path, root, opt, err)) {
        gbf_value_free(root);
        return 0;
    }

    gbf_value_free(root);
    return 1;
}

// ---------------------------
// Reader wrappers
// ---------------------------

int gbf_easy_read_file(const char* path, gbf_read_options_t opt, gbf_value_t** out_root, gbf_header_t** out_header, gbf_error_t* err) {
    if (!out_root) {
        gbf_easy_set_err(err, "out_root is NULL");
        return 0;
    }
    return gbf_read_file(path, opt, out_root, out_header, err);
}

int gbf_easy_read_var(const char* path, const char* var, gbf_read_options_t opt, gbf_value_t** out_value, gbf_error_t* err) {
    if (!out_value) {
        gbf_easy_set_err(err, "out_value is NULL");
        return 0;
    }
    return gbf_read_var(path, var, opt, out_value, err);
}
