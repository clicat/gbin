
#include "gbin/gbf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_sec(void) {
#if defined(CLOCK_MONOTONIC)
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#else
    return (double)clock() / (double)CLOCKS_PER_SEC;
#endif
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    const char* path = "bench.gbf";

    /* 1024x1024 f64 ~ 8 MB */
    size_t rows = 1024;
    size_t cols = 1024;
    size_t n = rows * cols;

    double* data = (double*)malloc(n * sizeof(double));
    if (!data) { fprintf(stderr, "oom\n"); return 1; }
    for (size_t i = 0; i < n; i++) data[i] = (double)i * 0.001;

    gbf_error_t err = {0};
    gbf_value_t* root = gbf_value_new_struct();
    gbf_value_t* mat = gbf_value_new_f64_matrix_rowmajor(data, rows, cols, &err);
    if (!mat) { fprintf(stderr, "error: %s\n", err.message); return 1; }
    gbf_struct_set(root, "big", mat, &err);

    gbf_write_options_t wopt;
    wopt.compression = GBF_COMP_AUTO;
    wopt.include_crc32 = 1;
    wopt.zlib_level = -1;

    double t0 = now_sec();
    if (!gbf_write_file(path, root, wopt, &err)) {
        fprintf(stderr, "write failed: %s\n", err.message ? err.message : "");
        return 1;
    }
    double t1 = now_sec();

    gbf_value_free(root);
    free(data);

    gbf_read_options_t ropt = {0};

    /* read var a few times */
    int iters = 10;
    double r0 = now_sec();
    size_t total_bytes = 0;
    for (int i = 0; i < iters; i++) {
        gbf_value_t* v = NULL;
        if (!gbf_read_var(path, "big", ropt, &v, &err)) {
            fprintf(stderr, "read failed: %s\n", err.message ? err.message : "");
            return 1;
        }
        if (v && v->kind == GBF_VALUE_NUMERIC) total_bytes += v->as.num.real_len;
        gbf_value_free(v);
    }
    double r1 = now_sec();

    double write_s = (t1 - t0);
    double read_s = (r1 - r0);

    double mb = (double)total_bytes / (1024.0 * 1024.0);
    printf("write: %.3f s\n", write_s);
    printf("read:  %.3f s (iters=%d, total=%.1f MiB, throughput=%.1f MiB/s)\n",
           read_s, iters, mb, (read_s > 0.0 ? (mb / read_s) : 0.0));

    remove(path);
    return 0;
}
