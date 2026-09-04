#ifndef SITERS_UI_THEME_H
#define SITERS_UI_THEME_H

#include <gtk/gtk.h>

/* Theme / appearance helpers: dark-theme detection, toolbar icon recoloring,
   and CSS application for dark widget styling. These are used by the app hub
   (building the toolbar and wiring theme-change signals) and by the session
   persistence module. */
gboolean   detect_system_dark_theme(void);
GtkWidget *create_toolbar_icon(const char *name);
void       recolor_all_toolbars(void);
void       on_theme_changed(GObject *gobject, GParamSpec *pspec, gpointer user_data);
void       apply_dark_css(gboolean apply);

#endif /* SITERS_UI_THEME_H */
