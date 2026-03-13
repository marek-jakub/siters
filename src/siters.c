#include <gtk/gtk.h>
#include <atk/atk.h>
#include "siters.h"

static void on_horiz_scroll_toggle(GtkToggleButton *button, gpointer user_data) {
    GtkImage *image = GTK_IMAGE(user_data);
    if (gtk_toggle_button_get_active(button)) {
        gtk_image_set_from_file(image, "./data/icons/horiz-scroll-on.png");
    } else {
        gtk_image_set_from_file(image, "./data/icons/horiz-scroll-off.png");
    }
}

static void on_title_bar_toggle(GtkToggleButton *button, gpointer user_data) {
    GtkImage *image = GTK_IMAGE(user_data);
    if (gtk_toggle_button_get_active(button)) {
        gtk_image_set_from_file(image, "./data/icons/title-bar-on.png");
    } else {
        gtk_image_set_from_file(image, "./data/icons/title-bar-off.png");
    }
}

static void on_helper_toggle(GtkToggleButton *button, gpointer user_data) {
    GtkImage *image = GTK_IMAGE(user_data);
    if (gtk_toggle_button_get_active(button)) {
        gtk_image_set_from_file(image, "./data/icons/sidebar-helper-on.png");
    } else {
        gtk_image_set_from_file(image, "./data/icons/sidebar-helper-off.png");
    }
}

static void on_minimize_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    GtkWindow *window = GTK_WINDOW(user_data);
    gtk_window_iconify(window);
}

static void on_maximize_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    GtkWindow *window = GTK_WINDOW(user_data);
    if (gtk_window_is_maximized(window)) {
        gtk_window_unmaximize(window);
    } else {
        gtk_window_maximize(window);
    }
}

GtkWidget* create_main_window(void) {
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "Siters");
    gtk_window_set_default_size(GTK_WINDOW(window), 1000, 800);

    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    /* Main horizontal container: toolbar on left, content on right */
    GtkWidget *main_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_container_add(GTK_CONTAINER(window), main_hbox);

    /* Left sidebar: main toolbar */
    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(toolbar), "toolbar");
    gtk_box_pack_start(GTK_BOX(main_hbox), toolbar, FALSE, FALSE, 0);

    /* Content area on the right*/
    GtkWidget *content_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(main_hbox), content_vbox, TRUE, TRUE, 0);

    /* Buttons*/
    /* Sessions button */
    GtkWidget *sessions_icon = gtk_image_new_from_file("./data/icons/sessions.png");
    GtkWidget *sessions_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(sessions_btn), sessions_icon);
    gtk_widget_set_tooltip_text(sessions_btn, "Sessions");
    atk_object_set_name(gtk_widget_get_accessible(sessions_btn), "Sessions");
    gtk_box_pack_start(GTK_BOX(toolbar), sessions_btn, FALSE, FALSE, 1);

    /* Table of contents button */
    GtkWidget *toc_icon = gtk_image_new_from_file("./data/icons/toc.png");
    GtkWidget *toc_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(toc_btn), toc_icon);
    gtk_widget_set_tooltip_text(toc_btn, "Table of contents");
    atk_object_set_name(gtk_widget_get_accessible(toc_btn), "Table of contents");
    gtk_box_pack_start(GTK_BOX(toolbar), toc_btn, FALSE, FALSE, 1);

    /* Settings button */
    GtkWidget *settings_icon = gtk_image_new_from_file("./data/icons/settings.png");
    GtkWidget *settings_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(settings_btn), settings_icon);
    gtk_widget_set_tooltip_text(settings_btn, "Settings");
    atk_object_set_name(gtk_widget_get_accessible(settings_btn), "Settings");
    gtk_box_pack_start(GTK_BOX(toolbar), settings_btn, FALSE, FALSE, 1);

    /* Separator */
    GtkWidget *separator_a = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(toolbar), separator_a, FALSE, FALSE, 5);

    /* Open file button */
    GtkWidget *open_file_icon = gtk_image_new_from_file("./data/icons/file-plus.png");
    GtkWidget *open_file_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(open_file_btn), open_file_icon);
    gtk_widget_set_tooltip_text(open_file_btn, "Open file");
    atk_object_set_name(gtk_widget_get_accessible(open_file_btn), "Open file");
    gtk_box_pack_start(GTK_BOX(toolbar), open_file_btn, FALSE, FALSE, 1);

    /* Close file button*/
    GtkWidget *close_file_icon = gtk_image_new_from_file("./data/icons/file-minus.png");
    GtkWidget *close_file_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(close_file_btn), close_file_icon);
    gtk_widget_set_tooltip_text(close_file_btn, "Close file");
    atk_object_set_name(gtk_widget_get_accessible(close_file_btn), "Close file");
    gtk_box_pack_start(GTK_BOX(toolbar), close_file_btn, FALSE, FALSE, 1);

    /* Separator */
    GtkWidget *separator_b = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(toolbar), separator_b, FALSE, FALSE, 5);

    /* Page up button*/
    GtkWidget *page_up_icon = gtk_image_new_from_file("./data/icons/page-up.png");
    GtkWidget *page_up_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(page_up_btn), page_up_icon);
    gtk_widget_set_tooltip_text(page_up_btn, "Page up");
    atk_object_set_name(gtk_widget_get_accessible(page_up_btn), "Page up");
    gtk_box_pack_start(GTK_BOX(toolbar), page_up_btn, FALSE, FALSE, 1);

    /* Page down button*/
    GtkWidget *page_down_icon = gtk_image_new_from_file("./data/icons/page-down.png");
    GtkWidget *page_down_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(page_down_btn), page_down_icon);
    gtk_widget_set_tooltip_text(page_down_btn, "Page down");
    atk_object_set_name(gtk_widget_get_accessible(page_down_btn), "Page down");
    gtk_box_pack_start(GTK_BOX(toolbar), page_down_btn, FALSE, FALSE, 1);

    /* Separator */
    GtkWidget *separator_c = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(toolbar), separator_c, FALSE, FALSE, 5);

    /* Zoom in button*/
    GtkWidget *zoom_in_icon = gtk_image_new_from_file("./data/icons/zoom-in.png");
    GtkWidget *zoom_in_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(zoom_in_btn), zoom_in_icon);
    gtk_widget_set_tooltip_text(zoom_in_btn, "Zoom in");
    atk_object_set_name(gtk_widget_get_accessible(zoom_in_btn), "Zoom in");
    gtk_box_pack_start(GTK_BOX(toolbar), zoom_in_btn, FALSE, FALSE, 1);

    /* Zoom out button*/
    GtkWidget *zoom_out_icon = gtk_image_new_from_file("./data/icons/zoom-out.png");
    GtkWidget *zoom_out_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(zoom_out_btn), zoom_out_icon);
    gtk_widget_set_tooltip_text(zoom_out_btn, "Zoom out");
    atk_object_set_name(gtk_widget_get_accessible(zoom_out_btn), "Zoom out");
    gtk_box_pack_start(GTK_BOX(toolbar), zoom_out_btn, FALSE, FALSE, 1);

    /* Separator */
    GtkWidget *separator_d = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(toolbar), separator_d, FALSE, FALSE, 5);

    /* Column view button*/
    GtkWidget *column_view_icon = gtk_image_new_from_file("./data/icons/column.png");
    GtkWidget *column_view_btn = gtk_radio_button_new(NULL);
    gtk_button_set_image(GTK_BUTTON(column_view_btn), column_view_icon);
    gtk_widget_set_tooltip_text(column_view_btn, "Page column");
    atk_object_set_name(gtk_widget_get_accessible(column_view_btn), "Page column");
    gtk_box_pack_start(GTK_BOX(toolbar), column_view_btn, FALSE, FALSE, 1);

    /* Double column view button*/
    GtkWidget *double_column_view_icon = gtk_image_new_from_file("./data/icons/double-column.png");
    GtkWidget *double_column_view_btn = gtk_radio_button_new_from_widget(GTK_RADIO_BUTTON(column_view_btn));
    gtk_button_set_image(GTK_BUTTON(double_column_view_btn), double_column_view_icon);
    gtk_widget_set_tooltip_text(double_column_view_btn, "Page double column");
    atk_object_set_name(gtk_widget_get_accessible(double_column_view_btn), "Page double column");
    gtk_box_pack_start(GTK_BOX(toolbar), double_column_view_btn, FALSE, FALSE, 1);

    /* Row view button*/
    GtkWidget *row_view_icon = gtk_image_new_from_file("./data/icons/row.png");
    GtkWidget *row_view_btn = gtk_radio_button_new_from_widget(GTK_RADIO_BUTTON(column_view_btn));
    gtk_button_set_image(GTK_BUTTON(row_view_btn), row_view_icon);
    gtk_widget_set_tooltip_text(row_view_btn, "Page row");
    atk_object_set_name(gtk_widget_get_accessible(row_view_btn), "Page row");
    gtk_box_pack_start(GTK_BOX(toolbar), row_view_btn, FALSE, FALSE, 1);

    /* Horizontal scroll toggle button*/
    GtkWidget *horiz_scroll_toggle_icon = gtk_image_new_from_file("./data/icons/horiz-scroll-off.png");
    GtkWidget *horiz_scroll_toggle_btn = gtk_toggle_button_new();
    gtk_button_set_image(GTK_BUTTON(horiz_scroll_toggle_btn), horiz_scroll_toggle_icon);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(horiz_scroll_toggle_btn), FALSE);
    gtk_widget_set_tooltip_text(horiz_scroll_toggle_btn, "Toggle horizontal scroll");
    atk_object_set_name(gtk_widget_get_accessible(horiz_scroll_toggle_btn), "Toggle horizontal scroll");
    g_signal_connect(horiz_scroll_toggle_btn, "toggled", G_CALLBACK(on_horiz_scroll_toggle), horiz_scroll_toggle_icon);
    gtk_box_pack_start(GTK_BOX(toolbar), horiz_scroll_toggle_btn, FALSE, FALSE, 1);

    /* Separator */
    GtkWidget *separator_e = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(toolbar), separator_e, FALSE, FALSE, 5);

    /* Title bar toggle button*/
    GtkWidget *title_bar_toggle_icon = gtk_image_new_from_file("./data/icons/title-bar-on.png");
    GtkWidget *title_bar_toggle_btn = gtk_toggle_button_new();
    gtk_button_set_image(GTK_BUTTON(title_bar_toggle_btn), title_bar_toggle_icon);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(title_bar_toggle_btn), TRUE);
    gtk_widget_set_tooltip_text(title_bar_toggle_btn, "Toggle title bar visibility");
    atk_object_set_name(gtk_widget_get_accessible(title_bar_toggle_btn), "Toggle title bar visibility");
    g_signal_connect(title_bar_toggle_btn, "toggled", G_CALLBACK(on_title_bar_toggle), title_bar_toggle_icon);
    gtk_box_pack_start(GTK_BOX(toolbar), title_bar_toggle_btn, FALSE, FALSE, 1);

    /* Sidebar helper toggle button*/
    GtkWidget *helper_toggle_icon = gtk_image_new_from_file("./data/icons/sidebar-helper-on.png");
    GtkWidget *helper_toggle_btn = gtk_toggle_button_new();
    gtk_button_set_image(GTK_BUTTON(helper_toggle_btn), helper_toggle_icon);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(helper_toggle_btn), FALSE);
    gtk_widget_set_tooltip_text(helper_toggle_btn, "Helper files");
    atk_object_set_name(gtk_widget_get_accessible(helper_toggle_btn), "Helper files");
    g_signal_connect(helper_toggle_btn, "toggled", G_CALLBACK(on_helper_toggle), helper_toggle_icon);
    gtk_box_pack_start(GTK_BOX(toolbar), helper_toggle_btn, FALSE, FALSE, 1);

    /* Separator */
    GtkWidget *separator_f = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(toolbar), separator_f, FALSE, FALSE, 5);

    /* Close button*/
    GtkWidget *close_icon = gtk_image_new_from_file("./data/icons/plug.png");
    GtkWidget *close_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(close_btn), close_icon);
    gtk_widget_set_tooltip_text(close_btn, "Close");
    atk_object_set_name(gtk_widget_get_accessible(close_btn), "Close");
    g_signal_connect(close_btn, "clicked", G_CALLBACK(gtk_main_quit), NULL);
    gtk_box_pack_end(GTK_BOX(toolbar), close_btn, FALSE, FALSE, 1);

    /* Maximize button*/
    GtkWidget *maximize_icon = gtk_image_new_from_file("./data/icons/maximize-2.png");
    GtkWidget *maximize_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(maximize_btn), maximize_icon);
    gtk_widget_set_tooltip_text(maximize_btn, "Maximize");
    atk_object_set_name(gtk_widget_get_accessible(maximize_btn), "Maximize");
    g_signal_connect(maximize_btn, "clicked", G_CALLBACK(on_maximize_clicked), window);
    gtk_box_pack_end(GTK_BOX(toolbar), maximize_btn, FALSE, FALSE, 1);

    /* Minimize button*/
    GtkWidget *minimize_icon = gtk_image_new_from_file("./data/icons/minimize-2.png");
    GtkWidget *minimize_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(minimize_btn), minimize_icon);
    gtk_widget_set_tooltip_text(minimize_btn, "Minimize");
    atk_object_set_name(gtk_widget_get_accessible(minimize_btn), "Minimize");
    g_signal_connect(minimize_btn, "clicked", G_CALLBACK(on_minimize_clicked), window);
    gtk_box_pack_end(GTK_BOX(toolbar), minimize_btn, FALSE, FALSE, 1);

    return window;
}