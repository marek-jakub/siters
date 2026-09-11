/* Toolbar construction (left and right). The button handlers live in the
   ui/session/view modules; this file only wires them up. */

#include "window.h"
#include "app.h"
#include "state.h"
#include "ui/theme.h"
#include "ui/sidebar.h"
#include "ui/toolbar.h"
#include "ui/zoom.h"
#include "ui/layout.h"
#include "fileinfo/fileinfo.h"
#include "session/sessions_actions.h"
#include "nav.h"

GtkWidget *window_create_main_toolbar(void) {
    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(toolbar), "Toolbar");
    gtk_widget_set_size_request(toolbar, 36, -1);
    return toolbar;
}

void window_fill_main_toolbar(GtkWidget *toolbar) {
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
}

void window_build_right_toolbar(void) {
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
}
