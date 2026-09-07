#include <gtk/gtk.h>
#include <string.h>
#include "app.h"
#include "state.h"
#include "sessions_sidebar.h"
#include "sessions_model.h"
#include "session_model.h"
#include "nav.h"
#include "ui/notebook.h"

extern App app;

static void switch_to_session(const char *session_name) {
    if (!session_name || !*session_name) return;

    if (app.current_selected_session && g_strcmp0(app.current_selected_session, session_name) == 0) {
        return;
    }

    if (app.current_selected_session) {
        save_open_tabs_for_session(app.current_selected_session);
        g_free(app.current_selected_session);
    }

    app.current_selected_session = g_strdup(session_name);
    restore_open_tabs_for_session(session_name);
    sync_page_widget_from_tab(get_current_left_tab());
    update_window_title_for_session(app.current_selected_session);

    if (app.sessions_model) {
        sessions_model_set_last_open_session(app.sessions_model, session_name);
    }

    /* Sync color buttons to switched session's colors */
    session_model_t *session = get_current_session_model();
    if (session) {
        const char *pc = session_model_get_page_color(session);
        const char *hpc = session_model_get_helper_page_color(session);

        if (app.left_color_btn) {
            g_signal_handlers_block_by_func(app.left_color_btn, G_CALLBACK(on_left_color_set), NULL);
            if (pc) {
                GdkRGBA c;
                if (gdk_rgba_parse(&c, pc))
                    gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(app.left_color_btn), &c);
            }
            g_signal_handlers_unblock_by_func(app.left_color_btn, G_CALLBACK(on_left_color_set), NULL);
            apply_page_color_to_notebook(app.left_notebook, pc ? pc : "#FFFFFF");
        }
        if (app.right_color_btn) {
            g_signal_handlers_block_by_func(app.right_color_btn, G_CALLBACK(on_right_color_set), NULL);
            if (hpc) {
                GdkRGBA c;
                if (gdk_rgba_parse(&c, hpc))
                    gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(app.right_color_btn), &c);
            }
            g_signal_handlers_unblock_by_func(app.right_color_btn, G_CALLBACK(on_right_color_set), NULL);
            apply_page_color_to_notebook(app.right_notebook, hpc ? hpc : "#FFFFFF");
        }
    }
}

void populate_sessions_treeview(void) {
    if (!app.sessions_tree_store || !app.sessions_model) return;

    app.sessions_tree_syncing = TRUE;
    reset_sessions_tree_selection_guard();

    /* Save expanded session rows before clearing */
    GPtrArray *expanded = g_ptr_array_new_with_free_func(g_free);
    GtkTreeIter iter;
    if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(app.sessions_tree_store), &iter)) {
        do {
            char *name = NULL;
            gtk_tree_model_get(GTK_TREE_MODEL(app.sessions_tree_store), &iter,
                               SESSION_COL_SESSION_NAME, &name, -1);
            if (name) {
                GtkTreePath *path = gtk_tree_model_get_path(GTK_TREE_MODEL(app.sessions_tree_store), &iter);
                if (path) {
                    if (gtk_tree_view_row_expanded(GTK_TREE_VIEW(app.sessions_tree_view), path)) {
                        g_ptr_array_add(expanded, name);
                    } else {
                        g_free(name);
                    }
                    gtk_tree_path_free(path);
                } else {
                    g_free(name);
                }
            }
        } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(app.sessions_tree_store), &iter));
    }

    gtk_tree_store_clear(app.sessions_tree_store);

    const GList *session_names = sessions_model_get_session_names(app.sessions_model);
    for (const GList *s = session_names; s; s = s->next) {
        const char *session_name = (const char *)s->data;
        GtkTreeIter parent;

        gtk_tree_store_append(app.sessions_tree_store, &parent, NULL);
        gtk_tree_store_set(
            app.sessions_tree_store, &parent,
            SESSION_COL_LABEL, session_name,
            SESSION_COL_ROW_KIND, SESSION_ROW_SESSION,
            SESSION_COL_SESSION_NAME, session_name,
            SESSION_COL_DOC_URI, "",
            -1
        );

        session_model_t *session = g_hash_table_lookup(app.session_models, session_name);
        if (!session) continue;

        // ONLY document_urls (left notebook docs), no helper_document_urls
        const GList *docs = session_model_get_document_urls(session);
        for (const GList *d = docs; d; d = d->next) {
            const char *uri = (const char *)d->data;
            GtkTreeIter child;

            char *filename = g_filename_from_uri(uri, NULL, NULL);
            char *basename = filename ? g_path_get_basename(filename) : g_strdup(uri);

            gtk_tree_store_append(app.sessions_tree_store, &child, &parent);
            gtk_tree_store_set(
                app.sessions_tree_store, &child,
                SESSION_COL_LABEL, basename,
                SESSION_COL_ROW_KIND, SESSION_ROW_FILE,
                SESSION_COL_SESSION_NAME, session_name,
                SESSION_COL_DOC_URI, uri,
                -1
            );

            g_free(basename);
            g_free(filename);
        }
    }

    /* Restore expanded session rows */
    for (guint i = 0; i < expanded->len; i++) {
        const char *name = g_ptr_array_index(expanded, i);
        GtkTreeIter row;
        if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(app.sessions_tree_store), &row)) {
            do {
                char *row_name = NULL;
                gtk_tree_model_get(GTK_TREE_MODEL(app.sessions_tree_store), &row,
                                   SESSION_COL_SESSION_NAME, &row_name, -1);
                if (row_name && strcmp(row_name, name) == 0) {
                    GtkTreePath *path = gtk_tree_model_get_path(GTK_TREE_MODEL(app.sessions_tree_store), &row);
                    if (path) {
                        gtk_tree_view_expand_row(GTK_TREE_VIEW(app.sessions_tree_view), path, FALSE);
                        gtk_tree_path_free(path);
                    }
                    g_free(row_name);
                    break;
                }
                g_free(row_name);
            } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(app.sessions_tree_store), &row));
        }
    }
    g_ptr_array_unref(expanded);

    /* Auto-select current session + document when sidebar is visible */
    if (app.current_sidebar_mode == SIDEBAR_SESSIONS && app.current_selected_session) {
        reset_sessions_tree_selection_guard();
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
                GtkTreePath *path = gtk_tree_model_get_path(GTK_TREE_MODEL(app.sessions_tree_store), &iter);
                if (path) {
                    gtk_tree_selection_select_path(sel, path);
                    gtk_tree_view_expand_row(GTK_TREE_VIEW(app.sessions_tree_view), path, FALSE);
                    gtk_tree_path_free(path);
                }
                TabData *tab = get_current_left_tab();
                if (tab && tab->current_file) {
                    char *uri = g_filename_to_uri(tab->current_file, NULL, NULL);
                    if (uri) {
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
                                    break;
                                }
                                g_free(doc_uri);
                            } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(app.sessions_tree_store), &child));
                        }
                        g_free(uri);
                        }
                    }
                g_free(name);
                break;
            } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(app.sessions_tree_store), &iter));
        }
    }

    app.sessions_tree_syncing = FALSE;
}

void on_sessions_treeview_cursor_changed(GtkTreeView *tree_view, gpointer user_data) {
    (void)user_data;
    if (app.sessions_tree_syncing) return;

    GtkTreeSelection *sel = gtk_tree_view_get_selection(tree_view);
    GtkTreeModel *model = NULL;
    GtkTreeIter iter;
    if (!gtk_tree_selection_get_selected(sel, &model, &iter)) return;

    gint row_kind = SESSION_ROW_SESSION;
    gchar *session_name = NULL;
    gchar *doc_uri = NULL;

    gtk_tree_model_get(model, &iter,
        SESSION_COL_ROW_KIND, &row_kind,
        SESSION_COL_SESSION_NAME, &session_name,
        SESSION_COL_DOC_URI, &doc_uri,
        -1);

    if (!session_name || !*session_name) goto cleanup;

    /* Build stable key for dedupe */
    gchar *key = (row_kind == SESSION_ROW_FILE && doc_uri && *doc_uri)
        ? g_strdup_printf("F|%s|%s", session_name, doc_uri)
        : g_strdup_printf("S|%s", session_name);

    if (app.last_tree_selection_key && g_strcmp0(app.last_tree_selection_key, key) == 0) {
        g_free(key);
        goto cleanup; /* noisy repeat */
    }

    g_free(app.last_tree_selection_key);
    app.last_tree_selection_key = key;

    switch_to_session(session_name);

    if (row_kind == SESSION_ROW_FILE && doc_uri && *doc_uri) {
        int idx = find_matching_tab_index(GTK_NOTEBOOK(app.left_notebook), doc_uri);
        if (idx >= 0) {
            gtk_notebook_set_current_page(GTK_NOTEBOOK(app.left_notebook), idx);
        }
    }

cleanup:
    g_free(session_name);
    g_free(doc_uri);
}
