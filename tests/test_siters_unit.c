#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include <setjmp.h> /* IWYU pragma: keep — required by cmocka.h for jmp_buf */
#include <cmocka.h>
#include <gtk/gtk.h>
#include "pdf.h"

/* Mock control variables for gtk_widget_get_allocation */
static int mock_alloc_width = 1000;
static int mock_alloc_height = 1200;

/* Mock control variable for gtk_adjustment_get_value */
static double mock_adjustment_value = 0.0;

/* Mock for gtk_widget_get_allocation */
void __wrap_gtk_widget_get_allocation(GtkWidget *widget, GtkAllocation *allocation) {
    (void)widget;
    allocation->x = 0;
    allocation->y = 0;
    allocation->width = mock_alloc_width;
    allocation->height = mock_alloc_height;
}

/* Mock for gtk_range_get_adjustment — returns a dummy GtkAdjustment pointer */
GtkAdjustment *__wrap_gtk_range_get_adjustment(GtkRange *range) {
    (void)range;
    return (GtkAdjustment *)g_object_new(GTK_TYPE_ADJUSTMENT, NULL);
}

/* Mock for gtk_adjustment_get_value */
double __wrap_gtk_adjustment_get_value(GtkAdjustment *adjustment) {
    (void)adjustment;
    return mock_adjustment_value;
}

/* Mock for gtk_scrolled_window_get_vadjustment — returns a dummy pointer */
GtkAdjustment *__wrap_gtk_scrolled_window_get_vadjustment(GtkScrolledWindow *sw) {
    (void)sw;
    return (GtkAdjustment *)g_object_new(GTK_TYPE_ADJUSTMENT, NULL);
}

/* Mock GTK functions to avoid display requirements */
static GtkWidget *mock_window = NULL;
static int gtk_window_new_called = 0;
static int gtk_window_set_title_called = 0;
static int gtk_window_set_default_size_called = 0;
static int g_signal_connect_called = 0;

static char title_buffer[256] = {0};
static int size_width = 0;
static int size_height = 0;

/* Mock gtk_window_new */
GtkWidget* __wrap_gtk_window_new(GtkWindowType type) {
    check_expected(type);
    gtk_window_new_called++;
    if (!mock_window) {
        mock_window = g_malloc0(sizeof(GtkWidget));
    }
    return mock_window;
}

/* Mock gtk_window_set_title */
void __wrap_gtk_window_set_title(GtkWindow *window, const gchar *title) {
    assert_non_null(window);
    assert_non_null(title);
    strncpy(title_buffer, title, sizeof(title_buffer) - 1);
    gtk_window_set_title_called++;
}

/* Mock gtk_window_set_default_size */
void __wrap_gtk_window_set_default_size(GtkWindow *window, gint width, gint height) {
    assert_non_null(window);
    size_width = width;
    size_height = height;
    gtk_window_set_default_size_called++;
}

/* Mock gtk_window_set_geometry_hints */
void __wrap_gtk_window_set_geometry_hints(GtkWindow *window,
    GtkWidget *geometry_widget,
    GdkGeometry *geometry,
    GdkWindowHints geom_mask) {
    (void)window;
    (void)geometry_widget;
    (void)geometry;
    (void)geom_mask;
}

/* Mock g_signal_connect */
gulong __wrap_g_signal_connect_data(gpointer instance, const gchar *detailed_signal,
                                     GCallback c_handler, gpointer data,
                                     GClosureNotify destroy_data, GConnectFlags connect_flags) {
    (void) instance;
    (void) detailed_signal;
    (void) c_handler;
    (void) data;
    (void) destroy_data;
    (void) connect_flags;
    g_signal_connect_called++;
    return 1;
}

/* Include siters.c to access static functions and types.
   Suppress -Wunused-function: many static callbacks are registered
   via g_signal_connect (function pointer) and appear unused to the compiler. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "../src/siters.c"
#pragma GCC diagnostic pop

/* Suppress GLib-GObject validation warnings during tests */
static void suppress_gtk_warnings(const gchar *log_domain, GLogLevelFlags log_level,
                                   const gchar *message, gpointer user_data) {
    (void) user_data;
    if (log_domain && g_str_has_prefix(log_domain, "GLib-GObject")) {
        if (message && g_str_has_prefix(message, "invalid unclassed pointer")) {
            return;
        }
    }
    g_log_default_handler(log_domain, log_level, message, user_data);
}

/* Setup function — reset mocks before each test */
static int setup(void **state) {
    (void) state;
    gtk_window_new_called = 0;
    gtk_window_set_title_called = 0;
    gtk_window_set_default_size_called = 0;
    g_signal_connect_called = 0;
    memset(title_buffer, 0, sizeof(title_buffer));
    size_width = 0;
    size_height = 0;
    mock_alloc_width = 1000;
    mock_alloc_height = 1200;
    mock_adjustment_value = 0.0;
    if (mock_window) {
        g_free(mock_window);
        mock_window = NULL;
    }
    if (!gtk_init_check(0, NULL)) {
        return -1;
    }
    return 0;
}

/* Teardown function */
static int teardown(void **state) {
    (void) state;
    if (mock_window) {
        g_free(mock_window);
        mock_window = NULL;
    }
    return 0;
}

/* ================================================================
   Tests for create_main_window (existing)
   ================================================================ */

static void test_create_main_window_calls_gtk_window_new(void **state) {
    (void) state;
    expect_value(__wrap_gtk_window_new, type, GTK_WINDOW_TOPLEVEL);
    GtkWidget *win = create_main_window();
    assert_non_null(win);
    assert_int_equal(gtk_window_new_called, 1);
    assert_int_equal(size_width, 1000);
    assert_int_equal(size_height, 800);
    assert_non_null(strstr(title_buffer, "Siters"));
    assert_non_null(strstr(title_buffer, "Default"));
}

static void test_create_main_window_title_is_siters(void **state) {
    (void) state;
    expect_value(__wrap_gtk_window_new, type, GTK_WINDOW_TOPLEVEL);
    GtkWidget *win = create_main_window();
    assert_non_null(win);
    assert_non_null(strstr(title_buffer, "Siters"));
    assert_non_null(strstr(title_buffer, "Default"));
    assert_true(gtk_window_set_title_called >= 1);
    assert_int_equal(size_width, 1000);
    assert_int_equal(size_height, 800);
}

static void test_create_main_window_sets_correct_size(void **state) {
    (void) state;
    expect_value(__wrap_gtk_window_new, type, GTK_WINDOW_TOPLEVEL);
    GtkWidget *win = create_main_window();
    assert_non_null(win);
    assert_int_equal(size_width, 1000);
    assert_int_equal(size_height, 800);
    assert_int_equal(gtk_window_set_default_size_called, 1);
}

static void test_create_main_window_connects_destroy_signal(void **state) {
    (void) state;
    expect_value(__wrap_gtk_window_new, type, GTK_WINDOW_TOPLEVEL);
    GtkWidget *win = create_main_window();
    assert_non_null(win);
    assert_int_equal(size_width, 1000);
    assert_int_equal(size_height, 800);
}

static void test_create_main_window_returns_valid_pointer(void **state) {
    (void) state;
    expect_value(__wrap_gtk_window_new, type, GTK_WINDOW_TOPLEVEL);
    GtkWidget *win = create_main_window();
    assert_non_null(win);
    assert_int_equal(size_width, 1000);
    assert_int_equal(size_height, 800);
}

static void test_create_main_window_initialization_sequence(void **state) {
    (void) state;
    expect_value(__wrap_gtk_window_new, type, GTK_WINDOW_TOPLEVEL);
    GtkWidget *win = create_main_window();
    assert_non_null(win);
    assert_int_equal(gtk_window_new_called, 1);
    assert_true(gtk_window_set_title_called >= 1);
    assert_int_equal(gtk_window_set_default_size_called, 1);
    assert_int_equal(size_width, 1000);
    assert_int_equal(size_height, 800);
}

/* ================================================================
   Tests for has_link_at
   ================================================================ */

/* Helper: create a minimal TabData with a single link on page 0 */
static void setup_tab_with_link(TabData *tab) {
    memset(tab, 0, sizeof(*tab));
    tab->page_links_n = 1;
    tab->page_links = g_malloc0(sizeof(PdfrLink *));
    tab->n_pages = 1;
    tab->cached_page_heights = g_malloc(sizeof(double));
    tab->cached_page_heights[0] = 800.0;
    PdfrLink *m = g_malloc0(sizeof(PdfrLink));
    m->rect.x1 = 100.0;
    m->rect.y1 = 200.0;
    m->rect.x2 = 300.0;
    m->rect.y2 = 400.0;
    m->type = PDF_LINK_UNKNOWN;
    m->next = NULL;
    m->uri = NULL;
    m->named_dest = NULL;
    tab->page_links[0] = m;
}

static void teardown_tab_with_link(TabData *tab) {
    if (tab->page_links) {
        if (tab->page_links[0]) {
            PdfrLink *cur = tab->page_links[0];
            while (cur) {
                PdfrLink *next = cur->next;
                free(cur->uri);
                free(cur->named_dest);
                free(cur);
                cur = next;
            }
        }
        g_free(tab->page_links);
        tab->page_links = NULL;
    }
    tab->page_links_n = 0;
    g_free(tab->cached_page_heights);
    tab->cached_page_heights = NULL;
}

static void test_has_link_at_inside_rectangle(void **state) {
    (void)state;
    TabData tab;
    setup_tab_with_link(&tab);
    /* Link rect in device/page space (y-down, 0=top): x=[100,300], y=[200,400]
       y1=200 is top, y2=400 is bottom. */
    assert_true(has_link_at(&tab, 0, 150.0, 350.0));
    assert_true(has_link_at(&tab, 0, 100.0, 200.0));
    assert_true(has_link_at(&tab, 0, 300.0, 400.0));
    assert_true(has_link_at(&tab, 0, 200.0, 300.0));
    teardown_tab_with_link(&tab);
}

static void test_has_link_at_outside_rectangle(void **state) {
    (void)state;
    TabData tab;
    setup_tab_with_link(&tab);
    /* Outside rectangle in rendering space (y-down): y range=[200,400] */
    assert_false(has_link_at(&tab, 0, 50.0, 350.0));   /* left of x */
    assert_false(has_link_at(&tab, 0, 350.0, 350.0));   /* right of x */
    assert_false(has_link_at(&tab, 0, 150.0, 150.0));   /* above (smaller y in y-down) */
    assert_false(has_link_at(&tab, 0, 150.0, 500.0));   /* below (larger y in y-down) */
    teardown_tab_with_link(&tab);
}

static void test_has_link_at_null_safety(void **state) {
    (void)state;
    assert_false(has_link_at(NULL, 0, 0.0, 0.0));
    TabData tab;
    memset(&tab, 0, sizeof(tab));
    /* page_links_n == 0, page_links == NULL */
    assert_false(has_link_at(&tab, 0, 0.0, 0.0));
    /* negative page index */
    tab.page_links_n = 1;
    tab.page_links = NULL;
    assert_false(has_link_at(&tab, -1, 0.0, 0.0));
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

static void test_widget_to_page_coords_inside_page(void **state) {
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

static void test_widget_to_page_coords_outside_page(void **state) {
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

static void test_widget_to_page_coords_null_safety(void **state) {
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
   Main
   ================================================================ */

int main(void) {
    g_log_set_handler("GLib-GObject", G_LOG_LEVEL_WARNING | G_LOG_LEVEL_CRITICAL,
                      suppress_gtk_warnings, NULL);

    const struct CMUnitTest tests[] = {
        /* create_main_window */
        cmocka_unit_test_setup_teardown(test_create_main_window_calls_gtk_window_new, setup, teardown),
        cmocka_unit_test_setup_teardown(test_create_main_window_title_is_siters, setup, teardown),
        cmocka_unit_test_setup_teardown(test_create_main_window_sets_correct_size, setup, teardown),
        cmocka_unit_test_setup_teardown(test_create_main_window_connects_destroy_signal, setup, teardown),
        cmocka_unit_test_setup_teardown(test_create_main_window_returns_valid_pointer, setup, teardown),
        cmocka_unit_test_setup_teardown(test_create_main_window_initialization_sequence, setup, teardown),
        /* has_link_at */
        cmocka_unit_test(test_has_link_at_inside_rectangle),
        cmocka_unit_test(test_has_link_at_outside_rectangle),
        cmocka_unit_test(test_has_link_at_null_safety),
        /* widget_to_page_coords */
        cmocka_unit_test(test_widget_to_page_coords_inside_page),
        cmocka_unit_test(test_widget_to_page_coords_outside_page),
        cmocka_unit_test(test_widget_to_page_coords_null_safety),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
