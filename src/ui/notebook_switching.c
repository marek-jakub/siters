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
            cancel_tab_restore(t);
            cancel_doc_model_debounce(t);
            search_cancel(t);
            search_free(t);
            if (t->zoom_scroll_source_id) {
                g_source_remove(t->zoom_scroll_source_id);
                t->zoom_scroll_source_id = 0;
            }
            if (t->page_links) {
                for (int j = 0; j < t->page_links_n; j++) {
                    if (t->page_links[j])
                        pdfr_free_links(t->doc, t->page_links[j]);
                }
                g_free(t->page_links);
                t->page_links = NULL;
                t->page_links_n = 0;
            }
            pdfr_close(t->doc);
            t->doc = NULL;
            g_free(t->cached_page_widths);
            g_free(t->cached_page_heights);
            g_free(t->cached_page_x0);
            g_free(t->cached_page_y0);
            t->cached_page_widths = NULL;
            t->cached_page_heights = NULL;
            t->cached_page_x0 = NULL;
            t->cached_page_y0 = NULL;
            invalidate_page_cache(t);
            g_free(t->page_cache);
            t->page_cache = NULL;
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
            cancel_tab_restore(t);
            cancel_doc_model_debounce(t);
            search_cancel(t);
            search_free(t);
            if (t->zoom_scroll_source_id) {
                g_source_remove(t->zoom_scroll_source_id);
                t->zoom_scroll_source_id = 0;
            }
            if (t->page_links) {
                for (int j = 0; j < t->page_links_n; j++) {
                    if (t->page_links[j])
                        pdfr_free_links(t->doc, t->page_links[j]);
                }
                g_free(t->page_links);
                t->page_links = NULL;
                t->page_links_n = 0;
            }
            pdfr_close(t->doc);
            t->doc = NULL;
            g_free(t->cached_page_widths);
            g_free(t->cached_page_heights);
            g_free(t->cached_page_x0);
            g_free(t->cached_page_y0);
            t->cached_page_widths = NULL;
            t->cached_page_heights = NULL;
            t->cached_page_x0 = NULL;
            t->cached_page_y0 = NULL;
            invalidate_page_cache(t);
            g_free(t->page_cache);
            t->page_cache = NULL;
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

    if (app.right_file_info_popover && gtk_widget_get_mapped(app.right_file_info_popover)) {
        TabData *rtab = tab;
        gchar *basename = rtab && rtab->current_file ? g_path_get_basename(rtab->current_file) : NULL;
        gchar *text = basename ? g_strdup_printf("Name: %s", basename) : g_strdup("Name: (no file)");
        gtk_label_set_text(GTK_LABEL(app.right_popover_name_label), text);
        g_free(text);
        g_free(basename);

        text = rtab && rtab->current_file ? g_strdup_printf("Path: %s", rtab->current_file) : g_strdup("Path: (none)");
        gtk_label_set_text(GTK_LABEL(app.right_popover_path_label), text);
        g_free(text);

        if (rtab && rtab->current_file) {
            GFile *gf = g_file_new_for_path(rtab->current_file);
            GFileInfo *info = g_file_query_info(gf, G_FILE_ATTRIBUTE_STANDARD_SIZE,
                                                 G_FILE_QUERY_INFO_NONE, NULL, NULL);
            if (info) {
                gchar *size_str = format_file_size(g_file_info_get_size(info));
                gchar *size_text = g_strdup_printf("Size: %s", size_str);
                gtk_label_set_text(GTK_LABEL(app.right_popover_size_label), size_text);
                g_free(size_text);
                g_free(size_str);
                g_object_unref(info);
            } else {
                gtk_label_set_text(GTK_LABEL(app.right_popover_size_label), "Size: Unknown");
            }
            g_object_unref(gf);

            if (rtab->doc) {
                gchar *pages_text = g_strdup_printf("Pages: %d", pdfr_count_pages(rtab->doc));
                gtk_label_set_text(GTK_LABEL(app.right_popover_pages_label), pages_text);
                g_free(pages_text);
            } else {
                gtk_label_set_text(GTK_LABEL(app.right_popover_pages_label), "Pages: N/A");
            }
        } else {
            gtk_label_set_text(GTK_LABEL(app.right_popover_size_label), "Size: (none)");
            gtk_label_set_text(GTK_LABEL(app.right_popover_pages_label), "Pages: (none)");
        }
    }
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