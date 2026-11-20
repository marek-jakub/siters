#include "test_common.h"

/* ================================================================
   Tests for vulnerability #5: search hit-count clamping
   ================================================================ */

void test_search_clamps_hit_count(void **state) {
    (void)state;
    char *dir = g_dir_make_tmp("siters-search-XXXXXX", NULL);
    assert_non_null(dir);
    char *path = g_build_filename(dir, "search.pdf", NULL);
    write_text_pdf(path, 200);

    PdfrDoc *doc = pdfr_open(path, NULL);
    assert_non_null(doc);

    PdfrRect matches[100];
    assert_int_equal(pdfr_search_page(doc, 0, "word", matches, 5), 5);
    assert_int_equal(pdfr_search_page(doc, 0, "word", matches, 50), 50);
    assert_int_equal(pdfr_search_page(doc, 0, "word", matches, 100), 100);
    /* more than MAX_SEARCH_QUADS clamps to 100 instead of overflowing the stack */
    assert_int_equal(pdfr_search_page(doc, 0, "word", matches, 1000), 100);
    assert_int_equal(pdfr_search_page(doc, 0, "word", matches, 0), 0);
    assert_int_equal(pdfr_search_page(doc, 0, "word", matches, -5), 0);

    /* guards: NULL doc / NULL text */
    assert_int_equal(pdfr_search_page(NULL, 0, "word", matches, 100), 0);
    assert_int_equal(pdfr_search_page(doc, 0, NULL, matches, 100), 0);
    /* out-of-range page index must fail gracefully */
    assert_int_equal(pdfr_search_page(doc, 5, "word", matches, 100), 0);

    pdfr_close(doc);
    g_free(path);
    g_free(dir);
}

/* ================================================================
   Tests for search_free
   ================================================================ */

void test_search_free_clears_results(void **state) {
    (void)state;
    TabData tab;
    memset(&tab, 0, sizeof(tab));

    /* Simulate 3 pages of search results with rects (matches real usage) */
    tab.search_results_cap = 32;
    tab.search_results_n = 3;
    tab.search_results = g_malloc0(sizeof(*tab.search_results) * tab.search_results_cap);
    tab.search_text = g_strdup("test query");

    for (int i = 0; i < 3; i++) {
        tab.search_results[i].page = i + 1;
        tab.search_results[i].n_matches = 5;
        tab.search_results[i].rects = g_malloc0(sizeof(PdfrRect) * 5);
    }

    search_free(&tab);

    assert_null(tab.search_results);
    assert_int_equal(tab.search_results_n, 0);
    assert_int_equal(tab.search_results_cap, 0);
    assert_null(tab.search_text);
}

void test_search_free_null_safety(void **state) {
    (void)state;
    /* Should not crash on NULL */
    search_free(NULL);

    TabData tab;
    memset(&tab, 0, sizeof(tab));
    /* Empty tab (all zeroed) — should not crash */
    search_free(&tab);
}
