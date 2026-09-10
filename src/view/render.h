#ifndef SITERS_RENDER_H
#define SITERS_RENDER_H

#include <gtk/gtk.h>
#include "tab.h"

/* To limit RAM use as large zoom takes many MB of resources */
#define MAX_SURFACE_DIM 2000
#define MAX_CACHE_BYTES (40 * 1024 * 1024)

/* Draw callback for the page-drawing area; wired to the "draw" signal by
   create_new_tab. Renders the visible pages of the current layout mode. */
gboolean on_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data);

#endif /* SITERS_RENDER_H */