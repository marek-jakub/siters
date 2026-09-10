#include <gtk/gtk.h>
#include <math.h>
#include "tab.h"
#include "app.h"
#include "view.h"
#include "document_state.h"
#include "session/state.h"
#include "nav/nav.h"
#include "ui/tab_lifecycle.h"
#include "scroll.h"

static int compute_page_from_scroll(TabData *tab, double scroll_y) {
    if (!tab || !tab->cached_page_widths || tab->n_pages <= 0) return 0;

    const double spacing = 6.0;
    double scale = get_ppi_scale(tab);
    double y = spacing;
    int visible_page = tab->n_pages - 1;

    /* Heuristic: O(1) check if still on last known page */
    if (tab->last_known_page >= 0 && tab->last_known_page < tab->n_pages) {
        double page_len;
        if (tab->layout_mode == 2) {
            page_len = tab->cached_page_widths[tab->last_known_page] * scale;
        } else {
            page_len = get_page_height_ppi(tab, tab->last_known_page);
        }
        if (page_len > 0 && scroll_y >= tab->last_known_page_start && scroll_y < tab->last_known_page_start + page_len + spacing)
            return tab->last_known_page;
    }

    if (tab->layout_mode == 2) {
        for (int i = 0; i < tab->n_pages; ++i) {
            double page_w = tab->cached_page_widths[i] * scale;
            if (scroll_y >= y && scroll_y < y + page_w) {
                tab->last_known_page = i;
                tab->last_known_page_start = y;
                return i;
            }
            y += page_w + spacing;
        }
    } else if (tab->layout_mode == 1) {
        for (int i = 0; i < tab->n_pages; i += 2) {
            double row_h = 0.0;
            for (int p = 0; p < 2; p++) {
                int idx = i + p;
                if (idx >= tab->n_pages) break;
                double ph = get_page_height_ppi(tab, idx);
                if (ph > row_h) row_h = ph;
            }
            if (row_h < 1.0) row_h = 1.0;
            if (y + row_h > scroll_y) {
                visible_page = i;
                tab->last_known_page = visible_page;
                tab->last_known_page_start = y;
                break;
            }
            y += row_h + spacing;
        }
    } else {
        for (int i = 0; i < tab->n_pages; ++i) {
            double page_h = get_page_height_ppi(tab, i);
            if (page_h <= 0) continue;
            if (scroll_y >= y && scroll_y < y + page_h) {
                tab->last_known_page = i;
                tab->last_known_page_start = y;
                return i;
            }
            y += page_h + spacing;
        }
    }

    /* A scroll position above the top of the first page (its leading spacing)
       still belongs to the first page; only positions at/below the end of the
       last page map to the last page. */
    if (scroll_y < spacing) {
        tab->last_known_page = 0;
        tab->last_known_page_start = spacing;
        return 0;
    }

    tab->last_known_page = visible_page;
    tab->last_known_page_start = y;
    return visible_page;
}

/* Multi-stage restore with layout settle - the robust approach */
static gboolean do_initial_scroll_stage(gpointer user_data) {
    RestoreState *restore = (RestoreState *)user_data;

    if (!restore || !restore->tab) {
        g_free(restore);
        return FALSE;
    }

    // Additional check: verify this restore is still the active one for the tab
    if (restore->tab->pending_restore != restore) {
        // This restore has been superseded or cancelled
        g_free(restore);
        return FALSE;
    }

    if (!restore->tab->doc) {
        restore->tab->pending_restore = NULL;
        g_free(restore);
        return FALSE;
    }

    TabData *tab = restore->tab;

    /* ========== STAGE 0: Initialize & Apply Zoom ========== */
    if (restore->restore_stage == 0) {
        /* Validate basic state */
        if (!gtk_widget_get_realized(tab->scrolled)) {
            /* Widget not ready, retry next idle */
            restore->settle_attempts++;
            if (restore->settle_attempts < 20) {
                restore->source_id = g_idle_add(do_initial_scroll_stage, restore);
                return FALSE;
            }
        } else {
            GtkAllocation alloc;
            gtk_widget_get_allocation(tab->scrolled, &alloc);
            if (alloc.width < 1 || alloc.height < 1) {
                restore->settle_attempts++;
                if (restore->settle_attempts < 20) {
                    /* Layout not finalized, retry next idle */
                    restore->source_id = g_idle_add(do_initial_scroll_stage, restore);
                    return FALSE;
                }
            }
        }

        /* Clamp page to valid range */
        if (restore->target_page < 0) restore->target_page = 0;
        if (restore->target_page >= tab->n_pages) restore->target_page = tab->n_pages - 1;

        /* Clamp zoom (PPI: 10-500) */
        if (restore->target_zoom < 10.0) restore->target_zoom = 10.0;
        if (restore->target_zoom > 500.0) restore->target_zoom = 500.0;

        /* Clamp fraction */
        if (restore->target_fraction < 0.0) restore->target_fraction = 0.0;
        if (restore->target_fraction > 1.0) restore->target_fraction = 1.0;

        /* Apply zoom */
        tab->zoom = restore->target_zoom;

        /* Rebuild layout with new zoom */
        build_continuous_view(tab);

        /* Move to next stage */
        restore->restore_stage = 1;
        restore->settle_attempts = 0;
        restore->source_id = g_idle_add(do_initial_scroll_stage, restore);
        return FALSE;
    }

    /* ========== STAGE 1: Wait for Layout to Settle ========== */
    if (restore->restore_stage == 1) {
        /* Check if layout has settled by seeing if page heights are stable */
        double page_h = get_page_height_ppi(tab, restore->target_page);

        if (page_h <= 0) {
            /* Page height not ready yet */
            restore->settle_attempts++;
            if (restore->settle_attempts < 20) {  /* Try up to 20 times (~100ms) */
                restore->source_id = g_idle_add(do_initial_scroll_stage, restore);
                return FALSE;
            }
            /* Timeout, proceed anyway */
        }

        /* Move to measurement stage */
        restore->restore_stage = 2;
        restore->source_id = g_idle_add(do_initial_scroll_stage, restore);
        return FALSE;
    }

    /* ========== STAGE 2: Calculate Exact Scroll Position ========== */
    if (restore->restore_stage == 2) {
        double scale = get_ppi_scale(tab);
        if (tab->layout_mode == 2) {
            const double spacing = 6.0;
            double x_offset = spacing;
            double page_w = 0;
            for (int i = 0; i < restore->target_page; ++i) {
                x_offset += tab->cached_page_widths[i] * scale + spacing;
            }
            page_w = tab->cached_page_widths[restore->target_page] * scale;
            if (page_w <= 0) page_w = 1.0;
            double target_scroll = x_offset + (restore->target_fraction * page_w);
            if (target_scroll < 0) target_scroll = 0;
            if (tab->h_scrollbar) {
                GtkAdjustment *sadj = gtk_range_get_adjustment(GTK_RANGE(tab->h_scrollbar));
                double upper = gtk_adjustment_get_upper(sadj);
                double page_size = gtk_adjustment_get_page_size(sadj);
                if (target_scroll > upper - page_size) target_scroll = upper - page_size;
                if (target_scroll < 0) target_scroll = 0;
                gtk_adjustment_set_value(sadj, target_scroll);
            }
        } else {
            double page_top = calculate_page_top_offset_ppi(tab, restore->target_page);
            double page_h = get_page_height_ppi(tab, restore->target_page);

            if (page_h <= 0) {
                /* Fallback: just scroll to page top */
                page_h = 1.0;
            }

            /* Calculate scroll position: page top + (fraction * page height) */
            double target_scroll = page_top + (restore->target_fraction * page_h);

            /* Clamp to valid range */
            GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(tab->scrolled));
            double upper = gtk_adjustment_get_upper(vadj);
            double page_size = gtk_adjustment_get_page_size(vadj);

            if (target_scroll < 0) target_scroll = 0;
            if (target_scroll > upper - page_size) target_scroll = upper - page_size;
            if (target_scroll < 0) target_scroll = 0;  /* In case page_size > upper */

            /* Set the scroll position */
            gtk_adjustment_set_value(vadj, target_scroll);
        }

        /* Move to verification stage */
        restore->restore_stage = 3;
        restore->source_id = g_idle_add(do_initial_scroll_stage, restore);
        return FALSE;
    }

    /* ========== STAGE 3: Verify & Finalize ========== */
    if (restore->restore_stage == 3) {
        double actual_scroll;
        if (tab->layout_mode == 2) {
            if (tab->h_scrollbar) {
                GtkAdjustment *sadj = gtk_range_get_adjustment(GTK_RANGE(tab->h_scrollbar));
                actual_scroll = gtk_adjustment_get_value(sadj);
            } else {
                actual_scroll = 0.0;
            }
        } else {
            GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(tab->scrolled));
            actual_scroll = gtk_adjustment_get_value(vadj);
        }

        /* Update cur_page based on actual scroll position */
        tab->cur_page = compute_page_from_scroll(tab, actual_scroll);

        /* Update UI */
        if (tab == get_current_left_tab()) {
            sync_page_widget_from_tab(tab);
        }
        if (tab == get_current_right_tab()) {
            sync_right_page_widget_from_tab(tab);
        }

        /* A freshly opened document stops being "new" once it has actually
           been shown, i.e. its restore completes while it is the current tab.
           Hidden tabs keep fresh_open so the first time the user opens them
           they still get page 1 rather than any leftover saved state. */
        if (tab == get_current_left_tab() || tab == get_current_right_tab()) {
            tab->fresh_open = FALSE;
        }

        /* Done - return FALSE to remove this idle callback */
        restore->tab->pending_restore = NULL;
        g_free(restore);

        return FALSE;
    }

    /* ========== STAGE 4: Finalize ========== */
    if (restore->restore_stage == 4) {
        /* Update UI */
        if (tab == get_current_left_tab()) {
            sync_page_widget_from_tab(tab);
        }
        if (tab == get_current_right_tab()) {
            sync_right_page_widget_from_tab(tab);
        }

        if (tab == get_current_left_tab() || tab == get_current_right_tab()) {
            tab->fresh_open = FALSE;
        }

        restore->tab->pending_restore = NULL;
        g_free(restore);
        return FALSE;
    }

    if (restore->tab) {
        if (restore->tab == get_current_left_tab() || restore->tab == get_current_right_tab()) {
            restore->tab->fresh_open = FALSE;
        }
        restore->tab->pending_restore = NULL;
    }
    g_free(restore);
    return FALSE;
}

/* Called to initiate the robust restore process */
void start_initial_scroll_restore(TabData *tab, int target_page, double target_zoom,
                                         double target_fraction) {
    if (!tab || target_page < 0 || target_zoom < 0.1) return;

    cancel_tab_restore(tab);

    RestoreState *restore = g_malloc(sizeof(RestoreState));
    restore->tab = tab;
    restore->restore_stage = 0;
    restore->settle_attempts = 0;
    restore->target_page = target_page;
    restore->target_zoom = target_zoom;
    restore->target_fraction = target_fraction;
    restore->source_id = 0;

    tab->initial_scroll_pending = TRUE;
    tab->pending_restore = restore;

    /* Start the multi-stage restore process */
    restore->source_id = g_idle_add(do_initial_scroll_stage, restore);
}

void on_scroll_value_changed(GtkAdjustment *adj, gpointer user_data) {
    TabData *tab = user_data;
    if (!tab || !tab->cached_page_widths) return;

    /* Ignore programmatic scroll adjustments while a document is still loading
       or its post-load restore is in flight: those movements are not user
       scrolling and must not update the current page or the saved doc model. */
    if (tab->initial_scroll_pending || tab->pending_restore) {
        return;
    }

    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(tab->scrolled));

    if (tab->layout_mode == 2) {
        if (adj == vadj) return;
        GtkAdjustment *sadj = tab->h_scrollbar ? gtk_range_get_adjustment(GTK_RANGE(tab->h_scrollbar)) : NULL;
        if (adj != sadj) return;
        double scroll_x = gtk_adjustment_get_value(adj);
        const double spacing = 6.0;
        double scale = get_ppi_scale(tab);

        double upper = gtk_adjustment_get_upper(adj);
        double page_size = gtk_adjustment_get_page_size(adj);
        if (page_size > 0 && tab->n_pages > 0
            && (upper - page_size) > 1.0
            && scroll_x >= (upper - page_size - 1.0)) {
            tab->cur_page = tab->n_pages - 1;
            if (tab == get_current_left_tab()) {
                sync_page_widget_from_tab(tab);
            }
            if (tab == get_current_right_tab()) {
                sync_right_page_widget_from_tab(tab);
            }
            schedule_doc_model_update(tab);
            gtk_widget_queue_draw(tab->pages_drawing);
            return;
        }

        double x = spacing;
        int visible_page = (tab->n_pages > 0) ? (tab->n_pages - 1) : 0;
        for (int i = 0; i < tab->n_pages; ++i) {
            double page_w = tab->cached_page_widths[i] * scale;
            if (x + page_w > scroll_x) {
                visible_page = i;
                break;
            }
            x += page_w + spacing;
        }

        tab->cur_page = visible_page;
        if (tab == get_current_left_tab()) {
            sync_page_widget_from_tab(tab);
        }
        if (tab == get_current_right_tab()) {
            sync_right_page_widget_from_tab(tab);
        }
        schedule_doc_model_update(tab);
        gtk_widget_queue_draw(tab->pages_drawing);
        return;
    }

    if (adj != vadj) return;

    double scroll_y = gtk_adjustment_get_value(adj);
    const double spacing = 6.0;
    double scale = get_ppi_scale(tab);

    double upper = gtk_adjustment_get_upper(adj);
    double page_size = gtk_adjustment_get_page_size(adj);
    if (tab->n_pages > 0
        && (upper - page_size) > 1.0
        && scroll_y >= (upper - page_size - 1.0)) {
        tab->cur_page = tab->n_pages - 1;
        if (tab == get_current_left_tab()) {
            sync_page_widget_from_tab(tab);
        }
        if (tab == get_current_right_tab()) {
            sync_right_page_widget_from_tab(tab);
        }
        schedule_doc_model_update(tab);
        return;
    }

    double y = spacing;
    int visible_page = (tab->n_pages > 0) ? (tab->n_pages - 1) : 0;

    if (tab->layout_mode == 0) {
        for (int i = 0; i < tab->n_pages; ++i) {
            double page_h = tab->cached_page_heights[i] * scale;
            if (y + page_h > scroll_y) {
                visible_page = i;
                break;
            }
            y += page_h + spacing;
        }
    } else if (tab->layout_mode == 1) {
        for (int i = 0; i < tab->n_pages; i += 2) {
            double row_h = 0.0;
            double h1 = tab->cached_page_heights[i] * scale;
            if (h1 > row_h) row_h = h1;
            if (i + 1 < tab->n_pages) {
                double h2 = tab->cached_page_heights[i + 1] * scale;
                if (h2 > row_h) row_h = h2;
            }
            if (row_h < 1.0) row_h = 1.0;

            if (y + row_h > scroll_y) {
                visible_page = i;
                break;
            }
            y += row_h + spacing;
        }
    }

    tab->cur_page = visible_page;

    if (tab == get_current_left_tab()) {
        sync_page_widget_from_tab(tab);
    }
    if (tab == get_current_right_tab()) {
        sync_right_page_widget_from_tab(tab);
    }

    schedule_doc_model_update(tab);
}

void on_tab_scrolled_size_allocate(GtkWidget *widget, GdkRectangle *allocation, gpointer user_data) {
    (void)widget;
    (void)allocation;
    TabData *tab = user_data;
    if (!tab) return;

    /* Ensure doc is loaded for the active tab that owns this size-allocate */
    if (!tab->cached_page_widths) {
        if (!app.is_restoring_session_tabs && !ensure_tab_doc_loaded(tab)) return;
    }

    double zoom = tab->zoom > 0 ? tab->zoom : 96.0;
    if (zoom != tab->last_zoom) {
        tab->last_zoom = zoom;
        build_continuous_view(tab);
    }

    /* For row view (mode 2), the size_request width is clamped to page_width_px
       to prevent the window from growing.  GTK's internal handler set the
       hadjustment upper from the clamped size_request, so we must extend it
       here to the full total_w BEFORE scroll_to_page runs below.  Recompute
       total_w and max_h from the page cache so the scroll range and drawing
       height are correct even if the last build used a different layout. */
    if (tab->layout_mode == 2 && tab->cached_page_widths && tab->n_pages > 0 && tab->h_scrollbar) {
        int vp_w = allocation ? allocation->width : 200;
        int vp_h = allocation ? allocation->height : 200;
        double scale = get_ppi_scale(tab);
        double total_w = 0.0;
        double max_h = 0.0;
        const double spacing = 6.0;
        for (int i = 0; i < tab->n_pages; ++i) {
            double page_w = tab->cached_page_widths[i] * scale;
            double page_h = tab->cached_page_heights[i] * scale;
            total_w += page_w + spacing;
            if (page_h > max_h) max_h = page_h;
        }
        if (total_w < 1.0) total_w = 1.0;
        if (max_h < 1.0) max_h = 1.0;
        tab->max_page_h = max_h;
        GtkAdjustment *sadj = gtk_range_get_adjustment(GTK_RANGE(tab->h_scrollbar));
        gtk_adjustment_set_page_size(sadj, vp_w > 0 ? vp_w : 200);
        gtk_adjustment_set_step_increment(sadj, vp_w > 0 ? vp_w * 0.1 : 20);
        gtk_adjustment_set_page_increment(sadj, vp_w * 0.9);
        gtk_adjustment_set_lower(sadj, 0.0);
        gtk_adjustment_set_upper(sadj, total_w);
        double target_h = MAX(vp_h, tab->max_page_h);
        gtk_widget_set_size_request(tab->pages_drawing, -1, clamp_double_to_int(ceil(target_h), MAX_SIZE_REQUEST));
    }

    if (tab->initial_scroll_pending) {
        scroll_to_page(tab, tab->cur_page, -1);
        tab->initial_scroll_pending = FALSE;
    }
}

gboolean auto_hide_h_scrollbar(gpointer data) {
    TabData *tab = data;
    tab->h_scrollbar_timer_id = 0;
    if (tab->h_scrollbar)
        gtk_widget_hide(tab->h_scrollbar);
    return G_SOURCE_REMOVE;
}

static void cancel_h_scrollbar_timer(TabData *tab) {
    if (tab->h_scrollbar_timer_id) {
        g_source_remove(tab->h_scrollbar_timer_id);
        tab->h_scrollbar_timer_id = 0;
    }
}

void show_h_scrollbar_temporarily(TabData *tab) {
    if (!tab->h_scrollbar) return;
    cancel_h_scrollbar_timer(tab);
    gtk_widget_show(tab->h_scrollbar);
    tab->h_scrollbar_timer_id = g_timeout_add(2000, auto_hide_h_scrollbar, tab);
}

void show_h_scrollbar(TabData *tab) {
    if (!tab->h_scrollbar) return;
    cancel_h_scrollbar_timer(tab);
    gtk_widget_show(tab->h_scrollbar);
}

gboolean on_h_scrollbar_enter(GtkWidget *w, GdkEvent *e, gpointer user_data) {
    (void)w; (void)e;
    TabData *tab = user_data;
    if (!tab->h_scrollbar) return FALSE;
    cancel_h_scrollbar_timer(tab);
    return FALSE;
}

gboolean on_h_scrollbar_leave(GtkWidget *w, GdkEvent *e, gpointer user_data) {
    (void)w; (void)e;
    TabData *tab = user_data;
    if (!tab->h_scrollbar) return FALSE;
    cancel_h_scrollbar_timer(tab);
    tab->h_scrollbar_timer_id = g_timeout_add(500, auto_hide_h_scrollbar, tab);
    return FALSE;
}
