#ifndef SETTINGS_H
#define SETTINGS_H

#include <gtk/gtk.h>
#include "session_model.h"

double get_angle_for_position(const char *pos);
void apply_tabbar_position(const char *pos);
void apply_tab_width(int width);
session_model_t *get_current_session_model(void);
void apply_page_color_to_notebook(GtkWidget *notebook, const char *color_str);
void on_keep_dark_toggled(GtkToggleButton *btn, gpointer user_data);
void on_left_color_set(GtkColorButton *btn, gpointer user_data);
void on_right_color_set(GtkColorButton *btn, gpointer user_data);
void on_tabbar_combo_changed(GtkComboBox *combo, gpointer user_data);
void on_tab_width_spin_changed(GtkSpinButton *spin, gpointer user_data);

#endif /* SETTINGS_H */
