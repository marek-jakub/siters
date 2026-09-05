#include "toolbar.h"
#include "app.h"
#include "state.h"

extern App app;

void on_window_destroy(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    (void)user_data;
    save_state();
    gtk_main_quit();
}

gboolean on_window_configure(GtkWidget *widget, GdkEventConfigure *event, gpointer user_data) {
    (void)user_data;

    // Update current geometry
    app.current_width = event->width;
    app.current_height = event->height;
    app.current_x = event->x;
    app.current_y = event->y;
    app.current_maximized = gtk_window_is_maximized(GTK_WINDOW(widget));

    return FALSE; // Allow further processing
}

void update_window_title_for_session(const char *session_name) {
    if (!app.window) return;

    const char *name = (session_name && *session_name) ? session_name : "Default";
    gchar *title = g_strdup_printf("Siters - %s", name);
    gtk_window_set_title(GTK_WINDOW(app.window), title);
    g_free(title);
}

void on_open_file_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    open_file_in_notebook(app.left_notebook, FALSE);
}

void on_open_helper_file_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    open_file_in_notebook(app.right_notebook, TRUE);
}

void on_close_file_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    (void)user_data;
    close_tab_in_notebook(GTK_NOTEBOOK(app.left_notebook));
}

void on_close_helper_file_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    (void)user_data;
    close_tab_in_notebook(GTK_NOTEBOOK(app.right_notebook));
}