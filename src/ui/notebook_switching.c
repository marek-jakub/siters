#include "notebook_switching.h"
#include "app.h"
#include "state.h"
#include "search.h"
#include "view.h"
#include "pdf.h"
#include "nav.h"
#include "ui/notebook.h"
#include "ui/tab_lifecycle.h"
#include "ui/layout.h"
#include "ui/toc.h"
#include "fileinfo/fileinfo.h"
#include "sessions_sidebar.h"
#include "sessions_tree.h"

extern App app;

void on_left_notebook_switch_page(GtkNotebook *notebook, GtkWidget *page, guint page_num, gpointer user_data) {
    (void)user_data;

    TabData *tab = g_object_get_data(G_OBJECT(page), "tab-data");

    /* Flush state for all left tabs with loaded docs before switching */
    int n_left = gtk_notebook_get_n_pages(notebook);
    for (int i = 0; i < n_left; i++) {
        GtkWidget *p = gtk_notebook_get_nth_page(notebook, i);
        TabData *t = g_object_get_data(G_OBJECT(p), "tab-data");
        if (t && t != tab && t->doc) update_document_model_from_tab(t);
    }

    /* Unload docs from all non-current tabs in this notebook to save RAM */
    for (int i = 0; i < n_left; i++) {
        GtkWidget *p = gtk_notebook_get_nth_page(notebook, i);
        TabData *t = g_object_get_data(G_OBJECT(p), "tab-data");
        if (t && t != tab && t->doc) {
            unload_tab_document(t);
        }
    }
    pdfr_purge_store();

    update_last_read_for_notebook(notebook, page, page_num);

    if (tab && !app.is_restoring_session_tabs) ensure_tab_doc_loaded(tab);

    // RESTORE STATE WHEN SWITCHING TO THIS TAB
    if (tab) {
        if (tab->initial_scroll_pending) {
            // First time seeing this tab after startup - do full restoration
            restore_document_model_to_tab(tab);
            tab->initial_scroll_pending = FALSE;
        } else {
            if (tab->current_file && app.document_models && tab->cached_page_widths) {
                char *uri = g_filename_to_uri(tab->current_file, NULL, NULL);
                if (uri) {
                    char *key = make_document_key(app.current_selected_session, uri, tab->is_helper);
                    document_model_t *dm = g_hash_table_lookup(app.document_models, key);
                    if (dm) {
                        tab->layout_mode = document_model_get_visualization_mode(dm);
                        double saved_zoom = document_model_get_zoom(dm);
                        if (saved_zoom >= 10.0 && saved_zoom <= 500.0)
                            tab->zoom = saved_zoom;
                        /* Rebuild when the layout mode OR zoom differs from the
                           last build so layout-only changes are reflected. */
                        if (tab->layout_mode != tab->built_layout_mode || tab->zoom != tab->last_zoom) {
                            build_continuous_view(tab);
                            tab->last_zoom = tab->zoom;
                        }
                        int saved_page = document_model_get_current_page(dm);
                        if (saved_page < 1) saved_page = 1;
                        if (saved_page > tab->n_pages) saved_page = tab->n_pages;
                        tab->cur_page = saved_page - 1;
                        scroll_to_page(tab, tab->cur_page, -1);
                    }
                    g_free(key);
                    g_free(uri);
                }
            }
        }
    }

    sync_left_layout_buttons(tab);
    sync_page_widget_from_tab(tab);
    if (app.current_sidebar_mode == SIDEBAR_TOC) {
        populate_toc_treeview_for_tab(tab);
        update_toc_selection_for_current_page(tab);
    }
    if (app.current_sidebar_mode == SIDEBAR_SESSIONS) {
        app.sessions_tree_syncing = TRUE;
        update_sessions_tree_document_selection_for_tab(tab);
        app.sessions_tree_syncing = FALSE;
    }
    if (app.current_sidebar_mode == SIDEBAR_FILE_INFO) update_file_info_labels(tab);
}

void on_right_notebook_switch_page(GtkNotebook *notebook, GtkWidget *page, guint page_num, gpointer user_data) {
    (void)user_data;

    /* Unload docs from all non-current tabs in this notebook */
    TabData *tab = g_object_get_data(G_OBJECT(page), "tab-data");
    int n_right = gtk_notebook_get_n_pages(notebook);
    for (int i = 0; i < n_right; i++) {
        GtkWidget *p = gtk_notebook_get_nth_page(notebook, i);
        TabData *t = g_object_get_data(G_OBJECT(p), "tab-data");
        if (t && t != tab && t->doc) {
            unload_tab_document(t);
        }
    }
    pdfr_purge_store();

    update_last_read_for_notebook(notebook, page, page_num);

    /* Load current tab's doc if needed */
    if (tab && !app.is_restoring_session_tabs) ensure_tab_doc_loaded(tab);

    // RESTORE STATE WHEN SWITCHING TO THIS TAB
    if (tab) {
        restore_document_model_to_tab(tab);
    }

    sync_right_layout_buttons(tab);
    /* keep left widget tied to primary (left) document */
    sync_page_widget_from_tab(get_current_left_tab());
    sync_right_page_widget_from_tab(tab);

    refresh_right_popover_labels();
}

void on_notebook_page_reordered(GtkNotebook *notebook, GtkWidget *page, guint page_num, gpointer user_data) {
    (void)notebook;
    (void)page;
    (void)page_num;
    (void)user_data;
    if (app.current_selected_session) {
        save_open_tabs_for_session(app.current_selected_session);
        populate_sessions_treeview();
    }
}
