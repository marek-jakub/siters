#include "fileinfo.h"
#include "app.h"
#include "state.h"
#include "search.h"
#include "ui/sidebar.h"

extern App app;

gchar* format_file_size(goffset size) {
    if (size < 1024)
        return g_strdup_printf("%lld B", (long long)size);
    else if (size < 1024 * 1024)
        return g_strdup_printf("%.1f KB", size / 1024.0);
    else if (size < 1024 * 1024 * 1024)
        return g_strdup_printf("%.1f MB", size / (1024.0 * 1024.0));
    else
        return g_strdup_printf("%.1f GB", size / (1024.0 * 1024.0 * 1024.0));
}

void update_file_info_labels(TabData *tab) {
    search_clear();
    if (app.search_entry) {
        gtk_entry_set_text(GTK_ENTRY(app.search_entry), "");
    }
    if (!tab || !tab->current_file) {
        gtk_label_set_text(GTK_LABEL(app.file_info_name_label), "Name: (no file)");
        gtk_label_set_text(GTK_LABEL(app.file_info_path_label), "Path: (none)");
        gtk_label_set_text(GTK_LABEL(app.file_info_size_label), "Size: (none)");
        gtk_label_set_text(GTK_LABEL(app.file_info_pages_label), "Pages: (none)");
        return;
    }

    gchar *basename = g_path_get_basename(tab->current_file);
    gchar *name_text = g_strdup_printf("Name: %s", basename);
    g_free(basename);
    gtk_label_set_text(GTK_LABEL(app.file_info_name_label), name_text);
    g_free(name_text);

    gchar *path_text = g_strdup_printf("Path: %s", tab->current_file);
    gtk_label_set_text(GTK_LABEL(app.file_info_path_label), path_text);
    g_free(path_text);

    GFile *gf = g_file_new_for_path(tab->current_file);
    GFileInfo *info = g_file_query_info(gf, G_FILE_ATTRIBUTE_STANDARD_SIZE,
                                         G_FILE_QUERY_INFO_NONE, NULL, NULL);
    if (info) {
        goffset size = g_file_info_get_size(info);
        gchar *size_str = format_file_size(size);
        gchar *size_text = g_strdup_printf("Size: %s", size_str);
        gtk_label_set_text(GTK_LABEL(app.file_info_size_label), size_text);
        g_free(size_text);
        g_free(size_str);
        g_object_unref(info);
    } else {
        gtk_label_set_text(GTK_LABEL(app.file_info_size_label), "Size: Unknown");
    }
    g_object_unref(gf);

    if (tab->doc) {
        int n_pages = pdfr_count_pages(tab->doc);
        gchar *pages_text = g_strdup_printf("Pages: %d", n_pages);
        gtk_label_set_text(GTK_LABEL(app.file_info_pages_label), pages_text);
        g_free(pages_text);
    } else {
        gtk_label_set_text(GTK_LABEL(app.file_info_pages_label), "Pages: N/A");
    }
}

void on_left_file_info_toggled(GtkToggleButton *btn, gpointer user_data) {
    (void)user_data;

    if (!gtk_toggle_button_get_active(btn)) {
        if (app.current_sidebar_mode == SIDEBAR_FILE_INFO) {
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

    g_signal_handlers_block_by_func(app.settings_btn, G_CALLBACK(on_settings_toggled), NULL);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app.settings_btn), FALSE);
    g_signal_handlers_unblock_by_func(app.settings_btn, G_CALLBACK(on_settings_toggled), NULL);

    gtk_widget_hide(app.sidebar_label);
    gtk_widget_hide(app.sessions_container);
    gtk_widget_hide(app.toc_container);
    gtk_widget_hide(app.settings_container);
    gtk_tree_store_clear(app.toc_tree_store);

    update_file_info_labels(get_current_left_tab());
    gtk_widget_show_all(app.file_info_container);
    gtk_widget_hide(app.search_no_results_label);

    gtk_box_pack_start(GTK_BOX(app.main_hbox), app.sidebar, FALSE, FALSE, 0);
    gtk_box_reorder_child(GTK_BOX(app.main_hbox), app.content_vbox, 2);
    gtk_widget_set_size_request(app.sidebar, 300, -1);
    gtk_widget_show(app.sidebar);
    app.current_sidebar_mode = SIDEBAR_FILE_INFO;
}

static void on_right_file_info_popover_closed(GtkPopover *popover, gpointer user_data) {
    (void)popover;
    GtkWidget *btn = GTK_WIDGET(user_data);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(btn), FALSE);
}

void on_right_file_info_clicked(GtkButton *button, gpointer user_data) {
    (void)user_data;
    GtkWidget *btn = GTK_WIDGET(button);

    if (!app.right_file_info_popover) {
        app.right_file_info_popover = gtk_popover_new(btn);
        gtk_popover_set_position(GTK_POPOVER(app.right_file_info_popover), GTK_POS_LEFT);
        gtk_popover_set_modal(GTK_POPOVER(app.right_file_info_popover), FALSE);

        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_container_set_border_width(GTK_CONTAINER(box), 8);

        app.right_popover_name_label = gtk_label_new("Name: (no file)");
        gtk_widget_set_halign(app.right_popover_name_label, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(box), app.right_popover_name_label, FALSE, FALSE, 0);

        app.right_popover_path_label = gtk_label_new("Path: (none)");
        gtk_widget_set_halign(app.right_popover_path_label, GTK_ALIGN_FILL);
        gtk_label_set_line_wrap(GTK_LABEL(app.right_popover_path_label), TRUE);
        gtk_label_set_line_wrap_mode(GTK_LABEL(app.right_popover_path_label), PANGO_WRAP_WORD_CHAR);
        gtk_label_set_max_width_chars(GTK_LABEL(app.right_popover_path_label), 60);
        gtk_box_pack_start(GTK_BOX(box), app.right_popover_path_label, FALSE, FALSE, 0);

        app.right_popover_size_label = gtk_label_new("Size: (none)");
        gtk_widget_set_halign(app.right_popover_size_label, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(box), app.right_popover_size_label, FALSE, FALSE, 0);

        app.right_popover_pages_label = gtk_label_new("Pages: (none)");
        gtk_widget_set_halign(app.right_popover_pages_label, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(box), app.right_popover_pages_label, FALSE, FALSE, 0);

        gtk_container_add(GTK_CONTAINER(app.right_file_info_popover), box);
        gtk_widget_show_all(box);
        g_signal_connect(app.right_file_info_popover, "closed",
                         G_CALLBACK(on_right_file_info_popover_closed), btn);
    }

    if (gtk_widget_get_mapped(app.right_file_info_popover)) {
        gtk_popover_popdown(GTK_POPOVER(app.right_file_info_popover));
    } else {
        TabData *tab = get_current_right_tab();

        if (tab && tab->current_file) {
            gchar *basename = g_path_get_basename(tab->current_file);
            gchar *text = g_strdup_printf("Name: %s", basename);
            gtk_label_set_text(GTK_LABEL(app.right_popover_name_label), text);
            g_free(text);
            g_free(basename);

            text = g_strdup_printf("Path: %s", tab->current_file);
            gtk_label_set_text(GTK_LABEL(app.right_popover_path_label), text);
            g_free(text);

            GFile *gf = g_file_new_for_path(tab->current_file);
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

            if (tab->doc) {
                gchar *pages_text = g_strdup_printf("Pages: %d", pdfr_count_pages(tab->doc));
                gtk_label_set_text(GTK_LABEL(app.right_popover_pages_label), pages_text);
                g_free(pages_text);
            } else {
                gtk_label_set_text(GTK_LABEL(app.right_popover_pages_label), "Pages: N/A");
            }
        } else {
            gtk_label_set_text(GTK_LABEL(app.right_popover_name_label), "Name: (no file)");
            gtk_label_set_text(GTK_LABEL(app.right_popover_path_label), "Path: (none)");
            gtk_label_set_text(GTK_LABEL(app.right_popover_size_label), "Size: (none)");
            gtk_label_set_text(GTK_LABEL(app.right_popover_pages_label), "Pages: (none)");
        }

        gtk_popover_popup(GTK_POPOVER(app.right_file_info_popover));
    }
}