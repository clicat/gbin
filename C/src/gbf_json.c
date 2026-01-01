
#include "gbf_internal.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Small, dependency-free JSON parser sufficient for GBF headers.
 *
 * Supported:
 *  - objects, arrays
 *  - strings (UTF-8; common escapes; \uXXXX is supported)
 *  - numbers (raw token kept; int/float)
 *  - booleans, null
 *
 * This is intentionally minimal.
 */

struct gbf_json {
    gbf_json_type_t type;
    union {
        int bool_val;
        gbf_json_number_t num;
        char* str;
        struct {
            gbf_json_t** items;
            size_t len;
            size_t cap;
        } arr;
        struct {
            char** keys;
            gbf_json_t** vals;
            size_t len;
            size_t cap;
        } obj;
    } u;
};

static void json_set_err(char** out_err, const char* fmt, ...) {
    if (!out_err) return;
    if (*out_err) { free(*out_err); *out_err = NULL; }

    if (!fmt) return;

    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);

    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) { va_end(ap2); return; }

    char* s = (char*)malloc((size_t)n + 1);
    if (!s) { va_end(ap2); return; }

    vsnprintf(s, (size_t)n + 1, fmt, ap2);
    va_end(ap2);

    *out_err = s;
}

static void* xrealloc(void* p, size_t n) {
    return gbf_xrealloc(p, n);
}

static void skip_ws(const char** p, const char* end) {
    while (*p < end && isspace((unsigned char)**p)) (*p)++;
}

static int is_hex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return (int)(c - '0');
    if (c >= 'a' && c <= 'f') return 10 + (int)(c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (int)(c - 'A');
    return 0;
}

static int utf8_append_codepoint(gbf_strbuf_t* sb, uint32_t cp) {
    if (cp <= 0x7F) {
        return gbf_sb_append_byte(sb, (char)cp);
    } else if (cp <= 0x7FF) {
        if (!gbf_sb_reserve(sb, 2)) return 0;
        sb->data[sb->len++] = (char)(0xC0 | ((cp >> 6) & 0x1F));
        sb->data[sb->len++] = (char)(0x80 | (cp & 0x3F));
        return 1;
    } else if (cp <= 0xFFFF) {
        if (!gbf_sb_reserve(sb, 3)) return 0;
        sb->data[sb->len++] = (char)(0xE0 | ((cp >> 12) & 0x0F));
        sb->data[sb->len++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        sb->data[sb->len++] = (char)(0x80 | (cp & 0x3F));
        return 1;
    } else {
        if (!gbf_sb_reserve(sb, 4)) return 0;
        sb->data[sb->len++] = (char)(0xF0 | ((cp >> 18) & 0x07));
        sb->data[sb->len++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        sb->data[sb->len++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        sb->data[sb->len++] = (char)(0x80 | (cp & 0x3F));
        return 1;
    }
}

static gbf_json_t* json_new(gbf_json_type_t t) {
    gbf_json_t* j = (gbf_json_t*)gbf_xcalloc(1, sizeof(gbf_json_t));
    j->type = t;
    return j;
}

static void json_obj_push(gbf_json_t* obj, char* key, gbf_json_t* val) {
    if (obj->u.obj.len == obj->u.obj.cap) {
        size_t nc = obj->u.obj.cap ? obj->u.obj.cap * 2 : 8;
        obj->u.obj.keys = (char**)xrealloc(obj->u.obj.keys, nc * sizeof(char*));
        obj->u.obj.vals = (gbf_json_t**)xrealloc(obj->u.obj.vals, nc * sizeof(gbf_json_t*));
        obj->u.obj.cap = nc;
    }
    obj->u.obj.keys[obj->u.obj.len] = key;
    obj->u.obj.vals[obj->u.obj.len] = val;
    obj->u.obj.len++;
}

static void json_arr_push(gbf_json_t* arr, gbf_json_t* val) {
    if (arr->u.arr.len == arr->u.arr.cap) {
        size_t nc = arr->u.arr.cap ? arr->u.arr.cap * 2 : 8;
        arr->u.arr.items = (gbf_json_t**)xrealloc(arr->u.arr.items, nc * sizeof(gbf_json_t*));
        arr->u.arr.cap = nc;
    }
    arr->u.arr.items[arr->u.arr.len++] = val;
}

static char* parse_string(const char** p, const char* end, char** out_err) {
    if (*p >= end || **p != '"') {
        json_set_err(out_err, "json: expected string");
        return NULL;
    }
    (*p)++;

    gbf_strbuf_t sb;
    gbf_sb_init(&sb);

    while (*p < end) {
        char c = **p;
        (*p)++;

        if (c == '"') {
            gbf_sb_append_byte(&sb, 0);
            char* s = gbf_strdup(sb.data ? sb.data : "");
            gbf_sb_free(&sb);
            return s;
        }

        if ((unsigned char)c < 0x20) {
            gbf_sb_free(&sb);
            json_set_err(out_err, "json: control character in string");
            return NULL;
        }

        if (c != '\\') {
            if (!gbf_sb_append_byte(&sb, c)) { gbf_sb_free(&sb); return NULL; }
            continue;
        }

        if (*p >= end) {
            gbf_sb_free(&sb);
            json_set_err(out_err, "json: bad escape");
            return NULL;
        }

        char e = **p;
        (*p)++;

        switch (e) {
            case '"':  gbf_sb_append_byte(&sb, '"'); break;
            case '\\': gbf_sb_append_byte(&sb, '\\'); break;
            case '/':  gbf_sb_append_byte(&sb, '/'); break;
            case 'b':  gbf_sb_append_byte(&sb, '\b'); break;
            case 'f':  gbf_sb_append_byte(&sb, '\f'); break;
            case 'n':  gbf_sb_append_byte(&sb, '\n'); break;
            case 'r':  gbf_sb_append_byte(&sb, '\r'); break;
            case 't':  gbf_sb_append_byte(&sb, '\t'); break;
            case 'u': {
                if (end - *p < 4) { gbf_sb_free(&sb); json_set_err(out_err, "json: short \\u escape"); return NULL; }
                uint32_t cp = 0;
                for (int i = 0; i < 4; i++) {
                    char h = (*p)[i];
                    if (!is_hex(h)) { gbf_sb_free(&sb); json_set_err(out_err, "json: invalid \\u escape"); return NULL; }
                    cp = (cp << 4) | (uint32_t)hex_val(h);
                }
                *p += 4;

                /* surrogate handling (best-effort) */
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    if (end - *p >= 6 && (*p)[0] == '\\' && (*p)[1] == 'u') {
                        const char* q = *p + 2;
                        uint32_t lo = 0;
                        int ok = 1;
                        for (int i = 0; i < 4; i++) {
                            char h = q[i];
                            if (!is_hex(h)) { ok = 0; break; }
                            lo = (lo << 4) | (uint32_t)hex_val(h);
                        }
                        if (ok && lo >= 0xDC00 && lo <= 0xDFFF) {
                            *p += 6;
                            uint32_t high = cp - 0xD800;
                            uint32_t low = lo - 0xDC00;
                            cp = 0x10000 + ((high << 10) | low);
                        }
                    }
                }

                if (!utf8_append_codepoint(&sb, cp)) { gbf_sb_free(&sb); return NULL; }
            } break;
            default:
                gbf_sb_free(&sb);
                json_set_err(out_err, "json: unsupported escape");
                return NULL;
        }
    }

    gbf_sb_free(&sb);
    json_set_err(out_err, "json: unterminated string");
    return NULL;
}

static gbf_json_t* parse_number(const char** p, const char* end, char** out_err) {
    const char* start = *p;
    const char* q = *p;

    if (q < end && (*q == '-' || *q == '+')) q++;

    int has_dot = 0;
    int has_exp = 0;

    while (q < end) {
        char c = *q;
        if (c >= '0' && c <= '9') { q++; continue; }
        if (c == '.') { has_dot = 1; q++; continue; }
        if (c == 'e' || c == 'E') { has_exp = 1; q++; if (q < end && (*q == '-' || *q == '+')) q++; continue; }
        break;
    }

    if (q == start) {
        json_set_err(out_err, "json: expected number");
        return NULL;
    }

    size_t n = (size_t)(q - start);
    char* raw = (char*)gbf_xmalloc(n + 1);
    memcpy(raw, start, n);
    raw[n] = 0;

    errno = 0;
    char* e2 = NULL;
    double v = strtod(raw, &e2);
    if (errno != 0 || e2 == raw) {
        free(raw);
        json_set_err(out_err, "json: bad number");
        return NULL;
    }

    gbf_json_t* j = json_new(GBF_JSON_NUMBER);
    j->u.num.value = v;
    j->u.num.raw = raw;
    j->u.num.is_int = (!has_dot && !has_exp);

    *p = q;
    return j;
}

static gbf_json_t* parse_value(const char** p, const char* end, char** out_err);

static gbf_json_t* parse_array(const char** p, const char* end, char** out_err) {
    if (*p >= end || **p != '[') {
        json_set_err(out_err, "json: expected array");
        return NULL;
    }
    (*p)++;

    gbf_json_t* arr = json_new(GBF_JSON_ARRAY);

    skip_ws(p, end);
    if (*p < end && **p == ']') {
        (*p)++;
        return arr;
    }

    for (;;) {
        skip_ws(p, end);
        gbf_json_t* v = parse_value(p, end, out_err);
        if (!v) { gbf_json_free(arr); return NULL; }
        json_arr_push(arr, v);

        skip_ws(p, end);
        if (*p >= end) { gbf_json_free(arr); json_set_err(out_err, "json: unterminated array"); return NULL; }

        char c = **p;
        (*p)++;
        if (c == ']') break;
        if (c != ',') { gbf_json_free(arr); json_set_err(out_err, "json: expected ',' or ']'"); return NULL; }
    }

    return arr;
}

static gbf_json_t* parse_object(const char** p, const char* end, char** out_err) {
    if (*p >= end || **p != '{') {
        json_set_err(out_err, "json: expected object");
        return NULL;
    }
    (*p)++;

    gbf_json_t* obj = json_new(GBF_JSON_OBJECT);

    skip_ws(p, end);
    if (*p < end && **p == '}') {
        (*p)++;
        return obj;
    }

    for (;;) {
        skip_ws(p, end);
        char* key = parse_string(p, end, out_err);
        if (!key) { gbf_json_free(obj); return NULL; }

        skip_ws(p, end);
        if (*p >= end || **p != ':') {
            free(key);
            gbf_json_free(obj);
            json_set_err(out_err, "json: expected ':'");
            return NULL;
        }
        (*p)++;

        skip_ws(p, end);
        gbf_json_t* val = parse_value(p, end, out_err);
        if (!val) { free(key); gbf_json_free(obj); return NULL; }
        json_obj_push(obj, key, val);

        skip_ws(p, end);
        if (*p >= end) { gbf_json_free(obj); json_set_err(out_err, "json: unterminated object"); return NULL; }

        char c = **p;
        (*p)++;
        if (c == '}') break;
        if (c != ',') { gbf_json_free(obj); json_set_err(out_err, "json: expected ',' or '}'"); return NULL; }
    }

    return obj;
}

static int match_lit(const char** p, const char* end, const char* lit) {
    size_t n = strlen(lit);
    if ((size_t)(end - *p) < n) return 0;
    if (memcmp(*p, lit, n) != 0) return 0;
    *p += n;
    return 1;
}

static gbf_json_t* parse_value(const char** p, const char* end, char** out_err) {
    skip_ws(p, end);
    if (*p >= end) { json_set_err(out_err, "json: unexpected end"); return NULL; }

    char c = **p;
    if (c == '"') {
        char* s = parse_string(p, end, out_err);
        if (!s) return NULL;
        gbf_json_t* j = json_new(GBF_JSON_STRING);
        j->u.str = s;
        return j;
    }
    if (c == '{') return parse_object(p, end, out_err);
    if (c == '[') return parse_array(p, end, out_err);

    if (c == 't') {
        if (!match_lit(p, end, "true")) { json_set_err(out_err, "json: bad literal"); return NULL; }
        gbf_json_t* j = json_new(GBF_JSON_BOOL);
        j->u.bool_val = 1;
        return j;
    }
    if (c == 'f') {
        if (!match_lit(p, end, "false")) { json_set_err(out_err, "json: bad literal"); return NULL; }
        gbf_json_t* j = json_new(GBF_JSON_BOOL);
        j->u.bool_val = 0;
        return j;
    }
    if (c == 'n') {
        if (!match_lit(p, end, "null")) { json_set_err(out_err, "json: bad literal"); return NULL; }
        return json_new(GBF_JSON_NULL);
    }

    return parse_number(p, end, out_err);
}

gbf_json_t* gbf_json_parse(const char* s, size_t n, char** out_err_string) {
    if (out_err_string) *out_err_string = NULL;

    if (!s) {
        json_set_err(out_err_string, "json: null input");
        return NULL;
    }

    const char* p = s;
    const char* end = s + n;

    gbf_json_t* v = parse_value(&p, end, out_err_string);
    if (!v) return NULL;

    skip_ws(&p, end);
    if (p != end) {
        gbf_json_free(v);
        json_set_err(out_err_string, "json: trailing characters");
        return NULL;
    }
    return v;
}

void gbf_json_free(gbf_json_t* j) {
    if (!j) return;
    switch (j->type) {
        case GBF_JSON_STRING:
            free(j->u.str);
            break;
        case GBF_JSON_NUMBER:
            free(j->u.num.raw);
            break;
        case GBF_JSON_ARRAY:
            for (size_t i = 0; i < j->u.arr.len; i++) gbf_json_free(j->u.arr.items[i]);
            free(j->u.arr.items);
            break;
        case GBF_JSON_OBJECT:
            for (size_t i = 0; i < j->u.obj.len; i++) {
                free(j->u.obj.keys[i]);
                gbf_json_free(j->u.obj.vals[i]);
            }
            free(j->u.obj.keys);
            free(j->u.obj.vals);
            break;
        default:
            break;
    }
    free(j);
}



gbf_json_type_t gbf_json_type(const gbf_json_t* j) {
    return j ? j->type : GBF_JSON_NULL;
}
const gbf_json_t* gbf_json_obj_get(const gbf_json_t* obj, const char* key) {
    if (!obj || obj->type != GBF_JSON_OBJECT || !key) return NULL;
    for (size_t i = 0; i < obj->u.obj.len; i++) {
        if (strcmp(obj->u.obj.keys[i], key) == 0) return obj->u.obj.vals[i];
    }
    return NULL;
}

size_t gbf_json_array_size(const gbf_json_t* arr) {
    if (!arr || arr->type != GBF_JSON_ARRAY) return 0;
    return arr->u.arr.len;
}

const gbf_json_t* gbf_json_array_get(const gbf_json_t* arr, size_t idx) {
    if (!arr || arr->type != GBF_JSON_ARRAY) return NULL;
    if (idx >= arr->u.arr.len) return NULL;
    return arr->u.arr.items[idx];
}

const char* gbf_json_as_cstr(const gbf_json_t* j) {
    if (!j || j->type != GBF_JSON_STRING) return NULL;
    return j->u.str;
}

double gbf_json_as_f64(const gbf_json_t* j, double default_) {
    if (!j) return default_;
    if (j->type == GBF_JSON_NUMBER) return j->u.num.value;
    if (j->type == GBF_JSON_BOOL) return j->u.bool_val ? 1.0 : 0.0;
    return default_;
}

uint64_t gbf_json_as_u64(const gbf_json_t* j, uint64_t default_) {
    if (!j) return default_;
    if (j->type == GBF_JSON_NUMBER) {
        if (j->u.num.is_int && j->u.num.raw) {
            errno = 0;
            unsigned long long x = strtoull(j->u.num.raw, NULL, 10);
            if (errno == 0) return (uint64_t)x;
        }
        double v = j->u.num.value;
        if (v <= 0.0) return 0;
        return (uint64_t)llround(v);
    }
    if (j->type == GBF_JSON_STRING && j->u.str) {
        errno = 0;
        unsigned long long x = strtoull(j->u.str, NULL, 10);
        if (errno == 0) return (uint64_t)x;
        if (strlen(j->u.str) >= 2 && j->u.str[0] == '0' && (j->u.str[1] == 'x' || j->u.str[1] == 'X')) {
            errno = 0;
            x = strtoull(j->u.str + 2, NULL, 16);
            if (errno == 0) return (uint64_t)x;
        }
    }
    return default_;
}

uint32_t gbf_json_as_u32(const gbf_json_t* j, uint32_t default_) {
    return (uint32_t)gbf_json_as_u64(j, (uint64_t)default_);
}

int gbf_json_as_bool(const gbf_json_t* j, int default_) {
    if (!j) return default_;
    if (j->type == GBF_JSON_BOOL) return j->u.bool_val ? 1 : 0;
    if (j->type == GBF_JSON_NUMBER) return (j->u.num.value != 0.0) ? 1 : 0;
    return default_;
}

/* ===== header CRC helpers ===== */

int gbf_zero_out_header_crc32_field(char* json, size_t len) {
    if (!json || len == 0) return 0;

    const char* needle = "\"header_crc32_hex\"";
    const size_t needle_len = strlen(needle);

    for (size_t i = 0; i + needle_len < len; i++) {
        if (memcmp(json + i, needle, needle_len) == 0) {
            size_t j = i + needle_len;
            while (j < len && json[j] != ':') j++;
            if (j >= len) return 0;
            j++;
            while (j < len && (json[j] == ' ' || json[j] == '\t' || json[j] == '\r' || json[j] == '\n')) j++;
            if (j >= len || json[j] != '"') return 0;
            j++;
            if (j + 8 > len) return 0;
            for (size_t k = 0; k < 8; k++) json[j + k] = '0';
            return 1;
        }
    }
    return 0;
}

uint32_t gbf_extract_header_crc32_hex_u32(const char* json, size_t len) {
    if (!json || len == 0) return 0;

    const char* needle = "\"header_crc32_hex\"";
    const size_t needle_len = strlen(needle);

    for (size_t i = 0; i + needle_len < len; i++) {
        if (memcmp(json + i, needle, needle_len) == 0) {
            size_t j = i + needle_len;
            while (j < len && json[j] != ':') j++;
            if (j >= len) return 0;
            j++;
            while (j < len && (json[j] == ' ' || json[j] == '\t' || json[j] == '\r' || json[j] == '\n')) j++;
            if (j >= len || json[j] != '"') return 0;
            j++;
            if (j + 8 > len) return 0;

            char tmp[9];
            memcpy(tmp, json + j, 8);
            tmp[8] = 0;

            errno = 0;
            unsigned long x = strtoul(tmp, NULL, 16);
            if (errno != 0) return 0;
            return (uint32_t)x;
        }
    }
    return 0;
}
