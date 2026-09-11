/* Main window factory: top-level window setup, session-init, theme wiring,
   and the paned / notebook layout. The sidebar and toolbar segments are
   built by window_sidebar.c and window_toolbars.c. */

#include "window.h"
#include "app.h"
#include "state.h"
#include "ui/notebook_switching.h"
#include "ui/theme.h"
#include "ui/toolbar.h"
#include "nav.h"

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
    GtkWidget *toolbar = window_create_main_toolbar();
    gtk_box_pack_start(GTK_BOX(app.main_hbox), toolbar, FALSE, FALSE, 0);

    window_build_sidebar();

    /* Content area on the right*/
    app.content_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(app.main_hbox), app.content_vbox, TRUE, TRUE, 0);

    window_fill_main_toolbar(toolbar);

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


    window_build_right_toolbar();

    return app.window;
}
