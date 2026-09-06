#ifndef SITERS_UI_TOC_H
#define SITERS_UI_TOC_H

#include <gtk/gtk.h>
#include "tab.h"

/* TOC tree-store columns. */
typedef enum {
    TOC_COL_LABEL = 0,
    TOC_COL_PAGE,
    TOC_COL_NAMED_DEST,
    TOC_COL_COUNT
} TOCTreeCols;

/* Rebuild the TOC tree from the given tab's outline. */
void populate_toc_treeview_for_tab(TabData *tab);

/* Rebuild the TOC tree from the current left tab's outline. */
void populate_toc_treeview(void);

/* Scroll-highlight the TOC entry matching the current page. */
void update_toc_selection_for_current_page(TabData *tab);

/* Clicking a TOC row navigates the current tab to that page. */
void on_toc_row_activated(GtkTreeView *tree_view, GtkTreePath *path, GtkTreeViewColumn *column, gpointer user_data);

#endif /* SITERS_UI_TOC_H */