
#include "gbin/gbf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* minimal assert */
#define ASSERT_TRUE(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "ASSERT_TRUE failed at %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        exit(1); \
    } \
} while(0)

static void assert_err_ok(const gbf_error_t* err) {
    if (err && err->message) {
        fprintf(stderr, "unexpected error: %s\n", err->message);
        exit(1);
    }
}

static void remove_if_exists(const char* path) {
    FILE* f = fopen(path, "rb");
    if (f) { fclose(f); remove(path); }
}

static void build_sample(gbf_value_t** out_root) {
    gbf_error_t err = {0};

    gbf_value_t* root = gbf_value_new_struct();

    /* numeric 2x3 matrix, row-major source */
    double mat[6] = { 1,2,3, 4,5,6 };
    gbf_value_t* weights = gbf_value_new_f64_matrix_rowmajor(mat, 2, 3, &err);
    assert_err_ok(&err);
    ASSERT_TRUE(weights != NULL);
    ASSERT_TRUE(gbf_struct_set(root, "weights", weights, &err));
    assert_err_ok(&err);

    /* logical 1x5 */
    uint8_t flags[5] = {1,0,1,1,0};
    size_t lshape[2] = {1,5};
    gbf_value_t* logical = gbf_value_new_logical_from_u8(flags, 5, lshape, 2, &err);
    assert_err_ok(&err);
    ASSERT_TRUE(gbf_struct_set(root, "flags", logical, &err));
    assert_err_ok(&err);

    /* string 1x3 */
    char* strs[3] = { "alpha", NULL, "gamma" };
    size_t sshape[2] = {1,3};
    gbf_value_t* sarr = gbf_value_new_string_array(strs, 3, sshape, 2, &err);
    assert_err_ok(&err);
    ASSERT_TRUE(gbf_struct_set(root, "labels", sarr, &err));
    assert_err_ok(&err);

    /* char 1x4 (UTF-16 code units) */
    uint16_t chars[4] = { 'A', 'B', 'C', 'D' };
    size_t cshape[2] = {1,4};
    gbf_value_t* carr = gbf_value_new_char_from_utf16(chars, 4, cshape, 2, &err);
    assert_err_ok(&err);
    ASSERT_TRUE(gbf_struct_set(root, "title", carr, &err));
    assert_err_ok(&err);

    /* nested struct */
    gbf_value_t* sub = gbf_value_new_struct();
    ASSERT_TRUE(gbf_struct_set(root, "sub", sub, &err));
    assert_err_ok(&err);

    double v1 = 42.0;
    uint64_t bits = 0;
    memcpy(&bits, &v1, 8);
    uint8_t real_le[8];
    for (int i = 0; i < 8; i++) real_le[i] = (uint8_t)((bits >> (8*i)) & 0xFF);
    size_t nshape[2] = {1,1};
    gbf_value_t* scalar = gbf_value_new_numeric_from_bytes(GBF_NUM_DOUBLE, nshape, 2, 0, real_le, 8, NULL, 0, &err);
    assert_err_ok(&err);
    ASSERT_TRUE(gbf_struct_set(sub, "scalar", scalar, &err));
    assert_err_ok(&err);

    /* empty struct leaf */
    gbf_value_t* empty = gbf_value_new_empty_struct_leaf();
    ASSERT_TRUE(gbf_struct_set(root, "empty", empty, &err));
    assert_err_ok(&err);

    *out_root = root;
}

static void test_roundtrip(void) {
    const char* path = "test_roundtrip.gbf";
    remove_if_exists(path);

    gbf_value_t* root = NULL;
    build_sample(&root);

    gbf_write_options_t wopt;
    wopt.compression = GBF_COMP_AUTO;
    wopt.include_crc32 = 1;
    wopt.zlib_level = -1;

    gbf_error_t err = {0};
    ASSERT_TRUE(gbf_write_file(path, root, wopt, &err));
    assert_err_ok(&err);

    gbf_value_free(root);

    gbf_read_options_t ropt = {1};

    /* read header */
    gbf_header_t* h = NULL;
    uint32_t hl = 0;
    char* raw = NULL;
    ASSERT_TRUE(gbf_read_header_only(path, ropt, &h, &hl, &raw, &err));
    assert_err_ok(&err);
    ASSERT_TRUE(h->fields_len >= 4);
    ASSERT_TRUE(hl > 10);
    gbf_header_free(h);
    free(raw);

    /* read full */
    gbf_value_t* read_root = NULL;
    ASSERT_TRUE(gbf_read_file(path, ropt, &read_root, NULL, &err));
    assert_err_ok(&err);

    ASSERT_TRUE(read_root && read_root->kind == GBF_VALUE_STRUCT);

    /* simple check: read var weights */
    gbf_value_t* weights = NULL;
    ASSERT_TRUE(gbf_read_var(path, "weights", ropt, &weights, &err));
    assert_err_ok(&err);
    ASSERT_TRUE(weights && weights->kind == GBF_VALUE_NUMERIC);
    ASSERT_TRUE(weights->as.num.shape_len == 2);
    ASSERT_TRUE(weights->as.num.shape[0] == 2 && weights->as.num.shape[1] == 3);
    ASSERT_TRUE(weights->as.num.real_len == 6 * 8);
    gbf_value_free(weights);

    /* read subtree */
    gbf_value_t* sub = NULL;
    ASSERT_TRUE(gbf_read_var(path, "sub", ropt, &sub, &err));
    assert_err_ok(&err);
    ASSERT_TRUE(sub && sub->kind == GBF_VALUE_STRUCT);
    gbf_value_free(sub);

    gbf_value_free(read_root);

    remove(path);
}

static void test_crc_detection(void) {
    const char* path = "test_crc.gbf";
    remove_if_exists(path);

    gbf_value_t* root = NULL;
    build_sample(&root);

    gbf_write_options_t wopt = { GBF_COMP_NEVER, 1, -1 };
    gbf_error_t err = {0};
    ASSERT_TRUE(gbf_write_file(path, root, wopt, &err));
    assert_err_ok(&err);
    gbf_value_free(root);

    /* read header to locate first field */
    gbf_read_options_t ropt = {1};
    gbf_header_t* h = NULL;
    uint32_t hl = 0;
    char* raw = NULL;
    ASSERT_TRUE(gbf_read_header_only(path, ropt, &h, &hl, &raw, &err));
    assert_err_ok(&err);
    free(raw);
    ASSERT_TRUE(h->fields_len > 0);

    uint64_t pos = h->payload_start + h->fields[0].offset;
    gbf_header_free(h);

    /* flip a byte in payload */
    FILE* f = fopen(path, "r+b");
    ASSERT_TRUE(f != NULL);
#if defined(_WIN32)
    fseek(f, (long)pos, SEEK_SET);
#else
    fseek(f, (long)pos, SEEK_SET);
#endif
    int c = fgetc(f);
    ASSERT_TRUE(c != EOF);
#if defined(_WIN32)
    fseek(f, (long)pos, SEEK_SET);
#else
    fseek(f, (long)pos, SEEK_SET);
#endif
    fputc((c ^ 0xFF) & 0xFF, f);
    fclose(f);

    /* validate read should fail */
    gbf_value_t* out = NULL;
    int ok = gbf_read_file(path, ropt, &out, NULL, &err);
    ASSERT_TRUE(ok == 0);
    ASSERT_TRUE(err.message != NULL);
    gbf_free_error(&err);

    remove(path);
}

int main(void) {
    test_roundtrip();
    test_crc_detection();
    printf("OK\n");
    return 0;
}
