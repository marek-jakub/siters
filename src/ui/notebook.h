#ifndef SITERS_UI_NOTEBOOK_H
#define SITERS_UI_NOTEBOOK_H

#include <gtk/gtk.h>
#include "tab.h"

/* Notebook lifecycle: file dialogs, closing tabs, and notebook helpers. */
void open_file_in_notebook(GtkWidget *notebook, gboolean is_helper);
void close_tab_in_notebook(GtkNotebook *notebook);
int find_matching_tab_index(GtkNotebook *notebook, const char *target_uri);
void update_last_read_for_notebook(GtkNotebook *notebook, GtkWidget *page, guint page_num);

/* Tab factory provided by the app hub (wires the per-tab widgets). */
TabData *create_new_tab(GtkWidget *notebook);

#endif /* SITERS_UI_NOTEBOOK_H */