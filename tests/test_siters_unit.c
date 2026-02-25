#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <cmocka.h>
#include <gtk/gtk.h>
#include "../src/siters.h"

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
        mock_window = malloc(sizeof(GtkWidget));
        memset(mock_window, 0, sizeof(GtkWidget));
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
    check_expected(width);
    check_expected(height);
    size_width = width;
    size_height = height;
    gtk_window_set_default_size_called++;
}

/* Mock g_signal_connect */
gulong __wrap_g_signal_connect_data(gpointer instance, const gchar *detailed_signal,
                                     GCallback c_handler, gpointer data, 
                                     GClosureNotify destroy_data, GConnectFlags connect_flags) {
    (void) data;
    (void) destroy_data;
    (void) connect_flags;
    assert_non_null(instance);
    assert_non_null(detailed_signal);
    assert_non_null(c_handler);
    assert_string_equal(detailed_signal, "destroy");
    g_signal_connect_called++;
    return 1;  /* Return a signal handler ID */
}

/* Suppress GLib-GObject validation warnings during tests */
static void suppress_gtk_warnings(const gchar *log_domain, GLogLevelFlags log_level,
                                   const gchar *message, gpointer user_data) {
    (void) user_data;
    /* Only suppress warnings from GLib-GObject domain about invalid pointers */
    if (log_domain && g_str_has_prefix(log_domain, "GLib-GObject")) {
        if (message && g_str_has_prefix(message, "invalid unclassed pointer")) {
            return;  /* Suppress this warning */
        }
    }
    /* For other messages, use default handler */
    g_log_default_handler(log_domain, log_level, message, user_data);
}

/* Setup function - reset mocks before each test */
static int setup(void **state) {
    (void) state;
    gtk_window_new_called = 0;
    gtk_window_set_title_called = 0;
    gtk_window_set_default_size_called = 0;
    g_signal_connect_called = 0;
    memset(title_buffer, 0, sizeof(title_buffer));
    size_width = 0;
    size_height = 0;
    if (mock_window) {
        free(mock_window);
        mock_window = NULL;
    }
    return 0;
}

/* Teardown function */
static int teardown(void **state) {
    (void) state;
    if (mock_window) {
        free(mock_window);
        mock_window = NULL;
    }
    return 0;
}

/* Test: create_main_window calls gtk_window_new with correct type */
static void test_create_main_window_calls_gtk_window_new(void **state) {
    (void) state;
    
    expect_value(__wrap_gtk_window_new, type, GTK_WINDOW_TOPLEVEL);
    expect_value(__wrap_gtk_window_set_default_size, width, 400);
    expect_value(__wrap_gtk_window_set_default_size, height, 300);
    
    GtkWidget *window = create_main_window();
    
    assert_non_null(window);
    assert_int_equal(gtk_window_new_called, 1);
    assert_string_equal(title_buffer, "Siters");
}

/* Test: create_main_window sets title "Siters" */
static void test_create_main_window_title_is_siters(void **state) {
    (void) state;
    
    expect_value(__wrap_gtk_window_new, type, GTK_WINDOW_TOPLEVEL);
    expect_value(__wrap_gtk_window_set_default_size, width, 400);
    expect_value(__wrap_gtk_window_set_default_size, height, 300);
    
    GtkWidget *window = create_main_window();
    
    assert_non_null(window);
    assert_string_equal(title_buffer, "Siters");
    assert_int_equal(gtk_window_set_title_called, 1);
}

/* Test: create_main_window sets default size to 400x300 */
static void test_create_main_window_sets_correct_size(void **state) {
    (void) state;
    
    expect_value(__wrap_gtk_window_new, type, GTK_WINDOW_TOPLEVEL);
    expect_value(__wrap_gtk_window_set_default_size, width, 400);
    expect_value(__wrap_gtk_window_set_default_size, height, 300);
    
    GtkWidget *window = create_main_window();
    
    assert_non_null(window);
    assert_int_equal(size_width, 400);
    assert_int_equal(size_height, 300);
}

/* Test: create_main_window connects destroy signal */
static void test_create_main_window_connects_destroy_signal(void **state) {
    (void) state;
    
    expect_value(__wrap_gtk_window_new, type, GTK_WINDOW_TOPLEVEL);
    expect_value(__wrap_gtk_window_set_default_size, width, 400);
    expect_value(__wrap_gtk_window_set_default_size, height, 300);
    
    GtkWidget *window = create_main_window();
    
    assert_non_null(window);
    assert_int_equal(g_signal_connect_called, 1);
}

/* Test: create_main_window returns non-null pointer */
static void test_create_main_window_returns_valid_pointer(void **state) {
    (void) state;
    
    expect_value(__wrap_gtk_window_new, type, GTK_WINDOW_TOPLEVEL);
    expect_value(__wrap_gtk_window_set_default_size, width, 400);
    expect_value(__wrap_gtk_window_set_default_size, height, 300);
    
    GtkWidget *window = create_main_window();
    
    assert_non_null(window);
}

/* Test: create_main_window initializes all properties */
static void test_create_main_window_initialization_sequence(void **state) {
    (void) state;
    
    expect_value(__wrap_gtk_window_new, type, GTK_WINDOW_TOPLEVEL);
    expect_value(__wrap_gtk_window_set_default_size, width, 400);
    expect_value(__wrap_gtk_window_set_default_size, height, 300);
    
    GtkWidget *window = create_main_window();
    
    assert_non_null(window);
    assert_int_equal(gtk_window_new_called, 1);
    assert_int_equal(gtk_window_set_title_called, 1);
    assert_int_equal(gtk_window_set_default_size_called, 1);
}

int main(void) {
    /* Install custom log handler to suppress expected warnings */
    g_log_set_handler("GLib-GObject", G_LOG_LEVEL_WARNING | G_LOG_LEVEL_CRITICAL,
                      suppress_gtk_warnings, NULL);
    
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_create_main_window_calls_gtk_window_new, setup, teardown),
        cmocka_unit_test_setup_teardown(test_create_main_window_title_is_siters, setup, teardown),
        cmocka_unit_test_setup_teardown(test_create_main_window_sets_correct_size, setup, teardown),
        cmocka_unit_test_setup_teardown(test_create_main_window_connects_destroy_signal, setup, teardown),
        cmocka_unit_test_setup_teardown(test_create_main_window_returns_valid_pointer, setup, teardown),
        cmocka_unit_test_setup_teardown(test_create_main_window_initialization_sequence, setup, teardown),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
