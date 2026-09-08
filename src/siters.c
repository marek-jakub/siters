#include <gtk/gtk.h>
#include <atk/atk.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include "pdf.h"
#include <math.h>
#include "siters.h"
#include "sessions_model.h"
#include "session_model.h"
#include "document_model.h"
#include "app.h"
#include "tab.h"
#include "search.h"
#include "state.h"
#include "sessions_sidebar.h"
#include "sessions_tree.h"
#include "view.h"
#include "theme.h"
#include "links.h"
#include "settings/settings.h"
#include "nav.h"
#include "ui/toolbar.h"
#include "ui/sidebar.h"
#include "ui/tab_lifecycle.h"
#include "ui/notebook.h"
#include "ui/notebook_switching.h"
#include "ui/zoom.h"
#include "ui/layout.h"
#include "ui/toc.h"
#include "fileinfo/fileinfo.h"

#include "mem_debug.h"

/* To limit RAM use as large zoom takes many MB of resources */
#define MAX_SURFACE_DIM 2000
#define MAX_CACHE_BYTES (40 * 1024 * 1024)


/* DATADIR is normally defined by -DDATADIR=... at build time.
   This fallback lets clang-based tools parse the file without flags. */
#ifndef DATADIR
#define DATADIR "."
#endif

/* Single application-wide state object. All former module-level statics now
   live as fields of this struct (defined in app.h). */
App app;


TabData *get_current_left_tab(void);

void cancel_doc_model_debounce(TabData *tab);

/* Function prototypes */
void save_state(void);


void hide_right_pane(void);
static void set_right_notebook_session(const gchar *session_name);
static void on_tab_close_clicked(GtkButton *btn, gpointer user_data);
static void on_tab_scrolled_size_allocate(GtkWidget *widget, GdkRectangle *allocation, gpointer user_data);


/* PDF handling function prototypes */
void queue_draw(TabData *tab);
static gboolean on_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data);
void scroll_to_page(TabData *tab, int page, double target_y);
static void on_scroll_value_changed(GtkAdjustment *adj, gpointer user_data);
static gboolean on_drawing_scroll(GtkWidget *widget, GdkEventScroll *event, gpointer user_data);
static gboolean on_drawing_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
static gboolean on_drawing_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
static void start_initial_scroll_restore(TabData *tab, int target_page, double target_zoom, double target_fraction);
/* Build a compound key "side:uri" to differentiate left vs right notebook state */
char* make_document_key(const char *session_name, const char *uri, gboolean is_helper) {
    const char *side = is_helper ? "right" : "left";
    return g_strdup_printf("%s:%s:%s", session_name ? session_name : "Unknown", side, uri);
}

void update_document_model_from_tab(TabData *tab) {
    if (!tab || !tab->current_file || !app.document_models) return;
    if (!tab->cached_page_widths) return;

    char *uri = g_filename_to_uri(tab->current_file, NULL, NULL);
    if (!uri) return;

    char *key = make_document_key(app.current_selected_session, uri, tab->is_helper);

    // Get or create document model
    document_model_t *doc_model = g_hash_table_lookup(app.document_models, key);
    if (!doc_model) {
        doc_model = document_model_new();
        document_model_set_url(doc_model, uri);
        g_hash_table_insert(app.document_models, key, doc_model);
    } else {
        g_free(key);
    }

    // Update current state
    document_model_set_zoom(doc_model, tab->zoom);
    document_model_set_visualization_mode(doc_model, tab->layout_mode);
    document_model_set_current_page(doc_model, tab->cur_page + 1);  // Store as 1-based
    document_model_set_page_count(doc_model, tab->n_pages);

    const double spacing = 6.0;
    double scale = get_ppi_scale(tab);

    if (tab->layout_mode == 2) {
        double scroll_x = 0.0;
        if (tab->h_scrollbar) {
            GtkAdjustment *sadj = gtk_range_get_adjustment(GTK_RANGE(tab->h_scrollbar));
            scroll_x = gtk_adjustment_get_value(sadj);
        }
        document_model_set_scroll_offset(doc_model, scroll_x);
        double x_top = spacing;
        double page_w = 0;
        for (int i = 0; i < tab->cur_page; ++i) {
            x_top += tab->cached_page_widths[i] * scale + spacing;
        }
        page_w = tab->cached_page_widths[tab->cur_page] * scale;
        double intra = scroll_x - x_top;
        double fraction = page_w > 0 ? intra / page_w : 0.0;
        if (fraction < 0) fraction = 0;
        if (fraction > 1) fraction = 1;
        document_model_set_intra_page_fraction(doc_model, fraction);
    } else {
        GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(tab->scrolled));
        document_model_set_scroll_offset(doc_model, gtk_adjustment_get_value(vadj));

        double scroll_y = gtk_adjustment_get_value(vadj);
        double y_top = spacing;
        double page_h = 0;
        if (tab->layout_mode == 1) {
            int cur_row = tab->cur_page / 2;
            for (int r = 0; r < cur_row; r++) {
                double row_h = 0.0;
                for (int p = 0; p < 2; p++) {
                    int idx = r * 2 + p;
                    if (idx >= tab->n_pages) break;
                    double h = tab->cached_page_heights[idx] * scale;
                    if (h > row_h) row_h = h;
                }
                y_top += row_h + spacing;
            }
            page_h = tab->cached_page_heights[tab->cur_page] * scale;
        } else {
            for (int i = 0; i < tab->cur_page; ++i) {
                y_top += tab->cached_page_heights[i] * scale + spacing;
            }
            page_h = tab->cached_page_heights[tab->cur_page] * scale;
        }

        double intra = scroll_y - y_top;
        double fraction = page_h > 0 ? intra / page_h : 0.0;
        if (fraction < 0) fraction = 0;
        if (fraction > 1) fraction = 1;
        document_model_set_intra_page_fraction(doc_model, fraction);
    }

    g_free(uri);
}

void restore_document_model_to_tab(TabData *tab) {
    if (!tab || !tab->current_file || !app.document_models) return;

    char *uri = g_filename_to_uri(tab->current_file, NULL, NULL);
    if (!uri) return;

    char *key = make_document_key(app.current_selected_session, uri, tab->is_helper);
    document_model_t *doc_model = g_hash_table_lookup(app.document_models, key);
    g_free(key);
    if (doc_model) {
        int saved_page = document_model_get_current_page(doc_model);
        double saved_zoom = document_model_get_zoom(doc_model);
        double saved_fraction = document_model_get_intra_page_fraction(doc_model);
        tab->layout_mode = document_model_get_visualization_mode(doc_model);
        tab->zoom = saved_zoom;

        /* Clamp page to valid range */
        if (saved_page < 1) saved_page = 1;
        if (saved_page > tab->n_pages) saved_page = tab->n_pages;

        /* Set cur_page immediately so it's available even if the async restore runs later */
        tab->cur_page = saved_page - 1;

        /* Initiate the robust, layout-aware scroll restore */
        start_initial_scroll_restore(tab, saved_page - 1, saved_zoom, saved_fraction);
    } else {
        tab->cur_page = 0;
        /* No saved state, restore to first page top */
        start_initial_scroll_restore(tab, 0, 96.0, 0.0);
    }

    g_free(uri);
}

/* Helper: Determine current page from scroll position */
static int compute_page_from_scroll(TabData *tab, double scroll_y) {
    if (!tab || !tab->cached_page_widths || tab->n_pages <= 0) return 0;

    const double spacing = 6.0;
    double scale = get_ppi_scale(tab);
    double y = spacing;
    int visible_page = tab->n_pages - 1;

    /* Heuristic: O(1) check if still on last known page */
    if (tab->last_known_page >= 0 && tab->last_known_page < tab->n_pages) {
        double page_len;
        if (tab->layout_mode == 2) {
            page_len = tab->cached_page_widths[tab->last_known_page] * scale;
        } else {
            page_len = get_page_height_ppi(tab, tab->last_known_page);
        }
        if (page_len > 0 && scroll_y >= tab->last_known_page_start && scroll_y < tab->last_known_page_start + page_len + spacing)
            return tab->last_known_page;
    }

    if (tab->layout_mode == 2) {
        for (int i = 0; i < tab->n_pages; ++i) {
            double page_w = tab->cached_page_widths[i] * scale;
            if (scroll_y >= y && scroll_y < y + page_w) {
                tab->last_known_page = i;
                tab->last_known_page_start = y;
                return i;
            }
            y += page_w + spacing;
        }
    } else if (tab->layout_mode == 1) {
        for (int i = 0; i < tab->n_pages; i += 2) {
            double row_h = 0.0;
            for (int p = 0; p < 2; p++) {
                int idx = i + p;
                if (idx >= tab->n_pages) break;
                double ph = get_page_height_ppi(tab, idx);
                if (ph > row_h) row_h = ph;
            }
            if (row_h < 1.0) row_h = 1.0;
            if (y + row_h > scroll_y) {
                visible_page = i;
                tab->last_known_page = visible_page;
                tab->last_known_page_start = y;
                break;
            }
            y += row_h + spacing;
        }
    } else {
        for (int i = 0; i < tab->n_pages; ++i) {
            double page_h = get_page_height_ppi(tab, i);
            if (page_h <= 0) continue;
            if (scroll_y >= y && scroll_y < y + page_h) {
                tab->last_known_page = i;
                tab->last_known_page_start = y;
                return i;
            }
            y += page_h + spacing;
        }
    }

    tab->last_known_page = visible_page;
    tab->last_known_page_start = y;
    return visible_page;
}

/* Multi-stage restore with layout settle - the robust approach */
static gboolean do_initial_scroll_stage(gpointer user_data) {
    RestoreState *restore = (RestoreState *)user_data;

    if (!restore || !restore->tab) {
        g_free(restore);
        return FALSE;
    }

    // Additional check: verify this restore is still the active one for the tab
    if (restore->tab->pending_restore != restore) {
        // This restore has been superseded or cancelled
        g_free(restore);
        return FALSE;
    }

    if (!restore->tab->doc) {
        restore->tab->pending_restore = NULL;
        g_free(restore);
        return FALSE;
    }

    TabData *tab = restore->tab;

    /* ========== STAGE 0: Initialize & Apply Zoom ========== */
    if (restore->restore_stage == 0) {
        /* Validate basic state */
        if (!gtk_widget_get_realized(tab->scrolled)) {
            /* Widget not ready, retry next idle */
            restore->settle_attempts++;
            if (restore->settle_attempts < 20) {
                restore->source_id = g_idle_add(do_initial_scroll_stage, restore);
                return FALSE;
            }
        } else {
            GtkAllocation alloc;
            gtk_widget_get_allocation(tab->scrolled, &alloc);
            if (alloc.width < 1 || alloc.height < 1) {
                restore->settle_attempts++;
                if (restore->settle_attempts < 20) {
                    /* Layout not finalized, retry next idle */
                    restore->source_id = g_idle_add(do_initial_scroll_stage, restore);
                    return FALSE;
                }
            }
        }

        /* Clamp page to valid range */
        if (restore->target_page < 0) restore->target_page = 0;
        if (restore->target_page >= tab->n_pages) restore->target_page = tab->n_pages - 1;

        /* Clamp zoom (PPI: 10-500) */
        if (restore->target_zoom < 10.0) restore->target_zoom = 10.0;
        if (restore->target_zoom > 500.0) restore->target_zoom = 500.0;

        /* Clamp fraction */
        if (restore->target_fraction < 0.0) restore->target_fraction = 0.0;
        if (restore->target_fraction > 1.0) restore->target_fraction = 1.0;

        /* Apply zoom */
        tab->zoom = restore->target_zoom;

        /* Rebuild layout with new zoom */
        build_continuous_view(tab);

        /* Move to next stage */
        restore->restore_stage = 1;
        restore->settle_attempts = 0;
        restore->source_id = g_idle_add(do_initial_scroll_stage, restore);
        return FALSE;
    }

    /* ========== STAGE 1: Wait for Layout to Settle ========== */
    if (restore->restore_stage == 1) {
        /* Check if layout has settled by seeing if page heights are stable */
        double page_h = get_page_height_ppi(tab, restore->target_page);

        if (page_h <= 0) {
            /* Page height not ready yet */
            restore->settle_attempts++;
            if (restore->settle_attempts < 20) {  /* Try up to 20 times (~100ms) */
                restore->source_id = g_idle_add(do_initial_scroll_stage, restore);
                return FALSE;
            }
            /* Timeout, proceed anyway */
        }

        /* Move to measurement stage */
        restore->restore_stage = 2;
        restore->source_id = g_idle_add(do_initial_scroll_stage, restore);
        return FALSE;
    }

    /* ========== STAGE 2: Calculate Exact Scroll Position ========== */
    if (restore->restore_stage == 2) {
        double scale = get_ppi_scale(tab);
        if (tab->layout_mode == 2) {
            const double spacing = 6.0;
            double x_offset = spacing;
            double page_w = 0;
            for (int i = 0; i < restore->target_page; ++i) {
                x_offset += tab->cached_page_widths[i] * scale + spacing;
            }
            page_w = tab->cached_page_widths[restore->target_page] * scale;
            if (page_w <= 0) page_w = 1.0;
            double target_scroll = x_offset + (restore->target_fraction * page_w);
            if (target_scroll < 0) target_scroll = 0;
            if (tab->h_scrollbar) {
                GtkAdjustment *sadj = gtk_range_get_adjustment(GTK_RANGE(tab->h_scrollbar));
                double upper = gtk_adjustment_get_upper(sadj);
                double page_size = gtk_adjustment_get_page_size(sadj);
                if (target_scroll > upper - page_size) target_scroll = upper - page_size;
                if (target_scroll < 0) target_scroll = 0;
                gtk_adjustment_set_value(sadj, target_scroll);
            }
        } else {
            double page_top = calculate_page_top_offset_ppi(tab, restore->target_page);
            double page_h = get_page_height_ppi(tab, restore->target_page);

            if (page_h <= 0) {
                /* Fallback: just scroll to page top */
                page_h = 1.0;
            }

            /* Calculate scroll position: page top + (fraction * page height) */
            double target_scroll = page_top + (restore->target_fraction * page_h);

            /* Clamp to valid range */
            GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(tab->scrolled));
            double upper = gtk_adjustment_get_upper(vadj);
            double page_size = gtk_adjustment_get_page_size(vadj);

            if (target_scroll < 0) target_scroll = 0;
            if (target_scroll > upper - page_size) target_scroll = upper - page_size;
            if (target_scroll < 0) target_scroll = 0;  /* In case page_size > upper */

            /* Set the scroll position */
            gtk_adjustment_set_value(vadj, target_scroll);
        }

        /* Move to verification stage */
        restore->restore_stage = 3;
        restore->source_id = g_idle_add(do_initial_scroll_stage, restore);
        return FALSE;
    }

    /* ========== STAGE 3: Verify & Finalize ========== */
    if (restore->restore_stage == 3) {
        double actual_scroll;
        if (tab->layout_mode == 2) {
            if (tab->h_scrollbar) {
                GtkAdjustment *sadj = gtk_range_get_adjustment(GTK_RANGE(tab->h_scrollbar));
                actual_scroll = gtk_adjustment_get_value(sadj);
            } else {
                actual_scroll = 0.0;
            }
        } else {
            GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(tab->scrolled));
            actual_scroll = gtk_adjustment_get_value(vadj);
        }

        /* Update cur_page based on actual scroll position */
        tab->cur_page = compute_page_from_scroll(tab, actual_scroll);

        /* Update UI */
        if (tab == get_current_left_tab()) {
            sync_page_widget_from_tab(tab);
        }
        if (tab == get_current_right_tab()) {
            sync_right_page_widget_from_tab(tab);
        }

        /* Done - return FALSE to remove this idle callback */
        restore->tab->pending_restore = NULL;
        g_free(restore);

        return FALSE;
    }

    /* ========== STAGE 4: Finalize ========== */
    if (restore->restore_stage == 4) {
        /* Update UI */
        if (tab == get_current_left_tab()) {
            sync_page_widget_from_tab(tab);
        }
        if (tab == get_current_right_tab()) {
            sync_right_page_widget_from_tab(tab);
        }

        restore->tab->pending_restore = NULL;
        g_free(restore);
        return FALSE;
    }

    if (restore->tab) {
        restore->tab->pending_restore = NULL;
    }
    g_free(restore);
    return FALSE;
}

/* Called to initiate the robust restore process */
static void start_initial_scroll_restore(TabData *tab, int target_page, double target_zoom,
                                         double target_fraction) {
    if (!tab || target_page < 0 || target_zoom < 0.1) return;

    cancel_tab_restore(tab);

    RestoreState *restore = g_malloc(sizeof(RestoreState));
    restore->tab = tab;
    restore->restore_stage = 0;
    restore->settle_attempts = 0;
    restore->target_page = target_page;
    restore->target_zoom = target_zoom;
    restore->target_fraction = target_fraction;
    restore->source_id = 0;

    tab->initial_scroll_pending = TRUE;
    tab->pending_restore = restore;

    /* Start the multi-stage restore process */
    restore->source_id = g_idle_add(do_initial_scroll_stage, restore);
}

static void on_title_bar_toggle(GtkToggleButton *button, gpointer user_data) {
    (void)user_data;
    gboolean active = gtk_toggle_button_get_active(button);
    GtkWidget *btn = GTK_WIDGET(button);
    if (active) {
        gtk_button_set_image(GTK_BUTTON(btn), create_toolbar_icon("title-bar-on"));
        gtk_window_set_decorated(GTK_WINDOW(app.window), TRUE);
    } else {
        gtk_button_set_image(GTK_BUTTON(btn), create_toolbar_icon("title-bar-off"));
        gboolean was_maximized = gtk_window_is_maximized(GTK_WINDOW(app.window));
        if (was_maximized) {
            gtk_window_unmaximize(GTK_WINDOW(app.window));
        }
        gtk_window_set_decorated(GTK_WINDOW(app.window), FALSE);
        if (was_maximized) {
            gtk_window_maximize(GTK_WINDOW(app.window));
        }
    }
    // Force layout update after changing decoration
    gtk_widget_queue_resize(app.window);
}

static void on_helper_toggle(GtkToggleButton *button, gpointer user_data) {
    (void)user_data;
    gboolean active = gtk_toggle_button_get_active(button);
    GtkWidget *btn = GTK_WIDGET(button);

    if (active) {
        gtk_button_set_image(GTK_BUTTON(btn), create_toolbar_icon("sidebar-helper-on"));
        if (app.right_pane) {
            gtk_widget_show_all(GTK_WIDGET(app.right_pane));
            set_right_notebook_session(app.current_selected_session ? app.current_selected_session : "Default");
            sync_right_layout_buttons(get_current_right_tab());
        }
    } else {
        // Save current helper tabs before hiding
        if (app.current_selected_session) {
            save_open_tabs_for_session(app.current_selected_session);
        }
        gtk_button_set_image(GTK_BUTTON(btn), create_toolbar_icon("sidebar-helper-off"));
        if (app.right_pane) {
            gtk_widget_hide(GTK_WIDGET(app.right_pane));
        }
    }
}

static void on_minimize_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    GtkWindow *win = GTK_WINDOW(user_data);
    gtk_window_iconify(win);
}

static gboolean defer_maximize_toggle(gpointer user_data) {
    GtkWindow *win = GTK_WINDOW(user_data);
    app.maximize_pending_id = 0;
    if (win) {
        if (gtk_window_is_maximized(win))
            gtk_window_unmaximize(win);
        else
            gtk_window_maximize(win);
        if (app.left_notebook) gtk_widget_queue_resize(app.left_notebook);
        if (app.right_notebook) gtk_widget_queue_resize(app.right_notebook);
    }
    g_object_unref(win);
    return FALSE;
}

static void on_maximize_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    GtkWindow *win = GTK_WINDOW(user_data);
    if (!win || app.maximize_pending_id) return;
    g_object_ref(win);
    app.maximize_pending_id = g_idle_add(defer_maximize_toggle, win);
}

static void on_close_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    save_state();
    gtk_main_quit();
}

static void on_sessions_add_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;

    const char *session_name = gtk_entry_get_text(GTK_ENTRY(app.sessions_entry));
    if (session_name && strlen(session_name) > 0) {
        // Check if session name already exists
        const GList *existing_sessions = sessions_model_get_session_names(app.sessions_model);
        for (const GList *iter = existing_sessions; iter != NULL; iter = iter->next) {
            if (strcmp((const char*)iter->data, session_name) == 0) {
                // Show error dialog
                GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(app.window),
                    GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                    GTK_MESSAGE_ERROR,
                    GTK_BUTTONS_OK,
                    "A session with the name '%s' already exists. Please choose a different name.",
                    session_name);
                gtk_dialog_run(GTK_DIALOG(dialog));
                gtk_widget_destroy(dialog);
                return;
            }
        }

        // Add to model
        sessions_model_add_session_name(app.sessions_model, session_name);

        // Create per-session model so notebook restore can find it
        if (app.session_models && !g_hash_table_lookup(app.session_models, session_name)) {
            session_model_t *session = session_model_new();
            session_model_set_session_name(session, session_name);
            g_hash_table_insert(app.session_models, g_strdup(session_name), session);
        }

        // Update tree view
        populate_sessions_treeview();

        // Clear entry
        gtk_entry_set_text(GTK_ENTRY(app.sessions_entry), "");

        // Save state to persist the new session
        save_state();
    }
}

static void on_sessions_remove_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;

    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(app.sessions_tree_view));
    GtkTreeIter iter;
    GtkTreeModel *model;

    if (gtk_tree_selection_get_selected(selection, &model, &iter)) {
        gchar *session_name;
        gtk_tree_model_get(model, &iter, SESSION_COL_SESSION_NAME, &session_name, -1);

        // Prevent removing the "Default" session
        if (strcmp(session_name, "Default") == 0) {
            // Show a message dialog that Default session cannot be removed
            GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(app.window),
                GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                GTK_MESSAGE_INFO,
                GTK_BUTTONS_OK,
                "The 'Default' session cannot be removed.");
            gtk_dialog_run(GTK_DIALOG(dialog));
            gtk_widget_destroy(dialog);
            g_free(session_name);
            return;
        }

        // Remove from model
        sessions_model_remove_session_name(app.sessions_model, session_name);

        // Remove stale document models for this session
        if (app.session_models && app.document_models) {
            session_model_t *sm = g_hash_table_lookup(app.session_models, session_name);
            if (sm) {
                const GList *iter;
                for (iter = session_model_get_document_urls(sm); iter; iter = iter->next) {
                    char *key = make_document_key(session_name, (const char *)iter->data, FALSE);
                    g_hash_table_remove(app.document_models, key);
                    g_free(key);
                }
                for (iter = session_model_get_helper_document_urls(sm); iter; iter = iter->next) {
                    char *key = make_document_key(session_name, (const char *)iter->data, TRUE);
                    g_hash_table_remove(app.document_models, key);
                    g_free(key);
                }
            }
        }

        // Remove from session_models hash table so stale data doesn't persist
        if (app.session_models)
            g_hash_table_remove(app.session_models, session_name);

        // Update tree view
        populate_sessions_treeview();

        // If the removed session is currently selected, switch to "Default" or first remaining session
        if (app.current_selected_session && strcmp(app.current_selected_session, session_name) == 0) {
            g_free(app.current_selected_session);
            app.current_selected_session = g_strdup("Default"); // or first remaining session

            restore_open_tabs_for_session(app.current_selected_session);
            sync_page_widget_from_tab(get_current_left_tab());

            if (app.sessions_model) {
                sessions_model_set_last_open_session(app.sessions_model, app.current_selected_session);
            }

            update_window_title_for_session(app.current_selected_session);
        }

        g_free(session_name);

        // Save state to persist the removed session
        save_state();
    }
}

static void on_sessions_update_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;

    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(app.sessions_tree_view));
    GtkTreeIter iter;
    GtkTreeModel *model;

    if (gtk_tree_selection_get_selected(selection, &model, &iter)) {
        const char *new_name = gtk_entry_get_text(GTK_ENTRY(app.sessions_entry));
        if (new_name && strlen(new_name) > 0) {
            gchar *old_name;
            gtk_tree_model_get(model, &iter, SESSION_COL_SESSION_NAME, &old_name, -1);

            // Prevent renaming the "Default" session
            if (strcmp(old_name, "Default") == 0) {
                GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(app.window),
                    GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                    GTK_MESSAGE_INFO,
                    GTK_BUTTONS_OK,
                    "The 'Default' session cannot be renamed.");
                gtk_dialog_run(GTK_DIALOG(dialog));
                gtk_widget_destroy(dialog);
                g_free(old_name);
                return;
            }

            // Check if new name already exists (excluding the current session)
            const GList *existing_sessions = sessions_model_get_session_names(app.sessions_model);
            for (const GList *iter_check = existing_sessions; iter_check != NULL; iter_check = iter_check->next) {
                if (strcmp((const char*)iter_check->data, old_name) != 0 &&
                    strcmp((const char*)iter_check->data, new_name) == 0) {
                    // Show error dialog
                    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(app.window),
                        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                        GTK_MESSAGE_ERROR,
                        GTK_BUTTONS_OK,
                        "A session with the name '%s' already exists. Please choose a different name.",
                        new_name);
                    gtk_dialog_run(GTK_DIALOG(dialog));
                    gtk_widget_destroy(dialog);
                    g_free(old_name);
                    return;
                }
            }

            // Migrate the session model in the hash table to the new key
            session_model_t *session_model = g_hash_table_lookup(app.session_models, old_name);
            if (session_model) {
                session_model_set_session_name(session_model, new_name);
                g_hash_table_steal(app.session_models, old_name);
                g_hash_table_insert(app.session_models, g_strdup(new_name), session_model);

                // Re-key all document_models entries that embed the old session name
                if (app.document_models) {
                    GList *keys = g_hash_table_get_keys(app.document_models);
                    GList *to_rekey = NULL;
                    int old_len = strlen(old_name);
                    for (GList *k = keys; k; k = k->next) {
                        char *key = (char *)k->data;
                        if (strncmp(key, old_name, old_len) == 0 && key[old_len] == ':') {
                            to_rekey = g_list_prepend(to_rekey, key);
                        }
                    }
                    g_list_free(keys);
                    for (GList *k = to_rekey; k; k = k->next) {
                        char *old_key = (char *)k->data;
                        document_model_t *doc = g_hash_table_lookup(app.document_models, old_key);
                        if (doc) {
                            char *new_key = g_strdup_printf("%s%s", new_name, old_key + old_len);
                            g_hash_table_steal(app.document_models, old_key);
                            g_hash_table_insert(app.document_models, new_key, doc);
                            g_free(old_key);
                        }
                    }
                    g_list_free(to_rekey);
                }
            }

            // Update in sessions_model
            sessions_model_remove_session_name(app.sessions_model, old_name);
            sessions_model_add_session_name(app.sessions_model, new_name);

            // Update current_selected_session BEFORE tree repopulation so the
            // cursor-changed handler sees it matches and avoids switch_to_session.
            gboolean was_current = (app.current_selected_session &&
                                    strcmp(app.current_selected_session, old_name) == 0);
            if (was_current) {
                g_free(app.current_selected_session);
                app.current_selected_session = g_strdup(new_name);
            }

            // Update tree view
            populate_sessions_treeview();

            // Re-select the renamed session in the tree
            app.sessions_tree_syncing = TRUE;
            {
                GtkTreeIter si;
                if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(app.sessions_tree_store), &si)) {
                    do {
                        char *sn = NULL;
                        gtk_tree_model_get(GTK_TREE_MODEL(app.sessions_tree_store), &si,
                                           SESSION_COL_SESSION_NAME, &sn, -1);
                        if (sn && strcmp(sn, new_name) == 0) {
                            GtkTreeSelection *sel = gtk_tree_view_get_selection(
                                GTK_TREE_VIEW(app.sessions_tree_view));
                            GtkTreePath *path = gtk_tree_model_get_path(
                                GTK_TREE_MODEL(app.sessions_tree_store), &si);
                            gtk_tree_selection_select_path(sel, path);
                            gtk_tree_path_free(path);
                            g_free(sn);
                            break;
                        }
                        g_free(sn);
                    } while (gtk_tree_model_iter_next(
                        GTK_TREE_MODEL(app.sessions_tree_store), &si));
                }
            }
            app.sessions_tree_syncing = FALSE;

            // Update window title for the renamed session
            if (was_current) {
                if (app.sessions_model) {
                    sessions_model_set_last_open_session(app.sessions_model, app.current_selected_session);
                }
                update_window_title_for_session(app.current_selected_session);
            }

            // Clear entry
            gtk_entry_set_text(GTK_ENTRY(app.sessions_entry), "");

            // Persist the rename
            save_state();

            g_free(old_name);
        }
    }
}

static void set_right_notebook_session(const gchar *session_name) {
    if (!app.right_notebook || !session_name || !app.session_models) return;

    app.is_restoring_session_tabs = TRUE;

    // Clear current right notebook
    while (gtk_notebook_get_n_pages(GTK_NOTEBOOK(app.right_notebook)) > 0) {
        gtk_notebook_remove_page(GTK_NOTEBOOK(app.right_notebook), 0);
    }
    gtk_widget_hide(app.right_page_nav_overlay);

    // Get the session model
    session_model_t *session = g_hash_table_lookup(app.session_models, session_name);
    if (!session) {
        app.is_restoring_session_tabs = FALSE;
        return;
    }

    const char *last_read_help_uri = session_model_get_last_read_help_document(session);

    // Restore saved helper documents in right notebook
    const GList *helper_docs = session_model_get_helper_document_urls(session);
    int matched_index = -1;
    int index = 0;
    for (const GList *iter = helper_docs; iter != NULL; iter = iter->next, index++) {
        const char *uri = (const char*)iter->data;
        char *filename = g_filename_from_uri(uri, NULL, NULL);
        if (filename) {
            TabData *tab = create_new_tab(app.right_notebook);
            if (tab) {
                load_file_into_tab(tab, filename);
                if (last_read_help_uri && g_strcmp0(uri, last_read_help_uri) == 0) {
                    matched_index = index;
                }
            }
            g_free(filename);
        }
    }

    // Re-focus last-read helper tab
    if (matched_index >= 0 && matched_index < gtk_notebook_get_n_pages(GTK_NOTEBOOK(app.right_notebook))) {
        gtk_notebook_set_current_page(GTK_NOTEBOOK(app.right_notebook), matched_index);
    }

    // If no helper documents, show an empty right notebook
    if (gtk_notebook_get_n_pages(GTK_NOTEBOOK(app.right_notebook)) == 0) {
        gtk_widget_show_all(app.right_notebook);
    }

    app.is_restoring_session_tabs = FALSE;
}

void save_open_tabs_for_session(const char *session_name) {
    if (!session_name || !app.session_models) return;

    session_model_t *session = g_hash_table_lookup(app.session_models, session_name);
    if (!session) return;

    // Replace current saved document URLs with the currently open tabs.
    if (session->document_urls) {
        g_list_free_full(session->document_urls, g_free);
        session->document_urls = NULL;
    }
    if (session->helper_document_urls) {
        g_list_free_full(session->helper_document_urls, g_free);
        session->helper_document_urls = NULL;
    }

    // Save open tabs from left notebook
    if (app.left_notebook) {
        int n_pages = gtk_notebook_get_n_pages(GTK_NOTEBOOK(app.left_notebook));
        for (int i = 0; i < n_pages; i++) {
            GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(app.left_notebook), i);
            if (page) {
                TabData *tab = g_object_get_data(G_OBJECT(page), "tab-data");
                if (tab && tab->current_file) {
                    update_document_model_from_tab(tab);
                    char *uri = g_filename_to_uri(tab->current_file, NULL, NULL);
                    if (uri) {
                        session_model_add_document_url(session, uri);
                        g_free(uri);
                    }
                }
            }
        }
    }

    // Save open tabs from right notebook
    if (app.right_notebook) {
        int n_pages = gtk_notebook_get_n_pages(GTK_NOTEBOOK(app.right_notebook));
        for (int i = 0; i < n_pages; i++) {
            GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(app.right_notebook), i);
            if (page) {
                TabData *tab = g_object_get_data(G_OBJECT(page), "tab-data");
                if (tab && tab->current_file) {
                    update_document_model_from_tab(tab);
                    char *uri = g_filename_to_uri(tab->current_file, NULL, NULL);
                    if (uri) {
                        session_model_add_helper_document_url(session, uri);
                        g_free(uri);
                    }
                }
            }
        }
    }

    /* Also snapshot the currently focused tabs as last-read markers */
    if (app.left_notebook) {
        int cur = gtk_notebook_get_current_page(GTK_NOTEBOOK(app.left_notebook));
        if (cur >= 0) {
            GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(app.left_notebook), cur);
            if (page) {
                update_last_read_for_notebook(GTK_NOTEBOOK(app.left_notebook), page, (guint)cur);
            }
        } else {
            session_model_set_last_read_document(session, "");
        }
    }

    if (app.right_notebook) {
        int cur = gtk_notebook_get_current_page(GTK_NOTEBOOK(app.right_notebook));
        if (cur >= 0) {
            GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(app.right_notebook), cur);
            if (page) {
                update_last_read_for_notebook(GTK_NOTEBOOK(app.right_notebook), page, (guint)cur);
            }
        } else {
            session_model_set_last_read_help_document(session, "");
        }
    }
}

void restore_open_tabs_for_session(const char *session_name) {
    if (!session_name || !app.session_models) return;

    session_model_t *session = g_hash_table_lookup(app.session_models, session_name);
    if (!session) {
        session = session_model_new();
        session_model_set_session_name(session, session_name);
        g_hash_table_insert(app.session_models, g_strdup(session_name), session);
    }

    app.is_restoring_session_tabs = TRUE;

    // Clear current notebooks
    if (app.left_notebook) {
        int n_pages = gtk_notebook_get_n_pages(GTK_NOTEBOOK(app.left_notebook));
        for (int i = n_pages - 1; i >= 0; i--) {
            gtk_notebook_remove_page(GTK_NOTEBOOK(app.left_notebook), i);
        }
    }

    if (app.right_notebook) {
        int n_pages = gtk_notebook_get_n_pages(GTK_NOTEBOOK(app.right_notebook));
        for (int i = n_pages - 1; i >= 0; i--) {
            gtk_notebook_remove_page(GTK_NOTEBOOK(app.right_notebook), i);
        }
    }

    const char *last_read_uri = session_model_get_last_read_document(session);
    const char *last_read_help_uri = session_model_get_last_read_help_document(session);

    int matched_left_index = -1;
    int matched_right_index = -1;
    int left_index = 0;
    int right_index = 0;

    // Pass 1: create placeholder tabs for all saved documents (fast, no parsing).
    // The focused (last-read) tabs are loaded synchronously in pass 2 so the user
    // immediately sees content; the remaining tabs are opened progressively at idle
    // priority so switching sessions does not freeze the UI on large document sets.
    const GList *docs = session_model_get_document_urls(session);
    for (const GList *iter = docs; iter != NULL; iter = iter->next) {
        char *uri = (char *)iter->data;
        char *filename = g_filename_from_uri(uri, NULL, NULL);
        if (!filename && uri && g_path_is_absolute(uri)) {
            filename = g_strdup(uri);
        }

        if (filename) {
            TabData *tab = create_new_tab(app.left_notebook);
            if (tab) {
                set_tab_filename(tab, filename);
                if (last_read_uri && g_strcmp0(uri, last_read_uri) == 0) {
                    matched_left_index = left_index;
                }
                left_index++;
            }
            g_free(filename);
        } else {
            g_warning("Failed to restore saved document path: %s", uri ? uri : "(null)");
        }
    }

    // Restore saved helper documents in right notebook
    const GList *helper_docs = session_model_get_helper_document_urls(session);
    for (const GList *iter = helper_docs; iter != NULL; iter = iter->next) {
        char *uri = (char *)iter->data;
        char *filename = g_filename_from_uri(uri, NULL, NULL);
        if (!filename && uri && g_path_is_absolute(uri)) {
            filename = g_strdup(uri);
        }
        if (filename) {
            TabData *tab = create_new_tab(app.right_notebook);
            if (tab) {
                set_tab_filename(tab, filename);
                if (last_read_help_uri && g_strcmp0(uri, last_read_help_uri) == 0) {
                    matched_right_index = right_index;
                }
                right_index++;
            }
            g_free(filename);
        } else {
            g_warning("Failed to restore saved document path: %s", uri ? uri : "(null)");
        }
    }

    app.is_restoring_session_tabs = FALSE;

    int n_left = app.left_notebook ? gtk_notebook_get_n_pages(GTK_NOTEBOOK(app.left_notebook)) : 0;
    int n_right = app.right_notebook ? gtk_notebook_get_n_pages(GTK_NOTEBOOK(app.right_notebook)) : 0;

    int focus_left = (matched_left_index >= 0) ? matched_left_index : (n_left > 0 ? 0 : -1);
    int focus_right = (matched_right_index >= 0) ? matched_right_index : (n_right > 0 ? 0 : -1);

    // Pass 2: synchronously load the focused tabs so content appears immediately;
    // defer all other tabs to idle-time progressive loading.
    for (int i = 0; i < n_left; i++) {
        GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(app.left_notebook), i);
        TabData *t = page ? g_object_get_data(G_OBJECT(page), "tab-data") : NULL;
        if (!t) continue;
        if (i == focus_left)
            open_document_in_tab(t);
        else
            schedule_tab_deferred_load(t);
    }

    for (int i = 0; i < n_right; i++) {
        GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(app.right_notebook), i);
        TabData *t = page ? g_object_get_data(G_OBJECT(page), "tab-data") : NULL;
        if (!t) continue;
        if (i == focus_right)
            open_document_in_tab(t);
        else
            schedule_tab_deferred_load(t);
    }

    // Re-focus last-read tabs (fires the notebook switch handler with loaded docs)
    if (app.left_notebook && focus_left >= 0 && focus_left < n_left) {
        gtk_notebook_set_current_page(GTK_NOTEBOOK(app.left_notebook), focus_left);
    }

    if (app.right_notebook && focus_right >= 0 && focus_right < n_right) {
        gtk_notebook_set_current_page(GTK_NOTEBOOK(app.right_notebook), focus_right);
    }

    /* Refresh sidebar state for the restored current tabs. The switch handler
       above may not fire if the focused tab was already current, so do it here. */
    if (app.current_sidebar_mode == SIDEBAR_TOC) populate_toc_treeview();
    if (app.current_sidebar_mode == SIDEBAR_FILE_INFO) update_file_info_labels(get_current_left_tab());
}

static gboolean deferred_update_document_model(gpointer data) {
    TabData *tab = data;
    tab->scroll_doc_debounce_id = 0;
    if (!tab || !tab->cached_page_widths) return G_SOURCE_REMOVE;
    update_document_model_from_tab(tab);
    if (app.current_sidebar_mode == SIDEBAR_TOC) {
        update_toc_selection_for_current_page(tab);
    }
    return G_SOURCE_REMOVE;
}

void cancel_doc_model_debounce(TabData *tab) {
    if (tab && tab->scroll_doc_debounce_id) {
        g_source_remove(tab->scroll_doc_debounce_id);
        tab->scroll_doc_debounce_id = 0;
    }
}

static void schedule_doc_model_update(TabData *tab) {
    if (!tab) return;
    cancel_doc_model_debounce(tab);
    tab->scroll_doc_debounce_id = g_timeout_add(400, deferred_update_document_model, tab);
}

static void on_scroll_value_changed(GtkAdjustment *adj, gpointer user_data) {
    TabData *tab = user_data;
    if (!tab || !tab->cached_page_widths) return;

    if (tab->initial_scroll_pending) {
        return;
    }

    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(tab->scrolled));

    if (tab->layout_mode == 2) {
        if (adj == vadj) return;
        GtkAdjustment *sadj = tab->h_scrollbar ? gtk_range_get_adjustment(GTK_RANGE(tab->h_scrollbar)) : NULL;
        if (adj != sadj) return;
        double scroll_x = gtk_adjustment_get_value(adj);
        const double spacing = 6.0;
        double scale = get_ppi_scale(tab);

        double upper = gtk_adjustment_get_upper(adj);
        double page_size = gtk_adjustment_get_page_size(adj);
        if (page_size > 0 && tab->n_pages > 0 && scroll_x >= (upper - page_size - 1.0)) {
            tab->cur_page = tab->n_pages - 1;
            if (tab == get_current_left_tab()) {
                sync_page_widget_from_tab(tab);
            }
            if (tab == get_current_right_tab()) {
                sync_right_page_widget_from_tab(tab);
            }
            schedule_doc_model_update(tab);
            gtk_widget_queue_draw(tab->pages_drawing);
            return;
        }

        double x = spacing;
        int visible_page = (tab->n_pages > 0) ? (tab->n_pages - 1) : 0;
        for (int i = 0; i < tab->n_pages; ++i) {
            double page_w = tab->cached_page_widths[i] * scale;
            if (x + page_w > scroll_x) {
                visible_page = i;
                break;
            }
            x += page_w + spacing;
        }

        tab->cur_page = visible_page;
        if (tab == get_current_left_tab()) {
            sync_page_widget_from_tab(tab);
        }
        if (tab == get_current_right_tab()) {
            sync_right_page_widget_from_tab(tab);
        }
        schedule_doc_model_update(tab);
        gtk_widget_queue_draw(tab->pages_drawing);
        return;
    }

    if (adj != vadj) return;

    double scroll_y = gtk_adjustment_get_value(adj);
    const double spacing = 6.0;
    double scale = get_ppi_scale(tab);

    double upper = gtk_adjustment_get_upper(adj);
    double page_size = gtk_adjustment_get_page_size(adj);
    if (tab->n_pages > 0 && scroll_y >= (upper - page_size - 1.0)) {
        tab->cur_page = tab->n_pages - 1;
        if (tab == get_current_left_tab()) {
            sync_page_widget_from_tab(tab);
        }
        if (tab == get_current_right_tab()) {
            sync_right_page_widget_from_tab(tab);
        }
        schedule_doc_model_update(tab);
        return;
    }

    double y = spacing;
    int visible_page = (tab->n_pages > 0) ? (tab->n_pages - 1) : 0;

    if (tab->layout_mode == 0) {
        for (int i = 0; i < tab->n_pages; ++i) {
            double page_h = tab->cached_page_heights[i] * scale;
            if (y + page_h > scroll_y) {
                visible_page = i;
                break;
            }
            y += page_h + spacing;
        }
    } else if (tab->layout_mode == 1) {
        for (int i = 0; i < tab->n_pages; i += 2) {
            double row_h = 0.0;
            double h1 = tab->cached_page_heights[i] * scale;
            if (h1 > row_h) row_h = h1;
            if (i + 1 < tab->n_pages) {
                double h2 = tab->cached_page_heights[i + 1] * scale;
                if (h2 > row_h) row_h = h2;
            }
            if (row_h < 1.0) row_h = 1.0;

            if (y + row_h > scroll_y) {
                visible_page = i;
                break;
            }
            y += row_h + spacing;
        }
    }

    tab->cur_page = visible_page;

    if (tab == get_current_left_tab()) {
        sync_page_widget_from_tab(tab);
    }
    if (tab == get_current_right_tab()) {
        sync_right_page_widget_from_tab(tab);
    }

    schedule_doc_model_update(tab);
}

static void on_tab_scrolled_size_allocate(GtkWidget *widget, GdkRectangle *allocation, gpointer user_data) {
    (void)widget;
    (void)allocation;
    TabData *tab = user_data;
    if (!tab) return;

    /* Ensure doc is loaded for the active tab that owns this size-allocate */
    if (!tab->cached_page_widths) {
        if (!app.is_restoring_session_tabs && !ensure_tab_doc_loaded(tab)) return;
    }

    double zoom = tab->zoom > 0 ? tab->zoom : 96.0;
    if (zoom != tab->last_zoom) {
        tab->last_zoom = zoom;
        build_continuous_view(tab);
    }

    /* For row view (mode 2), the size_request width is clamped to page_width_px
       to prevent the window from growing.  GTK's internal handler set the
       hadjustment upper from the clamped size_request, so we must extend it
       here to the full total_w BEFORE scroll_to_page runs below.  Recompute
       total_w and max_h from the page cache so the scroll range and drawing
       height are correct even if the last build used a different layout. */
    if (tab->layout_mode == 2 && tab->cached_page_widths && tab->n_pages > 0 && tab->h_scrollbar) {
        int vp_w = allocation ? allocation->width : 200;
        int vp_h = allocation ? allocation->height : 200;
        double scale = get_ppi_scale(tab);
        double total_w = 0.0;
        double max_h = 0.0;
        const double spacing = 6.0;
        for (int i = 0; i < tab->n_pages; ++i) {
            double page_w = tab->cached_page_widths[i] * scale;
            double page_h = tab->cached_page_heights[i] * scale;
            total_w += page_w + spacing;
            if (page_h > max_h) max_h = page_h;
        }
        if (total_w < 1.0) total_w = 1.0;
        if (max_h < 1.0) max_h = 1.0;
        tab->max_page_h = max_h;
        GtkAdjustment *sadj = gtk_range_get_adjustment(GTK_RANGE(tab->h_scrollbar));
        gtk_adjustment_set_page_size(sadj, vp_w > 0 ? vp_w : 200);
        gtk_adjustment_set_step_increment(sadj, vp_w > 0 ? vp_w * 0.1 : 20);
        gtk_adjustment_set_page_increment(sadj, vp_w * 0.9);
        gtk_adjustment_set_lower(sadj, 0.0);
        gtk_adjustment_set_upper(sadj, total_w);
        double target_h = MAX(vp_h, tab->max_page_h);
        gtk_widget_set_size_request(tab->pages_drawing, -1, clamp_double_to_int(ceil(target_h), MAX_SIZE_REQUEST));
    }

    if (tab->initial_scroll_pending) {
        scroll_to_page(tab, tab->cur_page, -1);
        tab->initial_scroll_pending = FALSE;
    }
}

static gboolean auto_hide_h_scrollbar(gpointer data) {
    TabData *tab = data;
    tab->h_scrollbar_timer_id = 0;
    if (tab->h_scrollbar)
        gtk_widget_hide(tab->h_scrollbar);
    return G_SOURCE_REMOVE;
}

static void cancel_h_scrollbar_timer(TabData *tab) {
    if (tab->h_scrollbar_timer_id) {
        g_source_remove(tab->h_scrollbar_timer_id);
        tab->h_scrollbar_timer_id = 0;
    }
}

static void show_h_scrollbar_temporarily(TabData *tab) {
    if (!tab->h_scrollbar) return;
    cancel_h_scrollbar_timer(tab);
    gtk_widget_show(tab->h_scrollbar);
    tab->h_scrollbar_timer_id = g_timeout_add(2000, auto_hide_h_scrollbar, tab);
}

static void show_h_scrollbar(TabData *tab) {
    if (!tab->h_scrollbar) return;
    cancel_h_scrollbar_timer(tab);
    gtk_widget_show(tab->h_scrollbar);
}

static gboolean on_h_scrollbar_enter(GtkWidget *w, GdkEvent *e, gpointer user_data) {
    (void)w; (void)e;
    TabData *tab = user_data;
    if (!tab->h_scrollbar) return FALSE;
    cancel_h_scrollbar_timer(tab);
    return FALSE;
}

static gboolean on_h_scrollbar_leave(GtkWidget *w, GdkEvent *e, gpointer user_data) {
    (void)w; (void)e;
    TabData *tab = user_data;
    if (!tab->h_scrollbar) return FALSE;
    cancel_h_scrollbar_timer(tab);
    tab->h_scrollbar_timer_id = g_timeout_add(500, auto_hide_h_scrollbar, tab);
    return FALSE;
}


/* Convert widget coordinates to page index and page-relative rendering-space
   (points, y-down, 0 at top of page — matches MuPDF's pixmap orientation).
   Returns 0-based page index, or -1 if no page at (wx, wy). */
static int widget_to_page_coords(TabData *tab, double wx, double wy,
                                  double *out_px, double *out_py) {
    if (!tab || !tab->cached_page_widths || !tab->pages_drawing) return -1;
    GtkAllocation alloc;
    gtk_widget_get_allocation(tab->pages_drawing, &alloc);
    double scale = get_ppi_scale(tab);
    const double spacing = 6.0;

    if (tab->layout_mode == 0) {
        double y = spacing;
        for (int i = 0; i < tab->n_pages; i++) {
            double pw = tab->cached_page_widths[i] * scale;
            double ph = tab->cached_page_heights[i] * scale;
            double ox = (alloc.width - pw) / 2.0;
            if (wx >= ox && wx < ox + pw && wy >= y && wy < y + ph) {
                if (out_px) *out_px = (wx - ox) / scale;
                if (out_py) *out_py = (wy - y) / scale;
                return i;
            }
            y += ph + spacing;
        }
    } else if (tab->layout_mode == 1) {
        double y = spacing;
        for (int i = 0; i < tab->n_pages; i += 2) {
            double pw1 = tab->cached_page_widths[i] * scale;
            double ph1 = tab->cached_page_heights[i] * scale;
            double pw2 = 0, ph2 = 0;
            if (i + 1 < tab->n_pages) {
                pw2 = tab->cached_page_widths[i + 1] * scale;
                ph2 = tab->cached_page_heights[i + 1] * scale;
            }
            double rw = pw1 + (pw2 > 0 ? spacing + pw2 : 0);
            double rh = ph1;
            if (ph2 > rh) rh = ph2;
            double rx = (alloc.width - rw) / 2.0;
            if (rx < spacing) rx = spacing;
            double lx = rx;
            double rx2 = lx + pw1 + spacing;
            if (wx >= lx && wx < lx + pw1 && wy >= y && wy < y + ph1) {
                if (out_px) *out_px = (wx - lx) / scale;
                if (out_py) *out_py = (wy - y) / scale;
                return i;
            }
            if (pw2 > 0 && wx >= rx2 && wx < rx2 + pw2 && wy >= y && wy < y + ph2) {
                if (out_px) *out_px = (wx - rx2) / scale;
                if (out_py) *out_py = (wy - y) / scale;
                return i + 1;
            }
            y += rh + spacing;
        }
    } else if (tab->layout_mode == 2) {
        double scroll_x = 0.0;
        if (tab->h_scrollbar) {
            GtkAdjustment *sadj = gtk_range_get_adjustment(GTK_RANGE(tab->h_scrollbar));
            scroll_x = gtk_adjustment_get_value(sadj);
        }
        double x = spacing - scroll_x;
        GtkAdjustment *vadj_row = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(tab->scrolled));
        double viewport_h = vadj_row ? gtk_adjustment_get_page_size(vadj_row) : 0.0;
        for (int i = 0; i < tab->n_pages; i++) {
            double pw = tab->cached_page_widths[i] * scale;
            double ph = tab->cached_page_heights[i] * scale;
            double oy = tab->max_page_h > viewport_h ? 0.0 : (alloc.height - ph) / 2.0;
            if (wx >= x && wx < x + pw && wy >= oy && wy < oy + ph) {
                if (out_px) *out_px = (wx - x) / scale;
                if (out_py) *out_py = (wy - oy) / scale;
                return i;
            }
            x += pw + spacing;
        }
    }
    return -1;
}


static gboolean on_drawing_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data) {
    TabData *tab = user_data;
    if (!tab || event->button != GDK_BUTTON_PRIMARY) return FALSE;
    tab->dragging = TRUE;
    tab->drag_start_x = event->x;
    tab->drag_start_y = event->y;
    GtkAdjustment *hadj_main = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(tab->scrolled));
    tab->drag_scroll_x = gtk_adjustment_get_value(hadj_main);
    if (tab->layout_mode == 2 && tab->h_scrollbar) {
        GtkAdjustment *sadj = gtk_range_get_adjustment(GTK_RANGE(tab->h_scrollbar));
        tab->drag_scroll_x = gtk_adjustment_get_value(sadj);
    }
    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(tab->scrolled));
    tab->drag_scroll_y = gtk_adjustment_get_value(vadj);
    GdkCursor *cursor = gdk_cursor_new_for_display(gtk_widget_get_display(widget), GDK_FLEUR);
    gdk_window_set_cursor(gtk_widget_get_window(widget), cursor);
    g_object_unref(cursor);
    return TRUE;
}

static gboolean on_drawing_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data) {
    TabData *tab = user_data;
    if (!tab || event->button != GDK_BUTTON_PRIMARY) return FALSE;
    gboolean was_dragging = tab->dragging;
    tab->dragging = FALSE;

    /* If the mouse barely moved, treat as a click — check for links */
    if (was_dragging) {
        double dx = event->x - tab->drag_start_x;
        double dy = event->y - tab->drag_start_y;
        if (dx * dx + dy * dy < 25.0) { /* 5px threshold */
            int page;
            double px, py;
            page = widget_to_page_coords(tab, event->x, event->y, &px, &py);
            if (page >= 0) {
                ensure_page_links_loaded(tab, page);
                activate_link_at(tab, page, px, py);
            }
        }
    }

    if (tab->last_cursor_type != GDK_LEFT_PTR) {
        GdkCursor *cursor = gdk_cursor_new_for_display(gtk_widget_get_display(widget), GDK_LEFT_PTR);
        gdk_window_set_cursor(gtk_widget_get_window(widget), cursor);
        g_object_unref(cursor);
        tab->last_cursor_type = GDK_LEFT_PTR;
    }
    return TRUE;
}

static gboolean on_drawing_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data) {
    TabData *tab = user_data;
    if (!tab) return FALSE;

    if (tab->dragging) {
        double dy = tab->drag_start_y - event->y;
        double dx = tab->drag_start_x - event->x;
        GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(tab->scrolled));
        gtk_adjustment_set_value(vadj, tab->drag_scroll_y + dy);
        if (tab->layout_mode == 2 && tab->h_scrollbar) {
            GtkAdjustment *sadj = gtk_range_get_adjustment(GTK_RANGE(tab->h_scrollbar));
            gtk_adjustment_set_value(sadj, tab->drag_scroll_x + dx);
        } else {
            GtkAdjustment *hadj = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(tab->scrolled));
            gtk_adjustment_set_value(hadj, tab->drag_scroll_x + dx);
        }
        return TRUE;
    }

    /* Throttle link cursor check to ~10 Hz to avoid O(n) loop on every motion */
    GdkCursorType cursor_type = GDK_LEFT_PTR;
    gint64 now = g_get_monotonic_time();
    if (now - tab->last_cursor_check > 100000) { /* 100ms */
        tab->last_cursor_check = now;
        int page;
        double px, py;
        page = widget_to_page_coords(tab, event->x, event->y, &px, &py);
        if (page >= 0) {
            ensure_page_links_loaded(tab, page);
            if (has_link_at(tab, page, px, py)) {
                cursor_type = GDK_HAND2;
            }
        }
    } else {
        /* Use current cursor type — it won't change between throttled checks */
        cursor_type = tab->last_cursor_type;
    }

    /* Only call into GDK/X11 when cursor type actually changes */
    if (cursor_type != tab->last_cursor_type) {
        GdkCursor *cursor = gdk_cursor_new_for_display(gtk_widget_get_display(widget), cursor_type);
        gdk_window_set_cursor(gtk_widget_get_window(widget), cursor);
        g_object_unref(cursor);
        tab->last_cursor_type = cursor_type;
    }

    if (tab->layout_mode != 2 || !tab->h_scrollbar) return FALSE;
    GtkAllocation alloc;
    gtk_widget_get_allocation(widget, &alloc);
    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(tab->scrolled));
    double scroll_y = gtk_adjustment_get_value(vadj);
    double page_size = gtk_adjustment_get_page_size(vadj);
    double visible_bottom = scroll_y + page_size;
    if (visible_bottom > alloc.height) visible_bottom = alloc.height;
    int bottom_zone = 30;
    if (event->y >= visible_bottom - bottom_zone) {
        show_h_scrollbar(tab);
    } else if (gtk_widget_get_visible(tab->h_scrollbar) && !tab->h_scrollbar_timer_id) {
        tab->h_scrollbar_timer_id = g_timeout_add(500, auto_hide_h_scrollbar, tab);
    }
    return FALSE;
}

static gboolean on_drawing_leave(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data) {
    (void)widget; (void)event;
    TabData *tab = user_data;
    if (!tab || tab->layout_mode != 2 || !tab->h_scrollbar) return FALSE;
    if (gtk_widget_get_visible(tab->h_scrollbar) && !tab->h_scrollbar_timer_id) {
        tab->h_scrollbar_timer_id = g_timeout_add(500, auto_hide_h_scrollbar, tab);
    }
    return FALSE;
}

static gboolean on_drawing_scroll(GtkWidget *widget, GdkEventScroll *event, gpointer user_data) {
    (void)widget;
    TabData *tab = user_data;
    if (!tab || tab->layout_mode != 2 || !tab->h_scrollbar) return FALSE;
    show_h_scrollbar_temporarily(tab);
    GtkAdjustment *adj = gtk_range_get_adjustment(GTK_RANGE(tab->h_scrollbar));
    double val = gtk_adjustment_get_value(adj);
    double step = gtk_adjustment_get_step_increment(adj);
    if (step < 1.0) step = 40.0;
    if (event->direction == GDK_SCROLL_SMOOTH) {
        double dx, dy;
        gdk_event_get_scroll_deltas((GdkEvent*)event, &dx, &dy);
        double delta = fabs(dx) >= fabs(dy) ? dx : dy;
        val += delta * step;
    } else if (event->direction == GDK_SCROLL_RIGHT || event->direction == GDK_SCROLL_DOWN) {
        val += step;
    } else if (event->direction == GDK_SCROLL_LEFT || event->direction == GDK_SCROLL_UP) {
        val -= step;
    }
    gtk_adjustment_set_value(adj, val);
    return TRUE;
}

static gboolean on_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data) {
    TabData *tab = user_data;
    GtkAllocation alloc;
    gtk_widget_get_allocation(widget, &alloc);

    if (!tab || !tab->cached_page_widths) {
        return FALSE;
    }

    MEM_INIT_DRAW();

    /* continuous mode: draw multiple pages vertically inside this drawing area
       Render only pages intersecting the current clip extents to save work. */
    double clip_x1, clip_y1, clip_x2, clip_y2;
    cairo_clip_extents(cr, &clip_x1, &clip_y1, &clip_x2, &clip_y2);

    const double spacing = 6.0;
    double scale = get_ppi_scale(tab);
    int first_visible = -1, last_visible = -1;
    if (tab->layout_mode == 0) {
        double y = spacing;
        double dsx, dsy;
        cairo_surface_get_device_scale(cairo_get_target(cr), &dsx, &dsy);
        cairo_font_options_t *fo = cairo_font_options_create();
        cairo_get_font_options(cr, fo);
        for (int i = 0; i < tab->n_pages; ++i) {
            double page_w = tab->cached_page_widths[i] * scale;
            double page_h = tab->cached_page_heights[i] * scale;
            double off_x = (alloc.width - page_w) / 2.0;
            double off_y = y;

            /* skip if page is outside clip */
            if (!(off_y + page_h < clip_y1 || off_y > clip_y2)) {
                /* draw background rectangle */
                cairo_save(cr);
                cairo_set_source_rgba(cr, tab->page_color.red, tab->page_color.green, tab->page_color.blue, tab->page_color.alpha);
                cairo_rectangle(cr, off_x, off_y, page_w, page_h);
                cairo_fill(cr);
                cairo_restore(cr);
                int iw = clamp_double_to_int(tab->cached_page_widths[i] * scale * dsx + 0.5, MAX_SURFACE_DIM);
                int ih = clamp_double_to_int(tab->cached_page_heights[i] * scale * dsy + 0.5, MAX_SURFACE_DIM);
                if (iw > 0 && ih > 0) {
                    if (tab->page_cache[i]) {
                        int cw = cairo_image_surface_get_width(tab->page_cache[i]);
                        int ch = cairo_image_surface_get_height(tab->page_cache[i]);
                        if (cw != iw || ch != ih)
                            cache_evict_idx(tab, i);
                    }
                    if (tab->page_cache[i]) {
                        cairo_set_source_surface(cr, tab->page_cache[i], off_x, off_y);
                        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
                        cairo_paint(cr);
                    } else {
                        PdfrPage *page = pdfr_load_page(tab->doc, i);
                        if (page) {
                            cairo_surface_t *pimg = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, iw, ih);
                            cairo_surface_set_device_scale(pimg, dsx, dsy);
                            cairo_t *picr = cairo_create(pimg);
                            cairo_set_font_options(picr, fo);
                            cairo_set_antialias(picr, CAIRO_ANTIALIAS_BEST);
                            cairo_scale(picr, scale, scale);
                            pdfr_render(tab->doc, page, picr);
                            cairo_destroy(picr);
                            pdfr_free_page(tab->doc, page);
                            cairo_set_source_surface(cr, pimg, off_x, off_y);
                            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
                            cairo_paint(cr);
                            /* Cache only if within byte budget */
                            int new_bytes = iw * ih * 4;
                            MEM_SURFACE_CREATED(new_bytes);
                            if (tab->total_cache_bytes + new_bytes <= MAX_CACHE_BYTES) {
                                tab->page_cache[i] = pimg;
                                tab->total_cache_bytes += new_bytes;
                            } else {
                                cairo_surface_destroy(pimg);
                                MEM_SURFACE_DESTROYED();
                            }
                        }
                    }
                    if (first_visible == -1) first_visible = i;
                    last_visible = i;
                }
                search_highlight_page(tab, cr, i + 1, off_x, off_y, scale);
            }

            y += page_h + spacing;
        }
        cairo_font_options_destroy(fo);

    } else if (tab->layout_mode == 1) {
        int n = tab->n_pages;
        double y = spacing;
        double dsx2, dsy2;
        cairo_surface_get_device_scale(cairo_get_target(cr), &dsx2, &dsy2);
        cairo_font_options_t *fo2 = cairo_font_options_create();
        cairo_get_font_options(cr, fo2);
        for (int i = 0; i < n; i += 2) {
            /* left page dims */
            double page_w1 = tab->cached_page_widths[i] * scale;
            double page_h1 = tab->cached_page_heights[i] * scale;
            double row_w = page_w1;
            double row_h = page_h1;

            /* right page dims */
            double page_w2 = 0, page_h2 = 0;
            if (i + 1 < n) {
                page_w2 = tab->cached_page_widths[i + 1] * scale;
                page_h2 = tab->cached_page_heights[i + 1] * scale;
            }
            if (page_w2 > 0) row_w += spacing + page_w2;
            if (page_h2 > row_h) row_h = page_h2;
            if (row_h < 1.0) row_h = 1.0;

            /* center the row horizontally within the drawing area */
            double row_x = (alloc.width - row_w) / 2.0;
            if (row_x < spacing) row_x = spacing;

            double left_x = row_x;
            double right_x = left_x + page_w1 + spacing;

            /* draw left page if visible */
            if (page_h1 > 0 && !(y + page_h1 < clip_y1 || y > clip_y2)) {
                cairo_save(cr);
                cairo_set_source_rgba(cr, tab->page_color.red, tab->page_color.green, tab->page_color.blue, tab->page_color.alpha);
                cairo_rectangle(cr, left_x, y, page_w1, page_h1);
                cairo_fill(cr);
                cairo_restore(cr);
                int iw1 = clamp_double_to_int(tab->cached_page_widths[i] * scale * dsx2 + 0.5, MAX_SURFACE_DIM);
                int ih1 = clamp_double_to_int(tab->cached_page_heights[i] * scale * dsy2 + 0.5, MAX_SURFACE_DIM);
                if (iw1 > 0 && ih1 > 0) {
                    if (tab->page_cache[i]) {
                        int cw = cairo_image_surface_get_width(tab->page_cache[i]);
                        int ch = cairo_image_surface_get_height(tab->page_cache[i]);
                        if (cw != iw1 || ch != ih1)
                            cache_evict_idx(tab, i);
                    }
                    if (tab->page_cache[i]) {
                        cairo_set_source_surface(cr, tab->page_cache[i], left_x, y);
                        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
                        cairo_paint(cr);
                    } else {
                        PdfrPage *p1 = pdfr_load_page(tab->doc, i);
                        if (p1) {
                            cairo_surface_t *pimg = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, iw1, ih1);
                            cairo_surface_set_device_scale(pimg, dsx2, dsy2);
                            cairo_t *picr = cairo_create(pimg);
                            cairo_set_font_options(picr, fo2);
                            cairo_set_antialias(picr, CAIRO_ANTIALIAS_BEST);
                            cairo_scale(picr, scale, scale);
                            pdfr_render(tab->doc, p1, picr);
                            cairo_destroy(picr);
                            pdfr_free_page(tab->doc, p1);
                            cairo_set_source_surface(cr, pimg, left_x, y);
                            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
                            cairo_paint(cr);
                            int new_bytes = iw1 * ih1 * 4;
                            MEM_SURFACE_CREATED(new_bytes);
                            if (tab->total_cache_bytes + new_bytes <= MAX_CACHE_BYTES) {
                                tab->page_cache[i] = pimg;
                                tab->total_cache_bytes += new_bytes;
                            } else {
                                cairo_surface_destroy(pimg);
                                MEM_SURFACE_DESTROYED();
                            }
                        }
                    }
                    if (first_visible == -1) first_visible = i;
                    last_visible = i;
                }
                search_highlight_page(tab, cr, i + 1, left_x, y, scale);
            }

            /* draw right page if visible */
            if (page_h2 > 0 && !(y + page_h2 < clip_y1 || y > clip_y2)) {
                cairo_save(cr);
                cairo_set_source_rgba(cr, tab->page_color.red, tab->page_color.green, tab->page_color.blue, tab->page_color.alpha);
                cairo_rectangle(cr, right_x, y, page_w2, page_h2);
                cairo_fill(cr);
                cairo_restore(cr);
                int iw2 = clamp_double_to_int(tab->cached_page_widths[i + 1] * scale * dsx2 + 0.5, MAX_SURFACE_DIM);
                int ih2 = clamp_double_to_int(tab->cached_page_heights[i + 1] * scale * dsy2 + 0.5, MAX_SURFACE_DIM);
                if (iw2 > 0 && ih2 > 0) {
                    if (tab->page_cache[i + 1]) {
                        int cw = cairo_image_surface_get_width(tab->page_cache[i + 1]);
                        int ch = cairo_image_surface_get_height(tab->page_cache[i + 1]);
                        if (cw != iw2 || ch != ih2)
                            cache_evict_idx(tab, i + 1);
                    }
                    if (tab->page_cache[i + 1]) {
                        cairo_set_source_surface(cr, tab->page_cache[i + 1], right_x, y);
                        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
                        cairo_paint(cr);
                    } else {
                        PdfrPage *p2 = pdfr_load_page(tab->doc, i + 1);
                        if (p2) {
                            cairo_surface_t *pimg = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, iw2, ih2);
                            cairo_surface_set_device_scale(pimg, dsx2, dsy2);
                            cairo_t *picr = cairo_create(pimg);
                            cairo_set_font_options(picr, fo2);
                            cairo_set_antialias(picr, CAIRO_ANTIALIAS_BEST);
                            cairo_scale(picr, scale, scale);
                            pdfr_render(tab->doc, p2, picr);
                            cairo_destroy(picr);
                            pdfr_free_page(tab->doc, p2);
                            cairo_set_source_surface(cr, pimg, right_x, y);
                            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
                            cairo_paint(cr);
                            int new_bytes = iw2 * ih2 * 4;
                            MEM_SURFACE_CREATED(new_bytes);
                            if (tab->total_cache_bytes + new_bytes <= MAX_CACHE_BYTES) {
                                tab->page_cache[i + 1] = pimg;
                                tab->total_cache_bytes += new_bytes;
                            } else {
                                cairo_surface_destroy(pimg);
                                MEM_SURFACE_DESTROYED();
                            }
                        }
                    }
                    if (first_visible == -1) first_visible = i;
                    last_visible = i;
                }
                search_highlight_page(tab, cr, i + 2, right_x, y, scale);
            }

            y += row_h + spacing;
        }
        cairo_font_options_destroy(fo2);
    } else if (tab->layout_mode == 2) {
        double scroll_x = 0.0;
        if (tab->h_scrollbar) {
            GtkAdjustment *sadj = gtk_range_get_adjustment(GTK_RANGE(tab->h_scrollbar));
            scroll_x = gtk_adjustment_get_value(sadj);
        }
        GtkAdjustment *vadj_row = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(tab->scrolled));
        double viewport_h = gtk_adjustment_get_page_size(vadj_row);
        double dsxh, dsyh;
        cairo_surface_get_device_scale(cairo_get_target(cr), &dsxh, &dsyh);
        cairo_font_options_t *foh = cairo_font_options_create();
        cairo_get_font_options(cr, foh);
        double x = spacing;
        for (int i = 0; i < tab->n_pages; ++i) {
            double page_w = tab->cached_page_widths[i] * scale;
            double page_h = tab->cached_page_heights[i] * scale;
            double dev_x = x - scroll_x;
            double off_y = tab->max_page_h > viewport_h ? 0.0 : (alloc.height - page_h) / 2.0;
            if (dev_x + page_w > 0 && dev_x < alloc.width &&
                off_y + page_h > 0 && off_y < alloc.height) {
                cairo_save(cr);
                cairo_set_source_rgba(cr, tab->page_color.red, tab->page_color.green, tab->page_color.blue, tab->page_color.alpha);
                cairo_rectangle(cr, dev_x, off_y, page_w, page_h);
                cairo_fill(cr);
                cairo_restore(cr);
                int iwh = clamp_double_to_int(tab->cached_page_widths[i] * scale * dsxh + 0.5, MAX_SURFACE_DIM);
                int ihh = clamp_double_to_int(tab->cached_page_heights[i] * scale * dsyh + 0.5, MAX_SURFACE_DIM);
                if (iwh > 0 && ihh > 0) {
                    if (tab->page_cache[i]) {
                        int cw = cairo_image_surface_get_width(tab->page_cache[i]);
                        int ch = cairo_image_surface_get_height(tab->page_cache[i]);
                        if (cw != iwh || ch != ihh)
                            cache_evict_idx(tab, i);
                    }
                    if (tab->page_cache[i]) {
                        cairo_set_source_surface(cr, tab->page_cache[i], dev_x, off_y);
                        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
                        cairo_paint(cr);
                    } else {
                        PdfrPage *page = pdfr_load_page(tab->doc, i);
                        if (page) {
                            cairo_surface_t *pimg = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, iwh, ihh);
                            cairo_surface_set_device_scale(pimg, dsxh, dsyh);
                            cairo_t *picr = cairo_create(pimg);
                            cairo_set_font_options(picr, foh);
                            cairo_set_antialias(picr, CAIRO_ANTIALIAS_BEST);
                            cairo_scale(picr, scale, scale);
                            pdfr_render(tab->doc, page, picr);
                            cairo_destroy(picr);
                            pdfr_free_page(tab->doc, page);
                            cairo_set_source_surface(cr, pimg, dev_x, off_y);
                            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
                            cairo_paint(cr);
                            int new_bytes = iwh * ihh * 4;
                            MEM_SURFACE_CREATED(new_bytes);
                            if (tab->total_cache_bytes + new_bytes <= MAX_CACHE_BYTES) {
                                tab->page_cache[i] = pimg;
                                tab->total_cache_bytes += new_bytes;
                            } else {
                                cairo_surface_destroy(pimg);
                                MEM_SURFACE_DESTROYED();
                            }
                        }
                    }
                    if (first_visible == -1) first_visible = i;
                    last_visible = i;
                }
                search_highlight_page(tab, cr, i + 1, dev_x, off_y, scale);
            }
            x += page_w + spacing;
        }
        cairo_font_options_destroy(foh);
    }

    /* Prune cache: keep only pages within margin of the visible range */
    if (first_visible >= 0 && last_visible >= 0) {
        int margin = 1;
        for (int i = 0; i < tab->n_pages; ++i) {
            if (tab->page_cache[i] && (i < first_visible - margin || i > last_visible + margin))
                cache_evict_idx(tab, i);
        }
    }

    MEM_REPORT_DRAW();
    return FALSE;
}


TabData *get_current_left_tab(void) {
    if (!app.left_notebook) return NULL;
    int idx = gtk_notebook_get_current_page(GTK_NOTEBOOK(app.left_notebook));
    if (idx < 0) return NULL;

    GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(app.left_notebook), idx);
    if (!page) return NULL;

    return g_object_get_data(G_OBJECT(page), "tab-data");
}

TabData *get_current_right_tab(void) {
    if (!app.right_notebook) return NULL;
    int idx = gtk_notebook_get_current_page(GTK_NOTEBOOK(app.right_notebook));
    if (idx < 0) return NULL;

    GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(app.right_notebook), idx);
    if (!page) return NULL;

    return g_object_get_data(G_OBJECT(page), "tab-data");
}


TabData *create_new_tab(GtkWidget *notebook) {
    TabData *tab = g_malloc0(sizeof(TabData));
    tab->zoom = 96.0;
    tab->layout_mode = 0;    /* single-column by default */
    tab->n_pages = 0;
    tab->cur_page = 0;
    tab->last_zoom = 96.0;
    tab->initial_scroll_pending = FALSE;
    tab->scroll_offset = -1.0;
    tab->is_helper = (notebook == app.right_notebook);
    tab->zoom_scroll_source_id = 0;
    tab->scroll_doc_debounce_id = 0;
    tab->last_cursor_type = GDK_LEFT_PTR;
    tab->last_cursor_check = 0;
    tab->last_known_page = -1;
    tab->last_known_page_start = 0.0;

    if (!notebook || !GTK_IS_NOTEBOOK(notebook)) {
        g_free(tab);
        return NULL;
    }

    gdk_rgba_parse(&tab->page_color, "white");
    /* Override with session's stored color if available */
    session_model_t *cur_session = get_current_session_model();
    if (cur_session) {
        const char *c = tab->is_helper
            ? session_model_get_helper_page_color(cur_session)
            : session_model_get_page_color(cur_session);
        if (c && *c) gdk_rgba_parse(&tab->page_color, c);
    }

    /* Create container for tab content */
    GtkWidget *tab_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    /* Create scrolled window and a single drawing area used for both single and continuous views */
    tab->scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_vexpand(tab->scrolled, TRUE);
    gtk_widget_set_hexpand(tab->scrolled, TRUE);
    /* connect scroll adjustments to update page display */
    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(tab->scrolled));
    g_signal_connect(G_OBJECT(vadj), "value-changed", G_CALLBACK(on_scroll_value_changed), tab);
    GtkAdjustment *hadj = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(tab->scrolled));
    g_signal_connect(G_OBJECT(hadj), "value-changed", G_CALLBACK(on_scroll_value_changed), tab);
    g_signal_connect(G_OBJECT(tab->scrolled), "size-allocate", G_CALLBACK(on_tab_scrolled_size_allocate), tab);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(tab->scrolled),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    tab->pages_drawing = gtk_drawing_area_new();
    gtk_widget_set_hexpand(tab->pages_drawing, TRUE);
    gtk_widget_set_vexpand(tab->pages_drawing, TRUE);
    g_signal_connect(G_OBJECT(tab->pages_drawing), "draw", G_CALLBACK(on_draw), tab);
    gtk_container_add(GTK_CONTAINER(tab->scrolled), tab->pages_drawing);
    gtk_box_pack_start(GTK_BOX(tab_box), tab->scrolled, TRUE, TRUE, 0);

    tab->h_scrollbar = gtk_scrollbar_new(GTK_ORIENTATION_HORIZONTAL, NULL);
    gtk_widget_set_no_show_all(tab->h_scrollbar, TRUE);
    gtk_widget_hide(tab->h_scrollbar);
    gtk_box_pack_start(GTK_BOX(tab_box), tab->h_scrollbar, FALSE, FALSE, 0);
    GtkAdjustment *scroll_adj = gtk_range_get_adjustment(GTK_RANGE(tab->h_scrollbar));
    g_signal_connect(G_OBJECT(scroll_adj), "value-changed", G_CALLBACK(on_scroll_value_changed), tab);
    g_signal_connect(G_OBJECT(tab->pages_drawing), "scroll-event", G_CALLBACK(on_drawing_scroll), tab);
    g_signal_connect(G_OBJECT(tab->pages_drawing), "button-press-event", G_CALLBACK(on_drawing_button_press), tab);
    g_signal_connect(G_OBJECT(tab->pages_drawing), "button-release-event", G_CALLBACK(on_drawing_button_release), tab);
    g_signal_connect(G_OBJECT(tab->pages_drawing), "motion-notify-event", G_CALLBACK(on_drawing_motion_notify), tab);
    g_signal_connect(G_OBJECT(tab->pages_drawing), "leave-notify-event", G_CALLBACK(on_drawing_leave), tab);
    gtk_widget_add_events(tab->pages_drawing, GDK_SCROLL_MASK | GDK_POINTER_MOTION_MASK | GDK_LEAVE_NOTIFY_MASK | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK);
    g_signal_connect(G_OBJECT(tab->h_scrollbar), "enter-notify-event", G_CALLBACK(on_h_scrollbar_enter), tab);
    g_signal_connect(G_OBJECT(tab->h_scrollbar), "leave-notify-event", G_CALLBACK(on_h_scrollbar_leave), tab);
    gtk_widget_add_events(tab->h_scrollbar, GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);

    gtk_widget_show_all(tab_box);

    /* Store tab data in the widget */
    g_object_set_data_full(G_OBJECT(tab_box), "tab-data", tab, destroy_tab_data);

    /* Determine box orientation based on tab position */
    GtkOrientation box_orientation = GTK_ORIENTATION_HORIZONTAL;
    if (app.sessions_model) {
        const char *pos = sessions_model_get_tabbar_position(app.sessions_model);
        if (g_strcmp0(pos, "left") == 0 || g_strcmp0(pos, "right") == 0)
            box_orientation = GTK_ORIENTATION_VERTICAL;
    }
    GtkWidget *label_box = gtk_box_new(box_orientation, 1);
    GtkWidget *label = gtk_label_new("New Document");
    if (app.sessions_model)
        gtk_label_set_angle(GTK_LABEL(label), get_angle_for_position(sessions_model_get_tabbar_position(app.sessions_model)));
    tab->tab_label = label;  /* Store reference to label for updates */
    tab->tab_label_box = label_box;  /* Store reference to container for orientation changes */
    GtkWidget *close_img = gtk_image_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_MENU);
    gtk_image_set_pixel_size(GTK_IMAGE(close_img), 8);
    GtkWidget *close_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(close_btn), close_img);
    gtk_widget_set_size_request(close_btn, 10, 10);
    tab->tab_label_close_btn = close_btn;
    gtk_widget_set_has_tooltip(close_btn, TRUE);
    gtk_widget_set_tooltip_text(close_btn, "Close tab");
    if (app.sessions_model && g_strcmp0(sessions_model_get_tabbar_position(app.sessions_model), "left") == 0) {
        gtk_box_pack_start(GTK_BOX(label_box), close_btn, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(label_box), label, FALSE, FALSE, 0);
    } else if (app.sessions_model && g_strcmp0(sessions_model_get_tabbar_position(app.sessions_model), "right") == 0) {
        gtk_box_pack_start(GTK_BOX(label_box), label, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(label_box), close_btn, FALSE, FALSE, 0);
    } else {
        gtk_box_pack_start(GTK_BOX(label_box), label, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(label_box), close_btn, FALSE, FALSE, 0);
    }
    /* allocate CloseInfo linking the notebook and this page so close removes correct page */
    typedef struct {
        GtkNotebook *notebook;
        GtkWidget *page;
    } CloseInfo;
    CloseInfo *ci = g_malloc(sizeof(CloseInfo));
    ci->notebook = GTK_NOTEBOOK(notebook);
    ci->page = tab_box;
    g_signal_connect(close_btn, "clicked", G_CALLBACK(on_tab_close_clicked), ci);
    gtk_widget_show_all(label_box);

    /* Add tab to notebook */
    int page_num = gtk_notebook_append_page(GTK_NOTEBOOK(notebook), tab_box, label_box);
    gtk_notebook_set_tab_reorderable(GTK_NOTEBOOK(notebook), tab_box, TRUE);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), page_num);

    return tab;
}

static void on_tab_close_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    typedef struct {
        GtkNotebook *notebook;
        GtkWidget *page;
    } CloseInfo;
    CloseInfo *ci = user_data;
    if (!ci || !ci->notebook || !GTK_IS_NOTEBOOK(ci->notebook) || !ci->page) {
        if (ci) g_free(ci);
        return;
    }

    gboolean is_left = (ci->notebook == GTK_NOTEBOOK(app.left_notebook));
    gboolean is_right = (ci->notebook == GTK_NOTEBOOK(app.right_notebook));
    session_model_t *session = NULL;
    if (app.current_selected_session && app.session_models) {
        session = g_hash_table_lookup(app.session_models, app.current_selected_session);
    }

    char *closed_uri = NULL;
    int page_idx = gtk_notebook_page_num(ci->notebook, ci->page);
    if (page_idx >= 0) {
        GtkWidget *child = gtk_notebook_get_nth_page(ci->notebook, page_idx);
        if (child) {
            TabData *tab = g_object_get_data(G_OBJECT(child), "tab-data");
            if (tab && tab->current_file && session) {
                closed_uri = g_filename_to_uri(tab->current_file, NULL, NULL);
                // Remove from open documents list
                if (closed_uri) {
                    if (is_left) {
                        session_model_remove_document_url(session, closed_uri);
                    } else if (is_right) {
                        session_model_remove_helper_document_url(session, closed_uri);
                    }
                }
                // Remove document model so reopened file starts fresh (zoom, page, view, etc.)
                if (closed_uri && app.document_models) {
                    char *key = make_document_key(app.current_selected_session, closed_uri, tab->is_helper);
                    g_hash_table_remove(app.document_models, key);
                    g_free(key);
                }
            }
            /* The tab will be freed when its page widget is destroyed. */
        }
        gtk_notebook_remove_page(ci->notebook, page_idx);

        if (session) {
            gboolean closed_was_last_read = FALSE;
            if (closed_uri) {
                if (is_left) {
                    const char *last = session_model_get_last_read_document(session);
                    closed_was_last_read = (g_strcmp0(last, closed_uri) == 0);
                } else if (is_right) {
                    const char *last = session_model_get_last_read_help_document(session);
                    closed_was_last_read = (g_strcmp0(last, closed_uri) == 0);
                }
            }
            if (closed_was_last_read) {
                int cur = gtk_notebook_get_current_page(ci->notebook);
                if (cur >= 0) {
                    GtkWidget *new_page = gtk_notebook_get_nth_page(ci->notebook, cur);
                    if (new_page) {
                        update_last_read_for_notebook(ci->notebook, new_page, (guint)cur);
                    }
                } else {
                    if (is_left) {
                        session_model_set_last_read_document(session, "");
                    } else if (is_right) {
                        session_model_set_last_read_help_document(session, "");
                    }
                }
            }
        }
    }

    if (closed_uri) g_free(closed_uri);

    if (is_left) {
        sync_page_widget_from_tab(get_current_left_tab());
    } else if (is_right) {
        sync_right_page_widget_from_tab(get_current_right_tab());
    }

    if (app.current_sidebar_mode == SIDEBAR_FILE_INFO) {
        update_file_info_labels(get_current_left_tab());
    }

    if (app.right_file_info_popover && gtk_widget_get_mapped(app.right_file_info_popover)) {
        TabData *rtab = get_current_right_tab();
        gchar *basename = rtab && rtab->current_file ? g_path_get_basename(rtab->current_file) : NULL;
        gchar *text = basename ? g_strdup_printf("Name: %s", basename) : g_strdup("Name: (no file)");
        gtk_label_set_text(GTK_LABEL(app.right_popover_name_label), text);
        g_free(text);
        g_free(basename);

        text = rtab && rtab->current_file ? g_strdup_printf("Path: %s", rtab->current_file) : g_strdup("Path: (none)");
        gtk_label_set_text(GTK_LABEL(app.right_popover_path_label), text);
        g_free(text);

        if (rtab && rtab->current_file) {
            GFile *gf = g_file_new_for_path(rtab->current_file);
            GFileInfo *info = g_file_query_info(gf, G_FILE_ATTRIBUTE_STANDARD_SIZE,
                                                 G_FILE_QUERY_INFO_NONE, NULL, NULL);
            if (info) {
                gchar *size_str = format_file_size(g_file_info_get_size(info));
                text = g_strdup_printf("Size: %s", size_str);
                gtk_label_set_text(GTK_LABEL(app.right_popover_size_label), text);
                g_free(text);
                g_free(size_str);
                g_object_unref(info);
            } else {
                gtk_label_set_text(GTK_LABEL(app.right_popover_size_label), "Size: Unknown");
            }
            g_object_unref(gf);

            if (rtab->doc) {
                gchar *pages_text = g_strdup_printf("Pages: %d", pdfr_count_pages(rtab->doc));
                gtk_label_set_text(GTK_LABEL(app.right_popover_pages_label), pages_text);
                g_free(pages_text);
            } else {
                gtk_label_set_text(GTK_LABEL(app.right_popover_pages_label), "Pages: N/A");
            }
        } else {
            gtk_label_set_text(GTK_LABEL(app.right_popover_size_label), "Size: (none)");
            gtk_label_set_text(GTK_LABEL(app.right_popover_pages_label), "Pages: (none)");
        }
    }

    g_free(ci);

    if (is_left) {
        populate_sessions_treeview();
    }
}


GtkWidget* create_main_window(void) {
    app.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app.window), "Siters");
    gtk_window_set_default_size(GTK_WINDOW(app.window), 1000, 800);

    // Load application icon from PNG (gdk-pixbuf always supports PNG)
    GError *icon_err = NULL;
    gchar *icon_path = g_strdup_printf(DATADIR "/data/icons/siters_64.png");
    GdkPixbuf *icon_pb = gdk_pixbuf_new_from_file(icon_path, &icon_err);
    if (icon_pb) {
        GList *icons = NULL;
        icons = g_list_append(icons, icon_pb);
        gtk_window_set_default_icon_list(icons);
        g_list_free(icons);
        g_object_unref(icon_pb);
    } else {
        g_warning("Could not load app icon: %s", icon_err->message);
        g_clear_error(&icon_err);
    }
    g_free(icon_path);

    // Set minimal size to prevent unusable layouts
    GdkGeometry hints = {0};
    hints.min_width = 1000;
    hints.min_height = 800;
    gtk_window_set_geometry_hints(GTK_WINDOW(app.window), NULL, &hints, GDK_HINT_MIN_SIZE);

    // Initialize sessions model
    if (!app.sessions_model) {
        app.sessions_model = sessions_model_new();
        // Initialize session models hash table
        app.session_models = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)session_model_free);

        // Initialize document models hash table
        if (!app.document_models) {
            app.document_models = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                                   (GDestroyNotify)document_model_free);
        }

        // Always ensure "Default" session exists
        const GList *existing_sessions = sessions_model_get_session_names(app.sessions_model);
        gboolean has_default = FALSE;
        for (const GList *iter = existing_sessions; iter != NULL; iter = iter->next) {
            if (strcmp((const char*)iter->data, "Default") == 0) {
                has_default = TRUE;
                break;
            }
        }
        if (!has_default) {
            sessions_model_add_session_name(app.sessions_model, "Default");
            // Create session model for Default
            session_model_t *default_session = session_model_new();
            session_model_set_session_name(default_session, "Default");
            g_hash_table_insert(app.session_models, g_strdup("Default"), default_session);
        } else {
            // Ensure we have a session model for Default
            if (!g_hash_table_lookup(app.session_models, "Default")) {
                session_model_t *default_session = session_model_new();
                session_model_set_session_name(default_session, "Default");
                g_hash_table_insert(app.session_models, g_strdup("Default"), default_session);
            }
        }
    }

    /* Auto-detect dark vs light theme: check both the prefer-dark setting
       and the theme name for known dark-theme keywords. */
    {
        GtkSettings *settings = gtk_settings_get_default();
        app.is_dark_theme = detect_system_dark_theme();
        sessions_model_set_theme(app.sessions_model, app.is_dark_theme ? "dark" : "light");
        g_signal_connect(settings, "notify::gtk-theme-name", G_CALLBACK(on_theme_changed), NULL);
        g_signal_connect(settings, "notify::gtk-application-prefer-dark-theme", G_CALLBACK(on_theme_changed), NULL);
    }

    g_signal_connect(app.window, "destroy", G_CALLBACK(on_window_destroy), NULL);
    g_signal_connect(app.window, "configure-event", G_CALLBACK(on_window_configure), NULL);

    /* Main horizontal container: toolbar on left, content on right */
    app.main_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_container_add(GTK_CONTAINER(app.window), app.main_hbox);

    /* Left sidebar: main toolbar */
    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(toolbar), "Toolbar");
    gtk_widget_set_size_request(toolbar, 36, -1);
    gtk_box_pack_start(GTK_BOX(app.main_hbox), toolbar, FALSE, FALSE, 0);

    /* Sidebar for sessions, toc, settings */
    app.sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    g_object_ref(app.sidebar);  /* Keep a reference to prevent destruction when removed */
    gtk_widget_set_size_request(app.sidebar, 200, -1);
    atk_object_set_name(gtk_widget_get_accessible(app.sidebar), "Sidebar");

    /* Content for sidebar */
    app.sidebar_label = gtk_label_new("");
    gtk_label_set_justify(GTK_LABEL(app.sidebar_label), GTK_JUSTIFY_LEFT);
    atk_object_set_name(gtk_widget_get_accessible(app.sidebar_label), "Sidebar label");
    g_object_ref(app.sidebar_label);  /* Keep a reference to prevent destruction when removed */
    gtk_box_pack_start(GTK_BOX(app.sidebar), app.sidebar_label, TRUE, TRUE, 0);
    gtk_widget_hide(app.sidebar_label);

    /* Sessions container */
    app.sessions_container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_set_border_width(GTK_CONTAINER(app.sessions_container), 5);
    g_object_ref(app.sessions_container);  /* Keep a reference */

    // Title
    app.sessions_title = gtk_label_new("Sessions");
    gtk_widget_set_halign(app.sessions_title, GTK_ALIGN_START);
    PangoAttrList *attr_list = pango_attr_list_new();
    PangoAttribute *attr = pango_attr_weight_new(PANGO_WEIGHT_BOLD);
    pango_attr_list_insert(attr_list, attr);
    attr = pango_attr_scale_new(PANGO_SCALE_LARGE);
    pango_attr_list_insert(attr_list, attr);
    gtk_label_set_attributes(GTK_LABEL(app.sessions_title), attr_list);
    pango_attr_list_unref(attr_list);
    gtk_box_pack_start(GTK_BOX(app.sessions_container), app.sessions_title, FALSE, FALSE, 0);

    // Entry field
    app.sessions_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(app.sessions_entry), "Enter session name...");
    gtk_box_pack_start(GTK_BOX(app.sessions_container), app.sessions_entry, FALSE, FALSE, 0);
    atk_object_set_name(gtk_widget_get_accessible(app.sessions_entry), "Sessions entry");

    // Buttons box
    GtkWidget *buttons_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_pack_start(GTK_BOX(app.sessions_container), buttons_box, FALSE, FALSE, 0);

    app.sessions_add_btn = gtk_button_new_with_label("Add");
    gtk_widget_set_tooltip_text(app.sessions_add_btn, "Add new session");
    atk_object_set_name(gtk_widget_get_accessible(app.sessions_add_btn), "Add session");
    g_signal_connect(app.sessions_add_btn, "clicked", G_CALLBACK(on_sessions_add_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(buttons_box), app.sessions_add_btn, TRUE, TRUE, 0);

    app.sessions_remove_btn = gtk_button_new_with_label("Remove");
    gtk_widget_set_tooltip_text(app.sessions_remove_btn, "Remove selected session");
    atk_object_set_name(gtk_widget_get_accessible(app.sessions_remove_btn), "Remove session");
    g_signal_connect(app.sessions_remove_btn, "clicked", G_CALLBACK(on_sessions_remove_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(buttons_box), app.sessions_remove_btn, TRUE, TRUE, 0);

    app.sessions_update_btn = gtk_button_new_with_label("Update");
    gtk_widget_set_tooltip_text(app.sessions_update_btn, "Update selected session name");
    atk_object_set_name(gtk_widget_get_accessible(app.sessions_update_btn), "Update session");
    g_signal_connect(app.sessions_update_btn, "clicked", G_CALLBACK(on_sessions_update_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(buttons_box), app.sessions_update_btn, TRUE, TRUE, 0);

    // Tree view
    app.sessions_tree_store = gtk_tree_store_new(
    SESSION_COL_COUNT,
    G_TYPE_STRING,  // label
    G_TYPE_INT,     // row kind
    G_TYPE_STRING,  // session name
    G_TYPE_STRING   // doc uri
    );

    app.sessions_tree_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(app.sessions_tree_store));
    g_object_unref(app.sessions_tree_store);
    atk_object_set_name(gtk_widget_get_accessible(app.sessions_tree_view), "Sessions tree");

    GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes("Session / File", renderer, "text", SESSION_COL_LABEL, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(app.sessions_tree_view), column);

    g_signal_connect(app.sessions_tree_view, "cursor-changed", G_CALLBACK(on_sessions_treeview_cursor_changed), NULL);

    // Scrolled window for tree view
    GtkWidget *scrolled_window = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scrolled_window), app.sessions_tree_view);
    gtk_box_pack_start(GTK_BOX(app.sessions_container), scrolled_window, TRUE, TRUE, 0);

    // Populate tree view with existing sessions
    populate_sessions_treeview();

    gtk_box_pack_start(GTK_BOX(app.sidebar), app.sessions_container, TRUE, TRUE, 0);
    gtk_widget_hide(app.sessions_container);

    /* TOC container */
    app.toc_container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_set_border_width(GTK_CONTAINER(app.toc_container), 5);
    g_object_ref(app.toc_container);

    GtkWidget *toc_title = gtk_label_new("Table of Contents");
    gtk_widget_set_halign(toc_title, GTK_ALIGN_START);
    PangoAttrList *toc_attr = pango_attr_list_new();
    pango_attr_list_insert(toc_attr, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    pango_attr_list_insert(toc_attr, pango_attr_scale_new(PANGO_SCALE_LARGE));
    gtk_label_set_attributes(GTK_LABEL(toc_title), toc_attr);
    pango_attr_list_unref(toc_attr);
    gtk_box_pack_start(GTK_BOX(app.toc_container), toc_title, FALSE, FALSE, 0);

    app.toc_tree_store = gtk_tree_store_new(TOC_COL_COUNT, G_TYPE_STRING, G_TYPE_INT, G_TYPE_STRING);

    app.toc_tree_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(app.toc_tree_store));
    g_object_unref(app.toc_tree_store);

    GtkCellRenderer *toc_renderer = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *toc_col = gtk_tree_view_column_new_with_attributes("Section", toc_renderer, "text", TOC_COL_LABEL, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(app.toc_tree_view), toc_col);

    gtk_tree_view_set_activate_on_single_click(GTK_TREE_VIEW(app.toc_tree_view), TRUE);
    g_signal_connect(app.toc_tree_view, "row-activated", G_CALLBACK(on_toc_row_activated), NULL);

    GtkWidget *toc_scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(toc_scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(toc_scrolled), app.toc_tree_view);
    gtk_box_pack_start(GTK_BOX(app.toc_container), toc_scrolled, TRUE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(app.sidebar), app.toc_container, TRUE, TRUE, 0);
    gtk_widget_hide(app.toc_container);

    /* Settings container */
    app.settings_container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_set_border_width(GTK_CONTAINER(app.settings_container), 5);
    g_object_ref(app.settings_container);

    GtkWidget *settings_title = gtk_label_new("Siters Preferences");
    gtk_widget_set_halign(settings_title, GTK_ALIGN_START);
    PangoAttrList *sattr = pango_attr_list_new();
    pango_attr_list_insert(sattr, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    pango_attr_list_insert(sattr, pango_attr_scale_new(PANGO_SCALE_LARGE));
    gtk_label_set_attributes(GTK_LABEL(settings_title), sattr);
    pango_attr_list_unref(sattr);
    gtk_box_pack_start(GTK_BOX(app.settings_container), settings_title, FALSE, FALSE, 0);

    GtkWidget *version_label = gtk_label_new("Version 0.1.0-19");
    gtk_widget_set_halign(version_label, GTK_ALIGN_START);
    PangoAttrList *vattr = pango_attr_list_new();
    pango_attr_list_insert(vattr, pango_attr_foreground_alpha_new(32768));
    gtk_label_set_attributes(GTK_LABEL(version_label), vattr);
    pango_attr_list_unref(vattr);
    gtk_box_pack_start(GTK_BOX(app.settings_container), version_label, FALSE, FALSE, 0);

    /* Tab section */
    GtkWidget *tab_section_label = gtk_label_new("Tab");
    gtk_widget_set_halign(tab_section_label, GTK_ALIGN_START);
    PangoAttrList *ts_attr = pango_attr_list_new();
    pango_attr_list_insert(ts_attr, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    gtk_label_set_attributes(GTK_LABEL(tab_section_label), ts_attr);
    pango_attr_list_unref(ts_attr);
    gtk_box_pack_start(GTK_BOX(app.settings_container), tab_section_label, FALSE, FALSE, 0);

    GtkWidget *tabbar_label = gtk_label_new("Tab bar position:");
    gtk_widget_set_halign(tabbar_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(app.settings_container), tabbar_label, FALSE, FALSE, 0);

    app.tabbar_combo = gtk_combo_box_text_new();
    gtk_widget_set_halign(app.tabbar_combo, GTK_ALIGN_START);
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(app.tabbar_combo), "left", "Left");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(app.tabbar_combo), "top", "Top");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(app.tabbar_combo), "right", "Right");
    if (app.sessions_model) {
        const char *cur = sessions_model_get_tabbar_position(app.sessions_model);
        if (g_strcmp0(cur, "left") == 0)
            gtk_combo_box_set_active(GTK_COMBO_BOX(app.tabbar_combo), 0);
        else if (g_strcmp0(cur, "right") == 0)
            gtk_combo_box_set_active(GTK_COMBO_BOX(app.tabbar_combo), 2);
        else
            gtk_combo_box_set_active(GTK_COMBO_BOX(app.tabbar_combo), 1);
    } else {
        gtk_combo_box_set_active(GTK_COMBO_BOX(app.tabbar_combo), 1);
    }
    gtk_box_pack_start(GTK_BOX(app.settings_container), app.tabbar_combo, FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(app.tabbar_combo), "changed", G_CALLBACK(on_tabbar_combo_changed), NULL);

    /* Tab text width */
    GtkWidget *tab_width_label = gtk_label_new("Tab text width:");
    gtk_widget_set_halign(tab_width_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(app.settings_container), tab_width_label, FALSE, FALSE, 0);

    app.tab_width_spin = gtk_spin_button_new_with_range(5, 50, 1);
    gtk_widget_set_halign(app.tab_width_spin, GTK_ALIGN_START);
    if (app.sessions_model)
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(app.tab_width_spin), sessions_model_get_tab_width(app.sessions_model));
    else
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(app.tab_width_spin), 50);
    gtk_box_pack_start(GTK_BOX(app.settings_container), app.tab_width_spin, FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(app.tab_width_spin), "value-changed", G_CALLBACK(on_tab_width_spin_changed), NULL);

    gtk_box_pack_start(GTK_BOX(app.settings_container), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);

    /* Page Colors section */
    GtkWidget *color_title = gtk_label_new("Page Colors");
    gtk_widget_set_halign(color_title, GTK_ALIGN_START);
    PangoAttrList *cat = pango_attr_list_new();
    pango_attr_list_insert(cat, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    gtk_label_set_attributes(GTK_LABEL(color_title), cat);
    pango_attr_list_unref(cat);
    gtk_box_pack_start(GTK_BOX(app.settings_container), color_title, FALSE, FALSE, 0);

    GtkWidget *left_color_label = gtk_label_new("Left notebook:");
    gtk_widget_set_halign(left_color_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(app.settings_container), left_color_label, FALSE, FALSE, 0);

    app.left_color_btn = gtk_color_button_new_with_rgba(&(GdkRGBA){1.0, 1.0, 1.0, 1.0});
    gtk_widget_set_halign(app.left_color_btn, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(app.settings_container), app.left_color_btn, FALSE, FALSE, 0);
    g_signal_connect(app.left_color_btn, "color-set", G_CALLBACK(on_left_color_set), NULL);

    GtkWidget *right_color_label = gtk_label_new("Right notebook:");
    gtk_widget_set_halign(right_color_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(app.settings_container), right_color_label, FALSE, FALSE, 0);

    app.right_color_btn = gtk_color_button_new_with_rgba(&(GdkRGBA){1.0, 1.0, 1.0, 1.0});
    gtk_widget_set_halign(app.right_color_btn, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(app.settings_container), app.right_color_btn, FALSE, FALSE, 0);
    g_signal_connect(app.right_color_btn, "color-set", G_CALLBACK(on_right_color_set), NULL);

    gtk_box_pack_start(GTK_BOX(app.settings_container), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);

    /* Theme section */
    GtkWidget *theme_section_label = gtk_label_new("Theme");
    gtk_widget_set_halign(theme_section_label, GTK_ALIGN_START);
    PangoAttrList *th_attr = pango_attr_list_new();
    pango_attr_list_insert(th_attr, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    gtk_label_set_attributes(GTK_LABEL(theme_section_label), th_attr);
    pango_attr_list_unref(th_attr);
    gtk_box_pack_start(GTK_BOX(app.settings_container), theme_section_label, FALSE, FALSE, 0);

    app.keep_dark_check = gtk_check_button_new_with_label("Keep dark theme");
    gtk_widget_set_halign(app.keep_dark_check, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(app.settings_container), app.keep_dark_check, FALSE, FALSE, 0);
    g_signal_connect(app.keep_dark_check, "toggled", G_CALLBACK(on_keep_dark_toggled), NULL);

    gtk_box_pack_start(GTK_BOX(app.sidebar), app.settings_container, TRUE, TRUE, 0);
    gtk_widget_hide(app.settings_container);

    /* File info container */
    app.file_info_container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(app.file_info_container), 8);
    g_object_ref(app.file_info_container);

    GtkWidget *file_info_title = gtk_label_new("File Information");
    gtk_widget_set_halign(file_info_title, GTK_ALIGN_START);
    PangoAttrList *fi_attr = pango_attr_list_new();
    pango_attr_list_insert(fi_attr, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    pango_attr_list_insert(fi_attr, pango_attr_scale_new(PANGO_SCALE_LARGE));
    gtk_label_set_attributes(GTK_LABEL(file_info_title), fi_attr);
    pango_attr_list_unref(fi_attr);
    gtk_box_pack_start(GTK_BOX(app.file_info_container), file_info_title, FALSE, FALSE, 0);

    app.file_info_name_label = gtk_label_new("Name: (no file)");
    gtk_widget_set_halign(app.file_info_name_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(app.file_info_container), app.file_info_name_label, FALSE, FALSE, 0);

    app.file_info_path_label = gtk_label_new("Path: (none)");
    gtk_widget_set_halign(app.file_info_path_label, GTK_ALIGN_FILL);
    gtk_label_set_xalign(GTK_LABEL(app.file_info_path_label), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(app.file_info_path_label), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(app.file_info_path_label), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(app.file_info_path_label), 30);
    gtk_box_pack_start(GTK_BOX(app.file_info_container), app.file_info_path_label, FALSE, FALSE, 0);

    app.file_info_size_label = gtk_label_new("Size: (none)");
    gtk_widget_set_halign(app.file_info_size_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(app.file_info_container), app.file_info_size_label, FALSE, FALSE, 0);

    app.file_info_pages_label = gtk_label_new("Pages: (none)");
    gtk_widget_set_halign(app.file_info_pages_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(app.file_info_container), app.file_info_pages_label, FALSE, FALSE, 0);

    /* Separator before search */
    GtkWidget *search_sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(app.file_info_container), search_sep, FALSE, FALSE, 5);

    /* Search title */
    GtkWidget *search_title = gtk_label_new("Search in Document");
    gtk_widget_set_halign(search_title, GTK_ALIGN_START);
    PangoAttrList *sa = pango_attr_list_new();
    pango_attr_list_insert(sa, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    pango_attr_list_insert(sa, pango_attr_scale_new(PANGO_SCALE_LARGE));
    gtk_label_set_attributes(GTK_LABEL(search_title), sa);
    pango_attr_list_unref(sa);
    gtk_box_pack_start(GTK_BOX(app.file_info_container), search_title, FALSE, FALSE, 0);

    /* Search entry + button row */
    GtkWidget *search_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    app.search_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(app.search_entry), "Search text...");
    gtk_box_pack_start(GTK_BOX(search_hbox), app.search_entry, TRUE, TRUE, 0);
    g_signal_connect(app.search_entry, "activate", G_CALLBACK(on_search_activated), NULL);

    app.search_btn = gtk_button_new_with_label("Search");
    gtk_box_pack_start(GTK_BOX(search_hbox), app.search_btn, FALSE, FALSE, 0);
    g_signal_connect(app.search_btn, "clicked", G_CALLBACK(on_search_clicked), NULL);

    gtk_box_pack_start(GTK_BOX(app.file_info_container), search_hbox, FALSE, FALSE, 0);

    /* Results tree view */
    app.search_results_store = gtk_list_store_new(SEARCH_COL_NCOL, G_TYPE_INT, G_TYPE_INT, G_TYPE_STRING);
    app.search_results_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(app.search_results_store));
    g_object_unref(app.search_results_store);

    GtkCellRenderer *search_renderer = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *search_col = gtk_tree_view_column_new_with_attributes("Page", search_renderer,
        "text", SEARCH_COL_LABEL, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(app.search_results_view), search_col);
    gtk_tree_view_set_activate_on_single_click(GTK_TREE_VIEW(app.search_results_view), TRUE);
    g_signal_connect(app.search_results_view, "row-activated", G_CALLBACK(on_search_row_activated), NULL);

    GtkWidget *search_scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(search_scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(search_scrolled), 80);
    gtk_container_add(GTK_CONTAINER(search_scrolled), app.search_results_view);
    gtk_box_pack_start(GTK_BOX(app.file_info_container), search_scrolled, TRUE, TRUE, 0);

    /* No results label */
    app.search_no_results_label = gtk_label_new("No results found");
    gtk_widget_set_halign(app.search_no_results_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(app.file_info_container), app.search_no_results_label, FALSE, FALSE, 0);
    gtk_widget_hide(app.search_no_results_label);

    gtk_widget_set_size_request(app.file_info_container, 300, -1);
    gtk_box_pack_start(GTK_BOX(app.sidebar), app.file_info_container, TRUE, TRUE, 0);
    gtk_widget_hide(app.file_info_container);

    /* Content area on the right*/
    app.content_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(app.main_hbox), app.content_vbox, TRUE, TRUE, 0);

    /* Buttons*/
    /* Sessions button */
    GtkWidget *sessions_icon = create_toolbar_icon("sessions");
    app.sessions_btn = gtk_toggle_button_new();
    gtk_button_set_image(GTK_BUTTON(app.sessions_btn), sessions_icon);
    g_object_set_data_full(G_OBJECT(app.sessions_btn), "icon-name", g_strdup("sessions"), g_free);
    gtk_widget_set_tooltip_text(app.sessions_btn, "Sessions");
    atk_object_set_name(gtk_widget_get_accessible(app.sessions_btn), "Sessions");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app.sessions_btn), FALSE);
    g_signal_connect(app.sessions_btn, "toggled", G_CALLBACK(on_sessions_toggled), NULL);
    gtk_box_pack_start(GTK_BOX(toolbar), app.sessions_btn, FALSE, FALSE, 1);

    /* Table of contents button */
    GtkWidget *toc_icon = create_toolbar_icon("toc");
    app.toc_btn = gtk_toggle_button_new();
    gtk_button_set_image(GTK_BUTTON(app.toc_btn), toc_icon);
    g_object_set_data_full(G_OBJECT(app.toc_btn), "icon-name", g_strdup("toc"), g_free);
    gtk_widget_set_tooltip_text(app.toc_btn, "Table of contents");
    atk_object_set_name(gtk_widget_get_accessible(app.toc_btn), "Table of contents");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app.toc_btn), FALSE);
    g_signal_connect(app.toc_btn, "toggled", G_CALLBACK(on_toc_toggled), NULL);
    gtk_box_pack_start(GTK_BOX(toolbar), app.toc_btn, FALSE, FALSE, 1);

    /* Settings button */
    GtkWidget *settings_icon = create_toolbar_icon("settings");
    app.settings_btn = gtk_toggle_button_new();
    gtk_button_set_image(GTK_BUTTON(app.settings_btn), settings_icon);
    g_object_set_data_full(G_OBJECT(app.settings_btn), "icon-name", g_strdup("settings"), g_free);
    gtk_widget_set_tooltip_text(app.settings_btn, "Settings");
    atk_object_set_name(gtk_widget_get_accessible(app.settings_btn), "Settings");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app.settings_btn), FALSE);
    g_signal_connect(app.settings_btn, "toggled", G_CALLBACK(on_settings_toggled), NULL);
    gtk_box_pack_start(GTK_BOX(toolbar), app.settings_btn, FALSE, FALSE, 1);

    /* File information button */
    GtkWidget *file_info_icon = create_toolbar_icon("file");
    app.file_info_btn = gtk_toggle_button_new();
    gtk_button_set_image(GTK_BUTTON(app.file_info_btn), file_info_icon);
    g_object_set_data_full(G_OBJECT(app.file_info_btn), "icon-name", g_strdup("file"), g_free);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app.file_info_btn), FALSE);
    gtk_widget_set_tooltip_text(app.file_info_btn, "File information");
    atk_object_set_name(gtk_widget_get_accessible(app.file_info_btn), "File information");
    g_signal_connect(app.file_info_btn, "toggled", G_CALLBACK(on_left_file_info_toggled), NULL);
    gtk_box_pack_start(GTK_BOX(toolbar), app.file_info_btn, FALSE, FALSE, 1);

    /* Separator */
    GtkWidget *separator_a = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(toolbar), separator_a, FALSE, FALSE, 5);

    /* Centered middle section: open file through helper toggle */
    GtkWidget *middle_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), middle_box, TRUE, FALSE, 0);

    /* Open file button */
    GtkWidget *open_file_icon = create_toolbar_icon("file-plus");
    GtkWidget *open_file_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(open_file_btn), open_file_icon);
    g_object_set_data_full(G_OBJECT(open_file_btn), "icon-name", g_strdup("file-plus"), g_free);
    gtk_widget_set_tooltip_text(open_file_btn, "Open file");
    atk_object_set_name(gtk_widget_get_accessible(open_file_btn), "Open file");
    g_signal_connect(open_file_btn, "clicked", G_CALLBACK(on_open_file_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(middle_box), open_file_btn, FALSE, FALSE, 1);

    /* Close file button*/
    GtkWidget *close_file_icon = create_toolbar_icon("file-minus");
    GtkWidget *close_file_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(close_file_btn), close_file_icon);
    g_object_set_data_full(G_OBJECT(close_file_btn), "icon-name", g_strdup("file-minus"), g_free);
    gtk_widget_set_tooltip_text(close_file_btn, "Close file");
    atk_object_set_name(gtk_widget_get_accessible(close_file_btn), "Close file");
    g_signal_connect(close_file_btn, "clicked", G_CALLBACK(on_close_file_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(middle_box), close_file_btn, FALSE, FALSE, 1);

    /* Separator */
    GtkWidget *separator_b = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(middle_box), separator_b, FALSE, FALSE, 5);

    /* Page backward button*/
    GtkWidget *page_up_icon = create_toolbar_icon("page-up");
    GtkWidget *page_up_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(page_up_btn), page_up_icon);
    g_object_set_data_full(G_OBJECT(page_up_btn), "icon-name", g_strdup("page-up"), g_free);
    gtk_widget_set_tooltip_text(page_up_btn, "Page backward");
    atk_object_set_name(gtk_widget_get_accessible(page_up_btn), "Page backward");
    g_signal_connect(page_up_btn, "clicked", G_CALLBACK(on_page_up_left), NULL);
    gtk_box_pack_start(GTK_BOX(middle_box), page_up_btn, FALSE, FALSE, 1);

    /* Page forward button*/
    GtkWidget *page_down_icon = create_toolbar_icon("page-down");
    GtkWidget *page_down_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(page_down_btn), page_down_icon);
    g_object_set_data_full(G_OBJECT(page_down_btn), "icon-name", g_strdup("page-down"), g_free);
    gtk_widget_set_tooltip_text(page_down_btn, "Page forward");
    atk_object_set_name(gtk_widget_get_accessible(page_down_btn), "Page forward");
    g_signal_connect(page_down_btn, "clicked", G_CALLBACK(on_page_down_left), NULL);
    gtk_box_pack_start(GTK_BOX(middle_box), page_down_btn, FALSE, FALSE, 1);

    /* Page navigation (entry + label) — placed in floating overlay later */
    app.page_entry = gtk_entry_new();
    gtk_widget_set_size_request(app.page_entry, 42, -1);
    gtk_entry_set_max_length(GTK_ENTRY(app.page_entry), 4);
    gtk_entry_set_width_chars(GTK_ENTRY(app.page_entry), 2);
    gtk_entry_set_max_width_chars(GTK_ENTRY(app.page_entry), 3);
    gtk_entry_set_input_purpose(GTK_ENTRY(app.page_entry), GTK_INPUT_PURPOSE_DIGITS);
    gtk_widget_set_tooltip_text(app.page_entry, "Current page (press Enter to jump)");
    atk_object_set_name(gtk_widget_get_accessible(app.page_entry), "Current page");

    app.page_total_label = gtk_label_new("/ 0");
    gtk_label_set_width_chars(GTK_LABEL(app.page_total_label), 4);
    gtk_label_set_xalign(GTK_LABEL(app.page_total_label), 0.0f);

    /* Allow only digits to be entered */
    g_signal_connect(app.page_entry, "insert-text", G_CALLBACK(on_page_entry_insert_text), NULL);

    /* Enter in spin jumps to page */
    g_signal_connect(app.page_entry, "activate", G_CALLBACK(on_page_entry_activate), NULL);

    /* Separator */
    GtkWidget *separator_c = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(middle_box), separator_c, FALSE, FALSE, 5);

    /* Zoom in button*/
    GtkWidget *zoom_in_icon = create_toolbar_icon("zoom-in");
    GtkWidget *zoom_in_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(zoom_in_btn), zoom_in_icon);
    g_object_set_data_full(G_OBJECT(zoom_in_btn), "icon-name", g_strdup("zoom-in"), g_free);
    gtk_widget_set_tooltip_text(zoom_in_btn, "Zoom in");
    atk_object_set_name(gtk_widget_get_accessible(zoom_in_btn), "Zoom in");
    gtk_box_pack_start(GTK_BOX(middle_box), zoom_in_btn, FALSE, FALSE, 1);
    g_signal_connect(G_OBJECT(zoom_in_btn), "clicked", G_CALLBACK(on_zoom_in_left), NULL);

    /* Zoom out button*/
    GtkWidget *zoom_out_icon = create_toolbar_icon("zoom-out");
    GtkWidget *zoom_out_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(zoom_out_btn), zoom_out_icon);
    g_object_set_data_full(G_OBJECT(zoom_out_btn), "icon-name", g_strdup("zoom-out"), g_free);
    gtk_widget_set_tooltip_text(zoom_out_btn, "Zoom out");
    atk_object_set_name(gtk_widget_get_accessible(zoom_out_btn), "Zoom out");
    gtk_box_pack_start(GTK_BOX(middle_box), zoom_out_btn, FALSE, FALSE, 1);
    g_signal_connect(G_OBJECT(zoom_out_btn), "clicked", G_CALLBACK(on_zoom_out_left), NULL);

    /* Separator */
    GtkWidget *separator_d = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(middle_box), separator_d, FALSE, FALSE, 5);

    /* Column view button*/
    GtkWidget *column_view_icon = create_toolbar_icon("column");
    app.left_column_btn = gtk_radio_button_new(NULL);
    gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(app.left_column_btn), FALSE);
    gtk_button_set_image(GTK_BUTTON(app.left_column_btn), column_view_icon);
    g_object_set_data_full(G_OBJECT(app.left_column_btn), "icon-name", g_strdup("column"), g_free);
    gtk_widget_set_tooltip_text(app.left_column_btn, "Page column");
    atk_object_set_name(gtk_widget_get_accessible(app.left_column_btn), "Page column");
    g_object_set_data(G_OBJECT(app.left_column_btn), "layout-id", GINT_TO_POINTER(0 + 1));
    g_signal_connect(app.left_column_btn, "toggled", G_CALLBACK(on_layout_left_toggled), NULL);
    gtk_box_pack_start(GTK_BOX(middle_box), app.left_column_btn, FALSE, FALSE, 1);

    /* Double column view button*/
    GtkWidget *double_column_view_icon = create_toolbar_icon("double-column");
    app.left_double_column_btn = gtk_radio_button_new_from_widget(GTK_RADIO_BUTTON(app.left_column_btn));
    gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(app.left_double_column_btn), FALSE);
    gtk_button_set_image(GTK_BUTTON(app.left_double_column_btn), double_column_view_icon);
    g_object_set_data_full(G_OBJECT(app.left_double_column_btn), "icon-name", g_strdup("double-column"), g_free);
    gtk_widget_set_tooltip_text(app.left_double_column_btn, "Page double column");
    atk_object_set_name(gtk_widget_get_accessible(app.left_double_column_btn), "Page double column");
    g_object_set_data(G_OBJECT(app.left_double_column_btn), "layout-id", GINT_TO_POINTER(1 + 1));
    g_signal_connect(app.left_double_column_btn, "toggled", G_CALLBACK(on_layout_left_toggled), NULL);
    gtk_box_pack_start(GTK_BOX(middle_box), app.left_double_column_btn, FALSE, FALSE, 1);

    /* Row view button*/
    GtkWidget *row_view_icon = create_toolbar_icon("row");
    app.left_row_btn = gtk_radio_button_new_from_widget(GTK_RADIO_BUTTON(app.left_column_btn));
    gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(app.left_row_btn), FALSE);
    gtk_button_set_image(GTK_BUTTON(app.left_row_btn), row_view_icon);
    g_object_set_data_full(G_OBJECT(app.left_row_btn), "icon-name", g_strdup("row"), g_free);
    gtk_widget_set_tooltip_text(app.left_row_btn, "Page row");
    atk_object_set_name(gtk_widget_get_accessible(app.left_row_btn), "Page row");
    g_object_set_data(G_OBJECT(app.left_row_btn), "layout-id", GINT_TO_POINTER(2 + 1));
    g_signal_connect(app.left_row_btn, "toggled", G_CALLBACK(on_layout_left_toggled), NULL);
    gtk_box_pack_start(GTK_BOX(middle_box), app.left_row_btn, FALSE, FALSE, 1);

    /* Separator */
    GtkWidget *separator_e = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(middle_box), separator_e, FALSE, FALSE, 5);

    /* Title bar toggle button*/
    GtkWidget *title_bar_toggle_icon = create_toolbar_icon("title-bar-on");
    GtkWidget *title_bar_toggle_btn = gtk_toggle_button_new();
    gtk_button_set_image(GTK_BUTTON(title_bar_toggle_btn), title_bar_toggle_icon);
    g_object_set_data_full(G_OBJECT(title_bar_toggle_btn), "icon-on", g_strdup("title-bar-on"), g_free);
    g_object_set_data_full(G_OBJECT(title_bar_toggle_btn), "icon-off", g_strdup("title-bar-off"), g_free);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(title_bar_toggle_btn), TRUE);
    gtk_widget_set_tooltip_text(title_bar_toggle_btn, "Toggle title bar visibility");
    atk_object_set_name(gtk_widget_get_accessible(title_bar_toggle_btn), "Toggle title bar visibility");
    g_signal_connect(title_bar_toggle_btn, "toggled", G_CALLBACK(on_title_bar_toggle), NULL);
    gtk_box_pack_start(GTK_BOX(middle_box), title_bar_toggle_btn, FALSE, FALSE, 1);

    /* Helpers toggle button*/
    GtkWidget *helper_toggle_icon = create_toolbar_icon("sidebar-helper-off");
    GtkWidget *helper_toggle_btn = gtk_toggle_button_new();
    gtk_button_set_image(GTK_BUTTON(helper_toggle_btn), helper_toggle_icon);
    g_object_set_data_full(G_OBJECT(helper_toggle_btn), "icon-on", g_strdup("sidebar-helper-on"), g_free);
    g_object_set_data_full(G_OBJECT(helper_toggle_btn), "icon-off", g_strdup("sidebar-helper-off"), g_free);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(helper_toggle_btn), FALSE);
    gtk_widget_set_tooltip_text(helper_toggle_btn, "Helper files");
    atk_object_set_name(gtk_widget_get_accessible(helper_toggle_btn), "Helper files");
    g_signal_connect(helper_toggle_btn, "toggled", G_CALLBACK(on_helper_toggle), NULL);
    gtk_box_pack_start(GTK_BOX(middle_box), helper_toggle_btn, FALSE, FALSE, 1);

    /* Separator */
    GtkWidget *separator_f = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(toolbar), separator_f, FALSE, FALSE, 5);

    /* Close button*/
    GtkWidget *close_icon = create_toolbar_icon("plug");
    GtkWidget *close_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(close_btn), close_icon);
    g_object_set_data_full(G_OBJECT(close_btn), "icon-name", g_strdup("plug"), g_free);
    gtk_widget_set_tooltip_text(close_btn, "Close");
    atk_object_set_name(gtk_widget_get_accessible(close_btn), "Close");
    g_signal_connect(close_btn, "clicked", G_CALLBACK(on_close_clicked), NULL);
    gtk_box_pack_end(GTK_BOX(toolbar), close_btn, FALSE, FALSE, 1);

    /* Maximize button*/
    GtkWidget *maximize_icon = create_toolbar_icon("maximize-2");
    GtkWidget *maximize_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(maximize_btn), maximize_icon);
    g_object_set_data_full(G_OBJECT(maximize_btn), "icon-name", g_strdup("maximize-2"), g_free);
    gtk_widget_set_tooltip_text(maximize_btn, "Maximize");
    atk_object_set_name(gtk_widget_get_accessible(maximize_btn), "Maximize");
    g_signal_connect(maximize_btn, "clicked", G_CALLBACK(on_maximize_clicked), app.window);
    gtk_box_pack_end(GTK_BOX(toolbar), maximize_btn, FALSE, FALSE, 1);

    /* Minimize button*/
    GtkWidget *minimize_icon = create_toolbar_icon("minimize-2");
    GtkWidget *minimize_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(minimize_btn), minimize_icon);
    g_object_set_data_full(G_OBJECT(minimize_btn), "icon-name", g_strdup("minimize-2"), g_free);
    gtk_widget_set_tooltip_text(minimize_btn, "Minimize");
    atk_object_set_name(gtk_widget_get_accessible(minimize_btn), "Minimize");
    g_signal_connect(minimize_btn, "clicked", G_CALLBACK(on_minimize_clicked), app.window);
    gtk_box_pack_end(GTK_BOX(toolbar), minimize_btn, FALSE, FALSE, 1);

    /* MAIN WINDOW PANED */
    /* Create a horizontal paned splitter containing two notebooks */
    app.paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);

    /* Wrap paned in an overlay for floating page navigation widget */
    GtkWidget *overlay = gtk_overlay_new();
    gtk_box_pack_start(GTK_BOX(app.content_vbox), overlay, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(overlay), app.paned);

    /* Floating page navigation overlay (lower-left corner) */
    app.page_nav_overlay = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_name(app.page_nav_overlay, "page-nav-overlay");
    gtk_widget_set_halign(app.page_nav_overlay, GTK_ALIGN_START);
    gtk_widget_set_valign(app.page_nav_overlay, GTK_ALIGN_END);
    gtk_widget_set_margin_start(app.page_nav_overlay, 8);
    gtk_widget_set_margin_bottom(app.page_nav_overlay, 8);
    gtk_box_pack_start(GTK_BOX(app.page_nav_overlay), app.page_entry, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(app.page_nav_overlay), app.page_total_label, FALSE, FALSE, 0);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), app.page_nav_overlay);

    /* Left notebook (primary) */
    app.left_notebook = gtk_notebook_new();
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(app.left_notebook), TRUE);
    gtk_widget_set_size_request(app.left_notebook, 70, -1);
    gtk_paned_pack1(GTK_PANED(app.paned), app.left_notebook, TRUE, TRUE);
    atk_object_set_name(gtk_widget_get_accessible(app.left_notebook), "Left Notebook");
    g_signal_connect(app.left_notebook, "switch-page", G_CALLBACK(on_left_notebook_switch_page), NULL);
    g_signal_connect(app.left_notebook, "page-reordered", G_CALLBACK(on_notebook_page_reordered), NULL);

    // Use last open session if available, otherwise default to "Default"
    const char *initial_session = "Default";
    if (app.sessions_model) {
        const char *last_session = sessions_model_get_last_open_session(app.sessions_model);
        if (last_session) {
            initial_session = last_session;
        }
    }

    app.current_selected_session = g_strdup(initial_session);
    update_window_title_for_session(app.current_selected_session);

    /* Right pane: container with notebook and toolbar */
    app.right_pane = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_size_request(app.right_pane, 36, -1);
    gtk_paned_pack2(GTK_PANED(app.paned), app.right_pane, TRUE, TRUE);
    gtk_paned_set_position(GTK_PANED(app.paned), 500);

    /* Right notebook wrapper with overlay for floating page nav */
    GtkWidget *right_nb_overlay = gtk_overlay_new();
    gtk_box_pack_start(GTK_BOX(app.right_pane), right_nb_overlay, TRUE, TRUE, 0);

    /* Right notebook (secondary) */
    app.right_notebook = gtk_notebook_new();
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(app.right_notebook), TRUE);
    gtk_container_add(GTK_CONTAINER(right_nb_overlay), app.right_notebook);
    g_signal_connect(app.right_notebook, "switch-page", G_CALLBACK(on_right_notebook_switch_page), NULL);
    g_signal_connect(app.right_notebook, "page-reordered", G_CALLBACK(on_notebook_page_reordered), NULL);

    /* Right page navigation entry + label */
    app.right_page_entry = gtk_entry_new();
    gtk_widget_set_size_request(app.right_page_entry, 42, -1);
    gtk_entry_set_max_length(GTK_ENTRY(app.right_page_entry), 4);
    gtk_entry_set_width_chars(GTK_ENTRY(app.right_page_entry), 2);
    gtk_entry_set_max_width_chars(GTK_ENTRY(app.right_page_entry), 3);
    gtk_entry_set_input_purpose(GTK_ENTRY(app.right_page_entry), GTK_INPUT_PURPOSE_DIGITS);
    gtk_widget_set_tooltip_text(app.right_page_entry, "Current page (press Enter to jump)");
    atk_object_set_name(gtk_widget_get_accessible(app.right_page_entry), "Current page");

    app.right_page_total_label = gtk_label_new("/ 0");
    gtk_label_set_width_chars(GTK_LABEL(app.right_page_total_label), 4);
    gtk_label_set_xalign(GTK_LABEL(app.right_page_total_label), 0.0f);

    g_signal_connect(app.right_page_entry, "insert-text", G_CALLBACK(on_page_entry_insert_text), NULL);
    g_signal_connect(app.right_page_entry, "activate", G_CALLBACK(on_right_page_entry_activate), NULL);

    /* Floating page navigation overlay (lower-right corner of right notebook) */
    app.right_page_nav_overlay = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_name(app.right_page_nav_overlay, "right-page-nav-overlay");
    gtk_widget_set_halign(app.right_page_nav_overlay, GTK_ALIGN_END);
    gtk_widget_set_valign(app.right_page_nav_overlay, GTK_ALIGN_END);
    gtk_widget_set_margin_end(app.right_page_nav_overlay, 8);
    gtk_widget_set_margin_bottom(app.right_page_nav_overlay, 8);
    gtk_box_pack_start(GTK_BOX(app.right_page_nav_overlay), app.right_page_entry, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(app.right_page_nav_overlay), app.right_page_total_label, FALSE, FALSE, 0);
    gtk_overlay_add_overlay(GTK_OVERLAY(right_nb_overlay), app.right_page_nav_overlay);
    /* Initially hidden until there are documents */
    gtk_widget_hide(app.right_page_nav_overlay);

    // Note: restore_open_tabs_for_session already handles both notebooks
    app.current_selected_session = g_strdup(initial_session);
    update_window_title_for_session(app.current_selected_session);

    /* Right pane toolbar (vertical) */
    GtkWidget *right_toolbar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(right_toolbar), "Toolbar");
    gtk_box_pack_start(GTK_BOX(app.right_pane), right_toolbar, FALSE, FALSE, 0);

    /* Centered section for right toolbar buttons */
    GtkWidget *right_middle_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(right_toolbar), right_middle_box, TRUE, FALSE, 0);

    /* Right toolbar buttons - Open file */
    GtkWidget *right_open_file_icon = create_toolbar_icon("file-plus");
    GtkWidget *right_open_file_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(right_open_file_btn), right_open_file_icon);
    g_object_set_data_full(G_OBJECT(right_open_file_btn), "icon-name", g_strdup("file-plus"), g_free);
    gtk_widget_set_tooltip_text(right_open_file_btn, "Open file");
    atk_object_set_name(gtk_widget_get_accessible(right_open_file_btn), "Open file");
    g_signal_connect(right_open_file_btn, "clicked", G_CALLBACK(on_open_helper_file_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(right_middle_box), right_open_file_btn, FALSE, FALSE, 1);

    /* Right toolbar - Close file */
    GtkWidget *right_close_file_icon = create_toolbar_icon("file-minus");
    GtkWidget *right_close_file_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(right_close_file_btn), right_close_file_icon);
    g_object_set_data_full(G_OBJECT(right_close_file_btn), "icon-name", g_strdup("file-minus"), g_free);
    gtk_widget_set_tooltip_text(right_close_file_btn, "Close file");
    atk_object_set_name(gtk_widget_get_accessible(right_close_file_btn), "Close file");
    g_signal_connect(right_close_file_btn, "clicked", G_CALLBACK(on_close_helper_file_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(right_middle_box), right_close_file_btn, FALSE, FALSE, 1);

    /* Right toolbar - File information */
    GtkWidget *right_file_info_icon = create_toolbar_icon("file");
    GtkWidget *right_file_info_btn = gtk_toggle_button_new();
    gtk_button_set_image(GTK_BUTTON(right_file_info_btn), right_file_info_icon);
    g_object_set_data_full(G_OBJECT(right_file_info_btn), "icon-name", g_strdup("file"), g_free);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(right_file_info_btn), FALSE);
    gtk_widget_set_tooltip_text(right_file_info_btn, "File information");
    atk_object_set_name(gtk_widget_get_accessible(right_file_info_btn), "File information");
    g_signal_connect(right_file_info_btn, "toggled", G_CALLBACK(on_right_file_info_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(right_middle_box), right_file_info_btn, FALSE, FALSE, 1);

    /* Right toolbar separator */
    GtkWidget *right_sep_a = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(right_middle_box), right_sep_a, FALSE, FALSE, 5);

    /* Right toolbar - Page backward */
    GtkWidget *right_page_up_icon = create_toolbar_icon("page-up");
    GtkWidget *right_page_up_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(right_page_up_btn), right_page_up_icon);
    g_object_set_data_full(G_OBJECT(right_page_up_btn), "icon-name", g_strdup("page-up"), g_free);
    gtk_widget_set_tooltip_text(right_page_up_btn, "Page backward");
    atk_object_set_name(gtk_widget_get_accessible(right_page_up_btn), "Page backward");
    g_signal_connect(right_page_up_btn, "clicked", G_CALLBACK(on_page_up_right), NULL);
    gtk_box_pack_start(GTK_BOX(right_middle_box), right_page_up_btn, FALSE, FALSE, 1);

    /* Right toolbar - Page forward */
    GtkWidget *right_page_down_icon = create_toolbar_icon("page-down");
    GtkWidget *right_page_down_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(right_page_down_btn), right_page_down_icon);
    g_object_set_data_full(G_OBJECT(right_page_down_btn), "icon-name", g_strdup("page-down"), g_free);
    gtk_widget_set_tooltip_text(right_page_down_btn, "Page forward");
    atk_object_set_name(gtk_widget_get_accessible(right_page_down_btn), "Page forward");
    g_signal_connect(right_page_down_btn, "clicked", G_CALLBACK(on_page_down_right), NULL);
    gtk_box_pack_start(GTK_BOX(right_middle_box), right_page_down_btn, FALSE, FALSE, 1);

    /* Right toolbar separator */
    GtkWidget *right_sep_b = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(right_middle_box), right_sep_b, FALSE, FALSE, 5);

    /* Right toolbar - Zoom in */
    GtkWidget *right_zoom_in_icon = create_toolbar_icon("zoom-in");
    GtkWidget *right_zoom_in_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(right_zoom_in_btn), right_zoom_in_icon);
    g_object_set_data_full(G_OBJECT(right_zoom_in_btn), "icon-name", g_strdup("zoom-in"), g_free);
    gtk_widget_set_tooltip_text(right_zoom_in_btn, "Zoom in");
    atk_object_set_name(gtk_widget_get_accessible(right_zoom_in_btn), "Zoom in");
    gtk_box_pack_start(GTK_BOX(right_middle_box), right_zoom_in_btn, FALSE, FALSE, 1);
    g_signal_connect(G_OBJECT(right_zoom_in_btn), "clicked", G_CALLBACK(on_zoom_in_right), NULL);

    /* Right toolbar - Zoom out */
    GtkWidget *right_zoom_out_icon = create_toolbar_icon("zoom-out");
    GtkWidget *right_zoom_out_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(right_zoom_out_btn), right_zoom_out_icon);
    g_object_set_data_full(G_OBJECT(right_zoom_out_btn), "icon-name", g_strdup("zoom-out"), g_free);
    gtk_widget_set_tooltip_text(right_zoom_out_btn, "Zoom out");
    atk_object_set_name(gtk_widget_get_accessible(right_zoom_out_btn), "Zoom out");
    gtk_box_pack_start(GTK_BOX(right_middle_box), right_zoom_out_btn, FALSE, FALSE, 1);
    g_signal_connect(G_OBJECT(right_zoom_out_btn), "clicked", G_CALLBACK(on_zoom_out_right), NULL);

    /* Right toolbar separator */
    GtkWidget *right_sep_c = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(right_middle_box), right_sep_c, FALSE, FALSE, 5);

    /* Right toolbar - Page column */
    GtkWidget *right_column_icon = create_toolbar_icon("column");
    app.right_column_btn = gtk_radio_button_new(NULL);
    gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(app.right_column_btn), FALSE);
    gtk_button_set_image(GTK_BUTTON(app.right_column_btn), right_column_icon);
    g_object_set_data_full(G_OBJECT(app.right_column_btn), "icon-name", g_strdup("column"), g_free);
    gtk_widget_set_tooltip_text(app.right_column_btn, "Page column");
    atk_object_set_name(gtk_widget_get_accessible(app.right_column_btn), "Page column");
    g_object_set_data(G_OBJECT(app.right_column_btn), "layout-id", GINT_TO_POINTER(0 + 1));
    g_signal_connect(app.right_column_btn, "toggled", G_CALLBACK(on_layout_right_toggled), NULL);
    gtk_box_pack_start(GTK_BOX(right_middle_box), app.right_column_btn, FALSE, FALSE, 1);

    /* Right toolbar - Page double column */
    GtkWidget *right_double_column_icon = create_toolbar_icon("double-column");
    app.right_double_column_btn = gtk_radio_button_new_from_widget(GTK_RADIO_BUTTON(app.right_column_btn));
    gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(app.right_double_column_btn), FALSE);
    gtk_button_set_image(GTK_BUTTON(app.right_double_column_btn), right_double_column_icon);
    g_object_set_data_full(G_OBJECT(app.right_double_column_btn), "icon-name", g_strdup("double-column"), g_free);
    gtk_widget_set_tooltip_text(app.right_double_column_btn, "Page double column");
    atk_object_set_name(gtk_widget_get_accessible(app.right_double_column_btn), "Page double column");
    g_object_set_data(G_OBJECT(app.right_double_column_btn), "layout-id", GINT_TO_POINTER(1 + 1));
    g_signal_connect(app.right_double_column_btn, "toggled", G_CALLBACK(on_layout_right_toggled), NULL);
    gtk_box_pack_start(GTK_BOX(right_middle_box), app.right_double_column_btn, FALSE, FALSE, 1);

    /* Right toolbar - Page row */
    GtkWidget *right_row_icon = create_toolbar_icon("row");
    app.right_row_btn = gtk_radio_button_new_from_widget(GTK_RADIO_BUTTON(app.right_column_btn));
    gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(app.right_row_btn), FALSE);
    gtk_button_set_image(GTK_BUTTON(app.right_row_btn), right_row_icon);
    g_object_set_data_full(G_OBJECT(app.right_row_btn), "icon-name", g_strdup("row"), g_free);
    gtk_widget_set_tooltip_text(app.right_row_btn, "Page row");
    atk_object_set_name(gtk_widget_get_accessible(app.right_row_btn), "Page row");
    g_object_set_data(G_OBJECT(app.right_row_btn), "layout-id", GINT_TO_POINTER(2 + 1));
    g_signal_connect(app.right_row_btn, "toggled", G_CALLBACK(on_layout_right_toggled), NULL);
    gtk_box_pack_start(GTK_BOX(right_middle_box), app.right_row_btn, FALSE, FALSE, 1);

    /* Right toolbar separator */
    GtkWidget *right_sep_d = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(right_middle_box), right_sep_d, FALSE, FALSE, 5);

    return app.window;
}

void hide_right_pane(void) {
    if (app.right_pane) {
        gtk_widget_hide(GTK_WIDGET(app.right_pane));
    }
}


