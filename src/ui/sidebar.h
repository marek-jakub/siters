#ifndef SITERS_UI_SIDEBAR_H
#define SITERS_UI_SIDEBAR_H

#include <gtk/gtk.h>

/* Sidebar toggle callbacks. Each one hides the other sidebars, deactivates
   the other toggle buttons (with their handlers blocked so the flip does not
   re-enter), and repacks the shared app.sidebar into app.main_hbox. They
   mutually reference each other via g_signal_handlers_block_by_func, so they
   are shared non-static hooks between the app hub and the sidebar modules. */
void on_sessions_toggled(GtkToggleButton *btn, gpointer user_data);
void on_toc_toggled(GtkToggleButton *btn, gpointer user_data);
void on_settings_toggled(GtkToggleButton *btn, gpointer user_data);
void on_left_file_info_toggled(GtkToggleButton *btn, gpointer user_data);

#endif /* SITERS_UI_SIDEBAR_H */