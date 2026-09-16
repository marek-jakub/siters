#include "tab.h"
#include "app.h"
#include "state.h"
#include "nav.h"
#include "view.h"
#include "search.h"
#include "pdf.h"
#include "log.h"
#include "sessions_model.h"
#include "ui/toc.h"
#include "ui/tab_lifecycle.h"
#include "fileinfo/fileinfo.h"
#include "ui/layout.h"

extern App app;

void cancel_tab_restore(TabData *tab) {
    if (!tab || !tab->pending_restore) return;
    if (tab->pending_restore->source_id) {
        g_source_remove(tab->pending_restore->source_id);
    }
    tab->pending_restore->tab = NULL; // invalidate the pointer
    g_free(tab->pending_restore);
    tab->pending_restore = NULL;
}

static void cancel_tab_deferred_load(TabData *tab) {
    if (!tab || !tab->load_idle_id) return;
    g_source_remove(tab->load_idle_id);
    tab->load_idle_id = 0;
}

void unload_tab_document(TabData *tab) {
    if (!tab) return;
    cancel_tab_restore(tab);
    cancel_doc_model_debounce(tab);
    search_cancel(tab);
    search_free(tab);
    if (tab->zoom_scroll_source_id) {
        g_source_remove(tab->zoom_scroll_source_id);
        tab->zoom_scroll_source_id = 0;
    }
    if (tab->page_links) {
        for (int i = 0; i < tab->page_links_n; i++) {
            if (tab->page_links[i])
                pdfr_free_links(tab->doc, tab->page_links[i]);
        }
        g_free(tab->page_links);
        tab->page_links = NULL;
        tab->page_links_n = 0;
    }
    pdfr_close(tab->doc);
    tab->doc = NULL;
    free_page_cached_arrays(tab);
    invalidate_page_cache(tab);
    g_free(tab->page_cache);
    tab->page_cache = NULL;
}

void destroy_tab_data(gpointer data) {
    TabData *tab = data;
    if (!tab) return;
    cancel_tab_restore(tab);
    cancel_tab_deferred_load(tab);
    cancel_doc_model_debounce(tab);
    if (tab->zoom_scroll_source_id) {
        g_source_remove(tab->zoom_scroll_source_id);
        tab->zoom_scroll_source_id = 0;
    }
    if (tab->h_scrollbar_timer_id) {
        g_source_remove(tab->h_scrollbar_timer_id);
        tab->h_scrollbar_timer_id = 0;
    }
    /* Free link mappings while doc is still alive */
    if (tab->page_links) {
        for (int i = 0; i < tab->page_links_n; i++) {
            if (tab->page_links[i])
                pdfr_free_links(tab->doc, tab->page_links[i]);
        }
        g_free(tab->page_links);
    }
    search_cancel(tab);
    search_free(tab);
    if (tab->doc) {
        pdfr_close(tab->doc);
        tab->doc = NULL;
        pdfr_purge_store();
    }
    g_free(tab->current_file);
    invalidate_page_cache(tab);
    g_free(tab->page_cache);
    free_page_cached_arrays(tab);
    g_free(tab);
}

static void refresh_tab_label(TabData *tab) {
    if (!tab || !tab->tab_label || !tab->current_file) return;

    char *basename = g_path_get_basename(tab->current_file);
    const char *label_text = basename;
    char *truncated = NULL;
    if (app.sessions_model) {
        int max_chars = sessions_model_get_tab_width(app.sessions_model);
        if (max_chars > 0 && (int)strlen(basename) > max_chars) {
            truncated = g_strndup(basename, max_chars);
            label_text = truncated;
        }
    }
    if (tab->load_failed) {
        char *marker = g_strdup_printf("%s (failed)", label_text);
        gtk_label_set_text(GTK_LABEL(tab->tab_label), marker);
        g_free(marker);
    } else {
        gtk_label_set_text(GTK_LABEL(tab->tab_label), label_text);
    }
    g_free(truncated);
    g_free(basename);
}

void set_tab_filename(TabData *tab, const char *filename) {
    if (!tab || !filename) return;

    /* track current filename for per-document settings */
    if (tab->current_file)
        g_free(tab->current_file);
    tab->current_file = g_strdup(filename);
    tab->load_failed = FALSE;

    /* Update the tab's label with filename */
    refresh_tab_label(tab);
}

gboolean ensure_tab_doc_loaded(TabData *tab) {
    if (!tab || !tab->current_file) return FALSE;
    if (tab->doc) {
        if (tab->load_failed) {
            tab->load_failed = FALSE;
            refresh_tab_label(tab);
        }
        return TRUE;
    }

    char *open_error = NULL;
    PdfrDoc *doc = pdfr_open(tab->current_file, &open_error);
    if (!doc) {
        LOG_ERROR("Failed to reopen PDF: %s", open_error ? open_error : "unknown error");
        free(open_error);
        tab->load_failed = TRUE;
        refresh_tab_label(tab);
        return FALSE;
    }
    free(open_error);

    if (tab->doc) {
        pdfr_close(tab->doc);
    }
    tab->doc = doc;
    tab->n_pages = pdfr_count_pages(doc);
    cache_page_dimensions(tab);
    /* Apply the saved per-document model (layout mode, zoom, page) BEFORE the
       first build so the initial view already uses the correct layout. */
    restore_document_model_to_tab(tab);
    build_continuous_view(tab);
    if (tab->load_failed) {
        tab->load_failed = FALSE;
        refresh_tab_label(tab);
    }
    queue_draw(tab);
    return TRUE;
}

void open_document_in_tab(TabData *tab) {
    if (!tab || !tab->current_file || tab->doc) return;

    char *open_error = NULL;
    PdfrDoc *doc = pdfr_open(tab->current_file, &open_error);
    if (!doc) {
        LOG_ERROR("Failed to open PDF: %s", open_error ? open_error : "unknown error");
        free(open_error);
        tab->load_failed = TRUE;
        refresh_tab_label(tab);
        return;
    }
    free(open_error);

    tab->doc = doc;
    tab->n_pages = pdfr_count_pages(doc);
    cache_page_dimensions(tab);
    tab->cur_page = 0;
    tab->zoom = 96.0;

    if (tab->load_failed) {
        tab->load_failed = FALSE;
        refresh_tab_label(tab);
    }

    queue_draw(tab);

    {
        restore_document_model_to_tab(tab);

        build_continuous_view(tab);

        /* Defer scrolling until widget is allocated */
        tab->initial_scroll_pending = TRUE;

        /* Ensure page counter and layout buttons show real values immediately after load. */
        if (tab == get_current_left_tab()) {
            sync_left_layout_buttons(tab);
        }
        if (tab == get_current_right_tab()) {
            sync_right_layout_buttons(tab);
        }
        sync_nav_for_tab(tab);
    }
}

static gboolean load_tab_deferred_cb(gpointer data) {
    TabData *tab = data;
    tab->load_idle_id = 0;
    if (tab && !tab->doc)
        open_document_in_tab(tab);
    return G_SOURCE_REMOVE;
}

void schedule_tab_deferred_load(TabData *tab) {
    if (!tab || tab->doc || tab->load_idle_id) return;
    tab->load_idle_id = g_idle_add_full(G_PRIORITY_LOW, load_tab_deferred_cb, tab, NULL);
}

void load_file_into_tab(TabData *tab, const char *filename) {
    if (!tab || !filename) return;
    set_tab_filename(tab, filename);
    open_document_in_tab(tab);
    if (app.current_sidebar_mode == SIDEBAR_TOC) populate_toc_treeview();
    if (app.current_sidebar_mode == SIDEBAR_FILE_INFO) update_file_info_labels(get_current_left_tab());
    refresh_right_popover_labels();
}
