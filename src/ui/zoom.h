#ifndef SITERS_UI_ZOOM_H
#define SITERS_UI_ZOOM_H

#include <gtk/gtk.h>

/* Zoom buttons: step the current tab's zoom in/out. */
void on_zoom_in_left(GtkButton *btn, gpointer user_data);
void on_zoom_out_left(GtkButton *btn, gpointer user_data);
void on_zoom_in_right(GtkButton *btn, gpointer user_data);
void on_zoom_out_right(GtkButton *btn, gpointer user_data);

/* Page navigation buttons: step the current tab's page up/down. */
void on_page_up_left(GtkButton *btn, gpointer user_data);
void on_page_down_left(GtkButton *btn, gpointer user_data);
void on_page_up_right(GtkButton *btn, gpointer user_data);
void on_page_down_right(GtkButton *btn, gpointer user_data);

#endif /* SITERS_UI_ZOOM_H */