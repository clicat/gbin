#include "gbin/gbf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

/*
 * Forward declarations (needed for C99; used by the TUI code below)
 */
static char* xstrdup(const char* s);
static void print_err(const gbf_error_t* err);
int interactive_show(const char* file, int validate);

/*
 * Interactive TUI (ncurses)
 *
 * This implementation provides a lightweight navigable browser similar to the Rust/C++ TUI:
 *  - Up/Down: move selection
 *  - Right / Enter: expand/collapse (nodes) OR preview (leaf)
 *  - Left: collapse
 *  - q / ESC: quit
 *
 * It uses the header-only view to build the tree, and reads a variable on-demand when
 * a leaf is selected (preview panel).
 */

#include <ctype.h>


#if defined(__has_include)
#  if __has_include(<ncurses.h>)
#    include <ncurses.h>
#  else
#    include <curses.h>
#  endif
#else
#  include <ncurses.h>
#endif

/* ---- ncurses colors ---- */
#ifndef UI_COLORS
#define UI_COLORS 1
#endif

enum {
    CP_DEFAULT = 1,
    CP_TITLE,
    CP_HINT,
    CP_NAME,
    CP_META,
    CP_PREVIEW,
    CP_ERROR,
    CP_SELECTED,
};

static void ui_init_colors(void) {
#if UI_COLORS
    if (!has_colors()) return;
    start_color();
    use_default_colors();
    init_pair(CP_DEFAULT, -1, -1);
    init_pair(CP_TITLE, COLOR_CYAN, -1);
    init_pair(CP_HINT, COLOR_BLUE, -1);
    init_pair(CP_NAME, COLOR_CYAN, -1);
    init_pair(CP_META, COLOR_YELLOW, -1);
    init_pair(CP_PREVIEW, COLOR_GREEN, -1);
    init_pair(CP_ERROR, COLOR_RED, -1);
    init_pair(CP_SELECTED, COLOR_BLACK, COLOR_WHITE);
#endif
}

static int ui_have_colors(void) {
#if UI_COLORS
    return has_colors();
#else
    return 0;
#endif
}

typedef struct ui_node {
    char* name;                 /* segment name */
    char* full_path;            /* dot-path from root */
    int is_leaf;
    const gbf_field_meta_t* meta; /* for leaf */
    struct ui_node* parent;
    struct ui_node* child;
    struct ui_node* sibling;
} ui_node_t;

typedef struct ui_row {
    const ui_node_t* node;
    int depth;
} ui_row_t;

static ui_node_t* ui_node_new(const char* name) {
    ui_node_t* n = (ui_node_t*)calloc(1, sizeof(ui_node_t));
    n->name = xstrdup(name ? name : "");
    return n;
}

static void ui_node_free(ui_node_t* n) {
    if (!n) return;
    ui_node_free(n->child);
    ui_node_free(n->sibling);
    free(n->name);
    free(n->full_path);
    free(n);
}

static ui_node_t* ui_find_child(ui_node_t* parent, const char* name) {
    for (ui_node_t* c = parent->child; c; c = c->sibling) {
        if (strcmp(c->name, name) == 0) return c;
    }
    return NULL;
}

static void ui_add_leaf(ui_node_t* root, const char* path, const gbf_field_meta_t* meta) {
    char* tmp = xstrdup(path);
    ui_node_t* cur = root;

    /* Build full path incrementally */
    char full[4096];
    full[0] = 0;

    for (char* tok = strtok(tmp, "."); tok; tok = strtok(NULL, ".")) {
        if (full[0] == 0) {
            snprintf(full, sizeof(full), "%s", tok);
        } else {
            size_t len = strlen(full);
            snprintf(full + len, sizeof(full) - len, ".%s", tok);
        }

        ui_node_t* child = ui_find_child(cur, tok);
        if (!child) {
            child = ui_node_new(tok);
            child->parent = cur;
            child->sibling = cur->child;
            cur->child = child;
        }
        if (!child->full_path) child->full_path = xstrdup(full);
        cur = child;
    }

    cur->is_leaf = 1;
    cur->meta = meta;

    free(tmp);
}

static int ui_has_children(const ui_node_t* n) {
    return n && n->child != NULL;
}

/* expanded set implemented as a linked list of strings */
typedef struct str_node {
    char* s;
    struct str_node* next;
} str_node_t;

static int strset_contains(const str_node_t* set, const char* s) {
    for (const str_node_t* it = set; it; it = it->next) {
        if (strcmp(it->s, s) == 0) return 1;
    }
    return 0;
}

static void strset_add(str_node_t** set, const char* s) {
    if (!s) return;
    if (strset_contains(*set, s)) return;
    str_node_t* n = (str_node_t*)calloc(1, sizeof(str_node_t));
    n->s = xstrdup(s);
    n->next = *set;
    *set = n;
}

static void strset_remove(str_node_t** set, const char* s) {
    if (!set || !*set || !s) return;
    str_node_t* prev = NULL;
    str_node_t* cur = *set;
    while (cur) {
        if (strcmp(cur->s, s) == 0) {
            if (prev) prev->next = cur->next;
            else *set = cur->next;
            free(cur->s);
            free(cur);
            return;
        }
        prev = cur;
        cur = cur->next;
    }
}

static void strset_free(str_node_t* set) {
    while (set) {
        str_node_t* n = set->next;
        free(set->s);
        free(set);
        set = n;
    }
}

static void ui_build_rows_rec(const ui_node_t* n, int depth, const str_node_t* expanded, ui_row_t* out, int* out_len, int out_cap) {
    if (!n) return;

    /* Sort-less: siblings are in reverse insertion order; OK for now */
    for (const ui_node_t* c = n->child; c; c = c->sibling) {
        if (*out_len < out_cap) {
            out[*out_len].node = c;
            out[*out_len].depth = depth;
            (*out_len)++;
        }

        int is_expanded = 0;
        if (c->full_path && strset_contains(expanded, c->full_path)) is_expanded = 1;

        if (ui_has_children(c) && is_expanded) {
            ui_build_rows_rec(c, depth + 1, expanded, out, out_len, out_cap);
        }
    }
}

static int ui_rebuild_rows(const ui_node_t* root, const str_node_t* expanded, ui_row_t** rows_out) {
    /* Upper bound: fields_len * avg depth; but simple cap */
    const int cap = 8192;
    ui_row_t* rows = (ui_row_t*)calloc((size_t)cap, sizeof(ui_row_t));
    int len = 0;
    ui_build_rows_rec(root, 0, expanded, rows, &len, cap);
    *rows_out = rows;
    return len;
}

static void ui_draw_header(WINDOW* w, const char* file, const char* root_name, int cols) {
    if (ui_have_colors()) wattron(w, COLOR_PAIR(CP_TITLE));
    wattron(w, A_BOLD);
    mvwprintw(w, 0, 1, "GBF");
    wattroff(w, A_BOLD);
    if (ui_have_colors()) wattroff(w, COLOR_PAIR(CP_TITLE));
    mvwprintw(w, 0, 5, "%s", file);

    /* right aligned hints */
    const char* hint = "q quit  ←/→ collapse/expand  ↑/↓ move  Enter preview";
    int hint_len = (int)strlen(hint);
    if (hint_len < cols - 2) {
        if (ui_have_colors()) wattron(w, COLOR_PAIR(CP_HINT));
        mvwprintw(w, 0, cols - hint_len - 2, "%s", hint);
        if (ui_have_colors()) wattroff(w, COLOR_PAIR(CP_HINT));
    }
    mvwhline(w, 1, 0, ACS_HLINE, cols);

    if (root_name && root_name[0]) {
        mvwprintw(w, 2, 1, "root=%s", root_name);
    }
}

static void ui_draw_tree(WINDOW* w, ui_row_t* rows, int rows_len, int selected, int scroll, int height, int width, const str_node_t* expanded) {
    (void)expanded;
    for (int i = 0; i < height; i++) {
        int idx = scroll + i;
        wmove(w, i, 0);
        wclrtoeol(w);
        if (idx < 0 || idx >= rows_len) continue;

        const ui_node_t* n = rows[idx].node;
        int depth = rows[idx].depth;
        int has_child = ui_has_children(n);
        int is_expanded = (n->full_path && strset_contains(expanded, n->full_path)) ? 1 : 0;

        char glyph = ' ';
        if (has_child) glyph = is_expanded ? 'v' : '>';
        else glyph = n->is_leaf ? '*' : ' ';

        if (idx == selected) {
            if (ui_have_colors()) wattron(w, COLOR_PAIR(CP_SELECTED));
            else wattron(w, A_REVERSE);
        }

        int x = 1;
        for (int d = 0; d < depth && x < width - 1; d++) {
            mvwprintw(w, i, x, "  ");
            x += 2;
        }

        mvwprintw(w, i, x, "%c ", glyph);
        x += 2;

        if (ui_have_colors()) wattron(w, COLOR_PAIR(CP_NAME));
        wattron(w, A_BOLD);
        mvwprintw(w, i, x, "%s", n->name);
        wattroff(w, A_BOLD);
        if (ui_have_colors()) wattroff(w, COLOR_PAIR(CP_NAME));

        /* Leaf meta (kind/class/shape) right aligned */
        if (n->meta) {
            char meta[256];
            const gbf_field_meta_t* m = n->meta;
            /* shape shown as [d1 x d2 x ...] */
            char shape[128];
            shape[0] = 0;
            size_t sp = 0;
            sp += (size_t)snprintf(shape + sp, sizeof(shape) - sp, "[");
            for (size_t si = 0; si < m->shape_len && sp + 16 < sizeof(shape); si++) {
                sp += (size_t)snprintf(shape + sp, sizeof(shape) - sp, "%s%" PRIu64, (si ? " x " : ""), (uint64_t)m->shape[si]);
            }
            sp += (size_t)snprintf(shape + sp, sizeof(shape) - sp, "]");

            snprintf(meta, sizeof(meta), "%s/%s %s", m->kind ? m->kind : "?", m->class_name ? m->class_name : "?", shape);
            int meta_len = (int)strlen(meta);
            int meta_x = width - meta_len - 2;
            if (meta_x > x + (int)strlen(n->name) + 2) {
                if (ui_have_colors()) wattron(w, COLOR_PAIR(CP_META));
                mvwprintw(w, i, meta_x, "%s", meta);
                if (ui_have_colors()) wattroff(w, COLOR_PAIR(CP_META));
            }
        }

        if (idx == selected) {
            if (ui_have_colors()) wattroff(w, COLOR_PAIR(CP_SELECTED));
            else wattroff(w, A_REVERSE);
        }
    }
}

static void ui_draw_right(WINDOW* w, const ui_node_t* n, const char* meta_line1, const char* meta_line2, const char* preview, int height, int width) {
    (void)width;
    for (int i = 0; i < height; i++) {
        wmove(w, i, 0);
        wclrtoeol(w);
    }

    if (!n) return;

    if (ui_have_colors()) wattron(w, COLOR_PAIR(CP_TITLE));
    wattron(w, A_BOLD);
    mvwprintw(w, 0, 1, "%s", n->full_path ? n->full_path : n->name);
    wattroff(w, A_BOLD);
    if (ui_have_colors()) wattroff(w, COLOR_PAIR(CP_TITLE));

    if (meta_line1 && meta_line1[0]) {
        if (ui_have_colors()) wattron(w, COLOR_PAIR(CP_META));
        mvwprintw(w, 2, 1, "%s", meta_line1);
        if (ui_have_colors()) wattroff(w, COLOR_PAIR(CP_META));
    }
    if (meta_line2 && meta_line2[0]) {
        if (ui_have_colors()) wattron(w, COLOR_PAIR(CP_META));
        mvwprintw(w, 3, 1, "%s", meta_line2);
        if (ui_have_colors()) wattroff(w, COLOR_PAIR(CP_META));
    }

    mvwhline(w, 5, 0, ACS_HLINE, width);

    if (ui_have_colors()) wattron(w, COLOR_PAIR(CP_PREVIEW));
    wattron(w, A_BOLD);
    mvwprintw(w, 6, 1, "preview");
    wattroff(w, A_BOLD);
    if (ui_have_colors()) wattroff(w, COLOR_PAIR(CP_PREVIEW));

    int y = 8;
    if (preview && preview[0]) {
        const char* p = preview;
        while (*p && y < height - 1) {
            char line[1024];
            int k = 0;
            while (*p && *p != '\n' && k < (int)sizeof(line) - 1) {
                line[k++] = *p++;
            }
            line[k] = 0;
            if (*p == '\n') p++;
            if (ui_have_colors()) wattron(w, COLOR_PAIR(CP_PREVIEW));
            mvwprintw(w, y++, 1, "%s", line);
            if (ui_have_colors()) wattroff(w, COLOR_PAIR(CP_PREVIEW));
        }
    } else {
        mvwprintw(w, y, 1, "(select a leaf and press Enter)");
    }
}

static char* preview_to_string(const gbf_value_t* v, int max_elems, int rows, int cols) {
    (void)rows; (void)cols;

    /* Simple string builder */
    size_t cap = 16 * 1024;
    char* out = (char*)calloc(1, cap);
    if (!out) return NULL;

    /* Reuse existing summary printing logic, but capture to string (very small preview). */
    /* For now: implement a small subset sufficient for inspection. */
    if (!v) {
        snprintf(out, cap, "(null)");
        return out;
    }

    switch (v->kind) {
        case GBF_VALUE_STRING: {
            size_t numel = v->as.str.len;
            snprintf(out, cap, "string: shape=[");
            size_t pos = strlen(out);
            for (size_t i = 0; i < v->as.str.shape_len; i++) {
                pos += (size_t)snprintf(out + pos, cap - pos, "%s%zu", (i ? " x " : ""), v->as.str.shape[i]);
            }
            pos += (size_t)snprintf(out + pos, cap - pos, "] numel=%zu\n", numel);
            size_t show = numel;
            if (show > (size_t)max_elems) show = (size_t)max_elems;
            for (size_t i = 0; i < show; i++) {
                const char* s = v->as.str.data[i] ? v->as.str.data[i] : "<missing>";
                pos += (size_t)snprintf(out + pos, cap - pos, "[%zu] \"%s\"\n", i, s);
                if (pos > cap - 256) break;
            }
            return out;
        }
        case GBF_VALUE_CHAR: {
            /* interpret UTF-16 codeunits as ASCII when possible */
            size_t numel = v->as.chr.len;
            snprintf(out, cap, "char: shape=[");
            size_t pos = strlen(out);
            for (size_t i = 0; i < v->as.chr.shape_len; i++) {
                pos += (size_t)snprintf(out + pos, cap - pos, "%s%zu", (i ? " x " : ""), v->as.chr.shape[i]);
            }
            pos += (size_t)snprintf(out + pos, cap - pos, "] numel=%zu\n", numel);

            /* Build a best-effort string */
            char s[1024];
            size_t k = 0;
            for (size_t i = 0; i < numel && k < sizeof(s) - 1; i++) {
                uint16_t cu = v->as.chr.data[i];
                if (cu == 0) continue;
                if (cu < 128 && isprint((int)cu)) s[k++] = (char)cu;
                else s[k++] = '?';
            }
            s[k] = 0;
            pos += (size_t)snprintf(out + pos, cap - pos, "ascii: \"%s\"\n", s);
            return out;
        }
        case GBF_VALUE_LOGICAL: {
            size_t numel = v->as.logical.len;
            size_t show = numel;
            if (show > (size_t)max_elems) show = (size_t)max_elems;
            if (show > 100) show = 100;

            size_t pos = 0;
            pos += (size_t)snprintf(out + pos, cap - pos, "logical: shape=[");
            for (size_t i = 0; i < v->as.logical.shape_len; i++) {
                pos += (size_t)snprintf(out + pos, cap - pos, "%s%zu", (i ? " x " : ""), v->as.logical.shape[i]);
            }
            pos += (size_t)snprintf(out + pos, cap - pos, "] numel=%zu\n", numel);

            pos += (size_t)snprintf(out + pos, cap - pos, "preview (first %zu): ", show);
            for (size_t i = 0; i < show && pos < cap - 16; i++) {
                pos += (size_t)snprintf(out + pos, cap - pos, "%s%u", (i ? " " : ""), (unsigned)v->as.logical.data[i]);
            }
            pos += (size_t)snprintf(out + pos, cap - pos, "\n");
            return out;
        }
        case GBF_VALUE_NUMERIC: {
            const gbf_numeric_array_t* a = &v->as.num;
            size_t pos = 0;

            pos += (size_t)snprintf(out + pos, cap - pos, "numeric: class=%d complex=%d shape=[", (int)a->class_id, a->complex);
            for (size_t i = 0; i < a->shape_len; i++) {
                pos += (size_t)snprintf(out + pos, cap - pos, "%s%zu", (i ? " x " : ""), a->shape[i]);
            }
            pos += (size_t)snprintf(out + pos, cap - pos, "]\n");

            size_t bpe = 1;
            switch (a->class_id) {
                case GBF_NUM_DOUBLE: bpe = 8; break;
                case GBF_NUM_SINGLE: bpe = 4; break;
                case GBF_NUM_INT16:
                case GBF_NUM_UINT16: bpe = 2; break;
                case GBF_NUM_INT32:
                case GBF_NUM_UINT32: bpe = 4; break;
                case GBF_NUM_INT64:
                case GBF_NUM_UINT64: bpe = 8; break;
                default: bpe = 1; break;
            }
            size_t numel = (bpe ? (a->real_len / bpe) : 0);

            size_t d0 = (a->shape_len > 0 ? a->shape[0] : 0);
            size_t d1 = (a->shape_len > 1 ? a->shape[1] : 0);
            size_t d2 = (a->shape_len > 2 ? a->shape[2] : 0);

            char tmp[64];

            /* decode element idx into tmp */
#define DECODE_AT(IDX) do { \
    tmp[0] = 0; \
    size_t __idx = (IDX); \
    size_t __off = __idx * bpe; \
    if (__off + bpe > a->real_len) { snprintf(tmp, sizeof(tmp), "?"); break; } \
    const uint8_t* p = a->real_le + __off; \
    if (a->class_id == GBF_NUM_DOUBLE) { \
        uint64_t u = (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) | ((uint64_t)p[3] << 24) | \
                     ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40) | ((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56); \
        double x = 0.0; memcpy(&x, &u, 8); snprintf(tmp, sizeof(tmp), "%g", x); \
    } else if (a->class_id == GBF_NUM_SINGLE) { \
        uint32_t u = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); \
        float x = 0.0f; memcpy(&x, &u, 4); snprintf(tmp, sizeof(tmp), "%g", (double)x); \
    } else if (a->class_id == GBF_NUM_INT32) { \
        int32_t x = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24)); \
        snprintf(tmp, sizeof(tmp), "%" PRId32, x); \
    } else if (a->class_id == GBF_NUM_UINT32) { \
        uint32_t x = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); \
        snprintf(tmp, sizeof(tmp), "%" PRIu32, x); \
    } else if (a->class_id == GBF_NUM_INT64) { \
        uint64_t u = (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) | ((uint64_t)p[3] << 24) | \
                     ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40) | ((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56); \
        int64_t x = 0; memcpy(&x, &u, 8); snprintf(tmp, sizeof(tmp), "%" PRId64, x); \
    } else if (a->class_id == GBF_NUM_UINT64) { \
        uint64_t u = (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) | ((uint64_t)p[3] << 24) | \
                     ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40) | ((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56); \
        snprintf(tmp, sizeof(tmp), "%" PRIu64, u); \
    } else { \
        snprintf(tmp, sizeof(tmp), "%u", (unsigned)p[0]); \
    } \
} while (0)

            if (a->shape_len == 3 && d0 && d1 && d2) {
                size_t r_show = d0 < 10 ? d0 : 10;
                size_t c_show = d1 < 10 ? d1 : 10;
                size_t z_show = d2 < 3 ? d2 : 3;
                pos += (size_t)snprintf(out + pos, cap - pos, "preview (top-left %zux%zux%zu, column-major):\n", r_show, c_show, z_show);
                for (size_t z = 0; z < z_show && pos < cap - 256; z++) {
                    pos += (size_t)snprintf(out + pos, cap - pos, "z=%zu\n", z);
                    for (size_t r = 0; r < r_show && pos < cap - 256; r++) {
                        for (size_t c = 0; c < c_show && pos < cap - 32; c++) {
                            size_t idx = r + c * d0 + z * d0 * d1;
                            if (idx >= numel) { pos += (size_t)snprintf(out + pos, cap - pos, " ?"); continue; }
                            DECODE_AT(idx);
                            pos += (size_t)snprintf(out + pos, cap - pos, "%s%s", (c ? " " : ""), tmp);
                        }
                        pos += (size_t)snprintf(out + pos, cap - pos, "\n");
                    }
                    pos += (size_t)snprintf(out + pos, cap - pos, "\n");
                }
                return out;
            }

            if (a->shape_len == 2 && d0 && d1) {
                size_t r_show = d0 < 10 ? d0 : 10;
                size_t c_show = d1 < 10 ? d1 : 10;
                pos += (size_t)snprintf(out + pos, cap - pos, "preview (top-left %zux%zu, column-major):\n", r_show, c_show);
                for (size_t r = 0; r < r_show && pos < cap - 256; r++) {
                    for (size_t c = 0; c < c_show && pos < cap - 32; c++) {
                        size_t idx = r + c * d0;
                        if (idx >= numel) { pos += (size_t)snprintf(out + pos, cap - pos, " ?"); continue; }
                        DECODE_AT(idx);
                        pos += (size_t)snprintf(out + pos, cap - pos, "%s%s", (c ? " " : ""), tmp);
                    }
                    pos += (size_t)snprintf(out + pos, cap - pos, "\n");
                }
                return out;
            }

            size_t show = numel;
            if (show > (size_t)max_elems) show = (size_t)max_elems;
            if (show > 100) show = 100;
            pos += (size_t)snprintf(out + pos, cap - pos, "preview (first %zu): ", show);
            for (size_t i = 0; i < show && pos < cap - 32; i++) {
                DECODE_AT(i);
                pos += (size_t)snprintf(out + pos, cap - pos, "%s%s", (i ? " " : ""), tmp);
            }
            pos += (size_t)snprintf(out + pos, cap - pos, "\n");

            #undef DECODE_AT
            return out;
        }
        default:
            snprintf(out, cap, "(preview not implemented for kind=%d)", (int)v->kind);
            return out;
    }
}

static void ui_format_meta(const gbf_field_meta_t* m, char* line1, size_t line1_cap, char* line2, size_t line2_cap) {
    if (!m) {
        if (line1 && line1_cap) line1[0] = 0;
        if (line2 && line2_cap) line2[0] = 0;
        return;
    }

    snprintf(line1, line1_cap,
             "kind=%s class=%s shape=[%" PRIu64 " x %" PRIu64 "] complex=%s comp=%s off=%" PRIu64 " csize=%" PRIu64 " usize=%" PRIu64,
             m->kind ? m->kind : "?",
             m->class_name ? m->class_name : "?",
             (m->shape_len > 0 ? m->shape[0] : 0),
             (m->shape_len > 1 ? m->shape[1] : 0),
             m->complex ? "true" : "false",
             m->compression ? m->compression : "?",
             m->offset, m->csize, m->usize);

    if (m->encoding && m->encoding[0]) {
        snprintf(line2, line2_cap, "crc32=%08X encoding=%s", m->crc32, m->encoding);
    } else {
        snprintf(line2, line2_cap, "crc32=%08X", m->crc32);
    }
}

int interactive_show(const char* file, int validate) {
    gbf_read_options_t ropt = { validate };

    gbf_header_t* h = NULL;
    uint32_t hl = 0;
    char* raw = NULL;
    gbf_error_t err = {0};

    if (!gbf_read_header_only(file, ropt, &h, &hl, &raw, &err)) {
        print_err(&err);
        gbf_free_error(&err);
        return 1;
    }
    free(raw);

    ui_node_t* root = ui_node_new("<root>");
    for (size_t i = 0; i < h->fields_len; i++) {
        ui_add_leaf(root, h->fields[i].name, &h->fields[i]);
    }

    /* Default expansion: expand top-level */
    str_node_t* expanded = NULL;
    for (ui_node_t* c = root->child; c; c = c->sibling) {
        if (c->full_path) strset_add(&expanded, c->full_path);
    }

    /* ncurses init */
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    ui_init_colors();
    curs_set(0);

    int term_h, term_w;
    getmaxyx(stdscr, term_h, term_w);

    int header_h = 3;
    int left_w = term_w / 2;
    int right_w = term_w - left_w;

    WINDOW* w_header = newwin(header_h, term_w, 0, 0);
    WINDOW* w_left = newwin(term_h - header_h, left_w, header_h, 0);
    WINDOW* w_right = newwin(term_h - header_h, right_w, header_h, left_w);

    int selected = 0;
    int scroll = 0;

    char meta1[1024];
    char meta2[1024];
    meta1[0] = 0;
    meta2[0] = 0;

    char* preview = NULL;

    int running = 1;
    while (running) {
        getmaxyx(stdscr, term_h, term_w);
        left_w = term_w / 2;
        right_w = term_w - left_w;

        wresize(w_header, header_h, term_w);
        wresize(w_left, term_h - header_h, left_w);
        wresize(w_right, term_h - header_h, right_w);
        mvwin(w_left, header_h, 0);
        mvwin(w_right, header_h, left_w);

        werase(w_header);
        werase(w_left);
        werase(w_right);

        ui_row_t* rows = NULL;
        int rows_len = ui_rebuild_rows(root, expanded, &rows);

        if (rows_len <= 0) {
            selected = 0;
            scroll = 0;
        } else {
            if (selected < 0) selected = 0;
            if (selected >= rows_len) selected = rows_len - 1;
        }

        int left_h = term_h - header_h;
        int visible = left_h;
        if (selected < scroll) scroll = selected;
        if (selected >= scroll + visible) scroll = selected - visible + 1;
        if (scroll < 0) scroll = 0;
        if (scroll > rows_len - 1) scroll = rows_len - 1;

        ui_draw_header(w_header, file, "<root>", term_w);

        /* borders */
        box(w_left, 0, 0);
        box(w_right, 0, 0);

        /* tree and right pane */
        ui_draw_tree(w_left, rows, rows_len, selected, scroll, left_h - 2, left_w - 2, expanded);

        const ui_node_t* sel_node = NULL;
        if (rows_len > 0) sel_node = rows[selected].node;

        if (sel_node && sel_node->meta) {
            ui_format_meta(sel_node->meta, meta1, sizeof(meta1), meta2, sizeof(meta2));
        } else {
            meta1[0] = 0;
            meta2[0] = 0;
        }

        ui_draw_right(w_right, sel_node, meta1, meta2, preview, term_h - header_h - 2, right_w - 2);

        wrefresh(w_header);
        wrefresh(w_left);
        wrefresh(w_right);

        int ch = wgetch(stdscr);
        if (ch == 'q' || ch == 27 /* ESC */) {
            running = 0;
        } else if (ch == KEY_UP) {
            if (selected > 0) selected--;
        } else if (ch == KEY_DOWN) {
            if (selected + 1 < rows_len) selected++;
        } else if (ch == KEY_RIGHT || ch == '\n') {
            if (!sel_node) {
                /* nothing */
            } else if (ui_has_children(sel_node) && !sel_node->is_leaf) {
                if (sel_node->full_path) {
                    if (strset_contains(expanded, sel_node->full_path)) strset_remove(&expanded, sel_node->full_path);
                    else strset_add(&expanded, sel_node->full_path);
                }
            } else if (sel_node->is_leaf && sel_node->meta && sel_node->full_path) {
                /* preview leaf */
                gbf_value_t* v = NULL;
                gbf_error_t e2 = {0};
                if (preview) {
                    free(preview);
                    preview = NULL;
                }
                if (gbf_read_var(file, sel_node->full_path, ropt, &v, &e2)) {
                    preview = preview_to_string(v, 32, 6, 6);
                    gbf_value_free(v);
                } else {
                    preview = xstrdup(e2.message ? e2.message : "read_var failed");
                    gbf_free_error(&e2);
                }
            }
        } else if (ch == KEY_LEFT) {
            if (sel_node && ui_has_children(sel_node) && sel_node->full_path && strset_contains(expanded, sel_node->full_path)) {
                strset_remove(&expanded, sel_node->full_path);
            } else if (sel_node && sel_node->parent && sel_node->parent->full_path) {
                /* move to parent if collapsed */
                /* find parent row */
                const ui_node_t* parent = sel_node->parent;
                for (int i = 0; i < rows_len; i++) {
                    if (rows[i].node == parent) {
                        selected = i;
                        break;
                    }
                }
            }
        }

        free(rows);
    }

    if (preview) free(preview);

    delwin(w_right);
    delwin(w_left);
    delwin(w_header);
    endwin();

    strset_free(expanded);
    ui_node_free(root);
    gbf_header_free(h);

    return 0;
}


static void print_err(const gbf_error_t* err) {
    if (err && err->message) fprintf(stderr, "error: %s\n", err->message);
}


static char* xstrdup(const char* s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char* p = (char*)malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n + 1);
    return p;
}

static void print_header(const gbf_header_t* h, uint32_t header_len) {
    printf("format: %s\n", h->format ? h->format : "");
    printf("magic: %s\n", h->magic ? h->magic : "");
    printf("version: %d\n", h->version);
    printf("endianness: %s\n", h->endianness ? h->endianness : "");
    printf("order: %s\n", h->order ? h->order : "");
    printf("root: %s\n", h->root ? h->root : "");
    printf("header_len: %u\n", header_len);
    printf("payload_start: %" PRIu64 "\n", h->payload_start);
    printf("file_size (header): %" PRIu64 "\n", h->file_size);
    printf("header_crc32_hex: %s\n", h->header_crc32_hex ? h->header_crc32_hex : "");
    printf("fields: %zu\n", h->fields_len);
}

typedef struct tree_node {
    char* name;
    struct tree_node* child;
    struct tree_node* sibling;
    const gbf_field_meta_t* meta; /* non-NULL for leaf */
} tree_node_t;

static tree_node_t* node_new(const char* name) {
    tree_node_t* n = (tree_node_t*)calloc(1, sizeof(tree_node_t));
    n->name = name ? xstrdup(name) : xstrdup("");
    return n;
}

static void node_free(tree_node_t* n) {
    if (!n) return;
    node_free(n->child);
    node_free(n->sibling);
    free(n->name);
    free(n);
}

static tree_node_t* node_find_child(tree_node_t* parent, const char* name) {
    for (tree_node_t* c = parent->child; c; c = c->sibling) {
        if (strcmp(c->name, name) == 0) return c;
    }
    return NULL;
}

static void node_add_leaf(tree_node_t* root, const char* path, const gbf_field_meta_t* meta) {
    /* split on '.' */
    char* tmp = xstrdup(path);
    tree_node_t* cur = root;

    for (char* tok = strtok(tmp, "."); tok; tok = strtok(NULL, ".")) {
        tree_node_t* child = node_find_child(cur, tok);
        if (!child) {
            child = node_new(tok);
            child->sibling = cur->child;
            cur->child = child;
        }
        cur = child;
    }
    cur->meta = meta;
    free(tmp);
}

static void print_tree_rec(const tree_node_t* n, int depth, int details) {
    for (const tree_node_t* c = n->child; c; c = c->sibling) {
        for (int i = 0; i < depth; i++) printf("  ");
        printf("%s", c->name);
        if (c->meta) {
            const gbf_field_meta_t* m = c->meta;
            printf("  [%s/%s]", m->kind, m->class_name);
            if (details) {
                printf(" shape=(");
                for (size_t i = 0; i < m->shape_len; i++) {
                    if (i) printf(",");
                    printf("%" PRIu64, m->shape[i]);
                }
                printf(") off=%" PRIu64 " csize=%" PRIu64 " usize=%" PRIu64 " crc32=%08X comp=%s",
                       m->offset, m->csize, m->usize, m->crc32, m->compression ? m->compression : "");
            }
        }
        printf("\n");
        print_tree_rec(c, depth + 1, details);
    }
}

static void print_value_summary(const gbf_value_t* v, int depth);

static void print_indent(int d) { for (int i = 0; i < d; i++) printf("  "); }

static void print_numeric_preview(const gbf_numeric_array_t* a) {
    /* show first up to 8 elements as double/int */
    size_t bpe = 1;
    switch (a->class_id) {
        case GBF_NUM_DOUBLE: bpe = 8; break;
        case GBF_NUM_SINGLE: bpe = 4; break;
        case GBF_NUM_INT16:
        case GBF_NUM_UINT16: bpe = 2; break;
        case GBF_NUM_INT32:
        case GBF_NUM_UINT32: bpe = 4; break;
        case GBF_NUM_INT64:
        case GBF_NUM_UINT64: bpe = 8; break;
        default: bpe = 1; break;
    }
    size_t n = a->real_len / bpe;
    if (n > 8) n = 8;

    printf(" preview=[");
    for (size_t i = 0; i < n; i++) {
        if (i) printf(", ");
        const uint8_t* p = a->real_le + i * bpe;
        if (a->class_id == GBF_NUM_DOUBLE) {
            uint64_t u = (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) | ((uint64_t)p[3] << 24) |
                         ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40) | ((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56);
            double x = 0.0;
            memcpy(&x, &u, 8);
            printf("%g", x);
        } else if (a->class_id == GBF_NUM_SINGLE) {
            uint32_t u = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
            float x = 0.0f;
            memcpy(&x, &u, 4);
            printf("%g", (double)x);
        } else if (a->class_id == GBF_NUM_INT32) {
            int32_t x = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
            printf("%" PRId32, x);
        } else if (a->class_id == GBF_NUM_UINT32) {
            uint32_t x = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
            printf("%" PRIu32, x);
        } else {
            printf("%u", (unsigned)p[0]);
        }
    }
    printf("]");
}

static void print_value_summary(const gbf_value_t* v, int depth) {
    if (!v) return;

    switch (v->kind) {
        case GBF_VALUE_STRUCT:
            print_indent(depth);
            printf("struct (%zu fields)\n", v->as.s.len);
            for (size_t i = 0; i < v->as.s.len; i++) {
                print_indent(depth + 1);
                printf("%s:\n", v->as.s.entries[i].key);
                print_value_summary(v->as.s.entries[i].value, depth + 2);
            }
            break;

        case GBF_VALUE_NUMERIC:
            print_indent(depth);
            printf("numeric class=%d shape=(", (int)v->as.num.class_id);
            for (size_t i = 0; i < v->as.num.shape_len; i++) {
                if (i) printf(",");
                printf("%zu", v->as.num.shape[i]);
            }
            printf(") complex=%d", v->as.num.complex);
            print_numeric_preview(&v->as.num);
            printf("\n");
            break;

        case GBF_VALUE_LOGICAL:
            print_indent(depth);
            printf("logical shape=(");
            for (size_t i = 0; i < v->as.logical.shape_len; i++) { if (i) printf(","); printf("%zu", v->as.logical.shape[i]); }
            printf(") preview=[");
            for (size_t i = 0; i < v->as.logical.len && i < 16; i++) {
                if (i) printf(",");
                printf("%u", (unsigned)v->as.logical.data[i]);
            }
            printf("]\n");
            break;

        case GBF_VALUE_STRING:
            print_indent(depth);
            printf("string shape=(");
            for (size_t i = 0; i < v->as.str.shape_len; i++) { if (i) printf(","); printf("%zu", v->as.str.shape[i]); }
            printf(") preview=[");
            for (size_t i = 0; i < v->as.str.len && i < 8; i++) {
                if (i) printf(", ");
                printf("%s", v->as.str.data[i] ? v->as.str.data[i] : "<missing>");
            }
            printf("]\n");
            break;

        case GBF_VALUE_CHAR:
            print_indent(depth);
            printf("char shape=(");
            for (size_t i = 0; i < v->as.chr.shape_len; i++) { if (i) printf(","); printf("%zu", v->as.chr.shape[i]); }
            printf(") codeunits=[");
            for (size_t i = 0; i < v->as.chr.len && i < 16; i++) {
                if (i) printf(",");
                printf("%u", (unsigned)v->as.chr.data[i]);
            }
            printf("]\n");
            break;

        case GBF_VALUE_DATETIME:
            print_indent(depth);
            printf("datetime shape=(");
            for (size_t i = 0; i < v->as.dt.shape_len; i++) { if (i) printf(","); printf("%zu", v->as.dt.shape[i]); }
            printf(") tz='%s' locale='%s' format='%s'\n",
                   v->as.dt.timezone ? v->as.dt.timezone : "",
                   v->as.dt.locale ? v->as.dt.locale : "",
                   v->as.dt.format ? v->as.dt.format : "");
            break;

        case GBF_VALUE_DURATION:
            print_indent(depth);
            printf("duration shape=(");
            for (size_t i = 0; i < v->as.dur.shape_len; i++) { if (i) printf(","); printf("%zu", v->as.dur.shape[i]); }
            printf(")\n");
            break;

        case GBF_VALUE_CALENDARDURATION:
            print_indent(depth);
            printf("calendarDuration shape=(");
            for (size_t i = 0; i < v->as.caldur.shape_len; i++) { if (i) printf(","); printf("%zu", v->as.caldur.shape[i]); }
            printf(")\n");
            break;

        case GBF_VALUE_CATEGORICAL:
            print_indent(depth);
            printf("categorical shape=(");
            for (size_t i = 0; i < v->as.cat.shape_len; i++) { if (i) printf(","); printf("%zu", v->as.cat.shape[i]); }
            printf(") categories=%zu\n", v->as.cat.categories_len);
            break;

        case GBF_VALUE_OPAQUE:
            print_indent(depth);
            printf("opaque kind='%s' class='%s' bytes=%zu\n",
                   v->as.opaque.kind ? v->as.opaque.kind : "",
                   v->as.opaque.class_name ? v->as.opaque.class_name : "",
                   v->as.opaque.bytes_len);
            break;
    }
}

static void usage(const char* argv0) {
    fprintf(stderr,
        "Usage:\n"
        "  %s header <file> [--validate]\n"
        "  %s tree   <file> [--details] [--validate]\n"
        "  %s show   <file> [<var>] [--validate]\n",
        argv0, argv0, argv0);
}

int main(int argc, char** argv) {
    if (argc < 3) {
        usage(argv[0]);
        return 2;
    }

    const char* cmd = argv[1];
    const char* file = argv[2];
    int validate = 0;
    int details = 0;
    const char* var = NULL;

    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--validate") == 0) validate = 1;
        else if (strcmp(argv[i], "--details") == 0) details = 1;
        else if (!var) var = argv[i];
    }

    gbf_read_options_t ropt = { validate };

    if (strcmp(cmd, "header") == 0) {
        gbf_header_t* h = NULL;
        uint32_t hl = 0;
        char* raw = NULL;
        gbf_error_t err = {0};
        if (!gbf_read_header_only(file, ropt, &h, &hl, &raw, &err)) {
            print_err(&err);
            gbf_free_error(&err);
            return 1;
        }
        print_header(h, hl);
        gbf_header_free(h);
        free(raw);
        return 0;
    }

    if (strcmp(cmd, "tree") == 0) {
        gbf_header_t* h = NULL;
        uint32_t hl = 0;
        char* raw = NULL;
        gbf_error_t err = {0};
        if (!gbf_read_header_only(file, ropt, &h, &hl, &raw, &err)) {
            print_err(&err);
            gbf_free_error(&err);
            return 1;
        }
        free(raw);

        tree_node_t* root = node_new("");
        for (size_t i = 0; i < h->fields_len; i++) {
            node_add_leaf(root, h->fields[i].name, &h->fields[i]);
        }
        print_tree_rec(root, 0, details);
        node_free(root);
        gbf_header_free(h);
        return 0;
    }

    if (strcmp(cmd, "show") == 0) {
        if (var) {
            gbf_value_t* v = NULL;
            gbf_error_t err = {0};
            if (!gbf_read_var(file, var, ropt, &v, &err)) {
                print_err(&err);
                gbf_free_error(&err);
                return 1;
            }
            print_value_summary(v, 0);
            gbf_value_free(v);
            return 0;
        } else {
            int ret = interactive_show(file, validate);
            if (ret != 0) {
                fprintf(stderr, "Interactive show failed\n");
            }
            return ret;
        }
    }

    usage(argv[0]);
    return 2;
}



