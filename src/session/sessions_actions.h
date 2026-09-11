#ifndef SITERS_SESSION_SESSIONS_ACTIONS_H
#define SITERS_SESSION_SESSIONS_ACTIONS_H

#include <gtk/gtk.h>

/* Window/session action handlers, wired up by create_main_window in src/ui/window.c. */
void on_title_bar_toggle(GtkToggleButton *button, gpointer user_data);
void on_helper_toggle(GtkToggleButton *button, gpointer user_data);
void on_minimize_clicked(GtkButton *button, gpointer user_data);
void on_maximize_clicked(GtkButton *button, gpointer user_data);
void on_close_clicked(GtkButton *button, gpointer user_data);
void on_sessions_add_clicked(GtkButton *button, gpointer user_data);
void on_sessions_remove_clicked(GtkButton *button, gpointer user_data);
void on_sessions_update_clicked(GtkButton *button, gpointer user_data);

#endif /* SITERS_SESSION_SESSIONS_ACTIONS_H */
