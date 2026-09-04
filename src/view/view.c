#include <gtk/gtk.h>
#include <cairo.h>
#include "tab.h"
#include "pdf.h"
#include "mem_debug.h"
#include "view.h"

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
