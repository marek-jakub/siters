#ifndef SITERS_APP_H
#define SITERS_APP_H

#include <gtk/gtk.h>
#include <json-glib/json-glib.h>
#include "pdf.h"
#include "sessions_model.h"
#include "session_model.h"
#include "document_model.h"

typedef enum {
    SIDEBAR_NONE,
    SIDEBAR_SESSIONS,
    SIDEBAR_TOC,
    SIDEBAR_SETTINGS,
    SIDEBAR_FILE_INFO
} SidebarMode;

/* Single global application state. All module-level state that used
   to be scattered as statics in siters.c now lives here. */
typedef struct App {
    gint current_width;
    gint current_height;
    gint current_x;
    gint current_y;
    gboolean current_maximized;
    SidebarMode current_sidebar_mode;
    GtkWidget *sidebar;
    GtkWidget *sidebar_label;
    GtkWidget *main_hbox;
    GtkWidget *content_vbox;
    GtkWidget *window;
    GtkWidget *sessions_container;
    GtkWidget *sessions_title;
    GtkWidget *sessions_entry;
    GtkWidget *sessions_add_btn;
    GtkWidget *sessions_remove_btn;
    GtkWidget *sessions_update_btn;
    GtkWidget *sessions_tree_view;
    GtkTreeStore *sessions_tree_store;
    sessions_model_t *sessions_model;
    gboolean sessions_tree_syncing;
    gchar *last_tree_selection_key;
    guint zoom_save_debounce_id;
    GtkWidget *toc_container;
    GtkWidget *toc_tree_view;
    GtkTreeStore *toc_tree_store;
    gboolean toc_tree_syncing;
    char *last_open_dir;
    int last_toc_selected_page;
    GtkWidget *settings_container;
    GtkWidget *tabbar_combo;
    GtkWidget *tab_width_spin;
    GtkWidget *left_color_btn;
    GtkWidget *right_color_btn;
    GtkWidget *sessions_btn;
    GtkWidget *toc_btn;
    GtkWidget *settings_btn;
    GtkWidget *file_info_btn;
    GtkWidget *file_info_container;
    GtkWidget *file_info_name_label;
    GtkWidget *file_info_path_label;
    GtkWidget *file_info_size_label;
    GtkWidget *file_info_pages_label;
    GtkWidget *search_entry;
    GtkWidget *search_btn;
    GtkWidget *search_results_view;
    GtkListStore *search_results_store;
    GtkWidget *search_no_results_label;
    GHashTable *session_models;
    GHashTable *document_models;
    GtkWidget *paned;
    GtkWidget *right_pane;
    GtkWidget *left_notebook;
    GtkWidget *right_notebook;
    gchar *current_selected_session;
    gboolean is_restoring_session_tabs;
    GtkWidget *page_entry;
    GtkWidget *page_total_label;
    GtkWidget *page_nav_overlay;
    gboolean page_spin_syncing;
    GtkWidget *right_page_entry;
    GtkWidget *right_page_total_label;
    GtkWidget *right_page_nav_overlay;
    gboolean right_page_spin_syncing;
    GtkWidget *left_column_btn;
    GtkWidget *left_double_column_btn;
    GtkWidget *left_row_btn;
    GtkWidget *right_column_btn;
    GtkWidget *right_double_column_btn;
    GtkWidget *right_row_btn;
    gboolean is_dark_theme;
    gboolean keep_dark_theme;
    GtkWidget *keep_dark_check;
    GtkCssProvider *dark_css_provider;
    guint maximize_pending_id;
    GtkWidget *right_file_info_popover;
    GtkWidget *right_popover_name_label;
    GtkWidget *right_popover_path_label;
    GtkWidget *right_popover_size_label;
    GtkWidget *right_popover_pages_label;
} App;

extern App app;

#endif
