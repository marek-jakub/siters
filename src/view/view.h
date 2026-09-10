#ifndef SITERS_VIEW_H
#define SITERS_VIEW_H

#include <gtk/gtk.h>
#include "tab.h"

/* Upper bound for widget size requests in pixels (int-safe). */
#define MAX_SIZE_REQUEST 100000000

/* Clamp a double to [0, max] before converting to int. Shared by the view
   rendering and size-request math (and exercised by the unit tests). */
int clamp_double_to_int(double v, int max);

/* Map widget coordinates (device pixels in the drawing area) onto a page:
   returns the 0-based page index and page-relative rendering-space point
   (px, py), or -1 when no page covers (wx, wy). Shared with the interaction
   handlers and exercised by the unit tests. */
int widget_to_page_coords(TabData *tab, double wx, double wy,
                          double *out_px, double *out_py);

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

/* Interaction handlers for the page-drawing area, wired up by create_new_tab.
   They map widget coordinates onto pages (via links.c) and drive the
   hand-cursor and row-mode horizontal-scrollbar behaviour. */
gboolean on_drawing_scroll(GtkWidget *widget, GdkEventScroll *event, gpointer user_data);
gboolean on_drawing_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
gboolean on_drawing_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
gboolean on_drawing_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data);
gboolean on_drawing_leave(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data);

#endif /* SITERS_VIEW_H */
