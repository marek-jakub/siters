/* Window/session action handlers, wired up by create_main_window in siters.c. */

#include "sessions_actions.h"
#include "app.h"
#include "state.h"
#include "sessions_sidebar.h"
#include "sessions_tree.h"
#include "document_state.h"
#include "nav.h"
#include "ui/layout.h"
#include "ui/theme.h"
#include "session_model.h"
#include "sessions_model.h"

void on_title_bar_toggle(GtkToggleButton *button, gpointer user_data) {
    (void)user_data;
    gboolean active = gtk_toggle_button_get_active(button);
    GtkWidget *btn = GTK_WIDGET(button);
    if (active) {
        gtk_button_set_image(GTK_BUTTON(btn), create_toolbar_icon("title-bar-on"));
        gtk_window_set_decorated(GTK_WINDOW(app.window), TRUE);
    } else {
        gtk_button_set_image(GTK_BUTTON(btn), create_toolbar_icon("title-bar-off"));
        gboolean was_maximized = gtk_window_is_maximized(GTK_WINDOW(app.window));
        if (was_maximized) {
            gtk_window_unmaximize(GTK_WINDOW(app.window));
        }
        gtk_window_set_decorated(GTK_WINDOW(app.window), FALSE);
        if (was_maximized) {
            gtk_window_maximize(GTK_WINDOW(app.window));
        }
    }
    // Force layout update after changing decoration
    gtk_widget_queue_resize(app.window);
}

void on_helper_toggle(GtkToggleButton *button, gpointer user_data) {
    (void)user_data;
    gboolean active = gtk_toggle_button_get_active(button);
    GtkWidget *btn = GTK_WIDGET(button);

    if (active) {
        gtk_button_set_image(GTK_BUTTON(btn), create_toolbar_icon("sidebar-helper-on"));
        if (app.right_pane) {
            gtk_widget_show_all(GTK_WIDGET(app.right_pane));
            set_right_notebook_session(app.current_selected_session ? app.current_selected_session : "Default");
            sync_right_layout_buttons(get_current_right_tab());
        }
    } else {
        // Save current helper tabs before hiding
        if (app.current_selected_session) {
            save_open_tabs_for_session(app.current_selected_session);
        }
        gtk_button_set_image(GTK_BUTTON(btn), create_toolbar_icon("sidebar-helper-off"));
        if (app.right_pane) {
            gtk_widget_hide(GTK_WIDGET(app.right_pane));
        }
    }
}

void on_minimize_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    GtkWindow *win = GTK_WINDOW(user_data);
    gtk_window_iconify(win);
}

static gboolean defer_maximize_toggle(gpointer user_data) {
    GtkWindow *win = GTK_WINDOW(user_data);
    app.maximize_pending_id = 0;
    if (win) {
        if (gtk_window_is_maximized(win))
            gtk_window_unmaximize(win);
        else
            gtk_window_maximize(win);
        if (app.left_notebook) gtk_widget_queue_resize(app.left_notebook);
        if (app.right_notebook) gtk_widget_queue_resize(app.right_notebook);
    }
    g_object_unref(win);
    return FALSE;
}

void on_maximize_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    GtkWindow *win = GTK_WINDOW(user_data);
    if (!win || app.maximize_pending_id) return;
    g_object_ref(win);
    app.maximize_pending_id = g_idle_add(defer_maximize_toggle, win);
}

void on_close_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    save_state();
    gtk_main_quit();
}

void on_sessions_add_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;

    const char *session_name = gtk_entry_get_text(GTK_ENTRY(app.sessions_entry));
    if (session_name && strlen(session_name) > 0) {
        // Check if session name already exists
        const GList *existing_sessions = sessions_model_get_session_names(app.sessions_model);
        for (const GList *iter = existing_sessions; iter != NULL; iter = iter->next) {
            if (strcmp((const char*)iter->data, session_name) == 0) {
                // Show error dialog
                GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(app.window),
                    GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                    GTK_MESSAGE_ERROR,
                    GTK_BUTTONS_OK,
                    "A session with the name '%s' already exists. Please choose a different name.",
                    session_name);
                gtk_dialog_run(GTK_DIALOG(dialog));
                gtk_widget_destroy(dialog);
                return;
            }
        }

        // Add to model
        sessions_model_add_session_name(app.sessions_model, session_name);

        // Create per-session model so notebook restore can find it
        if (app.session_models && !g_hash_table_lookup(app.session_models, session_name)) {
            session_model_t *session = session_model_new();
            session_model_set_session_name(session, session_name);
            g_hash_table_insert(app.session_models, g_strdup(session_name), session);
        }

        // Update tree view
        populate_sessions_treeview();

        // Clear entry
        gtk_entry_set_text(GTK_ENTRY(app.sessions_entry), "");

        // Save state to persist the new session
        save_state();
    }
}

void on_sessions_remove_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;

    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(app.sessions_tree_view));
    GtkTreeIter iter;
    GtkTreeModel *model;

    if (gtk_tree_selection_get_selected(selection, &model, &iter)) {
        gchar *session_name;
        gtk_tree_model_get(model, &iter, SESSION_COL_SESSION_NAME, &session_name, -1);

        // Prevent removing the "Default" session
        if (strcmp(session_name, "Default") == 0) {
            // Show a message dialog that Default session cannot be removed
            GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(app.window),
                GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                GTK_MESSAGE_INFO,
                GTK_BUTTONS_OK,
                "The 'Default' session cannot be removed.");
            gtk_dialog_run(GTK_DIALOG(dialog));
            gtk_widget_destroy(dialog);
            g_free(session_name);
            return;
        }

        // Remove from model
        sessions_model_remove_session_name(app.sessions_model, session_name);

        // Remove stale document models for this session
        if (app.session_models && app.document_models) {
            session_model_t *sm = g_hash_table_lookup(app.session_models, session_name);
            if (sm) {
                const GList *iter;
                for (iter = session_model_get_document_urls(sm); iter; iter = iter->next) {
                    char *key = make_document_key(session_name, (const char *)iter->data, FALSE);
                    g_hash_table_remove(app.document_models, key);
                    g_free(key);
                }
                for (iter = session_model_get_helper_document_urls(sm); iter; iter = iter->next) {
                    char *key = make_document_key(session_name, (const char *)iter->data, TRUE);
                    g_hash_table_remove(app.document_models, key);
                    g_free(key);
                }
            }
        }

        // Remove from session_models hash table so stale data doesn't persist
        if (app.session_models)
            g_hash_table_remove(app.session_models, session_name);

        // Update tree view
        populate_sessions_treeview();

        // If the removed session is currently selected, switch to "Default" or first remaining session
        if (app.current_selected_session && strcmp(app.current_selected_session, session_name) == 0) {
            g_free(app.current_selected_session);
            app.current_selected_session = g_strdup("Default"); // or first remaining session

            restore_open_tabs_for_session(app.current_selected_session);
            sync_page_widget_from_tab(get_current_left_tab());

            if (app.sessions_model) {
                sessions_model_set_last_open_session(app.sessions_model, app.current_selected_session);
            }

            update_window_title_for_session(app.current_selected_session);
        }

        g_free(session_name);

        // Save state to persist the removed session
        save_state();
    }
}

void on_sessions_update_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;

    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(app.sessions_tree_view));
    GtkTreeIter iter;
    GtkTreeModel *model;

    if (gtk_tree_selection_get_selected(selection, &model, &iter)) {
        const char *new_name = gtk_entry_get_text(GTK_ENTRY(app.sessions_entry));
        if (new_name && strlen(new_name) > 0) {
            gchar *old_name;
            gtk_tree_model_get(model, &iter, SESSION_COL_SESSION_NAME, &old_name, -1);

            // Prevent renaming the "Default" session
            if (strcmp(old_name, "Default") == 0) {
                GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(app.window),
                    GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                    GTK_MESSAGE_INFO,
                    GTK_BUTTONS_OK,
                    "The 'Default' session cannot be renamed.");
                gtk_dialog_run(GTK_DIALOG(dialog));
                gtk_widget_destroy(dialog);
                g_free(old_name);
                return;
            }

            // Check if new name already exists (excluding the current session)
            const GList *existing_sessions = sessions_model_get_session_names(app.sessions_model);
            for (const GList *iter_check = existing_sessions; iter_check != NULL; iter_check = iter_check->next) {
                if (strcmp((const char*)iter_check->data, old_name) != 0 &&
                    strcmp((const char*)iter_check->data, new_name) == 0) {
                    // Show error dialog
                    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(app.window),
                        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                        GTK_MESSAGE_ERROR,
                        GTK_BUTTONS_OK,
                        "A session with the name '%s' already exists. Please choose a different name.",
                        new_name);
                    gtk_dialog_run(GTK_DIALOG(dialog));
                    gtk_widget_destroy(dialog);
                    g_free(old_name);
                    return;
                }
            }

            // Migrate the session model in the hash table to the new key
            session_model_t *session_model = g_hash_table_lookup(app.session_models, old_name);
            if (session_model) {
                session_model_set_session_name(session_model, new_name);
                g_hash_table_steal(app.session_models, old_name);
                g_hash_table_insert(app.session_models, g_strdup(new_name), session_model);

                // Re-key all document_models entries that embed the old session name
                if (app.document_models) {
                    GList *keys = g_hash_table_get_keys(app.document_models);
                    GList *to_rekey = NULL;
                    int old_len = strlen(old_name);
                    for (GList *k = keys; k; k = k->next) {
                        char *key = (char *)k->data;
                        if (strncmp(key, old_name, old_len) == 0 && key[old_len] == ':') {
                            to_rekey = g_list_prepend(to_rekey, key);
                        }
                    }
                    g_list_free(keys);
                    for (GList *k = to_rekey; k; k = k->next) {
                        char *old_key = (char *)k->data;
                        document_model_t *doc = g_hash_table_lookup(app.document_models, old_key);
                        if (doc) {
                            char *new_key = g_strdup_printf("%s%s", new_name, old_key + old_len);
                            g_hash_table_steal(app.document_models, old_key);
                            g_hash_table_insert(app.document_models, new_key, doc);
                            g_free(old_key);
                        }
                    }
                    g_list_free(to_rekey);
                }
            }

            // Update in sessions_model
            sessions_model_remove_session_name(app.sessions_model, old_name);
            sessions_model_add_session_name(app.sessions_model, new_name);

            // Update current_selected_session BEFORE tree repopulation so the
            // cursor-changed handler sees it matches and avoids switch_to_session.
            gboolean was_current = (app.current_selected_session &&
                                    strcmp(app.current_selected_session, old_name) == 0);
            if (was_current) {
                g_free(app.current_selected_session);
                app.current_selected_session = g_strdup(new_name);
            }

            // Update tree view
            populate_sessions_treeview();

            // Re-select the renamed session in the tree
            app.sessions_tree_syncing = TRUE;
            {
                GtkTreeIter si;
                if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(app.sessions_tree_store), &si)) {
                    do {
                        char *sn = NULL;
                        gtk_tree_model_get(GTK_TREE_MODEL(app.sessions_tree_store), &si,
                                           SESSION_COL_SESSION_NAME, &sn, -1);
                        if (sn && strcmp(sn, new_name) == 0) {
                            GtkTreeSelection *sel = gtk_tree_view_get_selection(
                                GTK_TREE_VIEW(app.sessions_tree_view));
                            GtkTreePath *path = gtk_tree_model_get_path(
                                GTK_TREE_MODEL(app.sessions_tree_store), &si);
                            gtk_tree_selection_select_path(sel, path);
                            gtk_tree_path_free(path);
                            g_free(sn);
                            break;
                        }
                        g_free(sn);
                    } while (gtk_tree_model_iter_next(
                        GTK_TREE_MODEL(app.sessions_tree_store), &si));
                }
            }
            app.sessions_tree_syncing = FALSE;

            // Update window title for the renamed session
            if (was_current) {
                if (app.sessions_model) {
                    sessions_model_set_last_open_session(app.sessions_model, app.current_selected_session);
                }
                update_window_title_for_session(app.current_selected_session);
            }

            // Clear entry
            gtk_entry_set_text(GTK_ENTRY(app.sessions_entry), "");

            // Persist the rename
            save_state();

            g_free(old_name);
        }
    }
}
