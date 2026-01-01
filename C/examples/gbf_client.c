
#include "gbin/gbf.h"

#include <stdio.h>
#include <string.h>

int main(void) {
    gbf_error_t err = {0};

    /* Build a tiny GBF tree */
    gbf_value_t* root = gbf_value_new_struct();

    double mat[4] = { 1.0, 2.0,
                      3.0, 4.0 }; /* row-major 2x2 */
    gbf_value_t* a = gbf_value_new_f64_matrix_rowmajor(mat, 2, 2, &err);
    if (!a) {
        fprintf(stderr, "create matrix failed: %s\n", err.message ? err.message : "");
        gbf_free_error(&err);
        return 1;
    }
    gbf_struct_set(root, "A", a, &err);

    gbf_write_options_t wopt = { GBF_COMP_AUTO, 1, -1 };
    if (!gbf_write_file("client_example.gbf", root, wopt, &err)) {
        fprintf(stderr, "write failed: %s\n", err.message ? err.message : "");
        gbf_value_free(root);
        gbf_free_error(&err);
        return 1;
    }
    gbf_value_free(root);

    /* Read it back */
    gbf_read_options_t ropt = { 1 };
    gbf_value_t* A2 = NULL;
    if (!gbf_read_var("client_example.gbf", "A", ropt, &A2, &err)) {
        fprintf(stderr, "read failed: %s\n", err.message ? err.message : "");
        gbf_free_error(&err);
        return 1;
    }

    if (A2->kind == GBF_VALUE_NUMERIC) {
        printf("Read A: class=%d bytes=%zu\n", (int)A2->as.num.class_id, A2->as.num.real_len);
    } else {
        printf("Read A: unexpected kind\n");
    }

    gbf_value_free(A2);
    remove("client_example.gbf");
    return 0;
}
