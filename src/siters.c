#include <gtk/gtk.h>
#include <atk/atk.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include "pdf.h"
#include <math.h>
#include "siters.h"
#include "sessions_model.h"
#include "session_model.h"
#include "document_model.h"
#include "app.h"
#include "tab.h"
#include "search.h"
#include "state.h"
#include "sessions_sidebar.h"
#include "sessions_tree.h"
#include "view.h"
#include "render.h"
#include "document_state.h"
#include "view/scroll.h"
#include "theme.h"
#include "links.h"
#include "settings/settings.h"
#include "nav.h"
#include "ui/toolbar.h"
#include "ui/sidebar.h"
#include "ui/tab_lifecycle.h"
#include "ui/notebook.h"
#include "ui/notebook_switching.h"
#include "ui/zoom.h"
#include "ui/layout.h"
#include "ui/toc.h"
#include "fileinfo/fileinfo.h"

#include "mem_debug.h"

/* DATADIR is normally defined by -DDATADIR=... at build time.
   This fallback lets clang-based tools parse the file without flags. */
#ifndef DATADIR
#define DATADIR "."
#endif

/* Single application-wide state object. All former module-level statics now
   live as fields of this struct (defined in app.h). */
App app;



/* Function prototypes */
void save_state(void);


void hide_right_pane(void);

/* PDF handling function prototypes */
void queue_draw(TabData *tab);
void scroll_to_page(TabData *tab, int page, double target_y);
/* Build a compound key "side:uri" to differentiate left vs right notebook state */


static void on_title_bar_toggle(GtkToggleButton *button, gpointer user_data) {
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

static void on_helper_toggle(GtkToggleButton *button, gpointer user_data) {
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

static void on_minimize_clicked(GtkButton *button, gpointer user_data) {
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

static void on_maximize_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    GtkWindow *win = GTK_WINDOW(user_data);
    if (!win || app.maximize_pending_id) return;
    g_object_ref(win);
    app.maximize_pending_id = g_idle_add(defer_maximize_toggle, win);
}

static void on_close_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    save_state();
    gtk_main_quit();
}

static void on_sessions_add_clicked(GtkButton *button, gpointer user_data) {
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

static void on_sessions_remove_clicked(GtkButton *button, gpointer user_data) {
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

static void on_sessions_update_clicked(GtkButton *button, gpointer user_data) {
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





GtkWidget* create_main_window(void) {
    app.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app.window), "Siters");
    gtk_window_set_default_size(GTK_WINDOW(app.window), 1000, 800);

    // Load application icon from PNG (gdk-pixbuf always supports PNG)
    GError *icon_err = NULL;
    gchar *icon_path = g_strdup_printf(DATADIR "/data/icons/siters_64.png");
    GdkPixbuf *icon_pb = gdk_pixbuf_new_from_file(icon_path, &icon_err);
    if (icon_pb) {
        GList *icons = NULL;
        icons = g_list_append(icons, icon_pb);
        gtk_window_set_default_icon_list(icons);
        g_list_free(icons);
        g_object_unref(icon_pb);
    } else {
        g_warning("Could not load app icon: %s", icon_err->message);
        g_clear_error(&icon_err);
    }
    g_free(icon_path);

    // Set minimal size to prevent unusable layouts
    GdkGeometry hints = {0};
    hints.min_width = 1000;
    hints.min_height = 800;
    gtk_window_set_geometry_hints(GTK_WINDOW(app.window), NULL, &hints, GDK_HINT_MIN_SIZE);

    // Initialize sessions model
    if (!app.sessions_model) {
        app.sessions_model = sessions_model_new();
        // Initialize session models hash table
        app.session_models = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)session_model_free);

        // Initialize document models hash table
        if (!app.document_models) {
            app.document_models = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                                   (GDestroyNotify)document_model_free);
        }

        // Always ensure "Default" session exists
        const GList *existing_sessions = sessions_model_get_session_names(app.sessions_model);
        gboolean has_default = FALSE;
        for (const GList *iter = existing_sessions; iter != NULL; iter = iter->next) {
            if (strcmp((const char*)iter->data, "Default") == 0) {
                has_default = TRUE;
                break;
            }
        }
        if (!has_default) {
            sessions_model_add_session_name(app.sessions_model, "Default");
            // Create session model for Default
            session_model_t *default_session = session_model_new();
            session_model_set_session_name(default_session, "Default");
            g_hash_table_insert(app.session_models, g_strdup("Default"), default_session);
        } else {
            // Ensure we have a session model for Default
            if (!g_hash_table_lookup(app.session_models, "Default")) {
                session_model_t *default_session = session_model_new();
                session_model_set_session_name(default_session, "Default");
                g_hash_table_insert(app.session_models, g_strdup("Default"), default_session);
            }
        }
    }

    /* Auto-detect dark vs light theme: check both the prefer-dark setting
       and the theme name for known dark-theme keywords. */
    {
        GtkSettings *settings = gtk_settings_get_default();
        app.is_dark_theme = detect_system_dark_theme();
        sessions_model_set_theme(app.sessions_model, app.is_dark_theme ? "dark" : "light");
        g_signal_connect(settings, "notify::gtk-theme-name", G_CALLBACK(on_theme_changed), NULL);
        g_signal_connect(settings, "notify::gtk-application-prefer-dark-theme", G_CALLBACK(on_theme_changed), NULL);
    }

    g_signal_connect(app.window, "destroy", G_CALLBACK(on_window_destroy), NULL);
    g_signal_connect(app.window, "configure-event", G_CALLBACK(on_window_configure), NULL);

    /* Main horizontal container: toolbar on left, content on right */
    app.main_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_container_add(GTK_CONTAINER(app.window), app.main_hbox);

    /* Left sidebar: main toolbar */
    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(toolbar), "Toolbar");
    gtk_widget_set_size_request(toolbar, 36, -1);
    gtk_box_pack_start(GTK_BOX(app.main_hbox), toolbar, FALSE, FALSE, 0);

    /* Sidebar for sessions, toc, settings */
    app.sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    g_object_ref(app.sidebar);  /* Keep a reference to prevent destruction when removed */
    gtk_widget_set_size_request(app.sidebar, 200, -1);
    atk_object_set_name(gtk_widget_get_accessible(app.sidebar), "Sidebar");

    /* Content for sidebar */
    app.sidebar_label = gtk_label_new("");
    gtk_label_set_justify(GTK_LABEL(app.sidebar_label), GTK_JUSTIFY_LEFT);
    atk_object_set_name(gtk_widget_get_accessible(app.sidebar_label), "Sidebar label");
    g_object_ref(app.sidebar_label);  /* Keep a reference to prevent destruction when removed */
    gtk_box_pack_start(GTK_BOX(app.sidebar), app.sidebar_label, TRUE, TRUE, 0);
    gtk_widget_hide(app.sidebar_label);

    /* Sessions container */
    app.sessions_container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_set_border_width(GTK_CONTAINER(app.sessions_container), 5);
    g_object_ref(app.sessions_container);  /* Keep a reference */

    // Title
    app.sessions_title = gtk_label_new("Sessions");
    gtk_widget_set_halign(app.sessions_title, GTK_ALIGN_START);
    PangoAttrList *attr_list = pango_attr_list_new();
    PangoAttribute *attr = pango_attr_weight_new(PANGO_WEIGHT_BOLD);
    pango_attr_list_insert(attr_list, attr);
    attr = pango_attr_scale_new(PANGO_SCALE_LARGE);
    pango_attr_list_insert(attr_list, attr);
    gtk_label_set_attributes(GTK_LABEL(app.sessions_title), attr_list);
    pango_attr_list_unref(attr_list);
    gtk_box_pack_start(GTK_BOX(app.sessions_container), app.sessions_title, FALSE, FALSE, 0);

    // Entry field
    app.sessions_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(app.sessions_entry), "Enter session name...");
    gtk_box_pack_start(GTK_BOX(app.sessions_container), app.sessions_entry, FALSE, FALSE, 0);
    atk_object_set_name(gtk_widget_get_accessible(app.sessions_entry), "Sessions entry");

    // Buttons box
    GtkWidget *buttons_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_pack_start(GTK_BOX(app.sessions_container), buttons_box, FALSE, FALSE, 0);

    app.sessions_add_btn = gtk_button_new_with_label("Add");
    gtk_widget_set_tooltip_text(app.sessions_add_btn, "Add new session");
    atk_object_set_name(gtk_widget_get_accessible(app.sessions_add_btn), "Add session");
    g_signal_connect(app.sessions_add_btn, "clicked", G_CALLBACK(on_sessions_add_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(buttons_box), app.sessions_add_btn, TRUE, TRUE, 0);

    app.sessions_remove_btn = gtk_button_new_with_label("Remove");
    gtk_widget_set_tooltip_text(app.sessions_remove_btn, "Remove selected session");
    atk_object_set_name(gtk_widget_get_accessible(app.sessions_remove_btn), "Remove session");
    g_signal_connect(app.sessions_remove_btn, "clicked", G_CALLBACK(on_sessions_remove_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(buttons_box), app.sessions_remove_btn, TRUE, TRUE, 0);

    app.sessions_update_btn = gtk_button_new_with_label("Update");
    gtk_widget_set_tooltip_text(app.sessions_update_btn, "Update selected session name");
    atk_object_set_name(gtk_widget_get_accessible(app.sessions_update_btn), "Update session");
    g_signal_connect(app.sessions_update_btn, "clicked", G_CALLBACK(on_sessions_update_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(buttons_box), app.sessions_update_btn, TRUE, TRUE, 0);

    // Tree view
    app.sessions_tree_store = gtk_tree_store_new(
    SESSION_COL_COUNT,
    G_TYPE_STRING,  // label
    G_TYPE_INT,     // row kind
    G_TYPE_STRING,  // session name
    G_TYPE_STRING   // doc uri
    );

    app.sessions_tree_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(app.sessions_tree_store));
    g_object_unref(app.sessions_tree_store);
    atk_object_set_name(gtk_widget_get_accessible(app.sessions_tree_view), "Sessions tree");

    GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes("Session / File", renderer, "text", SESSION_COL_LABEL, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(app.sessions_tree_view), column);

    g_signal_connect(app.sessions_tree_view, "cursor-changed", G_CALLBACK(on_sessions_treeview_cursor_changed), NULL);

    // Scrolled window for tree view
    GtkWidget *scrolled_window = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scrolled_window), app.sessions_tree_view);
    gtk_box_pack_start(GTK_BOX(app.sessions_container), scrolled_window, TRUE, TRUE, 0);

    // Populate tree view with existing sessions
    populate_sessions_treeview();

    gtk_box_pack_start(GTK_BOX(app.sidebar), app.sessions_container, TRUE, TRUE, 0);
    gtk_widget_hide(app.sessions_container);

    /* TOC container */
    app.toc_container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_set_border_width(GTK_CONTAINER(app.toc_container), 5);
    g_object_ref(app.toc_container);

    GtkWidget *toc_title = gtk_label_new("Table of Contents");
    gtk_widget_set_halign(toc_title, GTK_ALIGN_START);
    PangoAttrList *toc_attr = pango_attr_list_new();
    pango_attr_list_insert(toc_attr, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    pango_attr_list_insert(toc_attr, pango_attr_scale_new(PANGO_SCALE_LARGE));
    gtk_label_set_attributes(GTK_LABEL(toc_title), toc_attr);
    pango_attr_list_unref(toc_attr);
    gtk_box_pack_start(GTK_BOX(app.toc_container), toc_title, FALSE, FALSE, 0);

    app.toc_tree_store = gtk_tree_store_new(TOC_COL_COUNT, G_TYPE_STRING, G_TYPE_INT, G_TYPE_STRING);

    app.toc_tree_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(app.toc_tree_store));
    g_object_unref(app.toc_tree_store);

    GtkCellRenderer *toc_renderer = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *toc_col = gtk_tree_view_column_new_with_attributes("Section", toc_renderer, "text", TOC_COL_LABEL, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(app.toc_tree_view), toc_col);

    gtk_tree_view_set_activate_on_single_click(GTK_TREE_VIEW(app.toc_tree_view), TRUE);
    g_signal_connect(app.toc_tree_view, "row-activated", G_CALLBACK(on_toc_row_activated), NULL);

    GtkWidget *toc_scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(toc_scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(toc_scrolled), app.toc_tree_view);
    gtk_box_pack_start(GTK_BOX(app.toc_container), toc_scrolled, TRUE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(app.sidebar), app.toc_container, TRUE, TRUE, 0);
    gtk_widget_hide(app.toc_container);

    /* Settings container */
    app.settings_container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_set_border_width(GTK_CONTAINER(app.settings_container), 5);
    g_object_ref(app.settings_container);

    GtkWidget *settings_title = gtk_label_new("Siters Preferences");
    gtk_widget_set_halign(settings_title, GTK_ALIGN_START);
    PangoAttrList *sattr = pango_attr_list_new();
    pango_attr_list_insert(sattr, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    pango_attr_list_insert(sattr, pango_attr_scale_new(PANGO_SCALE_LARGE));
    gtk_label_set_attributes(GTK_LABEL(settings_title), sattr);
    pango_attr_list_unref(sattr);
    gtk_box_pack_start(GTK_BOX(app.settings_container), settings_title, FALSE, FALSE, 0);

    GtkWidget *version_label = gtk_label_new("Version 0.1.0-19");
    gtk_widget_set_halign(version_label, GTK_ALIGN_START);
    PangoAttrList *vattr = pango_attr_list_new();
    pango_attr_list_insert(vattr, pango_attr_foreground_alpha_new(32768));
    gtk_label_set_attributes(GTK_LABEL(version_label), vattr);
    pango_attr_list_unref(vattr);
    gtk_box_pack_start(GTK_BOX(app.settings_container), version_label, FALSE, FALSE, 0);

    /* Tab section */
    GtkWidget *tab_section_label = gtk_label_new("Tab");
    gtk_widget_set_halign(tab_section_label, GTK_ALIGN_START);
    PangoAttrList *ts_attr = pango_attr_list_new();
    pango_attr_list_insert(ts_attr, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    gtk_label_set_attributes(GTK_LABEL(tab_section_label), ts_attr);
    pango_attr_list_unref(ts_attr);
    gtk_box_pack_start(GTK_BOX(app.settings_container), tab_section_label, FALSE, FALSE, 0);

    GtkWidget *tabbar_label = gtk_label_new("Tab bar position:");
    gtk_widget_set_halign(tabbar_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(app.settings_container), tabbar_label, FALSE, FALSE, 0);

    app.tabbar_combo = gtk_combo_box_text_new();
    gtk_widget_set_halign(app.tabbar_combo, GTK_ALIGN_START);
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(app.tabbar_combo), "left", "Left");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(app.tabbar_combo), "top", "Top");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(app.tabbar_combo), "right", "Right");
    if (app.sessions_model) {
        const char *cur = sessions_model_get_tabbar_position(app.sessions_model);
        if (g_strcmp0(cur, "left") == 0)
            gtk_combo_box_set_active(GTK_COMBO_BOX(app.tabbar_combo), 0);
        else if (g_strcmp0(cur, "right") == 0)
            gtk_combo_box_set_active(GTK_COMBO_BOX(app.tabbar_combo), 2);
        else
            gtk_combo_box_set_active(GTK_COMBO_BOX(app.tabbar_combo), 1);
    } else {
        gtk_combo_box_set_active(GTK_COMBO_BOX(app.tabbar_combo), 1);
    }
    gtk_box_pack_start(GTK_BOX(app.settings_container), app.tabbar_combo, FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(app.tabbar_combo), "changed", G_CALLBACK(on_tabbar_combo_changed), NULL);

    /* Tab text width */
    GtkWidget *tab_width_label = gtk_label_new("Tab text width:");
    gtk_widget_set_halign(tab_width_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(app.settings_container), tab_width_label, FALSE, FALSE, 0);

    app.tab_width_spin = gtk_spin_button_new_with_range(5, 50, 1);
    gtk_widget_set_halign(app.tab_width_spin, GTK_ALIGN_START);
    if (app.sessions_model)
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(app.tab_width_spin), sessions_model_get_tab_width(app.sessions_model));
    else
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(app.tab_width_spin), 50);
    gtk_box_pack_start(GTK_BOX(app.settings_container), app.tab_width_spin, FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(app.tab_width_spin), "value-changed", G_CALLBACK(on_tab_width_spin_changed), NULL);

    gtk_box_pack_start(GTK_BOX(app.settings_container), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);

    /* Page Colors section */
    GtkWidget *color_title = gtk_label_new("Page Colors");
    gtk_widget_set_halign(color_title, GTK_ALIGN_START);
    PangoAttrList *cat = pango_attr_list_new();
    pango_attr_list_insert(cat, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    gtk_label_set_attributes(GTK_LABEL(color_title), cat);
    pango_attr_list_unref(cat);
    gtk_box_pack_start(GTK_BOX(app.settings_container), color_title, FALSE, FALSE, 0);

    GtkWidget *left_color_label = gtk_label_new("Left notebook:");
    gtk_widget_set_halign(left_color_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(app.settings_container), left_color_label, FALSE, FALSE, 0);

    app.left_color_btn = gtk_color_button_new_with_rgba(&(GdkRGBA){1.0, 1.0, 1.0, 1.0});
    gtk_widget_set_halign(app.left_color_btn, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(app.settings_container), app.left_color_btn, FALSE, FALSE, 0);
    g_signal_connect(app.left_color_btn, "color-set", G_CALLBACK(on_left_color_set), NULL);

    GtkWidget *right_color_label = gtk_label_new("Right notebook:");
    gtk_widget_set_halign(right_color_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(app.settings_container), right_color_label, FALSE, FALSE, 0);

    app.right_color_btn = gtk_color_button_new_with_rgba(&(GdkRGBA){1.0, 1.0, 1.0, 1.0});
    gtk_widget_set_halign(app.right_color_btn, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(app.settings_container), app.right_color_btn, FALSE, FALSE, 0);
    g_signal_connect(app.right_color_btn, "color-set", G_CALLBACK(on_right_color_set), NULL);

    gtk_box_pack_start(GTK_BOX(app.settings_container), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);

    /* Theme section */
    GtkWidget *theme_section_label = gtk_label_new("Theme");
    gtk_widget_set_halign(theme_section_label, GTK_ALIGN_START);
    PangoAttrList *th_attr = pango_attr_list_new();
    pango_attr_list_insert(th_attr, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    gtk_label_set_attributes(GTK_LABEL(theme_section_label), th_attr);
    pango_attr_list_unref(th_attr);
    gtk_box_pack_start(GTK_BOX(app.settings_container), theme_section_label, FALSE, FALSE, 0);

    app.keep_dark_check = gtk_check_button_new_with_label("Keep dark theme");
    gtk_widget_set_halign(app.keep_dark_check, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(app.settings_container), app.keep_dark_check, FALSE, FALSE, 0);
    g_signal_connect(app.keep_dark_check, "toggled", G_CALLBACK(on_keep_dark_toggled), NULL);

    gtk_box_pack_start(GTK_BOX(app.sidebar), app.settings_container, TRUE, TRUE, 0);
    gtk_widget_hide(app.settings_container);

    /* File info container */
    app.file_info_container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(app.file_info_container), 8);
    g_object_ref(app.file_info_container);

    GtkWidget *file_info_title = gtk_label_new("File Information");
    gtk_widget_set_halign(file_info_title, GTK_ALIGN_START);
    PangoAttrList *fi_attr = pango_attr_list_new();
    pango_attr_list_insert(fi_attr, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    pango_attr_list_insert(fi_attr, pango_attr_scale_new(PANGO_SCALE_LARGE));
    gtk_label_set_attributes(GTK_LABEL(file_info_title), fi_attr);
    pango_attr_list_unref(fi_attr);
    gtk_box_pack_start(GTK_BOX(app.file_info_container), file_info_title, FALSE, FALSE, 0);

    app.file_info_name_label = gtk_label_new("Name: (no file)");
    gtk_widget_set_halign(app.file_info_name_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(app.file_info_container), app.file_info_name_label, FALSE, FALSE, 0);

    app.file_info_path_label = gtk_label_new("Path: (none)");
    gtk_widget_set_halign(app.file_info_path_label, GTK_ALIGN_FILL);
    gtk_label_set_xalign(GTK_LABEL(app.file_info_path_label), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(app.file_info_path_label), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(app.file_info_path_label), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(app.file_info_path_label), 30);
    gtk_box_pack_start(GTK_BOX(app.file_info_container), app.file_info_path_label, FALSE, FALSE, 0);

    app.file_info_size_label = gtk_label_new("Size: (none)");
    gtk_widget_set_halign(app.file_info_size_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(app.file_info_container), app.file_info_size_label, FALSE, FALSE, 0);

    app.file_info_pages_label = gtk_label_new("Pages: (none)");
    gtk_widget_set_halign(app.file_info_pages_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(app.file_info_container), app.file_info_pages_label, FALSE, FALSE, 0);

    /* Separator before search */
    GtkWidget *search_sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(app.file_info_container), search_sep, FALSE, FALSE, 5);

    /* Search title */
    GtkWidget *search_title = gtk_label_new("Search in Document");
    gtk_widget_set_halign(search_title, GTK_ALIGN_START);
    PangoAttrList *sa = pango_attr_list_new();
    pango_attr_list_insert(sa, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    pango_attr_list_insert(sa, pango_attr_scale_new(PANGO_SCALE_LARGE));
    gtk_label_set_attributes(GTK_LABEL(search_title), sa);
    pango_attr_list_unref(sa);
    gtk_box_pack_start(GTK_BOX(app.file_info_container), search_title, FALSE, FALSE, 0);

    /* Search entry + button row */
    GtkWidget *search_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    app.search_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(app.search_entry), "Search text...");
    gtk_box_pack_start(GTK_BOX(search_hbox), app.search_entry, TRUE, TRUE, 0);
    g_signal_connect(app.search_entry, "activate", G_CALLBACK(on_search_activated), NULL);

    app.search_btn = gtk_button_new_with_label("Search");
    gtk_box_pack_start(GTK_BOX(search_hbox), app.search_btn, FALSE, FALSE, 0);
    g_signal_connect(app.search_btn, "clicked", G_CALLBACK(on_search_clicked), NULL);

    gtk_box_pack_start(GTK_BOX(app.file_info_container), search_hbox, FALSE, FALSE, 0);

    /* Results tree view */
    app.search_results_store = gtk_list_store_new(SEARCH_COL_NCOL, G_TYPE_INT, G_TYPE_INT, G_TYPE_STRING);
    app.search_results_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(app.search_results_store));
    g_object_unref(app.search_results_store);

    GtkCellRenderer *search_renderer = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *search_col = gtk_tree_view_column_new_with_attributes("Page", search_renderer,
        "text", SEARCH_COL_LABEL, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(app.search_results_view), search_col);
    gtk_tree_view_set_activate_on_single_click(GTK_TREE_VIEW(app.search_results_view), TRUE);
    g_signal_connect(app.search_results_view, "row-activated", G_CALLBACK(on_search_row_activated), NULL);

    GtkWidget *search_scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(search_scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(search_scrolled), 80);
    gtk_container_add(GTK_CONTAINER(search_scrolled), app.search_results_view);
    gtk_box_pack_start(GTK_BOX(app.file_info_container), search_scrolled, TRUE, TRUE, 0);

    /* No results label */
    app.search_no_results_label = gtk_label_new("No results found");
    gtk_widget_set_halign(app.search_no_results_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(app.file_info_container), app.search_no_results_label, FALSE, FALSE, 0);
    gtk_widget_hide(app.search_no_results_label);

    gtk_widget_set_size_request(app.file_info_container, 300, -1);
    gtk_box_pack_start(GTK_BOX(app.sidebar), app.file_info_container, TRUE, TRUE, 0);
    gtk_widget_hide(app.file_info_container);

    /* Content area on the right*/
    app.content_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(app.main_hbox), app.content_vbox, TRUE, TRUE, 0);

    /* Buttons*/
    /* Sessions button */
    GtkWidget *sessions_icon = create_toolbar_icon("sessions");
    app.sessions_btn = gtk_toggle_button_new();
    gtk_button_set_image(GTK_BUTTON(app.sessions_btn), sessions_icon);
    g_object_set_data_full(G_OBJECT(app.sessions_btn), "icon-name", g_strdup("sessions"), g_free);
    gtk_widget_set_tooltip_text(app.sessions_btn, "Sessions");
    atk_object_set_name(gtk_widget_get_accessible(app.sessions_btn), "Sessions");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app.sessions_btn), FALSE);
    g_signal_connect(app.sessions_btn, "toggled", G_CALLBACK(on_sessions_toggled), NULL);
    gtk_box_pack_start(GTK_BOX(toolbar), app.sessions_btn, FALSE, FALSE, 1);

    /* Table of contents button */
    GtkWidget *toc_icon = create_toolbar_icon("toc");
    app.toc_btn = gtk_toggle_button_new();
    gtk_button_set_image(GTK_BUTTON(app.toc_btn), toc_icon);
    g_object_set_data_full(G_OBJECT(app.toc_btn), "icon-name", g_strdup("toc"), g_free);
    gtk_widget_set_tooltip_text(app.toc_btn, "Table of contents");
    atk_object_set_name(gtk_widget_get_accessible(app.toc_btn), "Table of contents");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app.toc_btn), FALSE);
    g_signal_connect(app.toc_btn, "toggled", G_CALLBACK(on_toc_toggled), NULL);
    gtk_box_pack_start(GTK_BOX(toolbar), app.toc_btn, FALSE, FALSE, 1);

    /* Settings button */
    GtkWidget *settings_icon = create_toolbar_icon("settings");
    app.settings_btn = gtk_toggle_button_new();
    gtk_button_set_image(GTK_BUTTON(app.settings_btn), settings_icon);
    g_object_set_data_full(G_OBJECT(app.settings_btn), "icon-name", g_strdup("settings"), g_free);
    gtk_widget_set_tooltip_text(app.settings_btn, "Settings");
    atk_object_set_name(gtk_widget_get_accessible(app.settings_btn), "Settings");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app.settings_btn), FALSE);
    g_signal_connect(app.settings_btn, "toggled", G_CALLBACK(on_settings_toggled), NULL);
    gtk_box_pack_start(GTK_BOX(toolbar), app.settings_btn, FALSE, FALSE, 1);

    /* File information button */
    GtkWidget *file_info_icon = create_toolbar_icon("file");
    app.file_info_btn = gtk_toggle_button_new();
    gtk_button_set_image(GTK_BUTTON(app.file_info_btn), file_info_icon);
    g_object_set_data_full(G_OBJECT(app.file_info_btn), "icon-name", g_strdup("file"), g_free);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app.file_info_btn), FALSE);
    gtk_widget_set_tooltip_text(app.file_info_btn, "File information");
    atk_object_set_name(gtk_widget_get_accessible(app.file_info_btn), "File information");
    g_signal_connect(app.file_info_btn, "toggled", G_CALLBACK(on_left_file_info_toggled), NULL);
    gtk_box_pack_start(GTK_BOX(toolbar), app.file_info_btn, FALSE, FALSE, 1);

    /* Separator */
    GtkWidget *separator_a = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(toolbar), separator_a, FALSE, FALSE, 5);

    /* Centered middle section: open file through helper toggle */
    GtkWidget *middle_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), middle_box, TRUE, FALSE, 0);

    /* Open file button */
    GtkWidget *open_file_icon = create_toolbar_icon("file-plus");
    GtkWidget *open_file_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(open_file_btn), open_file_icon);
    g_object_set_data_full(G_OBJECT(open_file_btn), "icon-name", g_strdup("file-plus"), g_free);
    gtk_widget_set_tooltip_text(open_file_btn, "Open file");
    atk_object_set_name(gtk_widget_get_accessible(open_file_btn), "Open file");
    g_signal_connect(open_file_btn, "clicked", G_CALLBACK(on_open_file_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(middle_box), open_file_btn, FALSE, FALSE, 1);

    /* Close file button*/
    GtkWidget *close_file_icon = create_toolbar_icon("file-minus");
    GtkWidget *close_file_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(close_file_btn), close_file_icon);
    g_object_set_data_full(G_OBJECT(close_file_btn), "icon-name", g_strdup("file-minus"), g_free);
    gtk_widget_set_tooltip_text(close_file_btn, "Close file");
    atk_object_set_name(gtk_widget_get_accessible(close_file_btn), "Close file");
    g_signal_connect(close_file_btn, "clicked", G_CALLBACK(on_close_file_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(middle_box), close_file_btn, FALSE, FALSE, 1);

    /* Separator */
    GtkWidget *separator_b = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(middle_box), separator_b, FALSE, FALSE, 5);

    /* Page backward button*/
    GtkWidget *page_up_icon = create_toolbar_icon("page-up");
    GtkWidget *page_up_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(page_up_btn), page_up_icon);
    g_object_set_data_full(G_OBJECT(page_up_btn), "icon-name", g_strdup("page-up"), g_free);
    gtk_widget_set_tooltip_text(page_up_btn, "Page backward");
    atk_object_set_name(gtk_widget_get_accessible(page_up_btn), "Page backward");
    g_signal_connect(page_up_btn, "clicked", G_CALLBACK(on_page_up_left), NULL);
    gtk_box_pack_start(GTK_BOX(middle_box), page_up_btn, FALSE, FALSE, 1);

    /* Page forward button*/
    GtkWidget *page_down_icon = create_toolbar_icon("page-down");
    GtkWidget *page_down_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(page_down_btn), page_down_icon);
    g_object_set_data_full(G_OBJECT(page_down_btn), "icon-name", g_strdup("page-down"), g_free);
    gtk_widget_set_tooltip_text(page_down_btn, "Page forward");
    atk_object_set_name(gtk_widget_get_accessible(page_down_btn), "Page forward");
    g_signal_connect(page_down_btn, "clicked", G_CALLBACK(on_page_down_left), NULL);
    gtk_box_pack_start(GTK_BOX(middle_box), page_down_btn, FALSE, FALSE, 1);

    /* Page navigation (entry + label) — placed in floating overlay later */
    app.page_entry = gtk_entry_new();
    gtk_widget_set_size_request(app.page_entry, 42, -1);
    gtk_entry_set_max_length(GTK_ENTRY(app.page_entry), 4);
    gtk_entry_set_width_chars(GTK_ENTRY(app.page_entry), 2);
    gtk_entry_set_max_width_chars(GTK_ENTRY(app.page_entry), 3);
    gtk_entry_set_input_purpose(GTK_ENTRY(app.page_entry), GTK_INPUT_PURPOSE_DIGITS);
    gtk_widget_set_tooltip_text(app.page_entry, "Current page (press Enter to jump)");
    atk_object_set_name(gtk_widget_get_accessible(app.page_entry), "Current page");

    app.page_total_label = gtk_label_new("/ 0");
    gtk_label_set_width_chars(GTK_LABEL(app.page_total_label), 4);
    gtk_label_set_xalign(GTK_LABEL(app.page_total_label), 0.0f);

    /* Allow only digits to be entered */
    g_signal_connect(app.page_entry, "insert-text", G_CALLBACK(on_page_entry_insert_text), NULL);

    /* Enter in spin jumps to page */
    g_signal_connect(app.page_entry, "activate", G_CALLBACK(on_page_entry_activate), NULL);

    /* Separator */
    GtkWidget *separator_c = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(middle_box), separator_c, FALSE, FALSE, 5);

    /* Zoom in button*/
    GtkWidget *zoom_in_icon = create_toolbar_icon("zoom-in");
    GtkWidget *zoom_in_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(zoom_in_btn), zoom_in_icon);
    g_object_set_data_full(G_OBJECT(zoom_in_btn), "icon-name", g_strdup("zoom-in"), g_free);
    gtk_widget_set_tooltip_text(zoom_in_btn, "Zoom in");
    atk_object_set_name(gtk_widget_get_accessible(zoom_in_btn), "Zoom in");
    gtk_box_pack_start(GTK_BOX(middle_box), zoom_in_btn, FALSE, FALSE, 1);
    g_signal_connect(G_OBJECT(zoom_in_btn), "clicked", G_CALLBACK(on_zoom_in_left), NULL);

    /* Zoom out button*/
    GtkWidget *zoom_out_icon = create_toolbar_icon("zoom-out");
    GtkWidget *zoom_out_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(zoom_out_btn), zoom_out_icon);
    g_object_set_data_full(G_OBJECT(zoom_out_btn), "icon-name", g_strdup("zoom-out"), g_free);
    gtk_widget_set_tooltip_text(zoom_out_btn, "Zoom out");
    atk_object_set_name(gtk_widget_get_accessible(zoom_out_btn), "Zoom out");
    gtk_box_pack_start(GTK_BOX(middle_box), zoom_out_btn, FALSE, FALSE, 1);
    g_signal_connect(G_OBJECT(zoom_out_btn), "clicked", G_CALLBACK(on_zoom_out_left), NULL);

    /* Separator */
    GtkWidget *separator_d = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(middle_box), separator_d, FALSE, FALSE, 5);

    /* Column view button*/
    GtkWidget *column_view_icon = create_toolbar_icon("column");
    app.left_column_btn = gtk_radio_button_new(NULL);
    gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(app.left_column_btn), FALSE);
    gtk_button_set_image(GTK_BUTTON(app.left_column_btn), column_view_icon);
    g_object_set_data_full(G_OBJECT(app.left_column_btn), "icon-name", g_strdup("column"), g_free);
    gtk_widget_set_tooltip_text(app.left_column_btn, "Page column");
    atk_object_set_name(gtk_widget_get_accessible(app.left_column_btn), "Page column");
    g_object_set_data(G_OBJECT(app.left_column_btn), "layout-id", GINT_TO_POINTER(0 + 1));
    g_signal_connect(app.left_column_btn, "toggled", G_CALLBACK(on_layout_left_toggled), NULL);
    gtk_box_pack_start(GTK_BOX(middle_box), app.left_column_btn, FALSE, FALSE, 1);

    /* Double column view button*/
    GtkWidget *double_column_view_icon = create_toolbar_icon("double-column");
    app.left_double_column_btn = gtk_radio_button_new_from_widget(GTK_RADIO_BUTTON(app.left_column_btn));
    gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(app.left_double_column_btn), FALSE);
    gtk_button_set_image(GTK_BUTTON(app.left_double_column_btn), double_column_view_icon);
    g_object_set_data_full(G_OBJECT(app.left_double_column_btn), "icon-name", g_strdup("double-column"), g_free);
    gtk_widget_set_tooltip_text(app.left_double_column_btn, "Page double column");
    atk_object_set_name(gtk_widget_get_accessible(app.left_double_column_btn), "Page double column");
    g_object_set_data(G_OBJECT(app.left_double_column_btn), "layout-id", GINT_TO_POINTER(1 + 1));
    g_signal_connect(app.left_double_column_btn, "toggled", G_CALLBACK(on_layout_left_toggled), NULL);
    gtk_box_pack_start(GTK_BOX(middle_box), app.left_double_column_btn, FALSE, FALSE, 1);

    /* Row view button*/
    GtkWidget *row_view_icon = create_toolbar_icon("row");
    app.left_row_btn = gtk_radio_button_new_from_widget(GTK_RADIO_BUTTON(app.left_column_btn));
    gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(app.left_row_btn), FALSE);
    gtk_button_set_image(GTK_BUTTON(app.left_row_btn), row_view_icon);
    g_object_set_data_full(G_OBJECT(app.left_row_btn), "icon-name", g_strdup("row"), g_free);
    gtk_widget_set_tooltip_text(app.left_row_btn, "Page row");
    atk_object_set_name(gtk_widget_get_accessible(app.left_row_btn), "Page row");
    g_object_set_data(G_OBJECT(app.left_row_btn), "layout-id", GINT_TO_POINTER(2 + 1));
    g_signal_connect(app.left_row_btn, "toggled", G_CALLBACK(on_layout_left_toggled), NULL);
    gtk_box_pack_start(GTK_BOX(middle_box), app.left_row_btn, FALSE, FALSE, 1);

    /* Separator */
    GtkWidget *separator_e = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(middle_box), separator_e, FALSE, FALSE, 5);

    /* Title bar toggle button*/
    GtkWidget *title_bar_toggle_icon = create_toolbar_icon("title-bar-on");
    GtkWidget *title_bar_toggle_btn = gtk_toggle_button_new();
    gtk_button_set_image(GTK_BUTTON(title_bar_toggle_btn), title_bar_toggle_icon);
    g_object_set_data_full(G_OBJECT(title_bar_toggle_btn), "icon-on", g_strdup("title-bar-on"), g_free);
    g_object_set_data_full(G_OBJECT(title_bar_toggle_btn), "icon-off", g_strdup("title-bar-off"), g_free);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(title_bar_toggle_btn), TRUE);
    gtk_widget_set_tooltip_text(title_bar_toggle_btn, "Toggle title bar visibility");
    atk_object_set_name(gtk_widget_get_accessible(title_bar_toggle_btn), "Toggle title bar visibility");
    g_signal_connect(title_bar_toggle_btn, "toggled", G_CALLBACK(on_title_bar_toggle), NULL);
    gtk_box_pack_start(GTK_BOX(middle_box), title_bar_toggle_btn, FALSE, FALSE, 1);

    /* Helpers toggle button*/
    GtkWidget *helper_toggle_icon = create_toolbar_icon("sidebar-helper-off");
    GtkWidget *helper_toggle_btn = gtk_toggle_button_new();
    gtk_button_set_image(GTK_BUTTON(helper_toggle_btn), helper_toggle_icon);
    g_object_set_data_full(G_OBJECT(helper_toggle_btn), "icon-on", g_strdup("sidebar-helper-on"), g_free);
    g_object_set_data_full(G_OBJECT(helper_toggle_btn), "icon-off", g_strdup("sidebar-helper-off"), g_free);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(helper_toggle_btn), FALSE);
    gtk_widget_set_tooltip_text(helper_toggle_btn, "Helper files");
    atk_object_set_name(gtk_widget_get_accessible(helper_toggle_btn), "Helper files");
    g_signal_connect(helper_toggle_btn, "toggled", G_CALLBACK(on_helper_toggle), NULL);
    gtk_box_pack_start(GTK_BOX(middle_box), helper_toggle_btn, FALSE, FALSE, 1);

    /* Separator */
    GtkWidget *separator_f = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(toolbar), separator_f, FALSE, FALSE, 5);

    /* Close button*/
    GtkWidget *close_icon = create_toolbar_icon("plug");
    GtkWidget *close_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(close_btn), close_icon);
    g_object_set_data_full(G_OBJECT(close_btn), "icon-name", g_strdup("plug"), g_free);
    gtk_widget_set_tooltip_text(close_btn, "Close");
    atk_object_set_name(gtk_widget_get_accessible(close_btn), "Close");
    g_signal_connect(close_btn, "clicked", G_CALLBACK(on_close_clicked), NULL);
    gtk_box_pack_end(GTK_BOX(toolbar), close_btn, FALSE, FALSE, 1);

    /* Maximize button*/
    GtkWidget *maximize_icon = create_toolbar_icon("maximize-2");
    GtkWidget *maximize_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(maximize_btn), maximize_icon);
    g_object_set_data_full(G_OBJECT(maximize_btn), "icon-name", g_strdup("maximize-2"), g_free);
    gtk_widget_set_tooltip_text(maximize_btn, "Maximize");
    atk_object_set_name(gtk_widget_get_accessible(maximize_btn), "Maximize");
    g_signal_connect(maximize_btn, "clicked", G_CALLBACK(on_maximize_clicked), app.window);
    gtk_box_pack_end(GTK_BOX(toolbar), maximize_btn, FALSE, FALSE, 1);

    /* Minimize button*/
    GtkWidget *minimize_icon = create_toolbar_icon("minimize-2");
    GtkWidget *minimize_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(minimize_btn), minimize_icon);
    g_object_set_data_full(G_OBJECT(minimize_btn), "icon-name", g_strdup("minimize-2"), g_free);
    gtk_widget_set_tooltip_text(minimize_btn, "Minimize");
    atk_object_set_name(gtk_widget_get_accessible(minimize_btn), "Minimize");
    g_signal_connect(minimize_btn, "clicked", G_CALLBACK(on_minimize_clicked), app.window);
    gtk_box_pack_end(GTK_BOX(toolbar), minimize_btn, FALSE, FALSE, 1);

    /* MAIN WINDOW PANED */
    /* Create a horizontal paned splitter containing two notebooks */
    app.paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);

    /* Wrap paned in an overlay for floating page navigation widget */
    GtkWidget *overlay = gtk_overlay_new();
    gtk_box_pack_start(GTK_BOX(app.content_vbox), overlay, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(overlay), app.paned);

    /* Floating page navigation overlay (lower-left corner) */
    app.page_nav_overlay = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_name(app.page_nav_overlay, "page-nav-overlay");
    gtk_widget_set_halign(app.page_nav_overlay, GTK_ALIGN_START);
    gtk_widget_set_valign(app.page_nav_overlay, GTK_ALIGN_END);
    gtk_widget_set_margin_start(app.page_nav_overlay, 8);
    gtk_widget_set_margin_bottom(app.page_nav_overlay, 8);
    gtk_box_pack_start(GTK_BOX(app.page_nav_overlay), app.page_entry, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(app.page_nav_overlay), app.page_total_label, FALSE, FALSE, 0);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), app.page_nav_overlay);

    /* Left notebook (primary) */
    app.left_notebook = gtk_notebook_new();
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(app.left_notebook), TRUE);
    gtk_widget_set_size_request(app.left_notebook, 70, -1);
    gtk_paned_pack1(GTK_PANED(app.paned), app.left_notebook, TRUE, TRUE);
    atk_object_set_name(gtk_widget_get_accessible(app.left_notebook), "Left Notebook");
    g_signal_connect(app.left_notebook, "switch-page", G_CALLBACK(on_left_notebook_switch_page), NULL);
    g_signal_connect(app.left_notebook, "page-reordered", G_CALLBACK(on_notebook_page_reordered), NULL);

    // Use last open session if available, otherwise default to "Default"
    const char *initial_session = "Default";
    if (app.sessions_model) {
        const char *last_session = sessions_model_get_last_open_session(app.sessions_model);
        if (last_session) {
            initial_session = last_session;
        }
    }

    app.current_selected_session = g_strdup(initial_session);
    update_window_title_for_session(app.current_selected_session);

    /* Right pane: container with notebook and toolbar */
    app.right_pane = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_size_request(app.right_pane, 36, -1);
    gtk_paned_pack2(GTK_PANED(app.paned), app.right_pane, TRUE, TRUE);
    gtk_paned_set_position(GTK_PANED(app.paned), 500);

    /* Right notebook wrapper with overlay for floating page nav */
    GtkWidget *right_nb_overlay = gtk_overlay_new();
    gtk_box_pack_start(GTK_BOX(app.right_pane), right_nb_overlay, TRUE, TRUE, 0);

    /* Right notebook (secondary) */
    app.right_notebook = gtk_notebook_new();
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(app.right_notebook), TRUE);
    gtk_container_add(GTK_CONTAINER(right_nb_overlay), app.right_notebook);
    g_signal_connect(app.right_notebook, "switch-page", G_CALLBACK(on_right_notebook_switch_page), NULL);
    g_signal_connect(app.right_notebook, "page-reordered", G_CALLBACK(on_notebook_page_reordered), NULL);

    /* Right page navigation entry + label */
    app.right_page_entry = gtk_entry_new();
    gtk_widget_set_size_request(app.right_page_entry, 42, -1);
    gtk_entry_set_max_length(GTK_ENTRY(app.right_page_entry), 4);
    gtk_entry_set_width_chars(GTK_ENTRY(app.right_page_entry), 2);
    gtk_entry_set_max_width_chars(GTK_ENTRY(app.right_page_entry), 3);
    gtk_entry_set_input_purpose(GTK_ENTRY(app.right_page_entry), GTK_INPUT_PURPOSE_DIGITS);
    gtk_widget_set_tooltip_text(app.right_page_entry, "Current page (press Enter to jump)");
    atk_object_set_name(gtk_widget_get_accessible(app.right_page_entry), "Current page");

    app.right_page_total_label = gtk_label_new("/ 0");
    gtk_label_set_width_chars(GTK_LABEL(app.right_page_total_label), 4);
    gtk_label_set_xalign(GTK_LABEL(app.right_page_total_label), 0.0f);

    g_signal_connect(app.right_page_entry, "insert-text", G_CALLBACK(on_page_entry_insert_text), NULL);
    g_signal_connect(app.right_page_entry, "activate", G_CALLBACK(on_right_page_entry_activate), NULL);

    /* Floating page navigation overlay (lower-right corner of right notebook) */
    app.right_page_nav_overlay = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_name(app.right_page_nav_overlay, "right-page-nav-overlay");
    gtk_widget_set_halign(app.right_page_nav_overlay, GTK_ALIGN_END);
    gtk_widget_set_valign(app.right_page_nav_overlay, GTK_ALIGN_END);
    gtk_widget_set_margin_end(app.right_page_nav_overlay, 8);
    gtk_widget_set_margin_bottom(app.right_page_nav_overlay, 8);
    gtk_box_pack_start(GTK_BOX(app.right_page_nav_overlay), app.right_page_entry, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(app.right_page_nav_overlay), app.right_page_total_label, FALSE, FALSE, 0);
    gtk_overlay_add_overlay(GTK_OVERLAY(right_nb_overlay), app.right_page_nav_overlay);
    /* Initially hidden until there are documents */
    gtk_widget_hide(app.right_page_nav_overlay);

    // Note: restore_open_tabs_for_session already handles both notebooks
    app.current_selected_session = g_strdup(initial_session);
    update_window_title_for_session(app.current_selected_session);

    /* Right pane toolbar (vertical) */
    GtkWidget *right_toolbar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(right_toolbar), "Toolbar");
    gtk_box_pack_start(GTK_BOX(app.right_pane), right_toolbar, FALSE, FALSE, 0);

    /* Centered section for right toolbar buttons */
    GtkWidget *right_middle_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(right_toolbar), right_middle_box, TRUE, FALSE, 0);

    /* Right toolbar buttons - Open file */
    GtkWidget *right_open_file_icon = create_toolbar_icon("file-plus");
    GtkWidget *right_open_file_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(right_open_file_btn), right_open_file_icon);
    g_object_set_data_full(G_OBJECT(right_open_file_btn), "icon-name", g_strdup("file-plus"), g_free);
    gtk_widget_set_tooltip_text(right_open_file_btn, "Open file");
    atk_object_set_name(gtk_widget_get_accessible(right_open_file_btn), "Open file");
    g_signal_connect(right_open_file_btn, "clicked", G_CALLBACK(on_open_helper_file_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(right_middle_box), right_open_file_btn, FALSE, FALSE, 1);

    /* Right toolbar - Close file */
    GtkWidget *right_close_file_icon = create_toolbar_icon("file-minus");
    GtkWidget *right_close_file_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(right_close_file_btn), right_close_file_icon);
    g_object_set_data_full(G_OBJECT(right_close_file_btn), "icon-name", g_strdup("file-minus"), g_free);
    gtk_widget_set_tooltip_text(right_close_file_btn, "Close file");
    atk_object_set_name(gtk_widget_get_accessible(right_close_file_btn), "Close file");
    g_signal_connect(right_close_file_btn, "clicked", G_CALLBACK(on_close_helper_file_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(right_middle_box), right_close_file_btn, FALSE, FALSE, 1);

    /* Right toolbar - File information */
    GtkWidget *right_file_info_icon = create_toolbar_icon("file");
    GtkWidget *right_file_info_btn = gtk_toggle_button_new();
    gtk_button_set_image(GTK_BUTTON(right_file_info_btn), right_file_info_icon);
    g_object_set_data_full(G_OBJECT(right_file_info_btn), "icon-name", g_strdup("file"), g_free);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(right_file_info_btn), FALSE);
    gtk_widget_set_tooltip_text(right_file_info_btn, "File information");
    atk_object_set_name(gtk_widget_get_accessible(right_file_info_btn), "File information");
    g_signal_connect(right_file_info_btn, "toggled", G_CALLBACK(on_right_file_info_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(right_middle_box), right_file_info_btn, FALSE, FALSE, 1);

    /* Right toolbar separator */
    GtkWidget *right_sep_a = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(right_middle_box), right_sep_a, FALSE, FALSE, 5);

    /* Right toolbar - Page backward */
    GtkWidget *right_page_up_icon = create_toolbar_icon("page-up");
    GtkWidget *right_page_up_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(right_page_up_btn), right_page_up_icon);
    g_object_set_data_full(G_OBJECT(right_page_up_btn), "icon-name", g_strdup("page-up"), g_free);
    gtk_widget_set_tooltip_text(right_page_up_btn, "Page backward");
    atk_object_set_name(gtk_widget_get_accessible(right_page_up_btn), "Page backward");
    g_signal_connect(right_page_up_btn, "clicked", G_CALLBACK(on_page_up_right), NULL);
    gtk_box_pack_start(GTK_BOX(right_middle_box), right_page_up_btn, FALSE, FALSE, 1);

    /* Right toolbar - Page forward */
    GtkWidget *right_page_down_icon = create_toolbar_icon("page-down");
    GtkWidget *right_page_down_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(right_page_down_btn), right_page_down_icon);
    g_object_set_data_full(G_OBJECT(right_page_down_btn), "icon-name", g_strdup("page-down"), g_free);
    gtk_widget_set_tooltip_text(right_page_down_btn, "Page forward");
    atk_object_set_name(gtk_widget_get_accessible(right_page_down_btn), "Page forward");
    g_signal_connect(right_page_down_btn, "clicked", G_CALLBACK(on_page_down_right), NULL);
    gtk_box_pack_start(GTK_BOX(right_middle_box), right_page_down_btn, FALSE, FALSE, 1);

    /* Right toolbar separator */
    GtkWidget *right_sep_b = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(right_middle_box), right_sep_b, FALSE, FALSE, 5);

    /* Right toolbar - Zoom in */
    GtkWidget *right_zoom_in_icon = create_toolbar_icon("zoom-in");
    GtkWidget *right_zoom_in_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(right_zoom_in_btn), right_zoom_in_icon);
    g_object_set_data_full(G_OBJECT(right_zoom_in_btn), "icon-name", g_strdup("zoom-in"), g_free);
    gtk_widget_set_tooltip_text(right_zoom_in_btn, "Zoom in");
    atk_object_set_name(gtk_widget_get_accessible(right_zoom_in_btn), "Zoom in");
    gtk_box_pack_start(GTK_BOX(right_middle_box), right_zoom_in_btn, FALSE, FALSE, 1);
    g_signal_connect(G_OBJECT(right_zoom_in_btn), "clicked", G_CALLBACK(on_zoom_in_right), NULL);

    /* Right toolbar - Zoom out */
    GtkWidget *right_zoom_out_icon = create_toolbar_icon("zoom-out");
    GtkWidget *right_zoom_out_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(right_zoom_out_btn), right_zoom_out_icon);
    g_object_set_data_full(G_OBJECT(right_zoom_out_btn), "icon-name", g_strdup("zoom-out"), g_free);
    gtk_widget_set_tooltip_text(right_zoom_out_btn, "Zoom out");
    atk_object_set_name(gtk_widget_get_accessible(right_zoom_out_btn), "Zoom out");
    gtk_box_pack_start(GTK_BOX(right_middle_box), right_zoom_out_btn, FALSE, FALSE, 1);
    g_signal_connect(G_OBJECT(right_zoom_out_btn), "clicked", G_CALLBACK(on_zoom_out_right), NULL);

    /* Right toolbar separator */
    GtkWidget *right_sep_c = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(right_middle_box), right_sep_c, FALSE, FALSE, 5);

    /* Right toolbar - Page column */
    GtkWidget *right_column_icon = create_toolbar_icon("column");
    app.right_column_btn = gtk_radio_button_new(NULL);
    gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(app.right_column_btn), FALSE);
    gtk_button_set_image(GTK_BUTTON(app.right_column_btn), right_column_icon);
    g_object_set_data_full(G_OBJECT(app.right_column_btn), "icon-name", g_strdup("column"), g_free);
    gtk_widget_set_tooltip_text(app.right_column_btn, "Page column");
    atk_object_set_name(gtk_widget_get_accessible(app.right_column_btn), "Page column");
    g_object_set_data(G_OBJECT(app.right_column_btn), "layout-id", GINT_TO_POINTER(0 + 1));
    g_signal_connect(app.right_column_btn, "toggled", G_CALLBACK(on_layout_right_toggled), NULL);
    gtk_box_pack_start(GTK_BOX(right_middle_box), app.right_column_btn, FALSE, FALSE, 1);

    /* Right toolbar - Page double column */
    GtkWidget *right_double_column_icon = create_toolbar_icon("double-column");
    app.right_double_column_btn = gtk_radio_button_new_from_widget(GTK_RADIO_BUTTON(app.right_column_btn));
    gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(app.right_double_column_btn), FALSE);
    gtk_button_set_image(GTK_BUTTON(app.right_double_column_btn), right_double_column_icon);
    g_object_set_data_full(G_OBJECT(app.right_double_column_btn), "icon-name", g_strdup("double-column"), g_free);
    gtk_widget_set_tooltip_text(app.right_double_column_btn, "Page double column");
    atk_object_set_name(gtk_widget_get_accessible(app.right_double_column_btn), "Page double column");
    g_object_set_data(G_OBJECT(app.right_double_column_btn), "layout-id", GINT_TO_POINTER(1 + 1));
    g_signal_connect(app.right_double_column_btn, "toggled", G_CALLBACK(on_layout_right_toggled), NULL);
    gtk_box_pack_start(GTK_BOX(right_middle_box), app.right_double_column_btn, FALSE, FALSE, 1);

    /* Right toolbar - Page row */
    GtkWidget *right_row_icon = create_toolbar_icon("row");
    app.right_row_btn = gtk_radio_button_new_from_widget(GTK_RADIO_BUTTON(app.right_column_btn));
    gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(app.right_row_btn), FALSE);
    gtk_button_set_image(GTK_BUTTON(app.right_row_btn), right_row_icon);
    g_object_set_data_full(G_OBJECT(app.right_row_btn), "icon-name", g_strdup("row"), g_free);
    gtk_widget_set_tooltip_text(app.right_row_btn, "Page row");
    atk_object_set_name(gtk_widget_get_accessible(app.right_row_btn), "Page row");
    g_object_set_data(G_OBJECT(app.right_row_btn), "layout-id", GINT_TO_POINTER(2 + 1));
    g_signal_connect(app.right_row_btn, "toggled", G_CALLBACK(on_layout_right_toggled), NULL);
    gtk_box_pack_start(GTK_BOX(right_middle_box), app.right_row_btn, FALSE, FALSE, 1);

    /* Right toolbar separator */
    GtkWidget *right_sep_d = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(right_middle_box), right_sep_d, FALSE, FALSE, 5);

    return app.window;
}

void hide_right_pane(void) {
    if (app.right_pane) {
        gtk_widget_hide(GTK_WIDGET(app.right_pane));
    }
}


