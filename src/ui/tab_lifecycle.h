#ifndef SITERS_UI_TAB_LIFECYCLE_H
#define SITERS_UI_TAB_LIFECYCLE_H

#include <gtk/gtk.h>
#include "tab.h"

/* Tab document lifecycle: manages a TabData from creation through destroy. */

/* Destroy-notify stored on notebook pages (owns the TabData). */
void destroy_tab_data(gpointer data);

/* Loading helpers used by the app hub (session restore, notebook switch). */
void set_tab_filename(TabData *tab, const char *filename);
void open_document_in_tab(TabData *tab);
void load_file_into_tab(TabData *tab, const char *filename);
void schedule_tab_deferred_load(TabData *tab);
gboolean ensure_tab_doc_loaded(TabData *tab);

/* Cancel a pending session-restore anchor; used by the view hub. */
void cancel_tab_restore(TabData *tab);

/* Apply a tab's saved per-document model (layout, zoom, page). Provided by
   the app hub and shared with the notebook switch handlers. */
void restore_document_model_to_tab(TabData *tab);

#endif /* SITERS_UI_TAB_LIFECYCLE_H */