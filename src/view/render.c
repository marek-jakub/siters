#include <gtk/gtk.h>
#include <cairo.h>
#include <math.h>
#include "tab.h"
#include "pdf.h"
#include "mem_debug.h"
#include "view.h"
#include "search.h"
#include "render.h"

gboolean on_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data) {
    TabData *tab = user_data;
    GtkAllocation alloc;
    gtk_widget_get_allocation(widget, &alloc);

    if (!tab || !tab->cached_page_widths) {
        return FALSE;
    }

    MEM_INIT_DRAW();

    /* continuous mode: draw multiple pages vertically inside this drawing area
       Render only pages intersecting the current clip extents to save work. */
    double clip_x1, clip_y1, clip_x2, clip_y2;
    cairo_clip_extents(cr, &clip_x1, &clip_y1, &clip_x2, &clip_y2);

    const double spacing = 6.0;
    double scale = get_ppi_scale(tab);
    int first_visible = -1, last_visible = -1;
    if (tab->layout_mode == 0) {
        double y = spacing;
        double dsx, dsy;
        cairo_surface_get_device_scale(cairo_get_target(cr), &dsx, &dsy);
        cairo_font_options_t *fo = cairo_font_options_create();
        cairo_get_font_options(cr, fo);
        for (int i = 0; i < tab->n_pages; ++i) {
            double page_w = tab->cached_page_widths[i] * scale;
            double page_h = tab->cached_page_heights[i] * scale;
            double off_x = (alloc.width - page_w) / 2.0;
            double off_y = y;

            /* skip if page is outside clip */
            if (!(off_y + page_h < clip_y1 || off_y > clip_y2)) {
                /* draw background rectangle */
                cairo_save(cr);
                cairo_set_source_rgba(cr, tab->page_color.red, tab->page_color.green, tab->page_color.blue, tab->page_color.alpha);
                cairo_rectangle(cr, off_x, off_y, page_w, page_h);
                cairo_fill(cr);
                cairo_restore(cr);
                int iw = clamp_double_to_int(tab->cached_page_widths[i] * scale * dsx + 0.5, MAX_SURFACE_DIM);
                int ih = clamp_double_to_int(tab->cached_page_heights[i] * scale * dsy + 0.5, MAX_SURFACE_DIM);
                if (iw > 0 && ih > 0) {
                    if (tab->page_cache[i]) {
                        int cw = cairo_image_surface_get_width(tab->page_cache[i]);
                        int ch = cairo_image_surface_get_height(tab->page_cache[i]);
                        if (cw != iw || ch != ih)
                            cache_evict_idx(tab, i);
                    }
                    if (tab->page_cache[i]) {
                        cairo_set_source_surface(cr, tab->page_cache[i], off_x, off_y);
                        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
                        cairo_paint(cr);
                    } else {
                        PdfrPage *page = pdfr_load_page(tab->doc, i);
                        if (page) {
                            cairo_surface_t *pimg = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, iw, ih);
                            cairo_surface_set_device_scale(pimg, dsx, dsy);
                            cairo_t *picr = cairo_create(pimg);
                            cairo_set_font_options(picr, fo);
                            cairo_set_antialias(picr, CAIRO_ANTIALIAS_BEST);
                            cairo_scale(picr, scale, scale);
                            pdfr_render(tab->doc, page, picr);
                            cairo_destroy(picr);
                            pdfr_free_page(tab->doc, page);
                            cairo_set_source_surface(cr, pimg, off_x, off_y);
                            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
                            cairo_paint(cr);
                            /* Cache only if within byte budget */
                            int new_bytes = iw * ih * 4;
                            MEM_SURFACE_CREATED(new_bytes);
                            if (tab->total_cache_bytes + new_bytes <= MAX_CACHE_BYTES) {
                                tab->page_cache[i] = pimg;
                                tab->total_cache_bytes += new_bytes;
                            } else {
                                cairo_surface_destroy(pimg);
                                MEM_SURFACE_DESTROYED();
                            }
                        }
                    }
                    if (first_visible == -1) first_visible = i;
                    last_visible = i;
                }
                search_highlight_page(tab, cr, i + 1, off_x, off_y, scale);
            }

            y += page_h + spacing;
        }
        cairo_font_options_destroy(fo);

    } else if (tab->layout_mode == 1) {
        int n = tab->n_pages;
        double y = spacing;
        double dsx2, dsy2;
        cairo_surface_get_device_scale(cairo_get_target(cr), &dsx2, &dsy2);
        cairo_font_options_t *fo2 = cairo_font_options_create();
        cairo_get_font_options(cr, fo2);
        for (int i = 0; i < n; i += 2) {
            /* left page dims */
            double page_w1 = tab->cached_page_widths[i] * scale;
            double page_h1 = tab->cached_page_heights[i] * scale;
            double row_w = page_w1;
            double row_h = page_h1;

            /* right page dims */
            double page_w2 = 0, page_h2 = 0;
            if (i + 1 < n) {
                page_w2 = tab->cached_page_widths[i + 1] * scale;
                page_h2 = tab->cached_page_heights[i + 1] * scale;
            }
            if (page_w2 > 0) row_w += spacing + page_w2;
            if (page_h2 > row_h) row_h = page_h2;
            if (row_h < 1.0) row_h = 1.0;

            /* center the row horizontally within the drawing area */
            double row_x = (alloc.width - row_w) / 2.0;
            if (row_x < spacing) row_x = spacing;

            double left_x = row_x;
            double right_x = left_x + page_w1 + spacing;

            /* draw left page if visible */
            if (page_h1 > 0 && !(y + page_h1 < clip_y1 || y > clip_y2)) {
                cairo_save(cr);
                cairo_set_source_rgba(cr, tab->page_color.red, tab->page_color.green, tab->page_color.blue, tab->page_color.alpha);
                cairo_rectangle(cr, left_x, y, page_w1, page_h1);
                cairo_fill(cr);
                cairo_restore(cr);
                int iw1 = clamp_double_to_int(tab->cached_page_widths[i] * scale * dsx2 + 0.5, MAX_SURFACE_DIM);
                int ih1 = clamp_double_to_int(tab->cached_page_heights[i] * scale * dsy2 + 0.5, MAX_SURFACE_DIM);
                if (iw1 > 0 && ih1 > 0) {
                    if (tab->page_cache[i]) {
                        int cw = cairo_image_surface_get_width(tab->page_cache[i]);
                        int ch = cairo_image_surface_get_height(tab->page_cache[i]);
                        if (cw != iw1 || ch != ih1)
                            cache_evict_idx(tab, i);
                    }
                    if (tab->page_cache[i]) {
                        cairo_set_source_surface(cr, tab->page_cache[i], left_x, y);
                        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
                        cairo_paint(cr);
                    } else {
                        PdfrPage *p1 = pdfr_load_page(tab->doc, i);
                        if (p1) {
                            cairo_surface_t *pimg = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, iw1, ih1);
                            cairo_surface_set_device_scale(pimg, dsx2, dsy2);
                            cairo_t *picr = cairo_create(pimg);
                            cairo_set_font_options(picr, fo2);
                            cairo_set_antialias(picr, CAIRO_ANTIALIAS_BEST);
                            cairo_scale(picr, scale, scale);
                            pdfr_render(tab->doc, p1, picr);
                            cairo_destroy(picr);
                            pdfr_free_page(tab->doc, p1);
                            cairo_set_source_surface(cr, pimg, left_x, y);
                            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
                            cairo_paint(cr);
                            int new_bytes = iw1 * ih1 * 4;
                            MEM_SURFACE_CREATED(new_bytes);
                            if (tab->total_cache_bytes + new_bytes <= MAX_CACHE_BYTES) {
                                tab->page_cache[i] = pimg;
                                tab->total_cache_bytes += new_bytes;
                            } else {
                                cairo_surface_destroy(pimg);
                                MEM_SURFACE_DESTROYED();
                            }
                        }
                    }
                    if (first_visible == -1) first_visible = i;
                    last_visible = i;
                }
                search_highlight_page(tab, cr, i + 1, left_x, y, scale);
            }

            /* draw right page if visible */
            if (page_h2 > 0 && !(y + page_h2 < clip_y1 || y > clip_y2)) {
                cairo_save(cr);
                cairo_set_source_rgba(cr, tab->page_color.red, tab->page_color.green, tab->page_color.blue, tab->page_color.alpha);
                cairo_rectangle(cr, right_x, y, page_w2, page_h2);
                cairo_fill(cr);
                cairo_restore(cr);
                int iw2 = clamp_double_to_int(tab->cached_page_widths[i + 1] * scale * dsx2 + 0.5, MAX_SURFACE_DIM);
                int ih2 = clamp_double_to_int(tab->cached_page_heights[i + 1] * scale * dsy2 + 0.5, MAX_SURFACE_DIM);
                if (iw2 > 0 && ih2 > 0) {
                    if (tab->page_cache[i + 1]) {
                        int cw = cairo_image_surface_get_width(tab->page_cache[i + 1]);
                        int ch = cairo_image_surface_get_height(tab->page_cache[i + 1]);
                        if (cw != iw2 || ch != ih2)
                            cache_evict_idx(tab, i + 1);
                    }
                    if (tab->page_cache[i + 1]) {
                        cairo_set_source_surface(cr, tab->page_cache[i + 1], right_x, y);
                        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
                        cairo_paint(cr);
                    } else {
                        PdfrPage *p2 = pdfr_load_page(tab->doc, i + 1);
                        if (p2) {
                            cairo_surface_t *pimg = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, iw2, ih2);
                            cairo_surface_set_device_scale(pimg, dsx2, dsy2);
                            cairo_t *picr = cairo_create(pimg);
                            cairo_set_font_options(picr, fo2);
                            cairo_set_antialias(picr, CAIRO_ANTIALIAS_BEST);
                            cairo_scale(picr, scale, scale);
                            pdfr_render(tab->doc, p2, picr);
                            cairo_destroy(picr);
                            pdfr_free_page(tab->doc, p2);
                            cairo_set_source_surface(cr, pimg, right_x, y);
                            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
                            cairo_paint(cr);
                            int new_bytes = iw2 * ih2 * 4;
                            MEM_SURFACE_CREATED(new_bytes);
                            if (tab->total_cache_bytes + new_bytes <= MAX_CACHE_BYTES) {
                                tab->page_cache[i + 1] = pimg;
                                tab->total_cache_bytes += new_bytes;
                            } else {
                                cairo_surface_destroy(pimg);
                                MEM_SURFACE_DESTROYED();
                            }
                        }
                    }
                    if (first_visible == -1) first_visible = i;
                    last_visible = i;
                }
                search_highlight_page(tab, cr, i + 2, right_x, y, scale);
            }

            y += row_h + spacing;
        }
        cairo_font_options_destroy(fo2);
    } else if (tab->layout_mode == 2) {
        double scroll_x = 0.0;
        if (tab->h_scrollbar) {
            GtkAdjustment *sadj = gtk_range_get_adjustment(GTK_RANGE(tab->h_scrollbar));
            scroll_x = gtk_adjustment_get_value(sadj);
        }
        GtkAdjustment *vadj_row = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(tab->scrolled));
        double viewport_h = gtk_adjustment_get_page_size(vadj_row);
        double dsxh, dsyh;
        cairo_surface_get_device_scale(cairo_get_target(cr), &dsxh, &dsyh);
        cairo_font_options_t *foh = cairo_font_options_create();
        cairo_get_font_options(cr, foh);
        double x = spacing;
        for (int i = 0; i < tab->n_pages; ++i) {
            double page_w = tab->cached_page_widths[i] * scale;
            double page_h = tab->cached_page_heights[i] * scale;
            double dev_x = x - scroll_x;
            double off_y = tab->max_page_h > viewport_h ? 0.0 : (alloc.height - page_h) / 2.0;
            if (dev_x + page_w > 0 && dev_x < alloc.width &&
                off_y + page_h > 0 && off_y < alloc.height) {
                cairo_save(cr);
                cairo_set_source_rgba(cr, tab->page_color.red, tab->page_color.green, tab->page_color.blue, tab->page_color.alpha);
                cairo_rectangle(cr, dev_x, off_y, page_w, page_h);
                cairo_fill(cr);
                cairo_restore(cr);
                int iwh = clamp_double_to_int(tab->cached_page_widths[i] * scale * dsxh + 0.5, MAX_SURFACE_DIM);
                int ihh = clamp_double_to_int(tab->cached_page_heights[i] * scale * dsyh + 0.5, MAX_SURFACE_DIM);
                if (iwh > 0 && ihh > 0) {
                    if (tab->page_cache[i]) {
                        int cw = cairo_image_surface_get_width(tab->page_cache[i]);
                        int ch = cairo_image_surface_get_height(tab->page_cache[i]);
                        if (cw != iwh || ch != ihh)
                            cache_evict_idx(tab, i);
                    }
                    if (tab->page_cache[i]) {
                        cairo_set_source_surface(cr, tab->page_cache[i], dev_x, off_y);
                        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
                        cairo_paint(cr);
                    } else {
                        PdfrPage *page = pdfr_load_page(tab->doc, i);
                        if (page) {
                            cairo_surface_t *pimg = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, iwh, ihh);
                            cairo_surface_set_device_scale(pimg, dsxh, dsyh);
                            cairo_t *picr = cairo_create(pimg);
                            cairo_set_font_options(picr, foh);
                            cairo_set_antialias(picr, CAIRO_ANTIALIAS_BEST);
                            cairo_scale(picr, scale, scale);
                            pdfr_render(tab->doc, page, picr);
                            cairo_destroy(picr);
                            pdfr_free_page(tab->doc, page);
                            cairo_set_source_surface(cr, pimg, dev_x, off_y);
                            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
                            cairo_paint(cr);
                            int new_bytes = iwh * ihh * 4;
                            MEM_SURFACE_CREATED(new_bytes);
                            if (tab->total_cache_bytes + new_bytes <= MAX_CACHE_BYTES) {
                                tab->page_cache[i] = pimg;
                                tab->total_cache_bytes += new_bytes;
                            } else {
                                cairo_surface_destroy(pimg);
                                MEM_SURFACE_DESTROYED();
                            }
                        }
                    }
                    if (first_visible == -1) first_visible = i;
                    last_visible = i;
                }
                search_highlight_page(tab, cr, i + 1, dev_x, off_y, scale);
            }
            x += page_w + spacing;
        }
        cairo_font_options_destroy(foh);
    }

    /* Prune cache: keep only pages within margin of the visible range */
    if (first_visible >= 0 && last_visible >= 0) {
        int margin = 1;
        for (int i = 0; i < tab->n_pages; ++i) {
            if (tab->page_cache[i] && (i < first_visible - margin || i > last_visible + margin))
                cache_evict_idx(tab, i);
        }
    }

    MEM_REPORT_DRAW();
    return FALSE;
}

