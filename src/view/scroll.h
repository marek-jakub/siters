#ifndef SITERS_SCROLL_H
#define SITERS_SCROLL_H

#include <gtk/gtk.h>
#include "tab.h"

/* Multi-stage scroll-restore state machine. Drives initial page+zoom
   positioning after a document loads or a session restore completes.
   start_initial_scroll_restore kicks off the first idle callback;
   on_scroll_value_changed keeps the current-page index in sync as the
   user scrolls.  on_tab_scrolled_size_allocate handles row-view
   horizontal-scrollbar bookkeeping on allocation changes. */
void start_initial_scroll_restore(TabData *tab, int target_page,
                                  double target_zoom, double target_fraction);
void on_scroll_value_changed(GtkAdjustment *adj, gpointer user_data);
void on_tab_scrolled_size_allocate(GtkWidget *widget,
                                   GdkRectangle *allocation,
                                   gpointer user_data);

/* Horizontal scrollbar show / hide for row layout mode. */
void     show_h_scrollbar(TabData *tab);
void     show_h_scrollbar_temporarily(TabData *tab);
gboolean auto_hide_h_scrollbar(gpointer data);

/* GTK enter / leave callbacks for the row-layout horizontal scrollbar. */
gboolean on_h_scrollbar_enter(GtkWidget *w, GdkEvent *e, gpointer user_data);
gboolean on_h_scrollbar_leave(GtkWidget *w, GdkEvent *e, gpointer user_data);

#endif /* SITERS_SCROLL_H */
