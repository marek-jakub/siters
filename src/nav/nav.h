#ifndef SITERS_NAV_H
#define SITERS_NAV_H

#include <gtk/gtk.h>
#include "tab.h"

/* Page-entry/total widgets: keep the 1-based page-number entry and the
   "/ N" total label in sync with the current tab's page. Exported so the
   app hub and the session module can refresh the nav bar after page
   changes. */
void sync_page_widget_from_tab(TabData *tab);
void sync_right_page_widget_from_tab(TabData *tab);

/* Page-number entry handlers: digit-only insertion filter and Enter-to-jump. */
void on_page_entry_insert_text(GtkEditable *editable,
                               const gchar *text,
                               gint length,
                               gint *position,
                               gpointer user_data);
void on_page_entry_activate(GtkEntry *entry, gpointer user_data);
void on_right_page_entry_activate(GtkEntry *entry, gpointer user_data);

#endif /* SITERS_NAV_H */