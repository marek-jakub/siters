#include "sessions_sidebar.h"
#include "app.h"
#include "state.h"
#include "ui/sidebar.h"

extern App app;

void reset_sessions_tree_selection_guard(void) {
    g_clear_pointer(&app.last_tree_selection_key, g_free);
}

void update_sessions_tree_document_selection_for_tab(TabData *tab) {
    if (!app.sessions_tree_store || !app.sessions_tree_view || !app.current_selected_session) return;

    if (!tab || !tab->current_file) return;

    char *uri = g_filename_to_uri(tab->current_file, NULL, NULL);
    if (!uri) return;

    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(app.sessions_tree_view));
    GtkTreeIter iter;
    if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(app.sessions_tree_store), &iter)) {
        do {
            gchar *name = NULL;
            gtk_tree_model_get(GTK_TREE_MODEL(app.sessions_tree_store), &iter,
                               SESSION_COL_SESSION_NAME, &name, -1);
            if (!name) continue;
            if (g_strcmp0(name, app.current_selected_session) != 0) {
                g_free(name);
                continue;
            }
            g_free(name);
            /* Found the session row. Select the matching document child. */
            GtkTreeIter child;
            if (gtk_tree_model_iter_children(GTK_TREE_MODEL(app.sessions_tree_store), &child, &iter)) {
                do {
                    gchar *doc_uri = NULL;
                    gtk_tree_model_get(GTK_TREE_MODEL(app.sessions_tree_store), &child,
                                       SESSION_COL_DOC_URI, &doc_uri, -1);
                    if (doc_uri && g_strcmp0(doc_uri, uri) == 0) {
                        GtkTreePath *cp = gtk_tree_model_get_path(GTK_TREE_MODEL(app.sessions_tree_store), &child);
                        if (cp) {
                            gtk_tree_selection_select_path(sel, cp);
                            gtk_tree_path_free(cp);
                        }
                        g_free(doc_uri);
                        g_free(uri);
                        return;
                    }
                    g_free(doc_uri);
                } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(app.sessions_tree_store), &child));
            }
            break;
        } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(app.sessions_tree_store), &iter));
    }
    g_free(uri);
}

static void update_sessions_tree_document_selection(void) {
    update_sessions_tree_document_selection_for_tab(get_current_left_tab());
}

void on_sessions_toggled(GtkToggleButton *btn, gpointer user_data) {
    (void)user_data;

    if (!gtk_toggle_button_get_active(btn)) {
        /* Toggled off: close sidebar if we are the active mode */
        if (app.current_sidebar_mode == SIDEBAR_SESSIONS) {
            gtk_container_remove(GTK_CONTAINER(app.main_hbox), app.sidebar);
            gtk_box_reorder_child(GTK_BOX(app.main_hbox), app.content_vbox, 1);
            app.current_sidebar_mode = SIDEBAR_NONE;
        }
        return;
    }

    /* Toggled on: open sessions sidebar, deactivate other buttons */
    if (gtk_widget_get_parent(app.sidebar) != NULL) {
        gtk_container_remove(GTK_CONTAINER(app.main_hbox), app.sidebar);
    }

    /* Deactivate other toggle buttons */
    g_signal_handlers_block_by_func(app.toc_btn, G_CALLBACK(on_toc_toggled), NULL);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app.toc_btn), FALSE);
    g_signal_handlers_unblock_by_func(app.toc_btn, G_CALLBACK(on_toc_toggled), NULL);

    g_signal_handlers_block_by_func(app.settings_btn, G_CALLBACK(on_settings_toggled), NULL);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app.settings_btn), FALSE);
    g_signal_handlers_unblock_by_func(app.settings_btn, G_CALLBACK(on_settings_toggled), NULL);

    g_signal_handlers_block_by_func(app.file_info_btn, G_CALLBACK(on_left_file_info_toggled), NULL);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app.file_info_btn), FALSE);
    g_signal_handlers_unblock_by_func(app.file_info_btn, G_CALLBACK(on_left_file_info_toggled), NULL);

    /* Hide other sidebar contents */
    gtk_widget_hide(app.sidebar_label);
    gtk_widget_hide(app.sessions_container);
    gtk_widget_hide(app.settings_container);
    gtk_widget_hide(app.toc_container);
    gtk_widget_hide(app.file_info_container);
    gtk_tree_store_clear(app.toc_tree_store);

    /* Show sessions container */
    gtk_widget_show_all(app.sessions_container);

    gtk_box_pack_start(GTK_BOX(app.main_hbox), app.sidebar, FALSE, FALSE, 0);
    gtk_box_reorder_child(GTK_BOX(app.main_hbox), app.content_vbox, 2);
    gtk_widget_set_size_request(app.sidebar, 300, -1);
    gtk_widget_show(app.sidebar);
    app.current_sidebar_mode = SIDEBAR_SESSIONS;

    /* Auto-select current session and document in the tree */
    if (app.sessions_tree_store && app.current_selected_session) {
        reset_sessions_tree_selection_guard();
        app.sessions_tree_syncing = TRUE;
        GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(app.sessions_tree_view));
        GtkTreeIter iter;
        if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(app.sessions_tree_store), &iter)) {
            do {
                gchar *name = NULL;
                gtk_tree_model_get(GTK_TREE_MODEL(app.sessions_tree_store), &iter,
                                   SESSION_COL_SESSION_NAME, &name, -1);
                if (!name) continue;
                if (g_strcmp0(name, app.current_selected_session) != 0) {
                    g_free(name);
                    continue;
                }
                /* Found matching session row — select and expand it */
                GtkTreePath *path = gtk_tree_model_get_path(GTK_TREE_MODEL(app.sessions_tree_store), &iter);
                if (path) {
                    gtk_tree_selection_select_path(sel, path);
                    gtk_tree_view_expand_row(GTK_TREE_VIEW(app.sessions_tree_view), path, FALSE);
                    gtk_tree_path_free(path);
                }
                update_sessions_tree_document_selection();
                g_free(name);
                break;
            } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(app.sessions_tree_store), &iter));
        }
        app.sessions_tree_syncing = FALSE;
    }
}
