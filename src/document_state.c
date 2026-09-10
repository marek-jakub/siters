#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include "tab.h"
#include "app.h"
#include "document_model.h"
#include "session_model.h"
#include "view.h"
#include "view/scroll.h"
#include "document_state.h"
#include "session/state.h"
#include "ui/notebook.h"
#include "ui/tab_lifecycle.h"
#include "ui/toc.h"
#include "fileinfo/fileinfo.h"

char* make_document_key(const char *session_name, const char *uri, gboolean is_helper) {
    const char *side = is_helper ? "right" : "left";
    return g_strdup_printf("%s:%s:%s", session_name ? session_name : "Unknown", side, uri);
}

void update_document_model_from_tab(TabData *tab) {
    if (!tab || !tab->current_file || !app.document_models) return;
    if (!tab->cached_page_widths) return;

    char *uri = g_filename_to_uri(tab->current_file, NULL, NULL);
    if (!uri) return;

    char *key = make_document_key(app.current_selected_session, uri, tab->is_helper);

    // Get or create document model
    document_model_t *doc_model = g_hash_table_lookup(app.document_models, key);
    if (!doc_model) {
        doc_model = document_model_new();
        document_model_set_url(doc_model, uri);
        g_hash_table_insert(app.document_models, key, doc_model);
    } else {
        g_free(key);
    }

    // Update current state
    document_model_set_zoom(doc_model, tab->zoom);
    document_model_set_visualization_mode(doc_model, tab->layout_mode);
    document_model_set_current_page(doc_model, tab->cur_page + 1);  // Store as 1-based
    document_model_set_page_count(doc_model, tab->n_pages);

    const double spacing = 6.0;
    double scale = get_ppi_scale(tab);

    if (tab->layout_mode == 2) {
        double scroll_x = 0.0;
        if (tab->h_scrollbar) {
            GtkAdjustment *sadj = gtk_range_get_adjustment(GTK_RANGE(tab->h_scrollbar));
            scroll_x = gtk_adjustment_get_value(sadj);
        }
        document_model_set_scroll_offset(doc_model, scroll_x);
        double x_top = spacing;
        double page_w = 0;
        for (int i = 0; i < tab->cur_page; ++i) {
            x_top += tab->cached_page_widths[i] * scale + spacing;
        }
        page_w = tab->cached_page_widths[tab->cur_page] * scale;
        double intra = scroll_x - x_top;
        double fraction = page_w > 0 ? intra / page_w : 0.0;
        if (fraction < 0) fraction = 0;
        if (fraction > 1) fraction = 1;
        document_model_set_intra_page_fraction(doc_model, fraction);
    } else {
        GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(tab->scrolled));
        document_model_set_scroll_offset(doc_model, gtk_adjustment_get_value(vadj));

        double scroll_y = gtk_adjustment_get_value(vadj);
        double y_top = spacing;
        double page_h = 0;
        if (tab->layout_mode == 1) {
            int cur_row = tab->cur_page / 2;
            for (int r = 0; r < cur_row; r++) {
                double row_h = 0.0;
                for (int p = 0; p < 2; p++) {
                    int idx = r * 2 + p;
                    if (idx >= tab->n_pages) break;
                    double h = tab->cached_page_heights[idx] * scale;
                    if (h > row_h) row_h = h;
                }
                y_top += row_h + spacing;
            }
            page_h = tab->cached_page_heights[tab->cur_page] * scale;
        } else {
            for (int i = 0; i < tab->cur_page; ++i) {
                y_top += tab->cached_page_heights[i] * scale + spacing;
            }
            page_h = tab->cached_page_heights[tab->cur_page] * scale;
        }

        double intra = scroll_y - y_top;
        double fraction = page_h > 0 ? intra / page_h : 0.0;
        if (fraction < 0) fraction = 0;
        if (fraction > 1) fraction = 1;
        document_model_set_intra_page_fraction(doc_model, fraction);
    }

    g_free(uri);
}

void restore_document_model_to_tab(TabData *tab) {
    if (!tab || !tab->current_file) return;

    /* A freshly opened document is treated as brand new: it must never
       inherit saved state (page/zoom/layout), whether left over from a past
       use of the same file in this session, or carried over from another
       session's saved data. Start at the first page with default settings. */
    if (tab->fresh_open) {
        tab->cur_page = 0;
        tab->zoom = 96.0;
        start_initial_scroll_restore(tab, 0, 96.0, 0.0);
        return;
    }

    if (!app.document_models) return;

    char *uri = g_filename_to_uri(tab->current_file, NULL, NULL);
    if (!uri) return;

    char *key = make_document_key(app.current_selected_session, uri, tab->is_helper);
    document_model_t *doc_model = g_hash_table_lookup(app.document_models, key);
    g_free(key);
    if (doc_model) {
        int saved_page = document_model_get_current_page(doc_model);
        double saved_zoom = document_model_get_zoom(doc_model);
        double saved_fraction = document_model_get_intra_page_fraction(doc_model);
        tab->layout_mode = document_model_get_visualization_mode(doc_model);
        tab->zoom = saved_zoom;

        /* Clamp page to valid range */
        if (saved_page < 1) saved_page = 1;
        if (saved_page > tab->n_pages) saved_page = tab->n_pages;

        /* Set cur_page immediately so it's available even if the async restore runs later */
        tab->cur_page = saved_page - 1;

        /* Initiate the robust, layout-aware scroll restore */
        start_initial_scroll_restore(tab, saved_page - 1, saved_zoom, saved_fraction);
    } else {
        tab->cur_page = 0;
        /* No saved state, restore to first page top */
        start_initial_scroll_restore(tab, 0, 96.0, 0.0);
    }

    g_free(uri);
}


void set_right_notebook_session(const gchar *session_name) {
    if (!app.right_notebook || !session_name || !app.session_models) return;

    app.is_restoring_session_tabs = TRUE;

    // Clear current right notebook
    while (gtk_notebook_get_n_pages(GTK_NOTEBOOK(app.right_notebook)) > 0) {
        gtk_notebook_remove_page(GTK_NOTEBOOK(app.right_notebook), 0);
    }
    gtk_widget_hide(app.right_page_nav_overlay);

    // Get the session model
    session_model_t *session = g_hash_table_lookup(app.session_models, session_name);
    if (!session) {
        app.is_restoring_session_tabs = FALSE;
        return;
    }

    const char *last_read_help_uri = session_model_get_last_read_help_document(session);

    // Restore saved helper documents in right notebook
    const GList *helper_docs = session_model_get_helper_document_urls(session);
    int matched_index = -1;
    int index = 0;
    for (const GList *iter = helper_docs; iter != NULL; iter = iter->next, index++) {
        const char *uri = (const char*)iter->data;
        char *filename = g_filename_from_uri(uri, NULL, NULL);
        if (filename) {
            TabData *tab = create_new_tab(app.right_notebook);
            if (tab) {
                load_file_into_tab(tab, filename);
                if (last_read_help_uri && g_strcmp0(uri, last_read_help_uri) == 0) {
                    matched_index = index;
                }
            }
            g_free(filename);
        }
    }

    // Re-focus last-read helper tab
    if (matched_index >= 0 && matched_index < gtk_notebook_get_n_pages(GTK_NOTEBOOK(app.right_notebook))) {
        gtk_notebook_set_current_page(GTK_NOTEBOOK(app.right_notebook), matched_index);
    }

    // If no helper documents, show an empty right notebook
    if (gtk_notebook_get_n_pages(GTK_NOTEBOOK(app.right_notebook)) == 0) {
        gtk_widget_show_all(app.right_notebook);
    }

    app.is_restoring_session_tabs = FALSE;
}


void save_open_tabs_for_session(const char *session_name) {
    if (!session_name || !app.session_models) return;

    session_model_t *session = g_hash_table_lookup(app.session_models, session_name);
    if (!session) return;

    // Replace current saved document URLs with the currently open tabs.
    if (session->document_urls) {
        g_list_free_full(session->document_urls, g_free);
        session->document_urls = NULL;
    }
    if (session->helper_document_urls) {
        g_list_free_full(session->helper_document_urls, g_free);
        session->helper_document_urls = NULL;
    }

    // Save open tabs from left notebook
    if (app.left_notebook) {
        int n_pages = gtk_notebook_get_n_pages(GTK_NOTEBOOK(app.left_notebook));
        for (int i = 0; i < n_pages; i++) {
            GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(app.left_notebook), i);
            if (page) {
                TabData *tab = g_object_get_data(G_OBJECT(page), "tab-data");
                if (tab && tab->current_file) {
                    update_document_model_from_tab(tab);
                    char *uri = g_filename_to_uri(tab->current_file, NULL, NULL);
                    if (uri) {
                        session_model_add_document_url(session, uri);
                        g_free(uri);
                    }
                }
            }
        }
    }

    // Save open tabs from right notebook
    if (app.right_notebook) {
        int n_pages = gtk_notebook_get_n_pages(GTK_NOTEBOOK(app.right_notebook));
        for (int i = 0; i < n_pages; i++) {
            GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(app.right_notebook), i);
            if (page) {
                TabData *tab = g_object_get_data(G_OBJECT(page), "tab-data");
                if (tab && tab->current_file) {
                    update_document_model_from_tab(tab);
                    char *uri = g_filename_to_uri(tab->current_file, NULL, NULL);
                    if (uri) {
                        session_model_add_helper_document_url(session, uri);
                        g_free(uri);
                    }
                }
            }
        }
    }

    /* Also snapshot the currently focused tabs as last-read markers */
    if (app.left_notebook) {
        int cur = gtk_notebook_get_current_page(GTK_NOTEBOOK(app.left_notebook));
        if (cur >= 0) {
            GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(app.left_notebook), cur);
            if (page) {
                update_last_read_for_notebook(GTK_NOTEBOOK(app.left_notebook), page, (guint)cur);
            }
        } else {
            session_model_set_last_read_document(session, "");
        }
    }

    if (app.right_notebook) {
        int cur = gtk_notebook_get_current_page(GTK_NOTEBOOK(app.right_notebook));
        if (cur >= 0) {
            GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(app.right_notebook), cur);
            if (page) {
                update_last_read_for_notebook(GTK_NOTEBOOK(app.right_notebook), page, (guint)cur);
            }
        } else {
            session_model_set_last_read_help_document(session, "");
        }
    }
}

void restore_open_tabs_for_session(const char *session_name) {
    if (!session_name || !app.session_models) return;

    session_model_t *session = g_hash_table_lookup(app.session_models, session_name);
    if (!session) {
        session = session_model_new();
        session_model_set_session_name(session, session_name);
        g_hash_table_insert(app.session_models, g_strdup(session_name), session);
    }

    app.is_restoring_session_tabs = TRUE;

    // Clear current notebooks
    if (app.left_notebook) {
        int n_pages = gtk_notebook_get_n_pages(GTK_NOTEBOOK(app.left_notebook));
        for (int i = n_pages - 1; i >= 0; i--) {
            gtk_notebook_remove_page(GTK_NOTEBOOK(app.left_notebook), i);
        }
    }

    if (app.right_notebook) {
        int n_pages = gtk_notebook_get_n_pages(GTK_NOTEBOOK(app.right_notebook));
        for (int i = n_pages - 1; i >= 0; i--) {
            gtk_notebook_remove_page(GTK_NOTEBOOK(app.right_notebook), i);
        }
    }

    const char *last_read_uri = session_model_get_last_read_document(session);
    const char *last_read_help_uri = session_model_get_last_read_help_document(session);

    int matched_left_index = -1;
    int matched_right_index = -1;
    int left_index = 0;
    int right_index = 0;

    // Pass 1: create placeholder tabs for all saved documents (fast, no parsing).
    // The focused (last-read) tabs are loaded synchronously in pass 2 so the user
    // immediately sees content; the remaining tabs are opened progressively at idle
    // priority so switching sessions does not freeze the UI on large document sets.
    const GList *docs = session_model_get_document_urls(session);
    for (const GList *iter = docs; iter != NULL; iter = iter->next) {
        char *uri = (char *)iter->data;
        char *filename = g_filename_from_uri(uri, NULL, NULL);
        if (!filename && uri && g_path_is_absolute(uri)) {
            filename = g_strdup(uri);
        }

        if (filename) {
            TabData *tab = create_new_tab(app.left_notebook);
            if (tab) {
                set_tab_filename(tab, filename);
                if (last_read_uri && g_strcmp0(uri, last_read_uri) == 0) {
                    matched_left_index = left_index;
                }
                left_index++;
            }
            g_free(filename);
        } else {
            g_warning("Failed to restore saved document path: %s", uri ? uri : "(null)");
        }
    }

    // Restore saved helper documents in right notebook
    const GList *helper_docs = session_model_get_helper_document_urls(session);
    for (const GList *iter = helper_docs; iter != NULL; iter = iter->next) {
        char *uri = (char *)iter->data;
        char *filename = g_filename_from_uri(uri, NULL, NULL);
        if (!filename && uri && g_path_is_absolute(uri)) {
            filename = g_strdup(uri);
        }
        if (filename) {
            TabData *tab = create_new_tab(app.right_notebook);
            if (tab) {
                set_tab_filename(tab, filename);
                if (last_read_help_uri && g_strcmp0(uri, last_read_help_uri) == 0) {
                    matched_right_index = right_index;
                }
                right_index++;
            }
            g_free(filename);
        } else {
            g_warning("Failed to restore saved document path: %s", uri ? uri : "(null)");
        }
    }

    app.is_restoring_session_tabs = FALSE;

    int n_left = app.left_notebook ? gtk_notebook_get_n_pages(GTK_NOTEBOOK(app.left_notebook)) : 0;
    int n_right = app.right_notebook ? gtk_notebook_get_n_pages(GTK_NOTEBOOK(app.right_notebook)) : 0;

    int focus_left = (matched_left_index >= 0) ? matched_left_index : (n_left > 0 ? 0 : -1);
    int focus_right = (matched_right_index >= 0) ? matched_right_index : (n_right > 0 ? 0 : -1);

    // Pass 2: synchronously load the focused tabs so content appears immediately;
    // defer all other tabs to idle-time progressive loading.
    for (int i = 0; i < n_left; i++) {
        GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(app.left_notebook), i);
        TabData *t = page ? g_object_get_data(G_OBJECT(page), "tab-data") : NULL;
        if (!t) continue;
        if (i == focus_left)
            open_document_in_tab(t);
        else
            schedule_tab_deferred_load(t);
    }

    for (int i = 0; i < n_right; i++) {
        GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(app.right_notebook), i);
        TabData *t = page ? g_object_get_data(G_OBJECT(page), "tab-data") : NULL;
        if (!t) continue;
        if (i == focus_right)
            open_document_in_tab(t);
        else
            schedule_tab_deferred_load(t);
    }

    // Re-focus last-read tabs (fires the notebook switch handler with loaded docs)
    if (app.left_notebook && focus_left >= 0 && focus_left < n_left) {
        gtk_notebook_set_current_page(GTK_NOTEBOOK(app.left_notebook), focus_left);
    }

    if (app.right_notebook && focus_right >= 0 && focus_right < n_right) {
        gtk_notebook_set_current_page(GTK_NOTEBOOK(app.right_notebook), focus_right);
    }

    /* Refresh sidebar state for the restored current tabs. The switch handler
       above may not fire if the focused tab was already current, so do it here. */
    if (app.current_sidebar_mode == SIDEBAR_TOC) populate_toc_treeview();
    if (app.current_sidebar_mode == SIDEBAR_FILE_INFO) update_file_info_labels(get_current_left_tab());
}

static gboolean deferred_update_document_model(gpointer data) {
    TabData *tab = data;
    tab->scroll_doc_debounce_id = 0;
    if (!tab || !tab->cached_page_widths) return G_SOURCE_REMOVE;
    update_document_model_from_tab(tab);
    if (app.current_sidebar_mode == SIDEBAR_TOC) {
        update_toc_selection_for_current_page(tab);
    }
    return G_SOURCE_REMOVE;
}

void cancel_doc_model_debounce(TabData *tab) {
    if (tab && tab->scroll_doc_debounce_id) {
        g_source_remove(tab->scroll_doc_debounce_id);
        tab->scroll_doc_debounce_id = 0;
    }
}

void schedule_doc_model_update(TabData *tab) {
    if (!tab) return;
    cancel_doc_model_debounce(tab);
    tab->scroll_doc_debounce_id = g_timeout_add(400, deferred_update_document_model, tab);
}
