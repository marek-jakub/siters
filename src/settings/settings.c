#include "settings/settings.h"
#include "app.h"
#include "tab.h"
#include "state.h"
#include "siters.h"
#include "view.h"
#include "theme.h"
#include "sessions_model.h"
#include "ui/sidebar.h"

extern App app;

double get_angle_for_position(const char *pos) {
    if (g_strcmp0(pos, "left") == 0) return 90.0;
    if (g_strcmp0(pos, "right") == 0) return -90.0;
    return 0.0;
}

void apply_tabbar_position(const char *pos) {
    GtkPositionType gpos = GTK_POS_TOP;
    if (g_strcmp0(pos, "left") == 0) gpos = GTK_POS_LEFT;
    else if (g_strcmp0(pos, "right") == 0) gpos = GTK_POS_RIGHT;
    if (app.left_notebook)
        gtk_notebook_set_tab_pos(GTK_NOTEBOOK(app.left_notebook), gpos);
    if (app.right_notebook)
        gtk_notebook_set_tab_pos(GTK_NOTEBOOK(app.right_notebook), gpos);

    gboolean is_side = (g_strcmp0(pos, "left") == 0 || g_strcmp0(pos, "right") == 0);
    if (app.left_notebook) {
        gtk_notebook_set_show_border(GTK_NOTEBOOK(app.left_notebook), !is_side);
        gtk_widget_set_size_request(app.left_notebook, is_side ? -1 : 70, is_side ? 200 : -1);
    }
    if (app.right_notebook) {
        gtk_notebook_set_show_border(GTK_NOTEBOOK(app.right_notebook), !is_side);
        gtk_widget_set_size_request(app.right_notebook, -1, is_side ? 200 : -1);
    }

    /* Adjust page nav overlay so it doesn't sit over a left-side tab bar */
    if (app.page_nav_overlay) {
        if (g_strcmp0(pos, "left") == 0) {
            gtk_widget_set_margin_start(app.page_nav_overlay, 60);
        } else {
            gtk_widget_set_margin_start(app.page_nav_overlay, 16);
        }
    }
    /* Adjust right page nav overlay for right-side tab bar */
    if (app.right_page_nav_overlay) {
        if (g_strcmp0(pos, "right") == 0) {
            gtk_widget_set_margin_end(app.right_page_nav_overlay, 60);
        } else {
            gtk_widget_set_margin_end(app.right_page_nav_overlay, 8);
        }
    }

    double angle = get_angle_for_position(pos);
    GtkOrientation box_orientation = is_side ? GTK_ORIENTATION_VERTICAL : GTK_ORIENTATION_HORIZONTAL;
    GtkWidget *notebooks[] = {app.left_notebook, app.right_notebook};
    for (int n = 0; n < 2; n++) {
        if (!notebooks[n]) continue;
        int np = gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebooks[n]));
        for (int i = 0; i < np; i++) {
            GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(notebooks[n]), i);
            TabData *tab = g_object_get_data(G_OBJECT(page), "tab-data");
            if (tab && tab->tab_label)
                gtk_label_set_angle(GTK_LABEL(tab->tab_label), angle);
            if (tab && tab->tab_label_box) {
                gtk_orientable_set_orientation(GTK_ORIENTABLE(tab->tab_label_box), box_orientation);
                if (g_strcmp0(pos, "left") == 0)
                    gtk_box_reorder_child(GTK_BOX(tab->tab_label_box), tab->tab_label, 1);
                else
                    gtk_box_reorder_child(GTK_BOX(tab->tab_label_box), tab->tab_label, 0);
            }
            if (tab && tab->tab_label_close_btn) {
                gtk_widget_set_has_tooltip(tab->tab_label_close_btn, TRUE);
                gtk_widget_set_tooltip_text(tab->tab_label_close_btn, "Close tab");
            }
        }
    }
}

void apply_tab_width(int width) {
    /* Truncate all existing tab labels to 'width' characters */
    GtkWidget *notebooks[] = {app.left_notebook, app.right_notebook};
    for (int n = 0; n < 2; n++) {
        if (!notebooks[n]) continue;
        int np = gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebooks[n]));
        for (int i = 0; i < np; i++) {
            GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(notebooks[n]), i);
            TabData *tab = g_object_get_data(G_OBJECT(page), "tab-data");
            if (!tab || !tab->tab_label || !tab->current_file) continue;
            char *basename = g_path_get_basename(tab->current_file);
            const char *label_text = basename;
            char *truncated = NULL;
            if (width > 0 && (int)strlen(basename) > width) {
                truncated = g_strndup(basename, width);
                label_text = truncated;
            }
            gtk_label_set_text(GTK_LABEL(tab->tab_label), label_text);
            g_free(truncated);
            g_free(basename);
        }
    }
}

void on_tabbar_combo_changed(GtkComboBox *combo, gpointer user_data) {
    (void)user_data;
    if (!app.sessions_model) return;
    const char *pos = gtk_combo_box_get_active_id(GTK_COMBO_BOX(combo));
    if (!pos) return;
    sessions_model_set_tabbar_position(app.sessions_model, pos);
    apply_tabbar_position(pos);
    save_state();
}

void on_tab_width_spin_changed(GtkSpinButton *spin, gpointer user_data) {
    (void)user_data;
    if (!app.sessions_model) return;
    int w = (int)gtk_spin_button_get_value(spin);
    sessions_model_set_tab_width(app.sessions_model, w);
    apply_tab_width(w);
    save_state();
}

void on_keep_dark_toggled(GtkToggleButton *btn, gpointer user_data) {
    (void)user_data;
    app.keep_dark_theme = gtk_toggle_button_get_active(btn);
    if (app.keep_dark_theme) {
        app.is_dark_theme = TRUE;
        apply_dark_css(TRUE);
    } else {
        apply_dark_css(FALSE);
        app.is_dark_theme = detect_system_dark_theme();
    }
    recolor_all_toolbars();
    if (app.sessions_model)
        sessions_model_set_keep_dark(app.sessions_model, app.keep_dark_theme);
    save_state();
}

session_model_t *get_current_session_model(void) {
    if (!app.current_selected_session || !app.session_models) return NULL;
    return g_hash_table_lookup(app.session_models, app.current_selected_session);
}

void apply_page_color_to_notebook(GtkWidget *notebook, const char *color_str) {
    if (!notebook) return;
    GdkRGBA color;
    if (!gdk_rgba_parse(&color, color_str)) return;
    int np = gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook));
    for (int i = 0; i < np; i++) {
        GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(notebook), i);
        TabData *tab = g_object_get_data(G_OBJECT(page), "tab-data");
        if (tab) {
            tab->page_color = color;
            queue_draw(tab);
        }
    }
}

void on_left_color_set(GtkColorButton *btn, gpointer user_data) {
    (void)user_data;
    session_model_t *session = get_current_session_model();
    if (!session) return;
    GdkRGBA color;
    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(btn), &color);
    char *str = gdk_rgba_to_string(&color);
    session_model_set_page_color(session, str);
    apply_page_color_to_notebook(app.left_notebook, str);
    g_free(str);
    save_state();
}

void on_right_color_set(GtkColorButton *btn, gpointer user_data) {
    (void)user_data;
    session_model_t *session = get_current_session_model();
    if (!session) return;
    GdkRGBA color;
    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(btn), &color);
    char *str = gdk_rgba_to_string(&color);
    session_model_set_helper_page_color(session, str);
    apply_page_color_to_notebook(app.right_notebook, str);
    g_free(str);
    save_state();
}

void on_settings_toggled(GtkToggleButton *btn, gpointer user_data) {
    (void)user_data;

    if (!gtk_toggle_button_get_active(btn)) {
        if (app.current_sidebar_mode == SIDEBAR_SETTINGS) {
            gtk_container_remove(GTK_CONTAINER(app.main_hbox), app.sidebar);
            gtk_box_reorder_child(GTK_BOX(app.main_hbox), app.content_vbox, 1);
            app.current_sidebar_mode = SIDEBAR_NONE;
        }
        return;
    }

    if (gtk_widget_get_parent(app.sidebar) != NULL) {
        gtk_container_remove(GTK_CONTAINER(app.main_hbox), app.sidebar);
    }

    /* Deactivate other toggle buttons */
    g_signal_handlers_block_by_func(app.sessions_btn, G_CALLBACK(on_sessions_toggled), NULL);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app.sessions_btn), FALSE);
    g_signal_handlers_unblock_by_func(app.sessions_btn, G_CALLBACK(on_sessions_toggled), NULL);

    g_signal_handlers_block_by_func(app.toc_btn, G_CALLBACK(on_toc_toggled), NULL);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app.toc_btn), FALSE);
    g_signal_handlers_unblock_by_func(app.toc_btn, G_CALLBACK(on_toc_toggled), NULL);

    g_signal_handlers_block_by_func(app.file_info_btn, G_CALLBACK(on_left_file_info_toggled), NULL);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app.file_info_btn), FALSE);
    g_signal_handlers_unblock_by_func(app.file_info_btn, G_CALLBACK(on_left_file_info_toggled), NULL);

    /* Hide other sidebar contents */
    gtk_widget_hide(app.sidebar_label);
    gtk_widget_hide(app.sessions_container);
    gtk_widget_hide(app.toc_container);
    gtk_widget_hide(app.file_info_container);
    gtk_tree_store_clear(app.toc_tree_store);

    /* Show settings container */
    gtk_widget_show_all(app.settings_container);

    gtk_box_pack_start(GTK_BOX(app.main_hbox), app.sidebar, FALSE, FALSE, 0);
    gtk_box_reorder_child(GTK_BOX(app.main_hbox), app.content_vbox, 2);
    gtk_widget_set_size_request(app.sidebar, 300, -1);
    gtk_widget_show(app.sidebar);
    app.current_sidebar_mode = SIDEBAR_SETTINGS;
}
