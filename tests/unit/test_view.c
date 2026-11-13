#include "test_common.h"

/* ================================================================
   Tests for vulnerability #6: clamp_double_to_int
   ================================================================ */

void test_clamp_double_to_int_boundaries(void **state) {
    (void)state;
    /* huge / infinite values saturate to the cap instead of triggering
       implementation-defined (int) casts */
    assert_int_equal(clamp_double_to_int(1e300, MAX_SURFACE_DIM), MAX_SURFACE_DIM);
    assert_int_equal(clamp_double_to_int(INFINITY, MAX_SURFACE_DIM), MAX_SURFACE_DIM);
    assert_int_equal(clamp_double_to_int(1e300, MAX_SIZE_REQUEST), MAX_SIZE_REQUEST);
    /* NaN, negatives and zero collapse to 0 */
    assert_int_equal(clamp_double_to_int(NAN, MAX_SURFACE_DIM), 0);
    assert_int_equal(clamp_double_to_int(-5.0, MAX_SURFACE_DIM), 0);
    assert_int_equal(clamp_double_to_int(0.0, MAX_SURFACE_DIM), 0);
    /* ordinary values round-trip unchanged */
    assert_int_equal(clamp_double_to_int(612.0 * (96.0 / 72.0) + 0.5, MAX_SURFACE_DIM), 816);
    assert_int_equal(clamp_double_to_int(612.0 * (96.0 / 72.0) * 2.0 + 0.5, MAX_SURFACE_DIM), 1632);
    /* the ceil() size-request call sites used in the draw path */
    assert_int_equal(clamp_double_to_int(ceil(1e300), MAX_SIZE_REQUEST), MAX_SIZE_REQUEST);
}

/* ================================================================
   Tests for widget_to_page_coords
   ================================================================ */

/* Helper: create a minimal TabData for single-page layout (mode 0) */
static void setup_tab_single_page(TabData *tab) {
    memset(tab, 0, sizeof(*tab));
    tab->layout_mode = 0;
    tab->n_pages = 1;
    tab->zoom = 72.0;  /* get_ppi_scale → 72/72 = 1.0 */
    tab->cached_page_widths = g_malloc(sizeof(double));
    tab->cached_page_heights = g_malloc(sizeof(double));
    tab->cached_page_x0 = g_malloc0(sizeof(double));
    tab->cached_page_y0 = g_malloc0(sizeof(double));
    tab->cached_page_widths[0] = 600.0;
    tab->cached_page_heights[0] = 800.0;
    tab->pages_drawing = (GtkWidget *)0x1;  /* non-NULL dummy */
    mock_alloc_width = 1000;
    mock_alloc_height = 1200;
}

static void teardown_tab_single_page(TabData *tab) {
    g_free(tab->cached_page_widths);
    g_free(tab->cached_page_heights);
    g_free(tab->cached_page_x0);
    g_free(tab->cached_page_y0);
    tab->cached_page_widths = NULL;
    tab->cached_page_heights = NULL;
    tab->cached_page_x0 = NULL;
    tab->cached_page_y0 = NULL;
    tab->pages_drawing = NULL;
}

void test_widget_to_page_coords_inside_page(void **state) {
    (void)state;
    TabData tab;
    setup_tab_single_page(&tab);
    double px, py;

    /* Page 0 centred in alloc 1000×1200:
         pw=600, ox=(1000-600)/2=200, y=6
         Page rect: x=[200,800], y=[6,806] */
    int page = widget_to_page_coords(&tab, 500.0, 400.0, &px, &py);
    assert_int_equal(page, 0);
    /* px = (500-200)/1 = 300 */
    assert_true(fabs(px - 300.0) < 0.001);
    /* py = (400-6)/1 = 394 (y-down, 0=top, no inversion) */
    assert_true(fabs(py - 394.0) < 0.001);

    teardown_tab_single_page(&tab);
}

void test_widget_to_page_coords_outside_page(void **state) {
    (void)state;
    TabData tab;
    setup_tab_single_page(&tab);

    /* Left of page */
    int page = widget_to_page_coords(&tab, 50.0, 400.0, NULL, NULL);
    assert_int_equal(page, -1);

    /* Below page */
    page = widget_to_page_coords(&tab, 500.0, 900.0, NULL, NULL);
    assert_int_equal(page, -1);

    teardown_tab_single_page(&tab);
}

void test_widget_to_page_coords_null_safety(void **state) {
    (void)state;
    double px, py;

    /* NULL tab */
    assert_int_equal(widget_to_page_coords(NULL, 0.0, 0.0, &px, &py), -1);

    /* No cached_page_widths */
    TabData tab;
    memset(&tab, 0, sizeof(tab));
    tab.pages_drawing = (GtkWidget *)0x1;
    assert_int_equal(widget_to_page_coords(&tab, 0.0, 0.0, &px, &py), -1);

    /* No pages_drawing */
    tab.cached_page_widths = g_malloc(sizeof(double));
    tab.cached_page_widths[0] = 600.0;
    tab.pages_drawing = NULL;
    assert_int_equal(widget_to_page_coords(&tab, 0.0, 0.0, &px, &py), -1);
    g_free(tab.cached_page_widths);
}

/* ================================================================
   Tests for cache_evict_idx
   ================================================================ */

void test_cache_evict_idx_removes_surface(void **state) {
    (void)state;
    TabData tab;
    memset(&tab, 0, sizeof(tab));

    /* Create a real 100x100 cairo image surface */
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 100, 100);
    cairo_surface_flush(surf);

    int expected_bytes = 100 * 100 * 4;
    tab.page_cache = g_malloc0(sizeof(cairo_surface_t *) * 4);
    tab.page_cache[0] = surf;
    tab.total_cache_bytes = expected_bytes;

    cache_evict_idx(&tab, 0);

    assert_null(tab.page_cache[0]);
    assert_int_equal(tab.total_cache_bytes, 0);

    g_free(tab.page_cache);
    tab.page_cache = NULL;
}

void test_cache_evict_idx_null_safety(void **state) {
    (void)state;
    TabData tab;
    memset(&tab, 0, sizeof(tab));

    /* NULL tab — no crash */
    cache_evict_idx(NULL, 0);

    /* NULL page_cache array — no crash */
    cache_evict_idx(&tab, 0);

    /* Negative index — no crash */
    tab.page_cache = g_malloc0(sizeof(cairo_surface_t *) * 2);
    cache_evict_idx(&tab, -1);

    /* NULL slot — no crash, bytes unchanged */
    tab.total_cache_bytes = 999;
    cache_evict_idx(&tab, 0);
    assert_int_equal(tab.total_cache_bytes, 999);

    g_free(tab.page_cache);
    tab.page_cache = NULL;
}
