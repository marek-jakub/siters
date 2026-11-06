#ifndef SITERS_TEST_MOCKS_H
#define SITERS_TEST_MOCKS_H

#include <gtk/gtk.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Mock control variables shared with the __wrap_* implementations. */
extern int mock_alloc_width;
extern int mock_alloc_height;
extern double mock_adjustment_value;
extern GtkAdjustment *mock_vadjustment_obj;
extern GtkAdjustment *mock_sadj_obj;
extern int mock_queue_draw_called;
extern GtkWidget *mock_draw_widget;
extern GtkWidget *mock_window;
extern int gtk_window_new_called;
extern int gtk_window_set_title_called;
extern int gtk_window_set_default_size_called;
extern int g_signal_connect_called;
extern char title_buffer[256];
extern int size_width;
extern int size_height;

/* Reset mocks before each test and free the fake window afterwards. */
int setup(void **state);
int teardown(void **state);

#ifdef __cplusplus
}
#endif

#endif /* SITERS_TEST_MOCKS_H */