#include "layout.h"
#include "app.h"
#include "state.h"
#include "view.h"
#include "nav.h"
#include "search.h"
#include "session/state.h"

extern App app;

static void apply_layout_to_tab(TabData *tab, int layout) {
    if (!tab) return;
    tab->layout_mode = layout;
    build_continuous_view(tab);
    tab->initial_scroll_pending = TRUE;
    gtk_widget_queue_resize(tab->scrolled);
    update_document_model_from_tab(tab);
    save_state();
    if (tab == get_current_left_tab() || tab == get_current_right_tab()) {
        sync_page_widget_from_tab(tab);
    }
}

void on_layout_left_toggled(GtkToggleButton *btn, gpointer user_data) {
    (void)user_data;
    if (!gtk_toggle_button_get_active(btn)) return;
    gpointer idptr = g_object_get_data(G_OBJECT(btn), "layout-id");
    if (!idptr) return;
    int layout = GPOINTER_TO_INT(idptr) - 1;
    TabData *tab = get_current_left_tab();
    if (!tab) return;
    apply_layout_to_tab(tab, layout);
}

void on_layout_right_toggled(GtkToggleButton *btn, gpointer user_data) {
    (void)user_data;
    if (!gtk_toggle_button_get_active(btn)) return;
    gpointer idptr = g_object_get_data(G_OBJECT(btn), "layout-id");
    if (!idptr) return;
    int layout = GPOINTER_TO_INT(idptr) - 1;
    TabData *tab = get_current_right_tab();
    if (!tab) return;
    apply_layout_to_tab(tab, layout);
}

void sync_left_layout_buttons(TabData *tab) {
    if (!app.left_column_btn) return;
    g_signal_handlers_block_by_func(app.left_column_btn, G_CALLBACK(on_layout_left_toggled), NULL);
    g_signal_handlers_block_by_func(app.left_double_column_btn, G_CALLBACK(on_layout_left_toggled), NULL);
    g_signal_handlers_block_by_func(app.left_row_btn, G_CALLBACK(on_layout_left_toggled), NULL);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app.left_column_btn), tab && tab->layout_mode == 0);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app.left_double_column_btn), tab && tab->layout_mode == 1);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app.left_row_btn), tab && tab->layout_mode == 2);
    g_signal_handlers_unblock_by_func(app.left_column_btn, G_CALLBACK(on_layout_left_toggled), NULL);
    g_signal_handlers_unblock_by_func(app.left_double_column_btn, G_CALLBACK(on_layout_left_toggled), NULL);
    g_signal_handlers_unblock_by_func(app.left_row_btn, G_CALLBACK(on_layout_left_toggled), NULL);
}

void sync_right_layout_buttons(TabData *tab) {
    if (!app.right_column_btn) return;
    g_signal_handlers_block_by_func(app.right_column_btn, G_CALLBACK(on_layout_right_toggled), NULL);
    g_signal_handlers_block_by_func(app.right_double_column_btn, G_CALLBACK(on_layout_right_toggled), NULL);
    g_signal_handlers_block_by_func(app.right_row_btn, G_CALLBACK(on_layout_right_toggled), NULL);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app.right_column_btn), tab && tab->layout_mode == 0);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app.right_double_column_btn), tab && tab->layout_mode == 1);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app.right_row_btn), tab && tab->layout_mode == 2);
    g_signal_handlers_unblock_by_func(app.right_column_btn, G_CALLBACK(on_layout_right_toggled), NULL);
    g_signal_handlers_unblock_by_func(app.right_double_column_btn, G_CALLBACK(on_layout_right_toggled), NULL);
    g_signal_handlers_unblock_by_func(app.right_row_btn, G_CALLBACK(on_layout_right_toggled), NULL);
}
