#include "test_common.h"

/* ================================================================
   Tests for vulnerability #4: outline depth guard
   ================================================================ */

static int count_outline_nodes(const PdfrOutline *o) {
    int c = 0;
    while (o) {
        c++;
        c += count_outline_nodes(o->down);
        o = o->next;
    }
    return c;
}

void test_outline_loads_shallow(void **state) {
    (void)state;
    char *dir = g_dir_make_tmp("siters-outline-XXXXXX", NULL);
    assert_non_null(dir);
    char *path = g_build_filename(dir, "outline.pdf", NULL);
    write_deep_outline_pdf(path, 3);

    PdfrDoc *doc = pdfr_open(path, NULL);
    assert_non_null(doc);
    PdfrOutline *o = pdfr_load_outline(doc);
    assert_non_null(o);
    assert_int_equal(count_outline_nodes(o), 3);
    assert_string_equal(o->title, "L0");
    assert_non_null(o->down);
    assert_string_equal(o->down->title, "L1");
    assert_non_null(o->down->down);
    assert_string_equal(o->down->down->title, "L2");
    pdfr_free_outline(doc, o);
    pdfr_close(doc);

    g_free(path);
    g_free(dir);
}

void test_outline_loads_at_max_depth(void **state) {
    (void)state;
    char *dir = g_dir_make_tmp("siters-outline-XXXXXX", NULL);
    assert_non_null(dir);
    char *path = g_build_filename(dir, "outline.pdf", NULL);
    write_deep_outline_pdf(path, 32);

    PdfrDoc *doc = pdfr_open(path, NULL);
    assert_non_null(doc);
    PdfrOutline *o = pdfr_load_outline(doc);
    assert_non_null(o);
    assert_int_equal(count_outline_nodes(o), 32);
    pdfr_free_outline(doc, o);
    pdfr_close(doc);

    g_free(path);
    g_free(dir);
}

void test_outline_refuses_too_deep(void **state) {
    (void)state;
    char *dir = g_dir_make_tmp("siters-outline-XXXXXX", NULL);
    assert_non_null(dir);
    char *path = g_build_filename(dir, "outline.pdf", NULL);
    write_deep_outline_pdf(path, 40);

    PdfrDoc *doc = pdfr_open(path, NULL);
    assert_non_null(doc);
    PdfrOutline *o = pdfr_load_outline(doc);
    assert_null(o); /* too deep — refused before any recursive load */
    pdfr_close(doc);

    g_free(path);
    g_free(dir);
}
