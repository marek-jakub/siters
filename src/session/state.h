#ifndef SITERS_SESSION_STATE_H
#define SITERS_SESSION_STATE_H

#include <gtk/gtk.h>
#include "app.h"
#include "tab.h"
#include "document_model.h"
#include "session_model.h"
#include "sessions_model.h"

/* Public persistence entry points (also declared in src/siters.h). */
void save_state(void);
void load_state(void);

/* Helpers provided by siters.c (the app hub) used by the persistence module. */
char*               make_document_key(const char *session_name, const char *uri, gboolean is_helper);
void                save_open_tabs_for_session(const char *session_name);
void                restore_open_tabs_for_session(const char *session_name);
void                populate_sessions_treeview(void);
TabData*            get_current_left_tab(void);
TabData*            get_current_right_tab(void);
session_model_t*    get_current_session_model(void);
void                sync_left_layout_buttons(TabData *tab);
void                sync_right_layout_buttons(TabData *tab);
void                sync_page_widget_from_tab(TabData *tab);
void                update_window_title_for_session(const char *session_name);
void                apply_tabbar_position(const char *pos);
void                apply_tab_width(int width);
void                apply_dark_css(gboolean apply);
void                recolor_all_toolbars(void);
void                apply_page_color_to_notebook(GtkWidget *notebook, const char *color_str);
void                on_keep_dark_toggled(GtkToggleButton *btn, gpointer user_data);
void                on_left_color_set(GtkColorButton *btn, gpointer user_data);
void                on_right_color_set(GtkColorButton *btn, gpointer user_data);
void                open_file_in_notebook(GtkWidget *notebook, gboolean is_helper);
void                close_tab_in_notebook(GtkNotebook *notebook);

#endif /* SITERS_SESSION_STATE_H */
