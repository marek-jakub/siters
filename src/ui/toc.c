#include "toc.h"
#include "app.h"
#include "state.h"
#include "view.h"
#include "search.h"
#include "pdf.h"
#include "ui/sidebar.h"

extern App app;

static void populate_toc_treeview_recursive(PdfrOutline *entry, GtkTreeIter *parent) {
    for (PdfrOutline *cur = entry; cur; cur = cur->next) {
        int page = cur->page;
        char *named_dest = NULL;

        if (page <= 0) {
            /* Try to resolve named dest if page unresolved */
            if (cur->title && strlen(cur->title) > 0) {
                /* No named dest stored in outline; leave page as 0 */
            }
        }

        GtkTreeIter child;
        gtk_tree_store_append(app.toc_tree_store, &child, parent);
        gtk_tree_store_set(app.toc_tree_store, &child,
                          TOC_COL_LABEL, cur->title ? cur->title : "(Untitled)",
                          TOC_COL_PAGE, page,
                          TOC_COL_NAMED_DEST, named_dest,
                          -1);
        g_free(named_dest);

        if (cur->down) {
            populate_toc_treeview_recursive(cur->down, &child);
        }
    }
}

void populate_toc_treeview_for_tab(TabData *tab) {
    app.last_toc_selected_page = -1;
    gtk_tree_store_clear(app.toc_tree_store);
    if (!tab) return;
    if (!tab->doc && !app.is_restoring_session_tabs) ensure_tab_doc_loaded(tab);
    if (!tab->doc) return;

    PdfrOutline *outline = pdfr_load_outline(tab->doc);
    if (!outline) return;

    populate_toc_treeview_recursive(outline, NULL);
    pdfr_free_outline(tab->doc, outline);
}

void populate_toc_treeview(void) {
    populate_toc_treeview_for_tab(get_current_left_tab());
}

typedef struct {
    int target_page;
    int best_page;
    GtkTreePath *best_path;
} TocFindData;

static gboolean toc_find_nearest_cb(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, gpointer data) {
    TocFindData *find = data;
    int page = 0;
    gtk_tree_model_get(model, iter, TOC_COL_PAGE, &page, -1);
    if (page > 0 && page <= find->target_page && page > find->best_page) {
        find->best_page = page;
        if (find->best_path) gtk_tree_path_free(find->best_path);
        find->best_path = gtk_tree_path_copy(path);
    }
    return FALSE;
}

void update_toc_selection_for_current_page(TabData *tab) {
    if (!app.toc_tree_store || !app.toc_tree_view || !tab) return;

    int target_page = tab->cur_page + 1;
    if (target_page < 1) target_page = 1;

    if (target_page == app.last_toc_selected_page) return;

    TocFindData find = { .target_page = target_page, .best_page = 0, .best_path = NULL };
    gtk_tree_model_foreach(GTK_TREE_MODEL(app.toc_tree_store), toc_find_nearest_cb, &find);

    if (find.best_path) {
        app.toc_tree_syncing = TRUE;
        GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(app.toc_tree_view));
        gtk_tree_selection_select_path(sel, find.best_path);
        gtk_tree_view_expand_to_path(GTK_TREE_VIEW(app.toc_tree_view), find.best_path);
        gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(app.toc_tree_view), find.best_path, NULL, FALSE, 0, 0);
        app.last_toc_selected_page = target_page;
        gtk_tree_path_free(find.best_path);
        app.toc_tree_syncing = FALSE;
    }
}

void on_toc_row_activated(GtkTreeView *tree_view, GtkTreePath *path, GtkTreeViewColumn *column, gpointer user_data) {
    (void)column;
    (void)user_data;

    if (app.toc_tree_syncing) return;
    if (!gtk_widget_get_visible(app.toc_container)) return;

    GtkTreeModel *model = gtk_tree_view_get_model(tree_view);
    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter(model, &iter, path)) return;

    int page = 0;
    char *named_dest = NULL;
    gtk_tree_model_get(model, &iter, TOC_COL_PAGE, &page, TOC_COL_NAMED_DEST, &named_dest, -1);

    if (page == 0 && named_dest) {
        TabData *tab = get_current_left_tab();
        if (tab && ensure_tab_doc_loaded(tab)) {
            int resolved = pdfr_resolve_named_dest(tab->doc, named_dest, NULL, NULL);
            if (resolved > 0) page = resolved;
        }
    }

    if (page > 0) {
        TabData *tab = get_current_left_tab();
        if (tab) {
            cancel_doc_model_debounce(tab);
            app.last_toc_selected_page = page;
            tab->cur_page = page - 1;
            scroll_to_page(tab, page - 1, -1);
            update_document_model_from_tab(tab);
        }
    }
    g_free(named_dest);
}

void on_toc_toggled(GtkToggleButton *btn, gpointer user_data) {
    (void)user_data;

    if (!gtk_toggle_button_get_active(btn)) {
        if (app.current_sidebar_mode == SIDEBAR_TOC) {
            gtk_container_remove(GTK_CONTAINER(app.main_hbox), app.sidebar);
            gtk_box_reorder_child(GTK_BOX(app.main_hbox), app.content_vbox, 1);
            app.current_sidebar_mode = SIDEBAR_NONE;
        }
        return;
    }

    if (gtk_widget_get_parent(app.sidebar) != NULL) {
        gtk_container_remove(GTK_CONTAINER(app.main_hbox), app.sidebar);
    }

    /* Deactivate other toggle buttons */
    g_signal_handlers_block_by_func(app.sessions_btn, G_CALLBACK(on_sessions_toggled), NULL);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app.sessions_btn), FALSE);
    g_signal_handlers_unblock_by_func(app.sessions_btn, G_CALLBACK(on_sessions_toggled), NULL);

    g_signal_handlers_block_by_func(app.settings_btn, G_CALLBACK(on_settings_toggled), NULL);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app.settings_btn), FALSE);
    g_signal_handlers_unblock_by_func(app.settings_btn, G_CALLBACK(on_settings_toggled), NULL);

    g_signal_handlers_block_by_func(app.file_info_btn, G_CALLBACK(on_left_file_info_toggled), NULL);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app.file_info_btn), FALSE);
    g_signal_handlers_unblock_by_func(app.file_info_btn, G_CALLBACK(on_left_file_info_toggled), NULL);

    /* Hide other sidebar contents */
    gtk_widget_hide(app.sidebar_label);
    gtk_widget_hide(app.sessions_container);
    gtk_widget_hide(app.settings_container);
    gtk_widget_hide(app.file_info_container);

    populate_toc_treeview();

    gtk_widget_show_all(app.toc_container);
    update_toc_selection_for_current_page(get_current_left_tab());

    gtk_box_pack_start(GTK_BOX(app.main_hbox), app.sidebar, FALSE, FALSE, 0);
    gtk_box_reorder_child(GTK_BOX(app.main_hbox), app.content_vbox, 2);
    gtk_widget_set_size_request(app.sidebar, 300, -1);
    gtk_widget_show(app.sidebar);
    app.current_sidebar_mode = SIDEBAR_TOC;
}