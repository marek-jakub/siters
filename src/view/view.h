#ifndef SITERS_VIEW_H
#define SITERS_VIEW_H

#include <gtk/gtk.h>
#include "tab.h"

/* Upper bound for widget size requests in pixels (int-safe). */
#define MAX_SIZE_REQUEST 100000000

/* Clamp a double to [0, max] before converting to int. Shared by the view
   rendering and size-request math (and exercised by the unit tests). */
int clamp_double_to_int(double v, int max);

/* Page-view geometry: PPI scale, page offsets and heights for the current
   layout mode, plus the rendered-page pixel-buffer cache used by on_draw. */
double get_ppi_scale(TabData *tab);
double calculate_page_top_offset_ppi(TabData *tab, int page_idx);
double get_page_height_ppi(TabData *tab, int page_idx);
void   cache_evict_idx(TabData *tab, int idx);
void   cache_page_dimensions(TabData *tab);
void   invalidate_page_cache(TabData *tab);

/* Scrolling / redraw entry points (also wired up by the view rendering). */
void   queue_draw(TabData *tab);
void   scroll_to_page(TabData *tab, int page, double target_y);

/* Recompute the continuous-view size request for the current layout mode. */
void   build_continuous_view(TabData *tab);

#endif /* SITERS_VIEW_H */
