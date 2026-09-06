#include "zoom.h"
#include "app.h"
#include "tab.h"
#include "state.h"
#include "view.h"
#include "search.h"
#include "ui/tab_lifecycle.h"
#include "siters.h"

extern App app;

/* Idle callback: restore scroll position after zoom view rebuild */
static gboolean restore_zoom_scroll_cb(gpointer user_data) {
    TabData *tab = user_data;
    if (!tab || !tab->cached_page_widths || !tab->scrolled || !tab->zoom_scroll_source_id) {
        return FALSE;
    }
    tab->zoom_scroll_source_id = 0;

    int page = tab->zoom_scroll_target_page;
    double fraction = tab->zoom_scroll_fraction;
    if (page < 0 || page >= tab->n_pages) return FALSE;

    const double spacing = 6.0;
    double scale = get_ppi_scale(tab);

    if (tab->layout_mode == 2) {
        if (!tab->h_scrollbar) return FALSE;
        double x = spacing;
        for (int i = 0; i < page; ++i) {
            x += tab->cached_page_widths[i] * scale + spacing;
        }
        double page_w = tab->cached_page_widths[page] * scale;
        double target = x + fraction * page_w;
        GtkAdjustment *sadj = gtk_range_get_adjustment(GTK_RANGE(tab->h_scrollbar));
        gtk_adjustment_set_value(sadj, target);
    } else if (tab->layout_mode == 1) {
        int row = page / 2;
        double y = spacing;
        for (int i = 0; i < row; ++i) {
            double row_h = 0.0;
            for (int p = 0; p < 2; p++) {
                int idx = i * 2 + p;
                if (idx >= tab->n_pages) break;
                double h = tab->cached_page_heights[idx] * scale;
                if (h > row_h) row_h = h;
            }
            if (row_h < 1.0) row_h = 1.0;
            y += row_h + spacing;
        }
        double curr_row_h = 0.0;
        for (int p = 0; p < 2; p++) {
            int idx = row * 2 + p;
            if (idx >= tab->n_pages) break;
            double h = tab->cached_page_heights[idx] * scale;
            if (h > curr_row_h) curr_row_h = h;
        }
        if (curr_row_h < 1.0) curr_row_h = 1.0;
        double target = y + fraction * curr_row_h;
        GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(tab->scrolled));
        gtk_adjustment_set_value(vadj, target);
    } else {
        double y = spacing;
        for (int i = 0; i < page; ++i) {
            y += tab->cached_page_heights[i] * scale + spacing;
        }
        double page_h = get_page_height_ppi(tab, page);
        double target = y + fraction * page_h;
        GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(tab->scrolled));
        gtk_adjustment_set_value(vadj, target);
    }
    return FALSE;
}

static gboolean deferred_zoom_save(gpointer data) {
    (void)data;
    app.zoom_save_debounce_id = 0;
    save_state();
    return FALSE;
}

static void schedule_zoom_save(void) {
    if (app.zoom_save_debounce_id)
        g_source_remove(app.zoom_save_debounce_id);
    app.zoom_save_debounce_id = g_timeout_add(200, deferred_zoom_save, NULL);
}

static void apply_zoom_to_tab(TabData *tab, int direction) {
    if (!tab) return;
    if (!tab->doc && !ensure_tab_doc_loaded(tab)) return;
    double new_zoom = tab->zoom + (direction > 0 ? 2.0 : -2.0);
    if (new_zoom < 10.0) new_zoom = 10.0;
    if (new_zoom > 500.0) new_zoom = 500.0;

    /* Save current scroll state (using pre-zoom page sizes) */
    if (tab->cached_page_widths && tab->scrolled && tab->n_pages > 0) {
        int page = 0;
        double fraction = 0.0;
        const double spacing = 6.0;
        double old_scale = get_ppi_scale(tab);

        if (tab->layout_mode == 2) {
            if (tab->h_scrollbar) {
                GtkAdjustment *sadj = gtk_range_get_adjustment(GTK_RANGE(tab->h_scrollbar));
                double scroll_x = gtk_adjustment_get_value(sadj);
                double x = spacing;
                for (int i = 0; i < tab->n_pages; ++i) {
                    double page_w = tab->cached_page_widths[i] * old_scale;
                    if (page_w <= 0) continue;
                    if (scroll_x >= x && (scroll_x < x + page_w || i == tab->n_pages - 1)) {
                        page = i;
                        fraction = (scroll_x - x) / page_w;
                        break;
                    }
                    x += page_w + spacing;
                }
            }
        } else if (tab->layout_mode == 1) {
            GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(tab->scrolled));
            double scroll_y = gtk_adjustment_get_value(vadj);
            double row_y = spacing;
            int n_rows = (tab->n_pages + 1) / 2;
            for (int r = 0; r < n_rows; ++r) {
                double row_h = 0.0;
                for (int p = 0; p < 2; p++) {
                    int idx = r * 2 + p;
                    if (idx >= tab->n_pages) break;
                    double h = tab->cached_page_heights[idx] * old_scale;
                    if (h > row_h) row_h = h;
                }
                if (row_h < 1.0) { row_y += spacing; continue; }
                if (scroll_y >= row_y && (scroll_y < row_y + row_h || r == n_rows - 1)) {
                    page = r * 2;
                    fraction = (scroll_y - row_y) / row_h;
                    break;
                }
                row_y += row_h + spacing;
            }
        } else {
            GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(tab->scrolled));
            double scroll_y = gtk_adjustment_get_value(vadj);
            double y = spacing;
            for (int i = 0; i < tab->n_pages; ++i) {
                double page_h = tab->cached_page_heights[i] * old_scale;
                if (page_h <= 0) { y += spacing; continue; }
                if (scroll_y >= y && (scroll_y < y + page_h || i == tab->n_pages - 1)) {
                    page = i;
                    fraction = (scroll_y - y) / page_h;
                    break;
                }
                y += page_h + spacing;
            }
        }

        tab->zoom_scroll_target_page = page;
        tab->zoom_scroll_fraction = CLAMP(fraction, 0.0, 1.0);
    }

    tab->zoom = new_zoom;
    tab->last_zoom = new_zoom;
    build_continuous_view(tab);
    gtk_widget_queue_resize(tab->scrolled);

    /* Schedule deferred scroll restore after layout settles */
    if (tab->doc && tab->n_pages > 0) {
        if (tab->zoom_scroll_source_id)
            g_source_remove(tab->zoom_scroll_source_id);
        tab->zoom_scroll_source_id = g_idle_add(restore_zoom_scroll_cb, tab);
    }

    update_document_model_from_tab(tab);
    schedule_zoom_save();
}

void on_zoom_in_left(GtkButton *btn, gpointer user_data) {
    (void)btn;
    (void)user_data;
    apply_zoom_to_tab(get_current_left_tab(), 1);
}

void on_zoom_out_left(GtkButton *btn, gpointer user_data) {
    (void)btn;
    (void)user_data;
    apply_zoom_to_tab(get_current_left_tab(), -1);
}

void on_zoom_in_right(GtkButton *btn, gpointer user_data) {
    (void)btn;
    (void)user_data;
    apply_zoom_to_tab(get_current_right_tab(), 1);
}

void on_zoom_out_right(GtkButton *btn, gpointer user_data) {
    (void)btn;
    (void)user_data;
    apply_zoom_to_tab(get_current_right_tab(), -1);
}

void on_page_up_left(GtkButton *btn, gpointer user_data) {
    (void)btn;
    (void)user_data;
    TabData *tab = get_current_left_tab();
    if (!tab || tab->n_pages <= 0 || tab->cur_page <= 0) return;
    tab->cur_page--;
    scroll_to_page(tab, tab->cur_page, -1);
}

void on_page_down_left(GtkButton *btn, gpointer user_data) {
    (void)btn;
    (void)user_data;
    TabData *tab = get_current_left_tab();
    if (!tab || tab->n_pages <= 0 || tab->cur_page >= tab->n_pages - 1) return;
    tab->cur_page++;
    scroll_to_page(tab, tab->cur_page, -1);
}

void on_page_up_right(GtkButton *btn, gpointer user_data) {
    (void)btn;
    (void)user_data;
    TabData *tab = get_current_right_tab();
    if (!tab || tab->n_pages <= 0 || tab->cur_page <= 0) return;
    tab->cur_page--;
    scroll_to_page(tab, tab->cur_page, -1);
}

void on_page_down_right(GtkButton *btn, gpointer user_data) {
    (void)btn;
    (void)user_data;
    TabData *tab = get_current_right_tab();
    if (!tab || tab->n_pages <= 0 || tab->cur_page >= tab->n_pages - 1) return;
    tab->cur_page++;
    scroll_to_page(tab, tab->cur_page, -1);
}
