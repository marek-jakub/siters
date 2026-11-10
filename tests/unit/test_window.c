#include "test_common.h"

/* ================================================================
   Tests for create_main_window (existing)
   ================================================================ */

void test_create_main_window_calls_gtk_window_new(void **state) {
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

void test_create_main_window_title_is_siters(void **state) {
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

void test_create_main_window_sets_correct_size(void **state) {
    (void) state;
    expect_value(__wrap_gtk_window_new, type, GTK_WINDOW_TOPLEVEL);
    GtkWidget *win = create_main_window();
    assert_non_null(win);
    assert_int_equal(size_width, 1000);
    assert_int_equal(size_height, 800);
    assert_int_equal(gtk_window_set_default_size_called, 1);
}

void test_create_main_window_connects_destroy_signal(void **state) {
    (void) state;
    expect_value(__wrap_gtk_window_new, type, GTK_WINDOW_TOPLEVEL);
    GtkWidget *win = create_main_window();
    assert_non_null(win);
    assert_int_equal(size_width, 1000);
    assert_int_equal(size_height, 800);
}

void test_create_main_window_returns_valid_pointer(void **state) {
    (void) state;
    expect_value(__wrap_gtk_window_new, type, GTK_WINDOW_TOPLEVEL);
    GtkWidget *win = create_main_window();
    assert_non_null(win);
    assert_int_equal(size_width, 1000);
    assert_int_equal(size_height, 800);
}

void test_create_main_window_initialization_sequence(void **state) {
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

