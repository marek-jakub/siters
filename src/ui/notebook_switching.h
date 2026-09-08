#ifndef SITERS_UI_NOTEBOOK_SWITCHING_H
#define SITERS_UI_NOTEBOOK_SWITCHING_H

#include <gtk/gtk.h>

/* Tab-switch callbacks for the left (document) and right (helper)
   notebooks: RAM-safe doc unloading of non-current tabs, per-tab state
   restore on switch, and the page-reorder persistence hook. */
void on_left_notebook_switch_page(GtkNotebook *notebook, GtkWidget *page, guint page_num, gpointer user_data);
void on_right_notebook_switch_page(GtkNotebook *notebook, GtkWidget *page, guint page_num, gpointer user_data);
void on_notebook_page_reordered(GtkNotebook *notebook, GtkWidget *page, guint page_num, gpointer user_data);

#endif /* SITERS_UI_NOTEBOOK_SWITCHING_H */