#include <gtk/gtk.h>
#include <cairo.h>
#include <math.h>
#include "tab.h"
#include "pdf.h"
#include "pdf/links.h"
#include "mem_debug.h"
#include "view.h"
#include "scroll.h"

/* Upper bound for widget size requests in pixels (int-safe). */
#define MAX_SIZE_REQUEST 100000000

/* Clamp a double to [0, max] before converting to int. This avoids
   implementation-defined (and potentially UB) behavior when absurd PDF page
   sizes or zoom values produce doubles beyond the int range. */
int clamp_double_to_int(double v, int max) {
    if (!(v > 0.0)) return 0;
    if (v >= (double)max) return max;
    return (int)v;
}

/* Page-view geometry: PPI scale, page offsets and page heights at the current
   zoom, plus the rendered-page pixel-buffer cache used by on_draw. */

/* Helper: Calculate PPI-based scale for a tab */
double get_ppi_scale(TabData *tab) {
    double eff = tab->zoom > 0 ? tab->zoom : 96.0;
    return eff / 72.0;
}

/* Helper: Calculate offset to top of a given page at current PPI zoom */
double calculate_page_top_offset_ppi(TabData *tab, int page_idx) {
    if (!tab || !tab->cached_page_widths || page_idx < 0 || page_idx >= tab->n_pages) {
        return 0.0;
    }

    const double spacing = 6.0;
    double scale = get_ppi_scale(tab);

    double y_offset = spacing;
    if (tab->layout_mode == 2) {
        for (int i = 0; i < page_idx; ++i) {
            y_offset += tab->cached_page_widths[i] * scale + spacing;
        }
    } else if (tab->layout_mode == 1) {
        int row = page_idx / 2;
        for (int r = 0; r < row; r++) {
            double row_h = 0.0;
            for (int p = 0; p < 2; p++) {
                int idx = r * 2 + p;
                if (idx >= tab->n_pages) break;
                double h = tab->cached_page_heights[idx] * scale;
                if (h > row_h) row_h = h;
            }
            if (row_h < 1.0) row_h = 1.0;
            y_offset += row_h + spacing;
        }
    } else {
        for (int i = 0; i < page_idx; ++i) {
            y_offset += tab->cached_page_heights[i] * scale + spacing;
        }
    }
    return y_offset;
}

/* Helper: Get the height of a specific page at current PPI zoom */
double get_page_height_ppi(TabData *tab, int page_idx) {
    if (!tab || !tab->cached_page_heights || page_idx < 0 || page_idx >= tab->n_pages) {
        return 0.0;
    }

    double ph = tab->cached_page_heights[page_idx];
    if (ph <= 0.0) return 0.0;

    double eff_zoom = tab->zoom > 0 ? tab->zoom : 96.0;
    double scale = eff_zoom / 72.0;
    return ph * scale;
}

/* Return pixel-buffer byte count for a cached surface (0 if NULL). */
static inline int surface_byte_size(cairo_surface_t *s) {
    if (!s) return 0;
    return cairo_image_surface_get_width(s) * cairo_image_surface_get_height(s) * 4;
}

/* Destroy a single cached surface and update the tab's byte counter. */
void cache_evict_idx(TabData *tab, int idx) {
    if (!tab || idx < 0 || !tab->page_cache || !tab->page_cache[idx]) return;
    tab->total_cache_bytes -= surface_byte_size(tab->page_cache[idx]);
    cairo_surface_destroy(tab->page_cache[idx]);
    MEM_SURFACE_DESTROYED();
    tab->page_cache[idx] = NULL;
}

void invalidate_page_cache(TabData *tab) {
    if (!tab || !tab->page_cache) return;
    for (int i = 0; i < tab->n_pages; ++i)
        cache_evict_idx(tab, i);
}

void cache_page_dimensions(TabData *tab) {
    if (!tab || !tab->doc || tab->n_pages <= 0) {
        if (tab) {
            invalidate_page_cache(tab);
            g_free(tab->page_cache);
            tab->page_cache = NULL;
            g_free(tab->cached_page_widths);
            g_free(tab->cached_page_heights);
            tab->cached_page_widths = NULL;
            tab->cached_page_heights = NULL;
            g_free(tab->cached_page_x0);
            g_free(tab->cached_page_y0);
            tab->cached_page_x0 = NULL;
            tab->cached_page_y0 = NULL;
        }
        return;
    }
    invalidate_page_cache(tab);
    g_free(tab->page_cache);
    g_free(tab->cached_page_widths);
    g_free(tab->cached_page_heights);
    g_free(tab->cached_page_x0);
    g_free(tab->cached_page_y0);
    tab->page_cache = g_malloc0(sizeof(cairo_surface_t *) * tab->n_pages);
    tab->cached_page_widths = g_malloc(sizeof(double) * tab->n_pages);
    tab->cached_page_heights = g_malloc(sizeof(double) * tab->n_pages);
    tab->cached_page_x0 = g_malloc0(sizeof(double) * tab->n_pages);
    tab->cached_page_y0 = g_malloc0(sizeof(double) * tab->n_pages);
    for (int i = 0; i < tab->n_pages; ++i) {
        PdfrPage *page = pdfr_load_page(tab->doc, i);
        double pw = 0, ph = 0, px0 = 0, py0 = 0;
        if (page) {
            pdfr_page_size(tab->doc, page, &pw, &ph, &px0, &py0);
            pdfr_free_page(tab->doc, page);
        }
        tab->cached_page_widths[i] = pw > 0 ? pw : 1.0;
        tab->cached_page_heights[i] = ph > 0 ? ph : 1.0;
        tab->cached_page_x0[i] = px0;
        tab->cached_page_y0[i] = py0;

    }
}

void queue_draw(TabData *tab) {
    if (tab && tab->pages_drawing)
        gtk_widget_queue_draw(tab->pages_drawing);
}

void scroll_to_page(TabData *tab, int page, double target_y) {
    if (!tab || !tab->scrolled || !tab->pages_drawing || !tab->cached_page_widths) return;
    if (page < 0 || page >= tab->n_pages) return;

    const double spacing = 6.0;
    double scale = get_ppi_scale(tab);

    if (tab->layout_mode == 2) {
        double x = spacing;
        for (int i = 0; i < page; ++i) {
            x += tab->cached_page_widths[i] * scale + spacing;
        }
        if (tab->h_scrollbar) {
            GtkAdjustment *sadj = gtk_range_get_adjustment(GTK_RANGE(tab->h_scrollbar));
            gtk_adjustment_set_value(sadj, x);
        }
        return;
    }

    double y = spacing;
    if (tab->layout_mode == 0) {
        for (int i = 0; i < page; ++i) {
            y += tab->cached_page_heights[i] * scale + spacing;
        }
        if (target_y >= 0) {
            y += (tab->cached_page_heights[page] - target_y) * scale;
        }
    } else if (tab->layout_mode == 1) {
        int row = page / 2;
        for (int i = 0; i < row; ++i) {
            double row_h = 0.0;
            double h1 = tab->cached_page_heights[i * 2] * scale;
            if (h1 > row_h) row_h = h1;
            if (i * 2 + 1 < tab->n_pages) {
                double h2 = tab->cached_page_heights[i * 2 + 1] * scale;
                if (h2 > row_h) row_h = h2;
            }
            y += row_h + spacing;
        }
        if (target_y >= 0) {
            y += (tab->cached_page_heights[page] - target_y) * scale;
        }
    }

    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(tab->scrolled));
    gtk_adjustment_set_value(vadj, y);
}

/* Recompute the continuous-view size request for the current layout mode and
   resize the drawing area / scroll policy accordingly. Called when the page
   cache dimensions change or the view must be rebuilt. */
void build_continuous_view(TabData *tab) {
    if (!tab || !tab->cached_page_widths || !tab->pages_drawing) return;
    invalidate_page_cache(tab);
    tab->built_layout_mode = tab->layout_mode;
    const double spacing = 6.0;
    double scale = get_ppi_scale(tab);
    int page_width_px = tab->n_pages > 0 ? clamp_double_to_int(ceil(tab->cached_page_widths[0] * scale), MAX_SIZE_REQUEST) : 800;
    if (page_width_px < 1) page_width_px = 800;

    if (tab->layout_mode == 0) {
        double total_h = spacing;
        for (int i = 0; i < tab->n_pages; ++i) {
            total_h += tab->cached_page_heights[i] * scale + spacing;
        }
        if (total_h < 1.0) total_h = 1.0;
        if (tab->h_scrollbar) gtk_widget_hide(tab->h_scrollbar);
        gtk_widget_set_size_request(tab->scrolled, -1, -1);
        gtk_widget_set_size_request(tab->pages_drawing, page_width_px, clamp_double_to_int(ceil(total_h), MAX_SIZE_REQUEST));
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(tab->scrolled),
                                       GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    } else if (tab->layout_mode == 1) {
        int n = tab->n_pages;
        double total_h = spacing;
        double max_row_w = 0.0;
        for (int i = 0; i < n; i += 2) {
            double pw1 = tab->cached_page_widths[i];
            double ph1 = tab->cached_page_heights[i];
            double page_w1 = pw1 * scale;
            double page_h1 = ph1 * scale;
            double row_w = page_w1;
            double row_h = page_h1;
            double page_w2 = 0, page_h2 = 0;
            if (i + 1 < n) {
                page_w2 = tab->cached_page_widths[i + 1] * scale;
                page_h2 = tab->cached_page_heights[i + 1] * scale;
            }
            if (page_w2 > 0) row_w += spacing + page_w2;
            if (page_h2 > row_h) row_h = page_h2;
            if (row_w > max_row_w) max_row_w = row_w;
            if (row_h < 1.0) row_h = 1.0;
            total_h += row_h + spacing;
        }
        if (total_h < 1.0) total_h = 1.0;
        if (max_row_w < 1.0) max_row_w = page_width_px;
        if (tab->h_scrollbar) gtk_widget_hide(tab->h_scrollbar);
        gtk_widget_set_size_request(tab->scrolled, -1, -1);
        gtk_widget_set_size_request(tab->pages_drawing, clamp_double_to_int(ceil(max_row_w), MAX_SIZE_REQUEST), clamp_double_to_int(ceil(total_h), MAX_SIZE_REQUEST));
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(tab->scrolled),
                                       GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    } else if (tab->layout_mode == 2) {
        double total_w = 0.0;
        double max_h = 0.0;
        for (int i = 0; i < tab->n_pages; ++i) {
            double page_w = tab->cached_page_widths[i] * scale;
            double page_h = tab->cached_page_heights[i] * scale;
            total_w += page_w + spacing;
            if (page_h > max_h) max_h = page_h;
        }
        if (total_w < 1.0) total_w = 1.0;
        if (max_h < 1.0) max_h = 1.0;
        tab->max_page_h = max_h;
        if (tab->h_scrollbar) {
            GtkAdjustment *adj = gtk_range_get_adjustment(GTK_RANGE(tab->h_scrollbar));
            gtk_adjustment_set_lower(adj, 0.0);
            gtk_adjustment_set_upper(adj, total_w);
        }
        gtk_widget_set_size_request(tab->scrolled, -1, -1);
        gtk_widget_set_size_request(tab->pages_drawing, page_width_px, clamp_double_to_int(ceil(max_h), MAX_SIZE_REQUEST));
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(tab->scrolled),
                                       GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    }
    gtk_widget_queue_draw(tab->pages_drawing);
}


/* Convert widget coordinates to page index and page-relative rendering-space
   (points, y-down, 0 at top of page — matches MuPDF's pixmap orientation).
   Returns 0-based page index, or -1 if no page at (wx, wy). */
int widget_to_page_coords(TabData *tab, double wx, double wy,
                                  double *out_px, double *out_py) {
    if (!tab || !tab->cached_page_widths || !tab->pages_drawing) return -1;
    GtkAllocation alloc;
    gtk_widget_get_allocation(tab->pages_drawing, &alloc);
    double scale = get_ppi_scale(tab);
    const double spacing = 6.0;

    if (tab->layout_mode == 0) {
        double y = spacing;
        for (int i = 0; i < tab->n_pages; i++) {
            double pw = tab->cached_page_widths[i] * scale;
            double ph = tab->cached_page_heights[i] * scale;
            double ox = (alloc.width - pw) / 2.0;
            if (wx >= ox && wx < ox + pw && wy >= y && wy < y + ph) {
                if (out_px) *out_px = (wx - ox) / scale;
                if (out_py) *out_py = (wy - y) / scale;
                return i;
            }
            y += ph + spacing;
        }
    } else if (tab->layout_mode == 1) {
        double y = spacing;
        for (int i = 0; i < tab->n_pages; i += 2) {
            double pw1 = tab->cached_page_widths[i] * scale;
            double ph1 = tab->cached_page_heights[i] * scale;
            double pw2 = 0, ph2 = 0;
            if (i + 1 < tab->n_pages) {
                pw2 = tab->cached_page_widths[i + 1] * scale;
                ph2 = tab->cached_page_heights[i + 1] * scale;
            }
            double rw = pw1 + (pw2 > 0 ? spacing + pw2 : 0);
            double rh = ph1;
            if (ph2 > rh) rh = ph2;
            double rx = (alloc.width - rw) / 2.0;
            if (rx < spacing) rx = spacing;
            double lx = rx;
            double rx2 = lx + pw1 + spacing;
            if (wx >= lx && wx < lx + pw1 && wy >= y && wy < y + ph1) {
                if (out_px) *out_px = (wx - lx) / scale;
                if (out_py) *out_py = (wy - y) / scale;
                return i;
            }
            if (pw2 > 0 && wx >= rx2 && wx < rx2 + pw2 && wy >= y && wy < y + ph2) {
                if (out_px) *out_px = (wx - rx2) / scale;
                if (out_py) *out_py = (wy - y) / scale;
                return i + 1;
            }
            y += rh + spacing;
        }
    } else if (tab->layout_mode == 2) {
        double scroll_x = 0.0;
        if (tab->h_scrollbar) {
            GtkAdjustment *sadj = gtk_range_get_adjustment(GTK_RANGE(tab->h_scrollbar));
            scroll_x = gtk_adjustment_get_value(sadj);
        }
        double x = spacing - scroll_x;
        GtkAdjustment *vadj_row = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(tab->scrolled));
        double viewport_h = vadj_row ? gtk_adjustment_get_page_size(vadj_row) : 0.0;
        for (int i = 0; i < tab->n_pages; i++) {
            double pw = tab->cached_page_widths[i] * scale;
            double ph = tab->cached_page_heights[i] * scale;
            double oy = tab->max_page_h > viewport_h ? 0.0 : (alloc.height - ph) / 2.0;
            if (wx >= x && wx < x + pw && wy >= oy && wy < oy + ph) {
                if (out_px) *out_px = (wx - x) / scale;
                if (out_py) *out_py = (wy - oy) / scale;
                return i;
            }
            x += pw + spacing;
        }
    }
    return -1;
}


gboolean on_drawing_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data) {
    TabData *tab = user_data;
    if (!tab || event->button != GDK_BUTTON_PRIMARY) return FALSE;
    tab->dragging = TRUE;
    tab->drag_start_x = event->x;
    tab->drag_start_y = event->y;
    GtkAdjustment *hadj_main = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(tab->scrolled));
    tab->drag_scroll_x = gtk_adjustment_get_value(hadj_main);
    if (tab->layout_mode == 2 && tab->h_scrollbar) {
        GtkAdjustment *sadj = gtk_range_get_adjustment(GTK_RANGE(tab->h_scrollbar));
        tab->drag_scroll_x = gtk_adjustment_get_value(sadj);
    }
    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(tab->scrolled));
    tab->drag_scroll_y = gtk_adjustment_get_value(vadj);
    GdkCursor *cursor = gdk_cursor_new_for_display(gtk_widget_get_display(widget), GDK_FLEUR);
    gdk_window_set_cursor(gtk_widget_get_window(widget), cursor);
    g_object_unref(cursor);
    return TRUE;
}

gboolean on_drawing_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data) {
    TabData *tab = user_data;
    if (!tab || event->button != GDK_BUTTON_PRIMARY) return FALSE;
    gboolean was_dragging = tab->dragging;
    tab->dragging = FALSE;

    /* If the mouse barely moved, treat as a click — check for links */
    if (was_dragging) {
        double dx = event->x - tab->drag_start_x;
        double dy = event->y - tab->drag_start_y;
        if (dx * dx + dy * dy < 25.0) { /* 5px threshold */
            int page;
            double px, py;
            page = widget_to_page_coords(tab, event->x, event->y, &px, &py);
            if (page >= 0) {
                ensure_page_links_loaded(tab, page);
                activate_link_at(tab, page, px, py);
            }
        }
    }

    if (tab->last_cursor_type != GDK_LEFT_PTR) {
        GdkCursor *cursor = gdk_cursor_new_for_display(gtk_widget_get_display(widget), GDK_LEFT_PTR);
        gdk_window_set_cursor(gtk_widget_get_window(widget), cursor);
        g_object_unref(cursor);
        tab->last_cursor_type = GDK_LEFT_PTR;
    }
    return TRUE;
}

gboolean on_drawing_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data) {
    TabData *tab = user_data;
    if (!tab) return FALSE;

    if (tab->dragging) {
        double dy = tab->drag_start_y - event->y;
        double dx = tab->drag_start_x - event->x;
        GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(tab->scrolled));
        gtk_adjustment_set_value(vadj, tab->drag_scroll_y + dy);
        if (tab->layout_mode == 2 && tab->h_scrollbar) {
            GtkAdjustment *sadj = gtk_range_get_adjustment(GTK_RANGE(tab->h_scrollbar));
            gtk_adjustment_set_value(sadj, tab->drag_scroll_x + dx);
        } else {
            GtkAdjustment *hadj = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(tab->scrolled));
            gtk_adjustment_set_value(hadj, tab->drag_scroll_x + dx);
        }
        return TRUE;
    }

    /* Throttle link cursor check to ~10 Hz to avoid O(n) loop on every motion */
    GdkCursorType cursor_type = GDK_LEFT_PTR;
    gint64 now = g_get_monotonic_time();
    if (now - tab->last_cursor_check > 100000) { /* 100ms */
        tab->last_cursor_check = now;
        int page;
        double px, py;
        page = widget_to_page_coords(tab, event->x, event->y, &px, &py);
        if (page >= 0) {
            ensure_page_links_loaded(tab, page);
            if (has_link_at(tab, page, px, py)) {
                cursor_type = GDK_HAND2;
            }
        }
    } else {
        /* Use current cursor type — it won't change between throttled checks */
        cursor_type = tab->last_cursor_type;
    }

    /* Only call into GDK/X11 when cursor type actually changes */
    if (cursor_type != tab->last_cursor_type) {
        GdkCursor *cursor = gdk_cursor_new_for_display(gtk_widget_get_display(widget), cursor_type);
        gdk_window_set_cursor(gtk_widget_get_window(widget), cursor);
        g_object_unref(cursor);
        tab->last_cursor_type = cursor_type;
    }

    if (tab->layout_mode != 2 || !tab->h_scrollbar) return FALSE;
    GtkAllocation alloc;
    gtk_widget_get_allocation(widget, &alloc);
    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(tab->scrolled));
    double scroll_y = gtk_adjustment_get_value(vadj);
    double page_size = gtk_adjustment_get_page_size(vadj);
    double visible_bottom = scroll_y + page_size;
    if (visible_bottom > alloc.height) visible_bottom = alloc.height;
    int bottom_zone = 30;
    if (event->y >= visible_bottom - bottom_zone) {
        show_h_scrollbar(tab);
    } else if (gtk_widget_get_visible(tab->h_scrollbar) && !tab->h_scrollbar_timer_id) {
        tab->h_scrollbar_timer_id = g_timeout_add(500, auto_hide_h_scrollbar, tab);
    }
    return FALSE;
}

gboolean on_drawing_leave(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data) {
    (void)widget; (void)event;
    TabData *tab = user_data;
    if (!tab || tab->layout_mode != 2 || !tab->h_scrollbar) return FALSE;
    if (gtk_widget_get_visible(tab->h_scrollbar) && !tab->h_scrollbar_timer_id) {
        tab->h_scrollbar_timer_id = g_timeout_add(500, auto_hide_h_scrollbar, tab);
    }
    return FALSE;
}

gboolean on_drawing_scroll(GtkWidget *widget, GdkEventScroll *event, gpointer user_data) {
    (void)widget;
    TabData *tab = user_data;
    if (!tab || tab->layout_mode != 2 || !tab->h_scrollbar) return FALSE;
    show_h_scrollbar_temporarily(tab);
    GtkAdjustment *adj = gtk_range_get_adjustment(GTK_RANGE(tab->h_scrollbar));
    double val = gtk_adjustment_get_value(adj);
    double step = gtk_adjustment_get_step_increment(adj);
    if (step < 1.0) step = 40.0;
    if (event->direction == GDK_SCROLL_SMOOTH) {
        double dx, dy;
        gdk_event_get_scroll_deltas((GdkEvent*)event, &dx, &dy);
        double delta = fabs(dx) >= fabs(dy) ? dx : dy;
        val += delta * step;
    } else if (event->direction == GDK_SCROLL_RIGHT || event->direction == GDK_SCROLL_DOWN) {
        val += step;
    } else if (event->direction == GDK_SCROLL_LEFT || event->direction == GDK_SCROLL_UP) {
        val -= step;
    }
    gtk_adjustment_set_value(adj, val);
    return TRUE;
}
