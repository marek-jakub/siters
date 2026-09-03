#ifndef SITERS_SEARCH_H
#define SITERS_SEARCH_H

#include <gtk/gtk.h>
#include "tab.h"

/* Search results list-store column layout (must match the store built in
   create_main_window). */
enum {
    SEARCH_COL_PAGE = 0,
    SEARCH_COL_COUNT,
    SEARCH_COL_LABEL,
    SEARCH_COL_NCOL
};

void    search_clear(void);
void    search_start(const char *text);
void    search_cancel(TabData *tab);
void    search_free(TabData *tab);
void    search_highlight_page(TabData *tab, cairo_t *cr, int page_1based,
                              double ox, double oy, double sc);

/* Signal-handler entry points wired up by create_main_window in siters.c */
void    on_search_activated(GtkEntry *entry, gpointer user_data);
void    on_search_clicked(GtkButton *btn, gpointer user_data);
void    on_search_row_activated(GtkTreeView *tv, GtkTreePath *path,
                                GtkTreeViewColumn *col, gpointer user_data);

/* Internal helpers provided by siters.c (the app hub). Declared here so the
   search module can drive navigation/redraw on the current document. */
TabData *get_current_left_tab(void);
gboolean ensure_tab_doc_loaded(TabData *tab);
void     queue_draw(TabData *tab);
void     cancel_doc_model_debounce(TabData *tab);
void     scroll_to_page(TabData *tab, int page, double target_y);
void     update_document_model_from_tab(TabData *tab);

#endif /* SITERS_SEARCH_H */
