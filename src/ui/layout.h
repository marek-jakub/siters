#ifndef SITERS_UI_LAYOUT_H
#define SITERS_UI_LAYOUT_H

#include <gtk/gtk.h>
#include "tab.h"

/* Layout toggle buttons (single column / double column / row). */
void on_layout_left_toggled(GtkToggleButton *btn, gpointer user_data);
void on_layout_right_toggled(GtkToggleButton *btn, gpointer user_data);

/* Mirror the active tab's layout mode onto the toolbar toggle buttons. */
void sync_left_layout_buttons(TabData *tab);
void sync_right_layout_buttons(TabData *tab);

#endif /* SITERS_UI_LAYOUT_H */