#include <stdarg.h>
#include <setjmp.h> /* IWYU pragma: keep — required by cmocka.h for jmp_buf */
#include <string.h>
#include <cmocka.h>
#include <gtk/gtk.h>

#include "test_mocks.h"

/* ---- mock control variables and __wrap_* implementations ---- */

/* Mock control variables for gtk_widget_get_allocation */
int mock_alloc_width = 1000;
int mock_alloc_height = 1200;

/* Mock control variable for gtk_adjustment_get_value */
double mock_adjustment_value = 0.0;

/* Mock control objects returned by the gtk_scrolled_window/gtk_range wraps.
   When NULL the wraps fall back to a fresh dummy object so existing tests
   that only check non-NULL behaviour are unaffected. */
GtkAdjustment *mock_vadjustment_obj = NULL;
GtkAdjustment *mock_sadj_obj = NULL;

/* Mock controls for gtk_widget_queue_draw. The handlers call
   gtk_widget_queue_draw(tab->pages_drawing); since the tab used in unit
   tests is a stack object with a fake/NULL pages_drawing, the real GTK
   implementation would trip a Gtk-CRITICAL, so it is wrapped into a no-op
   that records the call. */
int mock_queue_draw_called = 0;
GtkWidget *mock_draw_widget = NULL;

/* Mock for gtk_widget_get_allocation */
void __wrap_gtk_widget_get_allocation(GtkWidget *widget, GtkAllocation *allocation) {
    (void)widget;
    allocation->x = 0;
    allocation->y = 0;
    allocation->width = mock_alloc_width;
    allocation->height = mock_alloc_height;
}

/* Mock for gtk_widget_queue_draw — records the request instead of touching
   a real GtkWidget so the page-drawing area pointer used in scroll tests
   never reaches GTK. */
void __wrap_gtk_widget_queue_draw(GtkWidget *widget) {
    mock_queue_draw_called++;
    mock_draw_widget = widget;
}

/* Mock for gtk_range_get_adjustment — returns a controlled object or a dummy */
GtkAdjustment *__wrap_gtk_range_get_adjustment(GtkRange *range) {
    (void)range;
    if (mock_sadj_obj)
        return mock_sadj_obj;
    return (GtkAdjustment *)g_object_new(GTK_TYPE_ADJUSTMENT, NULL);
}

/* Mock for gtk_adjustment_get_value */
double __wrap_gtk_adjustment_get_value(GtkAdjustment *adjustment) {
    (void)adjustment;
    return mock_adjustment_value;
}

/* Mock for gtk_scrolled_window_get_vadjustment — returns a controlled object
   or a dummy pointer */
GtkAdjustment *__wrap_gtk_scrolled_window_get_vadjustment(GtkScrolledWindow *sw) {
    (void)sw;
    if (mock_vadjustment_obj)
        return mock_vadjustment_obj;
    return (GtkAdjustment *)g_object_new(GTK_TYPE_ADJUSTMENT, NULL);
}

/* Mock GTK functions to avoid display requirements */
GtkWidget *mock_window = NULL;
int gtk_window_new_called = 0;
int gtk_window_set_title_called = 0;
int gtk_window_set_default_size_called = 0;
int g_signal_connect_called = 0;

char title_buffer[256] = {0};
int size_width = 0;
int size_height = 0;

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

/* Setup function — reset mocks before each test */
int setup(void **state) {
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
int teardown(void **state) {
    (void) state;
    if (mock_window) {
        g_free(mock_window);
        mock_window = NULL;
    }
    return 0;
}
