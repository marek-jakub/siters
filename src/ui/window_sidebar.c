/* Sidebar construction: sessions, TOC, settings, file-info and search panes,
   all packed into app.sidebar (which the sidebar toggle handlers repack into
   app.main_hbox at runtime). */

#include "window.h"
#include "app.h"
#include "state.h"
#include "session/sessions_sidebar.h"
#include "session/sessions_tree.h"
#include "session/sessions_actions.h"
#include "ui/toc.h"
#include "search.h"
#include "settings/settings.h"

void window_build_sidebar(void) {
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
}
