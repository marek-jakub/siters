#ifndef SITERS_SESSION_SESSIONS_SIDEBAR_H
#define SITERS_SESSION_SESSIONS_SIDEBAR_H

#include <gtk/gtk.h>
#include "tab.h"

/* Sessions tree-store columns. */
typedef enum {
    SESSION_COL_LABEL = 0,      // visible text
    SESSION_COL_ROW_KIND,       // 0=session, 1=file
    SESSION_COL_SESSION_NAME,   // owning session
    SESSION_COL_DOC_URI,        // file row only
    SESSION_COL_COUNT
} SessionTreeCols;

typedef enum {
    SESSION_ROW_SESSION = 0,
    SESSION_ROW_FILE = 1
} SessionRowKind;

/* Reset the selected-row guard bookkeeping for the sessions tree. */
void reset_sessions_tree_selection_guard(void);

/* Highlight the matching document row (of the given tab) in the sessions tree. */
void update_sessions_tree_document_selection_for_tab(TabData *tab);

#endif /* SITERS_SESSION_SESSIONS_SIDEBAR_H */