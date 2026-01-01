// gbf_easy_demo.c
//
// Demo CLI for gbf_easy.h:
//
// - Allocates dynamic matrices/tensors and a UTF-16 char array
// - Writes them into a GBF file using gbf_easy_write_file()
// - Reads them back and prints previews
//
// Build (once wired in CMake):
//   ./build/gbf_easy_demo out.gbf
//
// Notes:
//   - We write arrays in ROW-MAJOR here (natural for C).
//   - gbf_easy_* helpers convert to GBF column-major as needed.

#include "gbin/gbf_easy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

static void die_err(const char* what, gbf_error_t* err) {
    fprintf(stderr, "%s: %s\n", what, err && err->message ? err->message : "(unknown)");
    if (err) gbf_free_error(err);
    exit(1);
}

static size_t numel(const size_t* shape, size_t shape_len) {
    size_t n = 1;
    for (size_t i = 0; i < shape_len; i++) n *= shape[i];
    return n;
}

/* Print a 2D preview from a flat array, assuming ROW-MAJOR storage. */
static void preview_f32_rowmajor(const float* a, size_t rows, size_t cols, size_t max_r, size_t max_c) {
    size_t r_show = rows < max_r ? rows : max_r;
    size_t c_show = cols < max_c ? cols : max_c;

    printf("preview top-left %zux%zu (row-major view):\n", r_show, c_show);
    for (size_t r = 0; r < r_show; r++) {
        printf("  ");
        for (size_t c = 0; c < c_show; c++) {
            printf("%8.3f ", a[r * cols + c]);
        }
        printf("\n");
    }
}

static void preview_f64_rowmajor(const double* a, size_t rows, size_t cols, size_t max_r, size_t max_c) {
    size_t r_show = rows < max_r ? rows : max_r;
    size_t c_show = cols < max_c ? cols : max_c;

    printf("preview top-left %zux%zu (row-major view):\n", r_show, c_show);
    for (size_t r = 0; r < r_show; r++) {
        printf("  ");
        for (size_t c = 0; c < c_show; c++) {
            printf("%10.4f ", a[r * cols + c]);
        }
        printf("\n");
    }
}

static void preview_i32_linear(const int32_t* a, size_t n, size_t max_n) {
    size_t show = n < max_n ? n : max_n;
    printf("preview first %zu:\n  ", show);
    for (size_t i = 0; i < show; i++) {
        printf("%" PRId32 " ", a[i]);
    }
    printf("\n");
}

/* Decode a UTF-16 (BMP only) into ASCII-ish for preview. */
static void preview_utf16_as_ascii(const uint16_t* u, size_t n_units) {
    printf("char preview (UTF-16 units -> ASCII-ish): \"");
    for (size_t i = 0; i < n_units; i++) {
        uint16_t cu = u[i];
        if (cu == 0) break;
        if (cu < 128) putchar((char)cu);
        else putchar('?');
    }
    printf("\"\n");
}

/* Extract numeric arrays back from GBF and show preview.
   We decode the stored little-endian bytes to host values for preview. */
static void readback_preview_numeric(const gbf_value_t* root, const char* path) {
    const gbf_value_t* v = gbf_easy_get(root, path);
    if (!v) {
        printf("readback: missing var '%s'\n", path);
        return;
    }

    const gbf_numeric_array_t* n = NULL;
    if (!gbf_easy_as_numeric(v, &n)) {
        printf("readback: '%s' is not numeric\n", path);
        return;
    }

    printf("readback '%s': numeric class=%s complex=%d shape_len=%zu [",
           path, gbf_easy_numeric_class_name(n->class_id), n->complex, n->shape_len);
    for (size_t i = 0; i < n->shape_len; i++) {
        printf("%zu", n->shape[i]);
        if (i + 1 < n->shape_len) printf(" x ");
    }
    printf("]\n");

    size_t esz = gbf_easy_numeric_elem_size(n->class_id);
    size_t N = 1;
    for (size_t i = 0; i < n->shape_len; i++) N *= n->shape[i];

    // We only "plot" a preview. For 2D float/double we print a 10x10 top-left
    // in MATLAB-ish column-major order transformed to row-major view for the user.
    // Here, we do a simpler preview: first min(50, N) elements by decoding
    // from stored little-endian bytes.
    size_t show = N < 50 ? N : 50;

    printf("preview first %zu elements (GBF stored column-major order):\n  ", show);
    for (size_t i = 0; i < show; i++) {
        const uint8_t* p = n->real_le + i * esz;
        if (n->real_len < (i + 1) * esz) break;

        if (n->class_id == GBF_NUM_SINGLE) {
            uint32_t u32 = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
            float f;
            memcpy(&f, &u32, sizeof(float));
            printf("%.3f ", f);
        } else if (n->class_id == GBF_NUM_DOUBLE) {
            uint64_t u64 =
                (uint64_t)p[0] |
                ((uint64_t)p[1] << 8) |
                ((uint64_t)p[2] << 16) |
                ((uint64_t)p[3] << 24) |
                ((uint64_t)p[4] << 32) |
                ((uint64_t)p[5] << 40) |
                ((uint64_t)p[6] << 48) |
                ((uint64_t)p[7] << 56);
            double d;
            memcpy(&d, &u64, sizeof(double));
            printf("%.4f ", d);
        } else if (n->class_id == GBF_NUM_INT32) {
            int32_t x =
                (int32_t)((uint32_t)p[0] |
                          ((uint32_t)p[1] << 8) |
                          ((uint32_t)p[2] << 16) |
                          ((uint32_t)p[3] << 24));
            printf("%" PRId32 " ", x);
        } else {
            // fallback: hex
            printf("0x");
            for (size_t k = 0; k < esz; k++) printf("%02X", p[k]);
            printf(" ");
        }
    }
    printf("\n");
}

static void readback_preview_char(const gbf_value_t* root, const char* path) {
    const gbf_value_t* v = gbf_easy_get(root, path);
    if (!v) {
        printf("readback: missing var '%s'\n", path);
        return;
    }

    const gbf_char_array_t* c = NULL;
    if (!gbf_easy_as_char(v, &c)) {
        printf("readback: '%s' is not char\n", path);
        return;
    }

    printf("readback '%s': char shape_len=%zu [", path, c->shape_len);
    for (size_t i = 0; i < c->shape_len; i++) {
        printf("%zu", c->shape[i]);
        if (i + 1 < c->shape_len) printf(" x ");
    }
    printf("] units=%zu\n", c->len);

    preview_utf16_as_ascii(c->data, c->len);
}

int main(int argc, char** argv) {
    const char* out_path = (argc >= 2) ? argv[1] : "gbf_easy_demo_out.gbf";

    gbf_error_t err = {0};

    // -------------------------------------------------------------------------
    // 1) Build dynamic data
    // -------------------------------------------------------------------------

    // (A) single matrix: 64 x 32
    size_t a_rows = 64, a_cols = 32;
    float* A = (float*)malloc(a_rows * a_cols * sizeof(float));
    if (!A) die_err("malloc A", &err);

    for (size_t r = 0; r < a_rows; r++) {
        for (size_t c = 0; c < a_cols; c++) {
            A[r * a_cols + c] = (float)(r * 1000.0 + c);
        }
    }

    // (B) double matrix: 16 x 10
    size_t b_rows = 16, b_cols = 10;
    double* B = (double*)malloc(b_rows * b_cols * sizeof(double));
    if (!B) die_err("malloc B", &err);

    for (size_t r = 0; r < b_rows; r++) {
        for (size_t c = 0; c < b_cols; c++) {
            B[r * b_cols + c] = (double)r + 0.01 * (double)c;
        }
    }

    // (C) int32 tensor: 3 x 3 x 4
    size_t tshape[3] = {3, 3, 4};
    size_t tN = numel(tshape, 3);
    int32_t* T = (int32_t*)malloc(tN * sizeof(int32_t));
    if (!T) die_err("malloc T", &err);

    for (size_t i = 0; i < tN; i++) {
        T[i] = (int32_t)(100 + (int)i);
    }

    // (D) UTF-16 char array: 1 x 4 ("GBF!")
    // MATLAB "char" is UTF-16 code units.
    uint16_t* C = (uint16_t*)malloc(4 * sizeof(uint16_t));
    if (!C) die_err("malloc C", &err);
    C[0] = (uint16_t)'G';
    C[1] = (uint16_t)'B';
    C[2] = (uint16_t)'F';
    C[3] = (uint16_t)'!';

    size_t cshape[2] = {1, 4};

    // Show what we are writing (row-major views)
    printf("Will write:\n");
    printf("  A: single %zux%zu\n", a_rows, a_cols);
    preview_f32_rowmajor(A, a_rows, a_cols, 5, 8);

    printf("  B: double %zux%zu\n", b_rows, b_cols);
    preview_f64_rowmajor(B, b_rows, b_cols, 5, 8);

    printf("  T: int32 3x3x4 (linear)\n");
    preview_i32_linear(T, tN, 24);

    printf("  txt: char UTF-16 1x4\n");
    preview_utf16_as_ascii(C, 4);

    // -------------------------------------------------------------------------
    // 2) Write using the easy interface
    // -------------------------------------------------------------------------

    gbf_write_options_t wopt;
    wopt.compression = GBF_COMP_AUTO;
    wopt.include_crc32 = 1;
    wopt.zlib_level = -1;

    int ok = gbf_easy_write_file(
        out_path,
        wopt,
        &err,
        // store under a struct "demo"
        gbf_easy_f32_nd("demo.single_A", A, (size_t[]){a_rows, a_cols}, 2, GBF_EASY_ROW_MAJOR, &err),
        gbf_easy_f64_nd("demo.double_B", B, (size_t[]){b_rows, b_cols}, 2, GBF_EASY_ROW_MAJOR, GBF_EASY_COPY, &err),
        gbf_easy_i32_nd("demo.tensor_T", T, tshape, 3, GBF_EASY_ROW_MAJOR, &err),
        gbf_easy_char_utf16_nd("demo.txt", C, 4, cshape, 2, GBF_EASY_COPY, &err),
        GBF_EASY_END
    );

    // Since we used GBF_EASY_COPY for some, we still own B/C; for A/T we may also still own
    // depending on your implementation. To be safe: we free our original buffers now.
    free(A);
    free(B);
    free(T);
    free(C);

    if (!ok) {
        die_err("gbf_easy_write_file failed", &err);
    }

    printf("\nWrote file: %s\n\n", out_path);

    // -------------------------------------------------------------------------
    // 3) Read back using the easy interface and "plot"/preview
    // -------------------------------------------------------------------------

    gbf_read_options_t ropt;
    ropt.validate = 1;

    gbf_value_t* root = NULL;
    gbf_header_t* header = NULL;

    if (!gbf_easy_read_file(out_path, ropt, &root, &header, &err)) {
        die_err("gbf_easy_read_file failed", &err);
    }

    printf("Read back OK. Header fields=%zu payload_start=%" PRIu64 "\n\n",
           header ? header->fields_len : 0,
           header ? header->payload_start : 0);

    // Print previews
    readback_preview_numeric(root, "demo.single_A");
    readback_preview_numeric(root, "demo.double_B");
    readback_preview_numeric(root, "demo.tensor_T");
    readback_preview_char(root, "demo.txt");

    // Cleanup
    if (header) gbf_header_free(header);
    if (root) gbf_value_free(root);

    printf("\nAll good.\n");
    return 0;
}
