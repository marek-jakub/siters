#ifndef SITERS_SESSION_SESSIONS_TREE_H
#define SITERS_SESSION_SESSIONS_TREE_H

#include <gtk/gtk.h>

/* Sessions sidebar tree: populate the tree store and handle row selection. */
void populate_sessions_treeview(void);
void on_sessions_treeview_cursor_changed(GtkTreeView *tree_view, gpointer user_data);

#endif /* SITERS_SESSION_SESSIONS_TREE_H */