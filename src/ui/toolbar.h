#ifndef SITERS_UI_TOOLBAR_H
#define SITERS_UI_TOOLBAR_H

#include <gtk/gtk.h>

/* Window lifecycle callbacks wired up in create_main_window. */
void on_window_destroy(GtkWidget *widget, gpointer user_data);
gboolean on_window_configure(GtkWidget *widget, GdkEventConfigure *event, gpointer user_data);
void update_window_title_for_session(const char *session_name);

/* Toolbar Open/Close buttons. The underlying file-open/close helpers are
   provided by the app hub (siters.c) and declared via state.h. */
void on_open_file_clicked(GtkButton *button, gpointer user_data);
void on_open_helper_file_clicked(GtkButton *button, gpointer user_data);
void on_close_file_clicked(GtkButton *btn, gpointer user_data);
void on_close_helper_file_clicked(GtkButton *btn, gpointer user_data);

#endif /* SITERS_UI_TOOLBAR_H */