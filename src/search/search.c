#include <gtk/gtk.h>
#include <cairo.h>
#include "app.h"
#include "search.h"
#include "pdf.h"

/* Cap per-page matches and per-idle-step batch so a huge page / document
   cannot monopolize the main loop or overflow the stack buffer. */
#define SEARCH_MAX_PER_PAGE 50
#define SEARCH_BATCH_SIZE 20

void search_cancel(TabData *tab) {
    if (!tab) return;
    tab->search_cancelled = TRUE;
    if (tab->search_idle_id) {
        g_source_remove(tab->search_idle_id);
        tab->search_idle_id = 0;
    }
}

void search_free(TabData *tab) {
    if (!tab) return;
    for (int i = 0; i < tab->search_results_n; i++)
        g_free(tab->search_results[i].rects);
    g_free(tab->search_results);
    tab->search_results = NULL;
    tab->search_results_n = 0;
    tab->search_results_cap = 0;
    g_free(tab->search_text);
    tab->search_text = NULL;
}

void search_clear(void) {
    TabData *tab = get_current_left_tab();
    if (tab) {
        search_cancel(tab);
        search_free(tab);
    }
    if (app.search_results_store)
        gtk_list_store_clear(app.search_results_store);
    if (app.search_no_results_label)
        gtk_widget_hide(app.search_no_results_label);
    if (tab) queue_draw(tab);
}

static gboolean search_idle(gpointer user_data) {
    TabData *tab = (TabData *)user_data;
    tab->search_idle_id = 0;

    if (!tab || tab->search_cancelled || !tab->doc)
        return FALSE;

    int n_pages = pdfr_count_pages(tab->doc);
    PdfrRect rects_buf[SEARCH_MAX_PER_PAGE];
    int batch = 0;

    for (int i = tab->search_page_idx; i < n_pages && !tab->search_cancelled && tab->doc && batch < SEARCH_BATCH_SIZE; i++, batch++) {
        int n = pdfr_search_page(tab->doc, i, tab->search_text, rects_buf, SEARCH_MAX_PER_PAGE);

        if (tab->search_cancelled || !tab->doc)
            return FALSE;

        tab->search_page_idx = i + 1;

        if (n <= 0) continue;

        if (tab->search_results_n >= tab->search_results_cap) {
            int new_cap = tab->search_results_cap ? tab->search_results_cap * 2 : 32;
            void *tmp = g_realloc(tab->search_results, new_cap * sizeof(*tab->search_results));
            if (!tmp) return FALSE;
            tab->search_results = tmp;
            tab->search_results_cap = new_cap;
        }
        tab->search_results[tab->search_results_n].page = i + 1;
        tab->search_results[tab->search_results_n].n_matches = n;
        tab->search_results[tab->search_results_n].rects = g_memdup2(rects_buf, sizeof(PdfrRect) * n);
        tab->search_results_n++;

        const char *plural = n == 1 ? "" : "es";
        gchar *label_ = g_strdup_printf("Page %d (%d match%s)", i + 1, n, plural);
        GtkTreeIter ti;
        gtk_list_store_append(app.search_results_store, &ti);
        gtk_list_store_set(app.search_results_store, &ti,
            SEARCH_COL_PAGE, i + 1,
            SEARCH_COL_COUNT, n,
            SEARCH_COL_LABEL, label_, -1);
        g_free(label_);
    }

    /* More pages remain — re-schedule */
    if (tab->search_page_idx < n_pages && !tab->search_cancelled && tab->doc) {
        queue_draw(tab);
        tab->search_idle_id = g_idle_add(search_idle, tab);
        return FALSE;
    }

    /* Search complete — shrink allocation to actual count */
    if (tab->search_results_n > 0 && tab->search_results_n < tab->search_results_cap) {
        tab->search_results = g_realloc(tab->search_results,
            tab->search_results_n * sizeof(*tab->search_results));
        tab->search_results_cap = tab->search_results_n;
    }

    if (tab->search_results_n == 0 && tab->search_text && *tab->search_text)
        gtk_widget_show(app.search_no_results_label);

    queue_draw(tab);
    return FALSE;
}

void search_start(const char *text) {
    TabData *tab = get_current_left_tab();
    if (!text || !*text) {
        if (tab) { search_clear(); queue_draw(tab); }
        return;
    }
    search_clear();
    tab = get_current_left_tab();
    if (!tab || !ensure_tab_doc_loaded(tab) || !tab->doc) return;
    tab->search_text = g_strdup(text);
    tab->search_cancelled = FALSE;
    tab->search_page_idx = 0;
    tab->search_idle_id = g_idle_add(search_idle, tab);
}

void on_search_activated(GtkEntry *entry, gpointer user_data) {
    (void)user_data;
    search_start(gtk_entry_get_text(entry));
}

void on_search_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn; (void)user_data;
    search_start(gtk_entry_get_text(GTK_ENTRY(app.search_entry)));
}

void on_search_row_activated(GtkTreeView *tv, GtkTreePath *path, GtkTreeViewColumn *col, gpointer user_data) {
    (void)tv; (void)col; (void)user_data;
    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(app.search_results_store), &iter, path)) return;
    int page = 0;
    gtk_tree_model_get(GTK_TREE_MODEL(app.search_results_store), &iter, SEARCH_COL_PAGE, &page, -1);
    if (page > 0) {
        TabData *tab = get_current_left_tab();
        if (tab) {
            cancel_doc_model_debounce(tab);
            tab->cur_page = page - 1;
            scroll_to_page(tab, page - 1, -1);
            update_document_model_from_tab(tab);
        }
    }
}

void search_highlight_page(TabData *tab, cairo_t *cr, int page_1based, double ox, double oy, double sc) {
    if (!tab || tab->search_results_n <= 0) return;
    for (int i = 0; i < tab->search_results_n; i++) {
        if (tab->search_results[i].page != page_1based) continue;
        cairo_save(cr);
        cairo_set_source_rgba(cr, 1.0, 1.0, 0.0, 0.3);
        for (int m = 0; m < tab->search_results[i].n_matches; m++) {
            PdfrRect *r = &tab->search_results[i].rects[m];
            cairo_rectangle(cr, ox + r->x1 * sc, oy + r->y1 * sc,
                            (r->x2 - r->x1) * sc, (r->y2 - r->y1) * sc);
        }
        cairo_fill(cr);
        cairo_restore(cr);
        break;
    }
}
