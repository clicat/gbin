
#include "gbf_internal.h"
#include "gbin/gbf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void gbf_sb_init(gbf_strbuf_t* sb) {
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
}

void gbf_sb_free(gbf_strbuf_t* sb) {
    if (!sb) return;
    free(sb->data);
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
}

int gbf_sb_reserve(gbf_strbuf_t* sb, size_t extra) {
    if (!sb) return 0;
    size_t need = sb->len + extra;
    if (need <= sb->cap) return 1;

    size_t cap = sb->cap ? sb->cap : 64;
    while (cap < need) cap *= 2;

    char* p = (char*)realloc(sb->data, cap);
    if (!p) return 0;
    sb->data = p;
    sb->cap = cap;
    return 1;
}

int gbf_sb_append_byte(gbf_strbuf_t* sb, char c) {
    if (!gbf_sb_reserve(sb, 1)) return 0;
    sb->data[sb->len++] = c;
    return 1;
}

int gbf_sb_append_mem(gbf_strbuf_t* sb, const void* p, size_t n) {
    if (n == 0) return 1;
    if (!gbf_sb_reserve(sb, n)) return 0;
    memcpy(sb->data + sb->len, p, n);
    sb->len += n;
    return 1;
}

int gbf_sb_append_str(gbf_strbuf_t* sb, const char* s) {
    if (!s) return 1;
    return gbf_sb_append_mem(sb, s, strlen(s));
}

int gbf_sb_append_fmt(gbf_strbuf_t* sb, const char* fmt, ...) {
    if (!sb || !fmt) return 0;

    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);

    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) { va_end(ap2); return 0; }

    if (!gbf_sb_reserve(sb, (size_t)n)) { va_end(ap2); return 0; }
    vsnprintf(sb->data + sb->len, (size_t)n + 1, fmt, ap2);
    va_end(ap2);

    sb->len += (size_t)n;
    return 1;
}

/* ===== memory ===== */

void* gbf_xmalloc(size_t n) {
    void* p = malloc(n ? n : 1);
    if (!p) {
        fprintf(stderr, "gbf: out of memory\n");
        abort();
    }
    return p;
}

void* gbf_xcalloc(size_t n, size_t sz) {
    void* p = calloc(n ? n : 1, sz ? sz : 1);
    if (!p) {
        fprintf(stderr, "gbf: out of memory\n");
        abort();
    }
    return p;
}

void* gbf_xrealloc(void* p, size_t n) {
    void* q = realloc(p, n ? n : 1);
    if (!q) {
        fprintf(stderr, "gbf: out of memory\n");
        abort();
    }
    return q;
}

char* gbf_strdup(const char* s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char* p = (char*)gbf_xmalloc(n + 1);
    memcpy(p, s, n + 1);
    return p;
}

/* ===== error ===== */

void gbf_free_error(gbf_error_t* err) {
    if (!err) return;
    free(err->message);
    err->message = NULL;
}

void gbf_set_err(gbf_error_t* err, const char* fmt, ...) {
    if (!err) return;
    gbf_free_error(err);

    if (!fmt) return;

    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);

    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) { va_end(ap2); return; }

    err->message = (char*)malloc((size_t)n + 1);
    if (!err->message) { va_end(ap2); return; }

    vsnprintf(err->message, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
}

/* ===== endian ===== */

uint32_t gbf_le_u32(const uint8_t b[4]) {
    return (uint32_t)b[0] |
           ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) |
           ((uint32_t)b[3] << 24);
}

uint64_t gbf_le_u64(const uint8_t b[8]) {
    return (uint64_t)b[0] |
           ((uint64_t)b[1] << 8) |
           ((uint64_t)b[2] << 16) |
           ((uint64_t)b[3] << 24) |
           ((uint64_t)b[4] << 32) |
           ((uint64_t)b[5] << 40) |
           ((uint64_t)b[6] << 48) |
           ((uint64_t)b[7] << 56);
}

int32_t gbf_le_i32(const uint8_t b[4]) {
    return (int32_t)gbf_le_u32(b);
}

int64_t gbf_le_i64(const uint8_t b[8]) {
    return (int64_t)gbf_le_u64(b);
}

void gbf_store_le_u32(uint8_t b[4], uint32_t v) {
    b[0] = (uint8_t)(v & 0xFF);
    b[1] = (uint8_t)((v >> 8) & 0xFF);
    b[2] = (uint8_t)((v >> 16) & 0xFF);
    b[3] = (uint8_t)((v >> 24) & 0xFF);
}

void gbf_store_le_u64(uint8_t b[8], uint64_t v) {
    b[0] = (uint8_t)(v & 0xFF);
    b[1] = (uint8_t)((v >> 8) & 0xFF);
    b[2] = (uint8_t)((v >> 16) & 0xFF);
    b[3] = (uint8_t)((v >> 24) & 0xFF);
    b[4] = (uint8_t)((v >> 32) & 0xFF);
    b[5] = (uint8_t)((v >> 40) & 0xFF);
    b[6] = (uint8_t)((v >> 48) & 0xFF);
    b[7] = (uint8_t)((v >> 56) & 0xFF);
}

void gbf_store_le_i32(uint8_t b[4], int32_t v) {
    gbf_store_le_u32(b, (uint32_t)v);
}

void gbf_store_le_i64(uint8_t b[8], int64_t v) {
    gbf_store_le_u64(b, (uint64_t)v);
}

/* ===== safe multiplication ===== */

int gbf_checked_mul_size(size_t a, size_t b, size_t* out) {
    if (!out) return 0;
    if (a == 0 || b == 0) {
        *out = 0;
        return 1;
    }
    if (a > (SIZE_MAX / b)) return 0;
    *out = a * b;
    return 1;
}
