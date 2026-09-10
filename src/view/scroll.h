#ifndef SITERS_SCROLL_H
#define SITERS_SCROLL_H

#include <gtk/gtk.h>
#include "tab.h"

/* Horizontal-scrollbar display control for row layout mode. Shared by the
   drawing interaction handlers and the scrollbar's own enter/leave hooks.
   The scrollbar becomes visible while the pointer hovers over its activation
   zone and auto-hides after a timeout. */
void     show_h_scrollbar(TabData *tab);
void     show_h_scrollbar_temporarily(TabData *tab);
gboolean auto_hide_h_scrollbar(gpointer data);

#endif /* SITERS_SCROLL_H */