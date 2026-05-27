#include <gtk/gtk.h>
#include <atk/atk.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <poppler.h>
#include <math.h>
#include "siters.h"
#include "sessions_model.h"
#include "session_model.h"
#include "document_model.h"


/* Forward declaration and struct definitions for tab management */
typedef struct TabDataStruct TabData;

/* State tracking for deferred, layout-aware restore */
typedef struct {
    TabData *tab;
    int restore_stage;      /* 0=init, 1=layout_settle, 2=measure, 3=apply, 4=finalize */
    int settle_attempts;    /* Track how many times we've waited for layout */
    double target_fraction; /* Saved fraction within page */
    int target_page;        /* Target page number (0-based) */
    double target_zoom;     /* Target zoom level */
    guint source_id;        /* Active idle source id for cancellation */
} RestoreState;

typedef struct TabDataStruct {
    PopplerDocument *doc;
    int n_pages;
    int cur_page;
    GtkWidget *drawing;
    double zoom;
    GdkRGBA page_color;
    /* per-document storage */
    char *current_file;
    GtkWidget *tab_label;  /* reference to the label widget in the tab */
    /* continuous view widgets */
    GtkWidget *scrolled;
    GtkWidget *pages_drawing; /* single drawing area used for both single and continuous views */
    GtkWidget *h_scrollbar;   /* manual horizontal scrollbar for row view */
    int layout_mode; /* 0=single-column,1=two-column,2=horizontal */
    double last_zoom; /* used to track when we need to rebuild the continuous view on zoom change */
    gboolean initial_scroll_pending; /* flag to indicate we need to scroll to the saved page after initial render */
    RestoreState *pending_restore; /* in-progress idle restore callback */
    double scroll_offset; /* used to restore scroll position in continuous view */
    gboolean is_helper; /* TRUE if this tab is in the right (helper) notebook */
} TabData;

typedef enum {
    SIDEBAR_NONE,
    SIDEBAR_SESSIONS,
    SIDEBAR_TOC,
    SIDEBAR_SETTINGS
} SidebarMode;

/* Current window geometry */
static gint current_width = 1000;
static gint current_height = 800;
static gint current_x = -1;
static gint current_y = -1;
static gboolean current_maximized = FALSE;

/* Sidebar for sessions, toc and settings */
static SidebarMode current_sidebar_mode = SIDEBAR_NONE;
static GtkWidget *sidebar;
static GtkWidget *sidebar_label;
static GtkWidget *main_hbox;
static GtkWidget *content_vbox;
static GtkWidget *window;

/* Sessions sidebar components */
static GtkWidget *sessions_container;
static GtkWidget *sessions_title;
static GtkWidget *sessions_entry;
static GtkWidget *sessions_add_btn;
static GtkWidget *sessions_remove_btn;
static GtkWidget *sessions_update_btn;
static GtkWidget *sessions_tree_view;
static GtkTreeStore *sessions_tree_store;
static sessions_model_t *sessions_model;
static gboolean sessions_tree_syncing = FALSE;
static gchar *last_tree_selection_key = NULL;

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

/* Session models - map from session name to session_model_t */
static GHashTable *session_models;

/* Document models - map from document URI to document_model_t */
static GHashTable *document_models = NULL;  // Hash table: URI -> document_model_t

/* Paned/Notebook components */
static GtkWidget *paned;
static GtkWidget *right_pane;
static GtkWidget *left_notebook;
static GtkWidget *right_notebook;
static gchar *current_selected_session = NULL;
static gboolean is_restoring_session_tabs = FALSE;

/* Page jump widget */
static GtkWidget *page_entry = NULL;
static GtkWidget *page_total_label = NULL;
static gboolean page_spin_syncing = FALSE;

/* Layout radio buttons (left and right toolbars) */
static GtkWidget *left_column_btn = NULL;
static GtkWidget *left_double_column_btn = NULL;
static GtkWidget *left_row_btn = NULL;
static GtkWidget *right_column_btn = NULL;
static GtkWidget *right_double_column_btn = NULL;
static GtkWidget *right_row_btn = NULL;

static TabData *get_current_left_tab(void);
static TabData *get_current_right_tab(void);
static void sync_page_widget_from_tab(TabData *tab);
static void on_page_entry_activate(GtkEntry *entry, gpointer user_data);

static void cancel_tab_restore(TabData *tab);
static void destroy_tab_data(gpointer data);
static void apply_layout_to_tab(TabData *tab, int layout);
static void on_layout_left_toggled(GtkToggleButton *btn, gpointer user_data);
static void on_layout_right_toggled(GtkToggleButton *btn, gpointer user_data);
static void sync_left_layout_buttons(TabData *tab);
static void sync_right_layout_buttons(TabData *tab);

/* Function prototypes */
void save_state(void);
void populate_sessions_treeview(void);
void hide_right_pane(void);
static void set_right_notebook_session(const gchar *session_name);
static void on_tab_close_clicked(GtkButton *btn, gpointer user_data);
static void on_page_entry_insert_text(GtkEditable *editable,
                                      const gchar *text,
                                      gint length,
                                      gint *position,
                                      gpointer user_data);
static void on_tab_scrolled_size_allocate(GtkWidget *widget, GdkRectangle *allocation, gpointer user_data);

/* Session tab persistence functions */
static void save_open_tabs_for_session(const char *session_name);
static void restore_open_tabs_for_session(const char *session_name);

/* PDF handling function prototypes */
static TabData *create_new_tab(GtkWidget *notebook);
static void load_file_into_tab(TabData *tab, const char *filename);
static void queue_draw(TabData *tab);
static gboolean on_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data);
static void build_continuous_view(TabData *tab);
static void scroll_to_page(TabData *tab, int page);
static void on_scroll_value_changed(GtkAdjustment *adj, gpointer user_data);
static gboolean on_drawing_scroll(GtkWidget *widget, GdkEventScroll *event, gpointer user_data);
static void open_file_in_notebook(GtkWidget *notebook, gboolean is_helper);
static void on_open_file_clicked(GtkButton *button, gpointer user_data);
static void on_open_helper_file_clicked(GtkButton *button, gpointer user_data);
static void update_last_read_for_notebook(GtkNotebook *notebook, GtkWidget *page, guint page_num);
static void on_left_notebook_switch_page(GtkNotebook *notebook, GtkWidget *page, guint page_num, gpointer user_data);
static void on_right_notebook_switch_page(GtkNotebook *notebook, GtkWidget *page, guint page_num, gpointer user_data);
static int find_matching_tab_index(GtkNotebook *notebook, const char *target_uri);
static void start_initial_scroll_restore(TabData *tab, int target_page, double target_zoom, double target_fraction);
static char* make_document_key(const char *uri, gboolean is_helper);

/* Save state on closing app */
static void cancel_tab_restore(TabData *tab) {
    if (!tab || !tab->pending_restore) return;
    if (tab->pending_restore->source_id) {
        g_source_remove(tab->pending_restore->source_id);
    }
    tab->pending_restore->tab = NULL; // invalidate the pointer
    g_free(tab->pending_restore);
    tab->pending_restore = NULL;
}

static void destroy_tab_data(gpointer data) {
    TabData *tab = data;
    if (!tab) return;
    cancel_tab_restore(tab);
    if (tab->doc) {
        g_object_unref(tab->doc);
    }
    g_free(tab->current_file);
    g_free(tab);
}

static void on_window_destroy(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    (void)user_data;
    save_state();
    gtk_main_quit();
}

static gboolean on_window_configure(GtkWidget *widget, GdkEventConfigure *event, gpointer user_data) {
    (void)user_data;
    
    // Update current geometry
    current_width = event->width;
    current_height = event->height;
    current_x = event->x;
    current_y = event->y;
    current_maximized = gtk_window_is_maximized(GTK_WINDOW(widget));
    
    return FALSE; // Allow further processing
}

static void update_window_title_for_session(const char *session_name) {
    if (!window) return;

    const char *name = (session_name && *session_name) ? session_name : "Default";
    gchar *title = g_strdup_printf("Siters - %s", name);
    gtk_window_set_title(GTK_WINDOW(window), title);
    g_free(title);
}

/* State management functions */
static void json_emit_document(JsonBuilder *builder, const char *uri, const char *side, document_model_t *dm) {
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "uri");
    json_builder_add_string_value(builder, uri);
    json_builder_set_member_name(builder, "side");
    json_builder_add_string_value(builder, side);
    json_builder_set_member_name(builder, "zoom");
    json_builder_add_double_value(builder, document_model_get_zoom(dm));
    json_builder_set_member_name(builder, "current_page");
    json_builder_add_int_value(builder, document_model_get_current_page(dm));
    json_builder_set_member_name(builder, "page_count");
    json_builder_add_int_value(builder, document_model_get_page_count(dm));
    json_builder_set_member_name(builder, "visualization_mode");
    json_builder_add_int_value(builder, document_model_get_visualization_mode(dm));
    json_builder_set_member_name(builder, "horizontal_scroll");
    json_builder_add_boolean_value(builder, document_model_get_horizontal_scroll(dm));
    json_builder_set_member_name(builder, "scroll_offset");
    json_builder_add_double_value(builder, document_model_get_scroll_offset(dm));
    json_builder_set_member_name(builder, "intra_page_fraction");
    json_builder_add_double_value(builder, document_model_get_intra_page_fraction(dm));
    json_builder_end_object(builder);
}

static void json_emit_session_docs(JsonBuilder *builder, const GList *uris, const char *side_prefix, GHashTable *models) {
    json_builder_begin_array(builder);
    for (const GList *d = uris; d; d = d->next) {
        const char *uri = (const char *)d->data;
        const char *side = (strcmp(side_prefix, "right") == 0) ? "right" : "left";
        char *key = make_document_key(uri, side[0] == 'r');
        document_model_t *dm = models ? g_hash_table_lookup(models, key) : NULL;
        if (dm) {
            json_emit_document(builder, uri, side, dm);
        }
        g_free(key);
    }
    json_builder_end_array(builder);
}

static document_model_t* json_parse_document(JsonObject *obj) {
    const char *uri = json_object_get_string_member_with_default(obj, "uri", NULL);
    if (!uri || !*uri) return NULL;

    document_model_t *dm = document_model_new();
    document_model_set_url(dm, uri);
    document_model_set_zoom(dm, json_object_get_double_member_with_default(obj, "zoom", 1.0));
    document_model_set_current_page(dm, (int)json_object_get_int_member_with_default(obj, "current_page", 1));
    document_model_set_page_count(dm, (int)json_object_get_int_member_with_default(obj, "page_count", 0));
    document_model_set_visualization_mode(dm, (int)json_object_get_int_member_with_default(obj, "visualization_mode", 0));
    document_model_set_horizontal_scroll(dm, json_object_get_boolean_member_with_default(obj, "horizontal_scroll", FALSE));
    document_model_set_scroll_offset(dm, json_object_get_double_member_with_default(obj, "scroll_offset", 0.0));
    document_model_set_intra_page_fraction(dm, json_object_get_double_member_with_default(obj, "intra_page_fraction", 0.0));
    return dm;
}

void save_state(void) {
    // Save current session's open tabs before saving state
    if (current_selected_session) {
        save_open_tabs_for_session(current_selected_session);
    }

    const gchar *config_dir = g_get_user_config_dir();
    gchar *app_config_dir = g_build_filename(config_dir, "siters", NULL);
    g_mkdir_with_parents(app_config_dir, 0755);

    JsonBuilder *builder = json_builder_new();
    json_builder_begin_object(builder);

    json_builder_set_member_name(builder, "window");
    json_builder_begin_object(builder);
    if (window) {
        json_builder_set_member_name(builder, "width");
        json_builder_add_int_value(builder, current_width);
        json_builder_set_member_name(builder, "height");
        json_builder_add_int_value(builder, current_height);
        json_builder_set_member_name(builder, "x");
        json_builder_add_int_value(builder, current_x);
        json_builder_set_member_name(builder, "y");
        json_builder_add_int_value(builder, current_y);
        json_builder_set_member_name(builder, "maximized");
        json_builder_add_boolean_value(builder, current_maximized);
    }
    json_builder_end_object(builder);

    json_builder_set_member_name(builder, "sessions");
    json_builder_begin_object(builder);
    if (sessions_model) {
        const GList *session_names = sessions_model_get_session_names(sessions_model);
        if (session_names) {
            json_builder_set_member_name(builder, "names");
            json_builder_begin_array(builder);
            for (const GList *iter = session_names; iter; iter = iter->next)
                json_builder_add_string_value(builder, (const char *)iter->data);
            json_builder_end_array(builder);

            const char *last_session = sessions_model_get_last_open_session(sessions_model);
            if (last_session) {
                json_builder_set_member_name(builder, "last_open_session");
                json_builder_add_string_value(builder, last_session);
            }

            json_builder_set_member_name(builder, "data");
            json_builder_begin_object(builder);
            for (const GList *iter = session_names; iter; iter = iter->next) {
                const char *session_name = (const char *)iter->data;
                json_builder_set_member_name(builder, session_name);
                json_builder_begin_object(builder);

                session_model_t *session = g_hash_table_lookup(session_models, session_name);
                if (session) {
                    json_builder_set_member_name(builder, "documents");
                    json_emit_session_docs(builder, session_model_get_document_urls(session), "left", document_models);

                    json_builder_set_member_name(builder, "helper_documents");
                    json_emit_session_docs(builder, session_model_get_helper_document_urls(session), "right", document_models);

                    const char *lr = session_model_get_last_read_document(session);
                    json_builder_set_member_name(builder, "last_read_document");
                    json_builder_add_string_value(builder, lr ? lr : "");

                    const char *pc = session_model_get_page_color(session);
                    json_builder_set_member_name(builder, "page_color");
                    json_builder_add_string_value(builder, pc ? pc : "#FFFFFF");

                    const char *lrh = session_model_get_last_read_help_document(session);
                    json_builder_set_member_name(builder, "last_read_help_document");
                    json_builder_add_string_value(builder, lrh ? lrh : "");

                    const char *hpc = session_model_get_helper_page_color(session);
                    json_builder_set_member_name(builder, "helper_page_color");
                    json_builder_add_string_value(builder, hpc ? hpc : "#FFFFFF");
                } else {
                    json_builder_set_member_name(builder, "documents");
                    json_builder_begin_array(builder); json_builder_end_array(builder);
                    json_builder_set_member_name(builder, "helper_documents");
                    json_builder_begin_array(builder); json_builder_end_array(builder);
                    json_builder_set_member_name(builder, "last_read_document");
                    json_builder_add_string_value(builder, "");
                    json_builder_set_member_name(builder, "page_color");
                    json_builder_add_string_value(builder, "#FFFFFF");
                    json_builder_set_member_name(builder, "last_read_help_document");
                    json_builder_add_string_value(builder, "");
                    json_builder_set_member_name(builder, "helper_page_color");
                    json_builder_add_string_value(builder, "#FFFFFF");
                }

                json_builder_end_object(builder);
            }
            json_builder_end_object(builder);
        }
    }
    json_builder_end_object(builder);

    json_builder_end_object(builder);

    JsonNode *root = json_builder_get_root(builder);
    JsonGenerator *gen = json_generator_new();
    json_generator_set_root(gen, root);
    json_generator_set_pretty(gen, TRUE);

    gchar *json_str = json_generator_to_data(gen, NULL);
    gchar *config_file = g_build_filename(app_config_dir, "siters.json", NULL);
    g_file_set_contents(config_file, json_str, -1, NULL);

    g_free(json_str);
    g_free(config_file);
    g_free(app_config_dir);
    g_object_unref(gen);
    json_node_free(root);
    g_object_unref(builder);
}

static void load_session_doc_array(JsonArray *arr, session_model_t *session, const char *default_side) {
    if (!arr) return;
    guint len = json_array_get_length(arr);
    for (guint i = 0; i < len; i++) {
        JsonObject *obj = json_array_get_object_element(arr, i);
        if (!obj) continue;
        const char *uri = json_object_get_string_member_with_default(obj, "uri", NULL);
        if (!uri || !*uri) continue;

        const char *side = json_object_get_string_member_with_default(obj, "side", default_side);
        gboolean is_helper = (g_strcmp0(side, "right") == 0);

        if (is_helper)
            session_model_add_helper_document_url(session, uri);
        else
            session_model_add_document_url(session, uri);

        document_model_t *dm = json_parse_document(obj);
        if (dm) {
            if (!document_models) {
                document_models = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                                       (GDestroyNotify)document_model_free);
            }
            char *key = make_document_key(uri, is_helper);
            g_hash_table_insert(document_models, key, dm);
        }
    }
}

void load_state(void) {
    const gchar *config_dir = g_get_user_config_dir();
    gchar *app_config_dir = g_build_filename(config_dir, "siters", NULL);
    gchar *config_file = g_build_filename(app_config_dir, "siters.json", NULL);

    JsonParser *parser = json_parser_new();
    GError *error = NULL;

    if (!json_parser_load_from_file(parser, config_file, &error)) {
        g_clear_error(&error);
        g_object_unref(parser);
        g_free(config_file);
        g_free(app_config_dir);
        return;
    }

    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
        g_object_unref(parser);
        g_free(config_file);
        g_free(app_config_dir);
        return;
    }

    JsonObject *root_obj = json_node_get_object(root);

    JsonObject *win = json_object_get_object_member(root_obj, "window");
    if (win && window) {
        int w = (int)json_object_get_int_member_with_default(win, "width", 1000);
        int h = (int)json_object_get_int_member_with_default(win, "height", 800);
        int x = (int)json_object_get_int_member_with_default(win, "x", -1);
        int y = (int)json_object_get_int_member_with_default(win, "y", -1);
        gboolean max = json_object_get_boolean_member_with_default(win, "maximized", FALSE);

        gtk_window_set_default_size(GTK_WINDOW(window), w, h);
        if (x >= 0 && y >= 0)
            gtk_window_move(GTK_WINDOW(window), x, y);
        if (max)
            gtk_window_maximize(GTK_WINDOW(window));

        current_width = w;
        current_height = h;
        current_x = x;
        current_y = y;
        current_maximized = max;
    }

    JsonObject *sessions_obj = json_object_get_object_member(root_obj, "sessions");
    if (sessions_obj) {
        if (!sessions_model)
            sessions_model = sessions_model_new();

        JsonArray *names_arr = json_object_get_array_member(sessions_obj, "names");
        if (names_arr) {
            guint nlen = json_array_get_length(names_arr);
            for (guint i = 0; i < nlen; i++) {
                const char *name = json_array_get_string_element(names_arr, i);
                if (!name || !*name) continue;
                sessions_model_add_session_name(sessions_model, name);

                session_model_t *session = session_model_new();
                session_model_set_session_name(session, name);

                JsonObject *sdata = json_object_get_object_member(sessions_obj, "data");
                if (sdata) {
                    JsonObject *sd = json_object_get_object_member(sdata, name);
                    if (sd) {
                        load_session_doc_array(json_object_get_array_member(sd, "documents"), session, "left");
                        load_session_doc_array(json_object_get_array_member(sd, "helper_documents"), session, "right");
                        session_model_set_last_read_document(session,
                            json_object_get_string_member_with_default(sd, "last_read_document", ""));
                        session_model_set_page_color(session,
                            json_object_get_string_member_with_default(sd, "page_color", "#FFFFFF"));
                        session_model_set_last_read_help_document(session,
                            json_object_get_string_member_with_default(sd, "last_read_help_document", ""));
                        session_model_set_helper_page_color(session,
                            json_object_get_string_member_with_default(sd, "helper_page_color", "#FFFFFF"));
                    }
                }

                g_hash_table_insert(session_models, g_strdup(name), session);
            }
        }

        const char *last_session = json_object_get_string_member_with_default(sessions_obj, "last_open_session", NULL);
        if (last_session && *last_session) {
            sessions_model_set_last_open_session(sessions_model, last_session);
        } else {
            sessions_model_set_last_open_session(sessions_model, "Default");
        }
    }

    populate_sessions_treeview();

    if (sessions_model && current_selected_session) {
        const char *loaded_session = sessions_model_get_last_open_session(sessions_model);
        if (loaded_session && strcmp(loaded_session, current_selected_session) != 0) {
            // Update current_selected_session and restore open tabs for the loaded session
            g_free(current_selected_session);
            current_selected_session = g_strdup(loaded_session);
            restore_open_tabs_for_session(loaded_session);
            sync_page_widget_from_tab(get_current_left_tab());
            update_window_title_for_session(current_selected_session);
        } else if (loaded_session) {
            restore_open_tabs_for_session(loaded_session);
            sync_page_widget_from_tab(get_current_left_tab());
        }
    }

    g_object_unref(parser);
    g_free(config_file);
    g_free(app_config_dir);
}

/* Helper: Calculate PPI-based scale for a tab */
static double get_ppi_scale(TabData *tab) {
    double eff = tab->zoom > 0 ? tab->zoom : 300.0;
    return eff / 72.0;
}

/* Helper: Calculate offset to top of a given page at current PPI zoom */
static double calculate_page_top_offset_ppi(TabData *tab, int page_idx) {
    if (!tab || !tab->doc || page_idx < 0 || page_idx >= tab->n_pages) {
        return 0.0;
    }
    
    const double spacing = 6.0;
    double scale = get_ppi_scale(tab);
    
    double y_offset = spacing;
    if (tab->layout_mode == 2) {
        for (int i = 0; i < page_idx; ++i) {
            PopplerPage *page = poppler_document_get_page(tab->doc, i);
            if (!page) continue;
            double pw, ph;
            poppler_page_get_size(page, &pw, &ph);
            if (pw > 0 && ph > 0) {
                double page_w = pw * scale;
                y_offset += page_w + spacing;
            }
            g_object_unref(page);
        }
    } else if (tab->layout_mode == 1) {
        int row = page_idx / 2;
        for (int r = 0; r < row; r++) {
            double row_h = 0.0;
            for (int p = 0; p < 2; p++) {
                int idx = r * 2 + p;
                if (idx >= tab->n_pages) break;
                PopplerPage *pp = poppler_document_get_page(tab->doc, idx);
                if (!pp) continue;
                double pw, ph;
                poppler_page_get_size(pp, &pw, &ph);
                if (pw > 0 && ph > 0) {
                    double h = ph * scale;
                    if (h > row_h) row_h = h;
                }
                g_object_unref(pp);
            }
            if (row_h < 1.0) row_h = 1.0;
            y_offset += row_h + spacing;
        }
    } else {
        for (int i = 0; i < page_idx; ++i) {
            PopplerPage *page = poppler_document_get_page(tab->doc, i);
            if (!page) continue;
            
            double pw, ph;
            poppler_page_get_size(page, &pw, &ph);
            if (pw > 0 && ph > 0) {
                double page_h = ph * scale;
                y_offset += page_h + spacing;
            }
            g_object_unref(page);
        }
    }
    return y_offset;
}

/* Helper: Get the height of a specific page at current PPI zoom */
static double get_page_height_ppi(TabData *tab, int page_idx) {
    if (!tab || !tab->doc || page_idx < 0 || page_idx >= tab->n_pages) {
        return 0.0;
    }
    
    PopplerPage *page = poppler_document_get_page(tab->doc, page_idx);
    if (!page) return 0.0;
    
    double pw, ph;
    poppler_page_get_size(page, &pw, &ph);
    g_object_unref(page);
    
    if (pw <= 0 || ph <= 0) return 0.0;
    
    double eff_zoom = tab->zoom > 0 ? tab->zoom : 300.0;
    double scale = eff_zoom / 72.0;
    return ph * scale;
}

/* Build a compound key "side:uri" to differentiate left vs right notebook state */
static char* make_document_key(const char *uri, gboolean is_helper) {
    const char *side = is_helper ? "right" : "left";
    return g_strdup_printf("%s:%s", side, uri);
}

static void update_document_model_from_tab(TabData *tab) {
    if (!tab || !tab->current_file || !document_models) return;
    
    char *uri = g_filename_to_uri(tab->current_file, NULL, NULL);
    if (!uri) return;
    
    char *key = make_document_key(uri, tab->is_helper);
    
    // Get or create document model
    document_model_t *doc_model = g_hash_table_lookup(document_models, key);
    if (!doc_model) {
        doc_model = document_model_new();
        document_model_set_url(doc_model, uri);
        g_hash_table_insert(document_models, key, doc_model);
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
            PopplerPage *p = poppler_document_get_page(tab->doc, i);
            if (!p) continue;
            double pw, ph;
            poppler_page_get_size(p, &pw, &ph);
            if (pw > 0 && ph > 0) {
                x_top += pw * scale + spacing;
            }
            g_object_unref(p);
        }
        PopplerPage *cp = poppler_document_get_page(tab->doc, tab->cur_page);
        if (cp) {
            double pw, ph;
            poppler_page_get_size(cp, &pw, &ph);
            if (pw > 0 && ph > 0)
                page_w = pw * scale;
            g_object_unref(cp);
        }
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
                    PopplerPage *pp = poppler_document_get_page(tab->doc, idx);
                    if (!pp) continue;
                    double pw, ph;
                    poppler_page_get_size(pp, &pw, &ph);
                    if (pw > 0 && ph > 0) {
                        double h = ph * scale;
                        if (h > row_h) row_h = h;
                    }
                    g_object_unref(pp);
                }
                y_top += row_h + spacing;
            }
            PopplerPage *cp = poppler_document_get_page(tab->doc, tab->cur_page);
            if (cp) {
                double pw, ph;
                poppler_page_get_size(cp, &pw, &ph);
                if (pw > 0 && ph > 0)
                    page_h = ph * scale;
                g_object_unref(cp);
            }
        } else {
            for (int i = 0; i < tab->cur_page; ++i) {
                PopplerPage *p = poppler_document_get_page(tab->doc, i);
                if (!p) continue;
                double pw, ph;
                poppler_page_get_size(p, &pw, &ph);
                double pi_h = ph * scale;
                y_top += pi_h + spacing;
                g_object_unref(p);
            }
            PopplerPage *cp = poppler_document_get_page(tab->doc, tab->cur_page);
            if (cp) {
                double pw, ph;
                poppler_page_get_size(cp, &pw, &ph);
                if (pw > 0 && ph > 0)
                    page_h = ph * scale;
                g_object_unref(cp);
            }
        }

        double intra = scroll_y - y_top;
        double fraction = page_h > 0 ? intra / page_h : 0.0;
        if (fraction < 0) fraction = 0;
        if (fraction > 1) fraction = 1;
        document_model_set_intra_page_fraction(doc_model, fraction);
    }

    g_free(uri);
}

static void restore_document_model_to_tab(TabData *tab) {
    if (!tab || !tab->current_file || !document_models) return;

    char *uri = g_filename_to_uri(tab->current_file, NULL, NULL);
    if (!uri) return;

    char *key = make_document_key(uri, tab->is_helper);
    document_model_t *doc_model = g_hash_table_lookup(document_models, key);
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
        start_initial_scroll_restore(tab, 0, 300.0, 0.0);
    }

    g_free(uri);
}

static void switch_to_session(const char *session_name) {
    if (!session_name || !*session_name) return;

    if (current_selected_session && g_strcmp0(current_selected_session, session_name) == 0) {
        return;
    }

    if (current_selected_session) {
        save_open_tabs_for_session(current_selected_session);
        g_free(current_selected_session);
    }

    current_selected_session = g_strdup(session_name);
    restore_open_tabs_for_session(session_name);
    sync_page_widget_from_tab(get_current_left_tab()); 
    update_window_title_for_session(current_selected_session);

    if (sessions_model) {
        sessions_model_set_last_open_session(sessions_model, session_name);
    }
}

static void reset_sessions_tree_selection_guard(void) {
    g_clear_pointer(&last_tree_selection_key, g_free);
}

void populate_sessions_treeview(void) {
    if (!sessions_tree_store || !sessions_model) return;

    sessions_tree_syncing = TRUE;
    reset_sessions_tree_selection_guard();

    gtk_tree_store_clear(sessions_tree_store);

    const GList *session_names = sessions_model_get_session_names(sessions_model);
    for (const GList *s = session_names; s; s = s->next) {
        const char *session_name = (const char *)s->data;
        GtkTreeIter parent;

        gtk_tree_store_append(sessions_tree_store, &parent, NULL);
        gtk_tree_store_set(
            sessions_tree_store, &parent,
            SESSION_COL_LABEL, session_name,
            SESSION_COL_ROW_KIND, SESSION_ROW_SESSION,
            SESSION_COL_SESSION_NAME, session_name,
            SESSION_COL_DOC_URI, "",
            -1
        );

        session_model_t *session = g_hash_table_lookup(session_models, session_name);
        if (!session) continue;

        // ONLY document_urls (left notebook docs), no helper_document_urls
        const GList *docs = session_model_get_document_urls(session);
        for (const GList *d = docs; d; d = d->next) {
            const char *uri = (const char *)d->data;
            GtkTreeIter child;

            char *filename = g_filename_from_uri(uri, NULL, NULL);
            char *basename = filename ? g_path_get_basename(filename) : g_strdup(uri);

            gtk_tree_store_append(sessions_tree_store, &child, &parent);
            gtk_tree_store_set(
                sessions_tree_store, &child,
                SESSION_COL_LABEL, basename,
                SESSION_COL_ROW_KIND, SESSION_ROW_FILE,
                SESSION_COL_SESSION_NAME, session_name,
                SESSION_COL_DOC_URI, uri,
                -1
            );

            g_free(basename);
            g_free(filename);
        }
    }

    sessions_tree_syncing = FALSE;
}

/* Helper: Determine current page from scroll position */
static int compute_page_from_scroll(TabData *tab, double scroll_y) {
    if (!tab || !tab->doc || tab->n_pages <= 0) return 0;
    
    const double spacing = 6.0;
    double scale = get_ppi_scale(tab);
    double y = spacing;
    int visible_page = tab->n_pages - 1;
    
    if (tab->layout_mode == 2) {
        for (int i = 0; i < tab->n_pages; ++i) {
            PopplerPage *page = poppler_document_get_page(tab->doc, i);
            if (!page) continue;
            double pw, ph;
            poppler_page_get_size(page, &pw, &ph);
            if (pw <= 0 || ph <= 0) { g_object_unref(page); continue; }
            double page_w = pw * scale;
            if (scroll_y >= y && scroll_y < y + page_w) {
                g_object_unref(page);
                return i;
            }
            y += page_w + spacing;
            g_object_unref(page);
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
                break;
            }
            y += row_h + spacing;
        }
    } else {
        for (int i = 0; i < tab->n_pages; ++i) {
            double page_h = get_page_height_ppi(tab, i);
            if (page_h <= 0) continue;
            if (scroll_y >= y && scroll_y < y + page_h) {
                return i;
            }
            y += page_h + spacing;
        }
    }
    
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
        g_free(restore);
        return FALSE;
    }
    
    TabData *tab = restore->tab;
    
    /* ========== STAGE 0: Initialize & Apply Zoom ========== */
    if (restore->restore_stage == 0) {
        /* Validate basic state */
        if (!gtk_widget_get_realized(tab->scrolled)) {
            /* Widget not ready, retry next idle */
            restore->source_id = g_idle_add(do_initial_scroll_stage, restore);
            return FALSE;
        }
        
        GtkAllocation alloc;
        gtk_widget_get_allocation(tab->scrolled, &alloc);
        if (alloc.width < 1 || alloc.height < 1) {
            /* Layout not finalized, retry next idle */
            restore->source_id = g_idle_add(do_initial_scroll_stage, restore);
            return FALSE;
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
            if (restore->settle_attempts < 50) {  /* Try up to 50 times (~250ms) */
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
                PopplerPage *p = poppler_document_get_page(tab->doc, i);
                if (!p) continue;
                double pw, ph;
                poppler_page_get_size(p, &pw, &ph);
                if (pw > 0 && ph > 0)
                    x_offset += pw * scale + spacing;
                g_object_unref(p);
            }
            PopplerPage *cp = poppler_document_get_page(tab->doc, restore->target_page);
            if (cp) {
                double pw, ph;
                poppler_page_get_size(cp, &pw, &ph);
                if (pw > 0 && ph > 0)
                    page_w = pw * scale;
                g_object_unref(cp);
            }
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
        
        /* Update document model */
        update_document_model_from_tab(tab);
        
        /* Mark restore as complete */
        tab->initial_scroll_pending = FALSE;
        tab->pending_restore = NULL;
        restore->source_id = 0;
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

static void on_horiz_scroll_toggle(GtkToggleButton *button, gpointer user_data) {
    GtkImage *image = GTK_IMAGE(user_data);
    if (gtk_toggle_button_get_active(button)) {
        gtk_image_set_from_file(image, "./data/icons/horiz-scroll-on.png");
    } else {
        gtk_image_set_from_file(image, "./data/icons/horiz-scroll-off.png");
    }
}

static void on_title_bar_toggle(GtkToggleButton *button, gpointer user_data) {
    GtkImage *image = GTK_IMAGE(user_data);
    gboolean active = gtk_toggle_button_get_active(button);
    if (active) {
        gtk_image_set_from_file(image, "./data/icons/title-bar-on.png");
        gtk_window_set_decorated(GTK_WINDOW(window), TRUE);
    } else {
        gtk_image_set_from_file(image, "./data/icons/title-bar-off.png");
        // If window is maximized, need to unmaximize, remove decoration, then re-maximize
        // to ensure it fills the screen properly
        gboolean was_maximized = gtk_window_is_maximized(GTK_WINDOW(window));
        if (was_maximized) {
            gtk_window_unmaximize(GTK_WINDOW(window));
        }
        gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
        if (was_maximized) {
            gtk_window_maximize(GTK_WINDOW(window));
        }
    }
    // Force layout update after changing decoration
    gtk_widget_queue_resize(window);
}

static void on_helper_toggle(GtkToggleButton *button, gpointer user_data) {
    GtkImage *image = GTK_IMAGE(user_data);
    gboolean active = gtk_toggle_button_get_active(button);

    if (active) {
        gtk_image_set_from_file(image, "./data/icons/sidebar-helper-on.png");
        if (right_pane) {
            gtk_widget_show_all(GTK_WIDGET(right_pane));
            set_right_notebook_session(current_selected_session ? current_selected_session : "Default");
            sync_right_layout_buttons(get_current_right_tab());
        }
    } else {
        // Save current helper tabs before hiding
        if (current_selected_session) {
            save_open_tabs_for_session(current_selected_session);
        }
        gtk_image_set_from_file(image, "./data/icons/sidebar-helper-off.png");
        if (right_pane) {
            gtk_widget_hide(GTK_WIDGET(right_pane));
        }
    }
}

static void on_minimize_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    GtkWindow *window = GTK_WINDOW(user_data);
    gtk_window_iconify(window);
}

static void on_maximize_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    GtkWindow *window = GTK_WINDOW(user_data);
    if (!window) return;
    if (gtk_window_is_maximized(window))
        gtk_window_unmaximize(window);
    else
        gtk_window_maximize(window);
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
    
    const char *session_name = gtk_entry_get_text(GTK_ENTRY(sessions_entry));
    if (session_name && strlen(session_name) > 0) {
        // Check if session name already exists
        const GList *existing_sessions = sessions_model_get_session_names(sessions_model);
        for (const GList *iter = existing_sessions; iter != NULL; iter = iter->next) {
            if (strcmp((const char*)iter->data, session_name) == 0) {
                // Show error dialog
                GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(window),
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
        sessions_model_add_session_name(sessions_model, session_name);

        // Create per-session model so notebook restore can find it
        if (session_models && !g_hash_table_lookup(session_models, session_name)) {
            session_model_t *session = session_model_new();
            session_model_set_session_name(session, session_name);
            g_hash_table_insert(session_models, g_strdup(session_name), session);
        }
        
        // Update tree view
        populate_sessions_treeview();
        
        // Clear entry
        gtk_entry_set_text(GTK_ENTRY(sessions_entry), "");

        // Save state to persist the new session
        save_state();
    }
}

static void on_sessions_remove_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(sessions_tree_view));
    GtkTreeIter iter;
    GtkTreeModel *model;
    
    if (gtk_tree_selection_get_selected(selection, &model, &iter)) {
        gchar *session_name;
        gtk_tree_model_get(model, &iter, 0, &session_name, -1);
        
        // Prevent removing the "Default" session
        if (strcmp(session_name, "Default") == 0) {
            // Show a message dialog that Default session cannot be removed
            GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(window),
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
        sessions_model_remove_session_name(sessions_model, session_name);
        
        // Update tree view
        populate_sessions_treeview();

        // If the removed session is currently selected, switch to "Default" or first remaining session
        if (current_selected_session && strcmp(current_selected_session, session_name) == 0) {
            g_free(current_selected_session);
            current_selected_session = g_strdup("Default"); // or first remaining session

            restore_open_tabs_for_session(current_selected_session);
            sync_page_widget_from_tab(get_current_left_tab());

            if (sessions_model) {
                sessions_model_set_last_open_session(sessions_model, current_selected_session);
            }

            update_window_title_for_session(current_selected_session);
        }
        
        g_free(session_name);

        // Save state to persist the removed session
        save_state();
    }
}

static void on_sessions_update_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(sessions_tree_view));
    GtkTreeIter iter;
    GtkTreeModel *model;
    
    if (gtk_tree_selection_get_selected(selection, &model, &iter)) {
        const char *new_name = gtk_entry_get_text(GTK_ENTRY(sessions_entry));
        if (new_name && strlen(new_name) > 0) {
            gchar *old_name;
            gtk_tree_model_get(model, &iter, 0, &old_name, -1);
            
            // Prevent renaming the "Default" session
            if (strcmp(old_name, "Default") == 0) {
                GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(window),
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
            const GList *existing_sessions = sessions_model_get_session_names(sessions_model);
            for (const GList *iter_check = existing_sessions; iter_check != NULL; iter_check = iter_check->next) {
                if (strcmp((const char*)iter_check->data, old_name) != 0 && 
                    strcmp((const char*)iter_check->data, new_name) == 0) {
                    // Show error dialog
                    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(window),
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
            
            // Update in model
            sessions_model_remove_session_name(sessions_model, old_name);
            sessions_model_add_session_name(sessions_model, new_name);
            
            // Update tree view
            populate_sessions_treeview();

            // If the updated session is currently selected, update current_selected_session and window title
            if (current_selected_session && strcmp(current_selected_session, old_name) == 0) {
                g_free(current_selected_session);
                current_selected_session = g_strdup(new_name);
                if (sessions_model) {
                    sessions_model_set_last_open_session(sessions_model, current_selected_session);
                }
                update_window_title_for_session(current_selected_session);
            }
            
            // Clear entry
            gtk_entry_set_text(GTK_ENTRY(sessions_entry), "");
            
            g_free(old_name);
        }
    }
}

static void set_right_notebook_session(const gchar *session_name) {
    if (!right_notebook || !session_name || !session_models) return;

    is_restoring_session_tabs = TRUE;

    // Clear current right notebook
    while (gtk_notebook_get_n_pages(GTK_NOTEBOOK(right_notebook)) > 0) {
        gtk_notebook_remove_page(GTK_NOTEBOOK(right_notebook), 0);
    }

    // Get the session model
    session_model_t *session = g_hash_table_lookup(session_models, session_name);
    if (!session) {
        is_restoring_session_tabs = FALSE;
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
            TabData *tab = create_new_tab(right_notebook);
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
    if (matched_index >= 0 && matched_index < gtk_notebook_get_n_pages(GTK_NOTEBOOK(right_notebook))) {
        gtk_notebook_set_current_page(GTK_NOTEBOOK(right_notebook), matched_index);
    }

    // If no helper documents, show an empty right notebook
    if (gtk_notebook_get_n_pages(GTK_NOTEBOOK(right_notebook)) == 0) {
        gtk_widget_show_all(right_notebook);
    }

    is_restoring_session_tabs = FALSE;
}

static void on_sessions_treeview_cursor_changed(GtkTreeView *tree_view, gpointer user_data) {
    (void)user_data;
    if (sessions_tree_syncing) return;

    GtkTreeSelection *sel = gtk_tree_view_get_selection(tree_view);
    GtkTreeModel *model = NULL;
    GtkTreeIter iter;
    if (!gtk_tree_selection_get_selected(sel, &model, &iter)) return;

    gint row_kind = SESSION_ROW_SESSION;
    gchar *session_name = NULL;
    gchar *doc_uri = NULL;

    gtk_tree_model_get(model, &iter,
        SESSION_COL_ROW_KIND, &row_kind,
        SESSION_COL_SESSION_NAME, &session_name,
        SESSION_COL_DOC_URI, &doc_uri,
        -1);

    if (!session_name || !*session_name) goto cleanup;

    /* Build stable key for dedupe */
    gchar *key = (row_kind == SESSION_ROW_FILE && doc_uri && *doc_uri)
        ? g_strdup_printf("F|%s|%s", session_name, doc_uri)
        : g_strdup_printf("S|%s", session_name);

    if (last_tree_selection_key && g_strcmp0(last_tree_selection_key, key) == 0) {
        g_free(key);
        goto cleanup; /* noisy repeat */
    }

    g_free(last_tree_selection_key);
    last_tree_selection_key = key;

    switch_to_session(session_name);

    if (row_kind == SESSION_ROW_FILE && doc_uri && *doc_uri) {
        int idx = find_matching_tab_index(GTK_NOTEBOOK(left_notebook), doc_uri);
        if (idx >= 0) {
            gtk_notebook_set_current_page(GTK_NOTEBOOK(left_notebook), idx);
        }
    }

cleanup:
    g_free(session_name);
    g_free(doc_uri);
}

static void on_sessions_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;

    if (current_sidebar_mode == SIDEBAR_SESSIONS) {
        gtk_container_remove(GTK_CONTAINER(main_hbox), sidebar);
        gtk_box_reorder_child(GTK_BOX(main_hbox), content_vbox, 1);
        current_sidebar_mode = SIDEBAR_NONE;
    } else {
        if (gtk_widget_get_parent(sidebar) != NULL) {
            gtk_container_remove(GTK_CONTAINER(main_hbox), sidebar);
        }
        
        // Hide other sidebar contents
        gtk_widget_hide(sidebar_label);
        gtk_widget_hide(sessions_container);
        
        // Show sessions container
        gtk_widget_show_all(sessions_container);
        
        gtk_box_pack_start(GTK_BOX(main_hbox), sidebar, FALSE, FALSE, 0);
        gtk_box_reorder_child(GTK_BOX(main_hbox), content_vbox, 2);
        gtk_widget_show(sidebar);
        current_sidebar_mode = SIDEBAR_SESSIONS;
    }
}

static void on_toc_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    if (current_sidebar_mode == SIDEBAR_TOC) {
        gtk_container_remove(GTK_CONTAINER(main_hbox), sidebar);
        gtk_box_reorder_child(GTK_BOX(main_hbox), content_vbox, 1);
        current_sidebar_mode = SIDEBAR_NONE;
    } else {
        if (gtk_widget_get_parent(sidebar) != NULL) {
            gtk_container_remove(GTK_CONTAINER(main_hbox), sidebar);
        }
        
        // Hide other sidebar contents
        gtk_widget_hide(sidebar_label);
        gtk_widget_hide(sessions_container);
        
        // Show sidebar_label with TOC text
        gtk_label_set_text(GTK_LABEL(sidebar_label), "Table of Contents\n\n• Chapter 1\n• Chapter 2\n• Chapter 3\n\nSelect a section to navigate.");
        gtk_widget_show_all(sidebar_label);
        
        gtk_box_pack_start(GTK_BOX(main_hbox), sidebar, FALSE, FALSE, 0);
        gtk_box_reorder_child(GTK_BOX(main_hbox), content_vbox, 2);
        gtk_widget_show(sidebar);
        current_sidebar_mode = SIDEBAR_TOC;
    }
}

static void on_settings_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    if (current_sidebar_mode == SIDEBAR_SETTINGS) {
        gtk_container_remove(GTK_CONTAINER(main_hbox), sidebar);
        gtk_box_reorder_child(GTK_BOX(main_hbox), content_vbox, 1);
        current_sidebar_mode = SIDEBAR_NONE;
    } else {
        if (gtk_widget_get_parent(sidebar) != NULL) {
            gtk_container_remove(GTK_CONTAINER(main_hbox), sidebar);
        }
        
        // Hide other sidebar contents
        gtk_widget_hide(sidebar_label);
        gtk_widget_hide(sessions_container);
        
        // Show sidebar_label with settings text
        gtk_label_set_text(GTK_LABEL(sidebar_label), "Settings\n\n• Display options\n• Keyboard shortcuts\n• Preferences\n\nConfigure application settings.");
        gtk_widget_show_all(sidebar_label);
        
        gtk_box_pack_start(GTK_BOX(main_hbox), sidebar, FALSE, FALSE, 0);
        gtk_box_reorder_child(GTK_BOX(main_hbox), content_vbox, 2);
        gtk_widget_show(sidebar);
        current_sidebar_mode = SIDEBAR_SETTINGS;
    }
}

static void save_open_tabs_for_session(const char *session_name) {
    if (!session_name || !session_models) return;
    
    session_model_t *session = g_hash_table_lookup(session_models, session_name);
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
    if (left_notebook) {
        int n_pages = gtk_notebook_get_n_pages(GTK_NOTEBOOK(left_notebook));
        for (int i = 0; i < n_pages; i++) {
            GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(left_notebook), i);
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
    if (right_notebook) {
        int n_pages = gtk_notebook_get_n_pages(GTK_NOTEBOOK(right_notebook));
        for (int i = 0; i < n_pages; i++) {
            GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(right_notebook), i);
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
    if (left_notebook) {
        int cur = gtk_notebook_get_current_page(GTK_NOTEBOOK(left_notebook));
        if (cur >= 0) {
            GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(left_notebook), cur);
            if (page) {
                update_last_read_for_notebook(GTK_NOTEBOOK(left_notebook), page, (guint)cur);
            }
        } else {
            session_model_set_last_read_document(session, "");
        }
    }

    if (right_notebook) {
        int cur = gtk_notebook_get_current_page(GTK_NOTEBOOK(right_notebook));
        if (cur >= 0) {
            GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(right_notebook), cur);
            if (page) {
                update_last_read_for_notebook(GTK_NOTEBOOK(right_notebook), page, (guint)cur);
            }
        } else {
            session_model_set_last_read_help_document(session, "");
        }
    }
}

static void restore_open_tabs_for_session(const char *session_name) {
    if (!session_name || !session_models) return;

    session_model_t *session = g_hash_table_lookup(session_models, session_name);
    if (!session) {
        session = session_model_new();
        session_model_set_session_name(session, session_name);
        g_hash_table_insert(session_models, g_strdup(session_name), session);
    }

    is_restoring_session_tabs = TRUE;

    // Clear current notebooks
    if (left_notebook) {
        int n_pages = gtk_notebook_get_n_pages(GTK_NOTEBOOK(left_notebook));
        for (int i = n_pages - 1; i >= 0; i--) {
            gtk_notebook_remove_page(GTK_NOTEBOOK(left_notebook), i);
        }
    }

    if (right_notebook) {
        int n_pages = gtk_notebook_get_n_pages(GTK_NOTEBOOK(right_notebook));
        for (int i = n_pages - 1; i >= 0; i--) {
            gtk_notebook_remove_page(GTK_NOTEBOOK(right_notebook), i);
        }
    }

    const char *last_read_uri = session_model_get_last_read_document(session);
    const char *last_read_help_uri = session_model_get_last_read_help_document(session);

    int matched_left_index = -1;
    int matched_right_index = -1;
    int left_index = 0;
    int right_index = 0;

    // Restore saved documents in left notebook
    const GList *docs = session_model_get_document_urls(session);
    for (const GList *iter = docs; iter != NULL; iter = iter->next) {
        char *uri = (char *)iter->data;
        char *filename = g_filename_from_uri(uri, NULL, NULL);
        if (!filename && uri && g_path_is_absolute(uri)) {
            filename = g_strdup(uri);
        }
    
        if (filename) {
            TabData *tab = create_new_tab(left_notebook);
            if (tab) {
                load_file_into_tab(tab, filename);
                tab->initial_scroll_pending = TRUE;
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
            TabData *tab = create_new_tab(right_notebook);
            if (tab) {
                load_file_into_tab(tab, filename);
                tab->initial_scroll_pending = TRUE;
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

    // Re-focus last-read tabs if present
    if (left_notebook && matched_left_index >= 0 &&
        matched_left_index < gtk_notebook_get_n_pages(GTK_NOTEBOOK(left_notebook))) {
        gtk_notebook_set_current_page(GTK_NOTEBOOK(left_notebook), matched_left_index);
    }

    if (right_notebook && matched_right_index >= 0 &&
        matched_right_index < gtk_notebook_get_n_pages(GTK_NOTEBOOK(right_notebook))) {
        gtk_notebook_set_current_page(GTK_NOTEBOOK(right_notebook), matched_right_index);
    }

    is_restoring_session_tabs = FALSE;
}

static void queue_draw(TabData *tab) {
    if (tab && tab->pages_drawing)
        gtk_widget_queue_draw(tab->pages_drawing);
}

static void scroll_to_page(TabData *tab, int page) {
    if (!tab || !tab->scrolled || !tab->pages_drawing || !tab->doc) return;
    if (page < 0 || page >= tab->n_pages) return;

    const double spacing = 6.0;
    double scale = get_ppi_scale(tab);

    double y = spacing;
    if (tab->layout_mode == 0) {
        for (int i = 0; i < page; ++i) {
            PopplerPage *p = poppler_document_get_page(tab->doc, i);
            if (!p) continue;
            double pw, ph;
            poppler_page_get_size(p, &pw, &ph);
            double page_h = ph * scale;
            y += page_h + spacing;
            g_object_unref(p);
        }
    } else if (tab->layout_mode == 1) {
        int row = page / 2;
        for (int i = 0; i < row; ++i) {
            double row_h = 0.0;
            PopplerPage *p1 = poppler_document_get_page(tab->doc, i * 2);
            if (p1) {
                double pw1, ph1;
                poppler_page_get_size(p1, &pw1, &ph1);
                if (pw1 > 0 && ph1 > 0)
                    row_h = ph1 * scale;
                g_object_unref(p1);
            }
            if (i * 2 + 1 < tab->n_pages) {
                PopplerPage *p2 = poppler_document_get_page(tab->doc, i * 2 + 1);
                if (p2) {
                    double pw2, ph2;
                    poppler_page_get_size(p2, &pw2, &ph2);
                    if (pw2 > 0 && ph2 > 0) {
                        double h2 = ph2 * scale;
                        if (h2 > row_h) row_h = h2;
                    }
                    g_object_unref(p2);
                }
            }
            y += row_h + spacing;
        }
    } else if (tab->layout_mode == 2) {
        double x = spacing;
        for (int i = 0; i < page; ++i) {
            PopplerPage *p = poppler_document_get_page(tab->doc, i);
            if (!p) continue;
            double pw, ph;
            poppler_page_get_size(p, &pw, &ph);
            if (pw > 0 && ph > 0) {
                x += pw * scale + spacing;
            }
            g_object_unref(p);
        }
        if (tab->h_scrollbar) {
            GtkAdjustment *sadj = gtk_range_get_adjustment(GTK_RANGE(tab->h_scrollbar));
            gtk_adjustment_set_value(sadj, x);
        }
        return;
    }

    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(tab->scrolled));
    gtk_adjustment_set_value(vadj, y);
}

static void on_scroll_value_changed(GtkAdjustment *adj, gpointer user_data) {
    TabData *tab = user_data;
    if (!tab || !tab->doc) return;

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
            update_document_model_from_tab(tab);
            gtk_widget_queue_draw(tab->pages_drawing);
            return;
        }

        double x = spacing;
        int visible_page = (tab->n_pages > 0) ? (tab->n_pages - 1) : 0;
        for (int i = 0; i < tab->n_pages; ++i) {
            PopplerPage *page = poppler_document_get_page(tab->doc, i);
            if (!page) continue;
            double pw, ph;
            poppler_page_get_size(page, &pw, &ph);
            if (pw <= 0 || ph <= 0) { g_object_unref(page); continue; }
            double page_w = pw * scale;
            if (x + page_w > scroll_x) {
                visible_page = i;
                g_object_unref(page);
                break;
            }
            x += page_w + spacing;
            g_object_unref(page);
        }

        tab->cur_page = visible_page;
        if (tab == get_current_left_tab()) {
            sync_page_widget_from_tab(tab);
        }
        update_document_model_from_tab(tab);
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
        update_document_model_from_tab(tab);
        return;
    }

    double y = spacing;
    int visible_page = (tab->n_pages > 0) ? (tab->n_pages - 1) : 0;

    if (tab->layout_mode == 0) {
        for (int i = 0; i < tab->n_pages; ++i) {
            PopplerPage *page = poppler_document_get_page(tab->doc, i);
            if (!page) continue;

            double pw, ph;
            poppler_page_get_size(page, &pw, &ph);
            if (pw <= 0 || ph <= 0) {
                g_object_unref(page);
                continue;
            }

            double page_h = ph * scale;

            if (y + page_h > scroll_y) {
                visible_page = i;
                g_object_unref(page);
                break;
            }

            y += page_h + spacing;
            g_object_unref(page);
        }
    } else if (tab->layout_mode == 1) {
        for (int i = 0; i < tab->n_pages; i += 2) {
            double row_h = 0.0;
            PopplerPage *p1 = poppler_document_get_page(tab->doc, i);
            if (p1) {
                double pw1, ph1;
                poppler_page_get_size(p1, &pw1, &ph1);
                if (pw1 > 0 && ph1 > 0)
                    row_h = ph1 * scale;
                g_object_unref(p1);
            }
            if (i + 1 < tab->n_pages) {
                PopplerPage *p2 = poppler_document_get_page(tab->doc, i + 1);
                if (p2) {
                    double pw2, ph2;
                    poppler_page_get_size(p2, &pw2, &ph2);
                    if (pw2 > 0 && ph2 > 0) {
                        double h2 = ph2 * scale;
                        if (h2 > row_h) row_h = h2;
                    }
                    g_object_unref(p2);
                }
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

    update_document_model_from_tab(tab);
}

static void on_tab_scrolled_size_allocate(GtkWidget *widget, GdkRectangle *allocation, gpointer user_data) {
    (void)widget;
    (void)allocation;
    TabData *tab = user_data;
    if (!tab || !tab->doc) return;

    double zoom = tab->zoom > 0 ? tab->zoom : 300.0;
    if (zoom != tab->last_zoom) {
        tab->last_zoom = zoom;
        build_continuous_view(tab);
    }

    /* For row view (mode 2), the size_request width is clamped to page_width_px
       to prevent the window from growing.  GTK's internal handler set the
       hadjustment upper from the clamped size_request, so we must extend it
       here to the full total_w BEFORE scroll_to_page runs below. */
    if (tab->layout_mode == 2 && tab->doc && tab->n_pages > 0 && tab->h_scrollbar) {
        int vp_w = allocation ? allocation->width : 200;
        GtkAdjustment *sadj = gtk_range_get_adjustment(GTK_RANGE(tab->h_scrollbar));
        gtk_adjustment_set_page_size(sadj, vp_w > 0 ? vp_w : 200);
        gtk_adjustment_set_page_increment(sadj, vp_w * 0.9);
        double upper = gtk_adjustment_get_upper(sadj);
        if (upper < 1.0) upper = 1.0;
        gtk_adjustment_set_upper(sadj, upper);
    }

    if (tab->initial_scroll_pending) {
        scroll_to_page(tab, tab->cur_page);
        tab->initial_scroll_pending = FALSE;
    }
}

static gboolean on_drawing_scroll(GtkWidget *widget, GdkEventScroll *event, gpointer user_data) {
    (void)widget;
    TabData *tab = user_data;
    if (!tab || tab->layout_mode != 2 || !tab->h_scrollbar) return FALSE;
    GtkAdjustment *adj = gtk_range_get_adjustment(GTK_RANGE(tab->h_scrollbar));
    double val = gtk_adjustment_get_value(adj);
    double step = 40.0;
    if (event->direction == GDK_SCROLL_SMOOTH) {
        double dx, dy;
        gdk_event_get_scroll_deltas((GdkEvent*)event, &dx, &dy);
        val += dx * step;
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

    if (!tab || !tab->doc) {
        return FALSE;
    }

    /* continuous mode: draw multiple pages vertically inside this drawing area
       Render only pages intersecting the current clip extents to save work. */
    double clip_x1, clip_y1, clip_x2, clip_y2;
    cairo_clip_extents(cr, &clip_x1, &clip_y1, &clip_x2, &clip_y2);

    const double spacing = 6.0;
    double scale = get_ppi_scale(tab);
    if (tab->layout_mode == 0) {
        double y = spacing;
        for (int i = 0; i < tab->n_pages; ++i) {
            PopplerPage *page = poppler_document_get_page(tab->doc, i);
            if (!page) continue;
            double pw, ph;
            poppler_page_get_size(page, &pw, &ph);
            if (pw <= 0 || ph <= 0) { g_object_unref(page); continue; }
            double page_w = pw * scale;
            double page_h = ph * scale;
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
                cairo_save(cr);
                cairo_translate(cr, off_x, off_y);
                cairo_scale(cr, scale, scale);
                poppler_page_render(page, cr);
                cairo_restore(cr);
            }

            y += page_h + spacing;
            g_object_unref(page);
        }
    } else if (tab->layout_mode == 1) {
        int n = tab->n_pages;
        double y = spacing;
        for (int i = 0; i < n; i += 2) {
            double row_h = 0.0;
            double row_w = 0.0;

            /* left page */
            double pw1 = 0, ph1 = 0, page_w1 = 0, page_h1 = 0;
            PopplerPage *p1 = poppler_document_get_page(tab->doc, i);
            if (p1) {
                poppler_page_get_size(p1, &pw1, &ph1);
                if (pw1 > 0 && ph1 > 0) {
                    page_w1 = pw1 * scale;
                    page_h1 = ph1 * scale;
                }
            }
            row_w = page_w1;
            row_h = page_h1;

            /* right page */
            double pw2 = 0, ph2 = 0, page_w2 = 0, page_h2 = 0;
            PopplerPage *p2 = NULL;
            if (i + 1 < n) {
                p2 = poppler_document_get_page(tab->doc, i + 1);
                if (p2) {
                    poppler_page_get_size(p2, &pw2, &ph2);
                    if (pw2 > 0 && ph2 > 0) {
                        page_w2 = pw2 * scale;
                        page_h2 = ph2 * scale;
                    }
                }
            }
            if (page_w2 > 0) row_w += spacing + page_w2;
            if (page_h2 > row_h) row_h = page_h2;
            if (row_h < 1.0) row_h = 1.0;

            /* center the row horizontally within the drawing area */
            double row_x = (alloc.width - row_w) / 2.0;
            if (row_x < spacing) row_x = spacing;

            double left_x = row_x;
            double right_x = left_x + page_w1 + spacing;

            /* draw left page */
            if (p1 && page_h1 > 0) {
                double off_y1 = y;
                if (!(off_y1 + page_h1 < clip_y1 || off_y1 > clip_y2)) {
                    cairo_save(cr);
                    cairo_set_source_rgba(cr, tab->page_color.red, tab->page_color.green, tab->page_color.blue, tab->page_color.alpha);
                    cairo_rectangle(cr, left_x, off_y1, page_w1, page_h1);
                    cairo_fill(cr);
                    cairo_restore(cr);
                    cairo_save(cr);
                    cairo_translate(cr, left_x, off_y1);
                    cairo_scale(cr, scale, scale);
                    poppler_page_render(p1, cr);
                    cairo_restore(cr);
                }
            }

            /* draw right page */
            if (p2 && page_h2 > 0) {
                double off_y2 = y;
                if (!(off_y2 + page_h2 < clip_y1 || off_y2 > clip_y2)) {
                    cairo_save(cr);
                    cairo_set_source_rgba(cr, tab->page_color.red, tab->page_color.green, tab->page_color.blue, tab->page_color.alpha);
                    cairo_rectangle(cr, right_x, off_y2, page_w2, page_h2);
                    cairo_fill(cr);
                    cairo_restore(cr);
                    cairo_save(cr);
                    cairo_translate(cr, right_x, off_y2);
                    cairo_scale(cr, scale, scale);
                    poppler_page_render(p2, cr);
                    cairo_restore(cr);
                }
            }

            if (p1) g_object_unref(p1);
            if (p2) g_object_unref(p2);
            y += row_h + spacing;
        }
    } else if (tab->layout_mode == 2) {
        double scroll_x = 0.0;
        if (tab->h_scrollbar) {
            GtkAdjustment *sadj = gtk_range_get_adjustment(GTK_RANGE(tab->h_scrollbar));
            scroll_x = gtk_adjustment_get_value(sadj);
        }
        double x = spacing;
        for (int i = 0; i < tab->n_pages; ++i) {
            PopplerPage *page = poppler_document_get_page(tab->doc, i);
            if (!page) continue;
            double pw, ph;
            poppler_page_get_size(page, &pw, &ph);
            if (pw <= 0 || ph <= 0) { g_object_unref(page); continue; }
            double page_w = pw * scale;
            double page_h = ph * scale;
            double dev_x = x - scroll_x;
            double off_y = (alloc.height - page_h) / 2.0;
            if (dev_x + page_w > 0 && dev_x < alloc.width &&
                off_y + page_h > 0 && off_y < alloc.height) {
                cairo_save(cr);
                cairo_set_source_rgba(cr, tab->page_color.red, tab->page_color.green, tab->page_color.blue, tab->page_color.alpha);
                cairo_rectangle(cr, dev_x, off_y, page_w, page_h);
                cairo_fill(cr);
                cairo_restore(cr);
                cairo_save(cr);
                cairo_translate(cr, dev_x, off_y);
                cairo_scale(cr, scale, scale);
                poppler_page_render(page, cr);
                cairo_restore(cr);
            }
            x += page_w + spacing;
            g_object_unref(page);
        }
    }

    return FALSE;
}

static void build_continuous_view(TabData *tab) {
    if (!tab || !tab->doc || !tab->pages_drawing) return;
    const double spacing = 6.0;
    double scale = get_ppi_scale(tab);
    int page_width_px = 0;
    if (tab->n_pages > 0) {
        PopplerPage *p = poppler_document_get_page(tab->doc, 0);
        if (p) {
            double pw, ph;
            poppler_page_get_size(p, &pw, &ph);
            if (pw > 0)
                page_width_px = (int)ceil(pw * scale);
            g_object_unref(p);
        }
    }
    if (page_width_px < 1) page_width_px = 800;

    if (tab->layout_mode == 0) {
        double total_h = spacing;
        for (int i = 0; i < tab->n_pages; ++i) {
            PopplerPage *page = poppler_document_get_page(tab->doc, i);
            if (!page) continue;
            double pw, ph;
            poppler_page_get_size(page, &pw, &ph);
            double page_h = ph * scale;
            total_h += page_h + spacing;
            g_object_unref(page);
        }
        if (total_h < 1.0) total_h = 1.0;
        if (tab->h_scrollbar) gtk_widget_hide(tab->h_scrollbar);
        gtk_widget_set_size_request(tab->scrolled, -1, -1);
        gtk_widget_set_size_request(tab->pages_drawing, page_width_px, (int)ceil(total_h));
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(tab->scrolled),
                                       GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    } else if (tab->layout_mode == 1) {
        int n = tab->n_pages;
        double total_h = spacing;
        double max_row_w = 0.0;
        for (int i = 0; i < n; i += 2) {
            double row_h = 0.0;
            double row_w = 0.0;
            double pw1 = 0, ph1 = 0;
            PopplerPage *p1 = poppler_document_get_page(tab->doc, i);
            if (p1) {
                poppler_page_get_size(p1, &pw1, &ph1);
                g_object_unref(p1);
            }
            double page_w1 = 0, page_h1 = 0;
            if (pw1 > 0 && ph1 > 0) {
                page_w1 = pw1 * scale;
                page_h1 = ph1 * scale;
            }
            row_w = page_w1;
            row_h = page_h1;
            double pw2 = 0, ph2 = 0;
            if (i + 1 < n) {
                PopplerPage *p2 = poppler_document_get_page(tab->doc, i + 1);
                if (p2) {
                    poppler_page_get_size(p2, &pw2, &ph2);
                    g_object_unref(p2);
                }
            }
            double page_w2 = 0, page_h2 = 0;
            if (pw2 > 0 && ph2 > 0) {
                page_w2 = pw2 * scale;
                page_h2 = ph2 * scale;
            }
            if (page_w2 > 0) row_w += spacing + page_w2;
            if (page_h2 > row_h) row_h = page_h2;
            if (row_w > max_row_w) max_row_w = row_w;
            if (row_h < 1.0) row_h = 1.0;
            total_h += row_h + spacing;
        }
        if (total_h < 1.0) total_h = 1.0;
        if (max_row_w < 1.0) max_row_w = page_width_px;
        if (tab->h_scrollbar) gtk_widget_hide(tab->h_scrollbar);
        gtk_widget_set_size_request(tab->scrolled, -1, -1);
        gtk_widget_set_size_request(tab->pages_drawing, (int)ceil(max_row_w), (int)ceil(total_h));
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(tab->scrolled),
                                       GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    } else if (tab->layout_mode == 2) {
        double total_w = 0.0;
        double max_h = 0.0;
        for (int i = 0; i < tab->n_pages; ++i) {
            PopplerPage *p = poppler_document_get_page(tab->doc, i);
            if (!p) continue;
            double pw, ph;
            poppler_page_get_size(p, &pw, &ph);
            if (pw > 0 && ph > 0) {
                double page_w = pw * scale;
                double page_h = ph * scale;
                total_w += page_w + spacing;
                if (page_h > max_h) max_h = page_h;
            }
            g_object_unref(p);
        }
        if (total_w < 1.0) total_w = 1.0;
        if (max_h < 1.0) max_h = 1.0;
        if (tab->h_scrollbar) {
            gtk_widget_show(tab->h_scrollbar);
            GtkAdjustment *adj = gtk_range_get_adjustment(GTK_RANGE(tab->h_scrollbar));
            gtk_adjustment_set_lower(adj, 0.0);
            gtk_adjustment_set_upper(adj, total_w);
            gtk_adjustment_set_step_increment(adj, 10.0);
        }
        gtk_widget_set_size_request(tab->scrolled, -1, -1);
        gtk_widget_set_size_request(tab->pages_drawing, page_width_px, (int)ceil(max_h));
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(tab->scrolled),
                                       GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    }
    gtk_widget_queue_draw(tab->pages_drawing);
}

static TabData *get_current_left_tab(void) {
    if (!left_notebook) return NULL;
    int idx = gtk_notebook_get_current_page(GTK_NOTEBOOK(left_notebook));
    if (idx < 0) return NULL;

    GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(left_notebook), idx);
    if (!page) return NULL;

    return g_object_get_data(G_OBJECT(page), "tab-data");
}

static TabData *get_current_right_tab(void) {
    if (!right_notebook) return NULL;
    int idx = gtk_notebook_get_current_page(GTK_NOTEBOOK(right_notebook));
    if (idx < 0) return NULL;

    GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(right_notebook), idx);
    if (!page) return NULL;

    return g_object_get_data(G_OBJECT(page), "tab-data");
}

static void sync_page_widget_from_tab(TabData *tab) {
    if (!page_entry || !page_total_label) return;

    int total = 0;
    int current = 0;

    if (tab && tab->n_pages > 0) {
        total = tab->n_pages;
        current = tab->cur_page + 1; /* UI is 1-based */
        if (current < 1) current = 1;
        if (current > total) current = total;
    }

    page_spin_syncing = TRUE;
    if (total == 0) {
        /* pick one of these two lines */
        gtk_entry_set_text(GTK_ENTRY(page_entry), "");   /* blank */
        /* gtk_entry_set_text(GTK_ENTRY(page_entry), "0"); */ /* or 0 */
    } else {
        gchar *cur_txt = g_strdup_printf("%d", current);
        gtk_entry_set_text(GTK_ENTRY(page_entry), cur_txt);
        g_free(cur_txt);
    }
    page_spin_syncing = FALSE;

    gchar *txt = g_strdup_printf("/ %d", total);
    gtk_label_set_text(GTK_LABEL(page_total_label), txt);
    g_free(txt);
}

static void on_page_entry_insert_text(GtkEditable *editable,
                                      const gchar *text,
                                      gint length,
                                      gint *position,
                                      gpointer user_data) {
    (void)position;
    (void)user_data;

    /* Allow only ASCII digits to be inserted (typed or pasted). */
    for (gint i = 0; i < length; i++) {
        if (!g_ascii_isdigit((guchar)text[i])) {
            g_signal_stop_emission_by_name(editable, "insert-text");
            return;
        }
    }
}

static void on_page_entry_activate(GtkEntry *entry, gpointer user_data) {
    (void)user_data;
    if (page_spin_syncing) return;

    TabData *tab = get_current_left_tab();
    if (!tab || tab->n_pages <= 0) return;

    const char *raw = gtk_entry_get_text(GTK_ENTRY(entry));
    char *endptr = NULL;
    long requested_ui = strtol(raw, &endptr, 10);
    if (endptr == raw || *endptr != '\0') {
        sync_page_widget_from_tab(tab);
        return;
    }

    if (requested_ui < 1 || requested_ui > tab->n_pages) return;

    int target_zero_based = requested_ui - 1;
    tab->cur_page = target_zero_based;
    scroll_to_page(tab, target_zero_based);

    update_document_model_from_tab(tab);
}

static void load_file_into_tab(TabData *tab, const char *filename) {
    if (!tab || !filename) return;
    GError *error = NULL;
    char *uri = g_filename_to_uri(filename, NULL, &error);
    if (!uri) {
        g_warning("Failed to convert filename to URI: %s", error->message);
        g_clear_error(&error);
        return;
    }

    PopplerDocument *doc = poppler_document_new_from_file(uri, NULL, &error);
    g_free(uri);

    if (!doc) {
        g_warning("Failed to open PDF: %s", error->message);
        g_clear_error(&error);
        return;
    }

    if (tab->doc)
        g_object_unref(tab->doc);

    tab->doc = doc;
    tab->n_pages = poppler_document_get_n_pages(doc);
    tab->cur_page = 0;
    tab->zoom = 300.0;
    /* track current filename for per-document settings */
    if (tab->current_file)
        g_free(tab->current_file);
    tab->current_file = g_strdup(filename);

    /* Update the tab's label with filename */
    if (tab->tab_label) {
        const char *basename = g_path_get_basename(filename);
        gtk_label_set_text(GTK_LABEL(tab->tab_label), basename);
    }

    queue_draw(tab);

    {
        restore_document_model_to_tab(tab);

        build_continuous_view(tab);

        /* Defer scrolling until widget is allocated */
        tab->initial_scroll_pending = TRUE;

        /* Ensure page counter shows real total immediately after load. */
        if (tab == get_current_left_tab()) {
            sync_page_widget_from_tab(tab);
        }
    }
}

static TabData *create_new_tab(GtkWidget *notebook) {
    TabData *tab = g_malloc0(sizeof(TabData));
    tab->zoom = 300.0;
    tab->layout_mode = 0;    /* single-column by default */
    tab->n_pages = 0;
    tab->cur_page = 0;
    tab->last_zoom = 300.0;
    tab->initial_scroll_pending = FALSE;
    tab->scroll_offset = -1.0;
    tab->is_helper = (notebook == right_notebook);
    
    if (!notebook || !GTK_IS_NOTEBOOK(notebook)) {
        g_free(tab);
        return NULL;
    }

    gdk_rgba_parse(&tab->page_color, "white");
    
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
    gtk_widget_add_events(tab->pages_drawing, GDK_SCROLL_MASK);

    gtk_widget_show_all(tab_box);
    
    /* Store tab data in the widget */
    g_object_set_data_full(G_OBJECT(tab_box), "tab-data", tab, destroy_tab_data);
    
    /* Create tab label with close button */
    GtkWidget *label_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *label = gtk_label_new("New Document");
    gtk_box_pack_start(GTK_BOX(label_box), label, FALSE, FALSE, 0);
    tab->tab_label = label;  /* Store reference to label for updates */
    GtkWidget *close_btn = gtk_button_new_with_label("×");
    gtk_widget_set_size_request(close_btn, 30, -1);
    /* allocate CloseInfo linking the notebook and this page so close removes correct page */
    typedef struct {
        GtkNotebook *notebook;
        GtkWidget *page;
    } CloseInfo;
    CloseInfo *ci = g_malloc(sizeof(CloseInfo));
    ci->notebook = GTK_NOTEBOOK(notebook);
    ci->page = tab_box;
    g_signal_connect(close_btn, "clicked", G_CALLBACK(on_tab_close_clicked), ci);
    gtk_box_pack_start(GTK_BOX(label_box), close_btn, FALSE, FALSE, 0);
    gtk_widget_show_all(label_box);
    
    /* Add tab to notebook */
    int page_num = gtk_notebook_append_page(GTK_NOTEBOOK(notebook), tab_box, label_box);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), page_num);
    
    return tab;
}

static void open_file_in_notebook(GtkWidget *notebook, gboolean is_helper) {
    if (!notebook) return;
    GtkWidget *dialog = gtk_file_chooser_dialog_new("Open PDF",
                                                   GTK_WINDOW(window),
                                                   GTK_FILE_CHOOSER_ACTION_OPEN,
                                                   "_Cancel", GTK_RESPONSE_CANCEL,
                                                   "_Open", GTK_RESPONSE_ACCEPT,
                                                   NULL);

    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_add_pattern(filter, "*.pdf");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
    char *fname = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
    if (fname) {
        char *uri = g_filename_to_uri(fname, NULL, NULL);
        int existing_idx = -1;
        if (uri) {
            existing_idx = find_matching_tab_index(GTK_NOTEBOOK(notebook), uri);
        }
        if (existing_idx >= 0) {
            /* Already open in this notebook: focus existing tab */
            gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), existing_idx);
        } else {
            /* Not open yet: create tab and load */
            TabData *tab = create_new_tab(notebook);
            if (tab) {
                load_file_into_tab(tab, fname);
                /* Add to session model only for newly opened docs */
                if (current_selected_session) {
                    session_model_t *session = g_hash_table_lookup(session_models, current_selected_session);
                    if (session && uri) {
                        if (is_helper) {
                            session_model_add_helper_document_url(session, uri);
                        } else {
                            session_model_add_document_url(session, uri);
                            /* TODO: Implement adding to tree view */
                        }
                    }
                }
            }
        }
        if (uri) g_free(uri);
        g_free(fname);
        }
    }
    gtk_widget_destroy(dialog);
}

static void on_open_file_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    open_file_in_notebook(left_notebook, FALSE);
}

static void on_open_helper_file_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    open_file_in_notebook(right_notebook, TRUE);
}

static int find_matching_tab_index(GtkNotebook *notebook, const char *target_uri) {
    if (!notebook || !target_uri || !*target_uri) return -1;
    int n_pages = gtk_notebook_get_n_pages(notebook);
    for (int i = 0; i < n_pages; i++) {
        GtkWidget *page = gtk_notebook_get_nth_page(notebook, i);
        if (!page) continue;
        TabData *tab = g_object_get_data(G_OBJECT(page), "tab-data");
        if (!tab || !tab->current_file) continue;
        char *uri = g_filename_to_uri(tab->current_file, NULL, NULL);
        if (!uri) continue;
        gboolean matched = (g_strcmp0(uri, target_uri) == 0);
        g_free(uri);
        if (matched) return i;
    }
    return -1;
}

static void update_last_read_for_notebook(GtkNotebook *notebook, GtkWidget *page, guint page_num) {
    (void)page_num;
    if (is_restoring_session_tabs) return;
    if (!notebook || !page || !current_selected_session || !session_models) return;
    session_model_t *session = g_hash_table_lookup(session_models, current_selected_session);
    if (!session) return;
    TabData *tab = g_object_get_data(G_OBJECT(page), "tab-data");
    if (!tab || !tab->current_file) return;
    char *uri = g_filename_to_uri(tab->current_file, NULL, NULL);
    if (!uri) return;
    if (notebook == GTK_NOTEBOOK(left_notebook)) {
        session_model_set_last_read_document(session, uri);
    } else if (notebook == GTK_NOTEBOOK(right_notebook)) {
        session_model_set_last_read_help_document(session, uri);
    }
    g_free(uri);
}

static void on_left_notebook_switch_page(GtkNotebook *notebook, GtkWidget *page, guint page_num, gpointer user_data) {
    (void)user_data;

    /* Flush state for all left tabs before switching */
    int n_left = gtk_notebook_get_n_pages(notebook);
    for (int i = 0; i < n_left; i++) {
        GtkWidget *p = gtk_notebook_get_nth_page(notebook, i);
        TabData *t = g_object_get_data(G_OBJECT(p), "tab-data");
        if (t) update_document_model_from_tab(t);
    }

    update_last_read_for_notebook(notebook, page, page_num);
    TabData *tab = g_object_get_data(G_OBJECT(page), "tab-data");

    // RESTORE STATE WHEN SWITCHING TO THIS TAB
    if (tab && tab->doc) {
        if (tab->initial_scroll_pending) {
            // First time seeing this tab after startup - do full restoration
            restore_document_model_to_tab(tab);
            tab->initial_scroll_pending = FALSE;
        } else {
            if (tab->current_file && document_models) {
                char *uri = g_filename_to_uri(tab->current_file, NULL, NULL);
                if (uri) {
                    char *key = make_document_key(uri, tab->is_helper);
                    document_model_t *dm = g_hash_table_lookup(document_models, key);
                    if (dm) {
                        tab->layout_mode = document_model_get_visualization_mode(dm);
                        double saved_zoom = document_model_get_zoom(dm);
                        if (saved_zoom >= 10.0 && saved_zoom <= 500.0)
                            tab->zoom = saved_zoom;
                        build_continuous_view(tab);
                        int saved_page = document_model_get_current_page(dm);
                        if (saved_page < 1) saved_page = 1;
                        if (saved_page > tab->n_pages) saved_page = tab->n_pages;
                        tab->cur_page = saved_page - 1;
                        scroll_to_page(tab, tab->cur_page);
                    }
                    g_free(key);
                    g_free(uri);
                }
            }
        }
    }

    sync_left_layout_buttons(tab);
    sync_page_widget_from_tab(tab);
}

static void on_right_notebook_switch_page(GtkNotebook *notebook, GtkWidget *page, guint page_num, gpointer user_data) {
    (void)user_data;
    update_last_read_for_notebook(notebook, page, page_num);
    TabData *tab = g_object_get_data(G_OBJECT(page), "tab-data");

    // RESTORE STATE WHEN SWITCHING TO THIS TAB
    if (tab && tab->doc) {
        restore_document_model_to_tab(tab);
    }

    sync_right_layout_buttons(tab);
    /* keep widget tied to primary (left) document */
    sync_page_widget_from_tab(get_current_left_tab());
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

    gboolean is_left = (ci->notebook == GTK_NOTEBOOK(left_notebook));
    gboolean is_right = (ci->notebook == GTK_NOTEBOOK(right_notebook));
    session_model_t *session = NULL;
    if (current_selected_session && session_models) {
        session = g_hash_table_lookup(session_models, current_selected_session);
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
    g_free(ci);
}

/* =============== Layout button management =============== */

static void apply_zoom_to_tab(TabData *tab, int direction) {
    if (!tab) return;
    double new_zoom = tab->zoom + (direction > 0 ? 2.0 : -2.0);
    if (new_zoom < 10.0) new_zoom = 10.0;
    if (new_zoom > 500.0) new_zoom = 500.0;
    tab->zoom = new_zoom;
    tab->last_zoom = new_zoom;
    build_continuous_view(tab);
    gtk_widget_queue_resize(tab->scrolled);
    update_document_model_from_tab(tab);
    save_state();
}

static void on_zoom_in_left(GtkButton *btn, gpointer user_data) {
    (void)btn;
    (void)user_data;
    apply_zoom_to_tab(get_current_left_tab(), 1);
}

static void on_zoom_out_left(GtkButton *btn, gpointer user_data) {
    (void)btn;
    (void)user_data;
    apply_zoom_to_tab(get_current_left_tab(), -1);
}

static void on_zoom_in_right(GtkButton *btn, gpointer user_data) {
    (void)btn;
    (void)user_data;
    apply_zoom_to_tab(get_current_right_tab(), 1);
}

static void on_zoom_out_right(GtkButton *btn, gpointer user_data) {
    (void)btn;
    (void)user_data;
    apply_zoom_to_tab(get_current_right_tab(), -1);
}

static void apply_layout_to_tab(TabData *tab, int layout) {
    if (!tab) return;
    tab->layout_mode = layout;
    build_continuous_view(tab);
    tab->initial_scroll_pending = TRUE;
    gtk_widget_queue_resize(tab->scrolled);
    update_document_model_from_tab(tab);
    save_state();
    if (tab == get_current_left_tab() || tab == get_current_right_tab()) {
        sync_page_widget_from_tab(tab);
    }
}

static void on_layout_left_toggled(GtkToggleButton *btn, gpointer user_data) {
    (void)user_data;
    if (!gtk_toggle_button_get_active(btn)) return;
    gpointer idptr = g_object_get_data(G_OBJECT(btn), "layout-id");
    if (!idptr) return;
    int layout = GPOINTER_TO_INT(idptr) - 1;
    TabData *tab = get_current_left_tab();
    if (!tab) return;
    apply_layout_to_tab(tab, layout);
}

static void on_layout_right_toggled(GtkToggleButton *btn, gpointer user_data) {
    (void)user_data;
    if (!gtk_toggle_button_get_active(btn)) return;
    gpointer idptr = g_object_get_data(G_OBJECT(btn), "layout-id");
    if (!idptr) return;
    int layout = GPOINTER_TO_INT(idptr) - 1;
    TabData *tab = get_current_right_tab();
    if (!tab) return;
    apply_layout_to_tab(tab, layout);
}

static void sync_left_layout_buttons(TabData *tab) {
    if (!left_column_btn) return;
    g_signal_handlers_block_by_func(left_column_btn, G_CALLBACK(on_layout_left_toggled), NULL);
    g_signal_handlers_block_by_func(left_double_column_btn, G_CALLBACK(on_layout_left_toggled), NULL);
    g_signal_handlers_block_by_func(left_row_btn, G_CALLBACK(on_layout_left_toggled), NULL);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(left_column_btn), tab && tab->layout_mode == 0);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(left_double_column_btn), tab && tab->layout_mode == 1);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(left_row_btn), tab && tab->layout_mode == 2);
    g_signal_handlers_unblock_by_func(left_column_btn, G_CALLBACK(on_layout_left_toggled), NULL);
    g_signal_handlers_unblock_by_func(left_double_column_btn, G_CALLBACK(on_layout_left_toggled), NULL);
    g_signal_handlers_unblock_by_func(left_row_btn, G_CALLBACK(on_layout_left_toggled), NULL);
}

static void sync_right_layout_buttons(TabData *tab) {
    if (!right_column_btn) return;
    g_signal_handlers_block_by_func(right_column_btn, G_CALLBACK(on_layout_right_toggled), NULL);
    g_signal_handlers_block_by_func(right_double_column_btn, G_CALLBACK(on_layout_right_toggled), NULL);
    g_signal_handlers_block_by_func(right_row_btn, G_CALLBACK(on_layout_right_toggled), NULL);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(right_column_btn), tab && tab->layout_mode == 0);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(right_double_column_btn), tab && tab->layout_mode == 1);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(right_row_btn), tab && tab->layout_mode == 2);
    g_signal_handlers_unblock_by_func(right_column_btn, G_CALLBACK(on_layout_right_toggled), NULL);
    g_signal_handlers_unblock_by_func(right_double_column_btn, G_CALLBACK(on_layout_right_toggled), NULL);
    g_signal_handlers_unblock_by_func(right_row_btn, G_CALLBACK(on_layout_right_toggled), NULL);
}

GtkWidget* create_main_window(void) {
    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "Siters");
    gtk_window_set_default_size(GTK_WINDOW(window), 1000, 800);

    // Set minimal size to prevent unusable layouts
    GdkGeometry hints = {0};
    hints.min_width = 1000;
    hints.min_height = 800;
    gtk_window_set_geometry_hints(GTK_WINDOW(window), NULL, &hints, GDK_HINT_MIN_SIZE);

    // Initialize sessions model
    if (!sessions_model) {
        sessions_model = sessions_model_new();
        // Initialize session models hash table
        session_models = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)session_model_free);
        
        // Initialize document models hash table
        if (!document_models) {
            document_models = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                                   (GDestroyNotify)document_model_free);
        }
        
        // Always ensure "Default" session exists
        const GList *existing_sessions = sessions_model_get_session_names(sessions_model);
        gboolean has_default = FALSE;
        for (const GList *iter = existing_sessions; iter != NULL; iter = iter->next) {
            if (strcmp((const char*)iter->data, "Default") == 0) {
                has_default = TRUE;
                break;
            }
        }
        if (!has_default) {
            sessions_model_add_session_name(sessions_model, "Default");
            // Create session model for Default
            session_model_t *default_session = session_model_new();
            session_model_set_session_name(default_session, "Default");
            g_hash_table_insert(session_models, g_strdup("Default"), default_session);
        } else {
            // Ensure we have a session model for Default
            if (!g_hash_table_lookup(session_models, "Default")) {
                session_model_t *default_session = session_model_new();
                session_model_set_session_name(default_session, "Default");
                g_hash_table_insert(session_models, g_strdup("Default"), default_session);
            }
        }
    }

    g_signal_connect(window, "destroy", G_CALLBACK(on_window_destroy), NULL);
    g_signal_connect(window, "configure-event", G_CALLBACK(on_window_configure), NULL);

    /* Main horizontal container: toolbar on left, content on right */
    main_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_container_add(GTK_CONTAINER(window), main_hbox);

    /* Left sidebar: main toolbar */
    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(toolbar), "Toolbar");
    gtk_widget_set_size_request(toolbar, 48, -1);
    gtk_box_pack_start(GTK_BOX(main_hbox), toolbar, FALSE, FALSE, 0);

    /* Sidebar for sessions, toc, settings */
    sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    g_object_ref(sidebar);  /* Keep a reference to prevent destruction when removed */
    gtk_widget_set_size_request(sidebar, 200, -1);
    atk_object_set_name(gtk_widget_get_accessible(sidebar), "Sidebar");

    /* Content for sidebar */
    sidebar_label = gtk_label_new("");
    gtk_label_set_justify(GTK_LABEL(sidebar_label), GTK_JUSTIFY_LEFT);
    atk_object_set_name(gtk_widget_get_accessible(sidebar_label), "Sidebar label");
    g_object_ref(sidebar_label);  /* Keep a reference to prevent destruction when removed */
    gtk_box_pack_start(GTK_BOX(sidebar), sidebar_label, TRUE, TRUE, 0);
    gtk_widget_hide(sidebar_label);

    /* Sessions container */
    sessions_container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_set_border_width(GTK_CONTAINER(sessions_container), 5);
    g_object_ref(sessions_container);  /* Keep a reference */
    
    // Title
    sessions_title = gtk_label_new("Sessions");
    gtk_widget_set_halign(sessions_title, GTK_ALIGN_START);
    PangoAttrList *attr_list = pango_attr_list_new();
    PangoAttribute *attr = pango_attr_weight_new(PANGO_WEIGHT_BOLD);
    pango_attr_list_insert(attr_list, attr);
    attr = pango_attr_scale_new(PANGO_SCALE_LARGE);
    pango_attr_list_insert(attr_list, attr);
    gtk_label_set_attributes(GTK_LABEL(sessions_title), attr_list);
    pango_attr_list_unref(attr_list);
    gtk_box_pack_start(GTK_BOX(sessions_container), sessions_title, FALSE, FALSE, 0);
    
    // Entry field
    sessions_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(sessions_entry), "Enter session name...");
    gtk_box_pack_start(GTK_BOX(sessions_container), sessions_entry, FALSE, FALSE, 0);
    atk_object_set_name(gtk_widget_get_accessible(sessions_entry), "Sessions entry");
    
    // Buttons box
    GtkWidget *buttons_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_pack_start(GTK_BOX(sessions_container), buttons_box, FALSE, FALSE, 0);
    
    sessions_add_btn = gtk_button_new_with_label("Add");
    gtk_widget_set_tooltip_text(sessions_add_btn, "Add new session");
    atk_object_set_name(gtk_widget_get_accessible(sessions_add_btn), "Add session");
    g_signal_connect(sessions_add_btn, "clicked", G_CALLBACK(on_sessions_add_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(buttons_box), sessions_add_btn, TRUE, TRUE, 0);
    
    sessions_remove_btn = gtk_button_new_with_label("Remove");
    gtk_widget_set_tooltip_text(sessions_remove_btn, "Remove selected session");
    atk_object_set_name(gtk_widget_get_accessible(sessions_remove_btn), "Remove session");
    g_signal_connect(sessions_remove_btn, "clicked", G_CALLBACK(on_sessions_remove_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(buttons_box), sessions_remove_btn, TRUE, TRUE, 0);
    
    sessions_update_btn = gtk_button_new_with_label("Update");
    gtk_widget_set_tooltip_text(sessions_update_btn, "Update selected session name");
    atk_object_set_name(gtk_widget_get_accessible(sessions_update_btn), "Update session");
    g_signal_connect(sessions_update_btn, "clicked", G_CALLBACK(on_sessions_update_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(buttons_box), sessions_update_btn, TRUE, TRUE, 0);
    
    // Tree view
    sessions_tree_store = gtk_tree_store_new(
    SESSION_COL_COUNT,
    G_TYPE_STRING,  // label
    G_TYPE_INT,     // row kind
    G_TYPE_STRING,  // session name
    G_TYPE_STRING   // doc uri
    );

    sessions_tree_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(sessions_tree_store));
    g_object_unref(sessions_tree_store);
    
    GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes("Session / File", renderer, "text", SESSION_COL_LABEL, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(sessions_tree_view), column);

    g_signal_connect(sessions_tree_view, "cursor-changed", G_CALLBACK(on_sessions_treeview_cursor_changed), NULL);
    
    // Scrolled window for tree view
    GtkWidget *scrolled_window = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scrolled_window), sessions_tree_view);
    gtk_box_pack_start(GTK_BOX(sessions_container), scrolled_window, TRUE, TRUE, 0);
    
    // Populate tree view with existing sessions
    populate_sessions_treeview();

    gtk_box_pack_start(GTK_BOX(sidebar), sessions_container, TRUE, TRUE, 0);
    gtk_widget_hide(sessions_container);

    /* Content area on the right*/
    content_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(main_hbox), content_vbox, TRUE, TRUE, 0);

    /* Buttons*/
    /* Sessions button */
    GtkWidget *sessions_icon = gtk_image_new_from_file("./data/icons/sessions.png");
    GtkWidget *sessions_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(sessions_btn), sessions_icon);
    gtk_widget_set_tooltip_text(sessions_btn, "Sessions");
    atk_object_set_name(gtk_widget_get_accessible(sessions_btn), "Sessions");
    g_signal_connect(sessions_btn, "clicked", G_CALLBACK(on_sessions_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(toolbar), sessions_btn, FALSE, FALSE, 1);

    /* Table of contents button */
    GtkWidget *toc_icon = gtk_image_new_from_file("./data/icons/toc.png");
    GtkWidget *toc_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(toc_btn), toc_icon);
    gtk_widget_set_tooltip_text(toc_btn, "Table of contents");
    atk_object_set_name(gtk_widget_get_accessible(toc_btn), "Table of contents");
    g_signal_connect(toc_btn, "clicked", G_CALLBACK(on_toc_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(toolbar), toc_btn, FALSE, FALSE, 1);

    /* Settings button */
    GtkWidget *settings_icon = gtk_image_new_from_file("./data/icons/settings.png");
    GtkWidget *settings_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(settings_btn), settings_icon);
    gtk_widget_set_tooltip_text(settings_btn, "Settings");
    atk_object_set_name(gtk_widget_get_accessible(settings_btn), "Settings");
    g_signal_connect(settings_btn, "clicked", G_CALLBACK(on_settings_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(toolbar), settings_btn, FALSE, FALSE, 1);

    /* Separator */
    GtkWidget *separator_a = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(toolbar), separator_a, FALSE, FALSE, 5);

    /* Open file button */
    GtkWidget *open_file_icon = gtk_image_new_from_file("./data/icons/file-plus.png");
    GtkWidget *open_file_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(open_file_btn), open_file_icon);
    gtk_widget_set_tooltip_text(open_file_btn, "Open file");
    atk_object_set_name(gtk_widget_get_accessible(open_file_btn), "Open file");
    g_signal_connect(open_file_btn, "clicked", G_CALLBACK(on_open_file_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(toolbar), open_file_btn, FALSE, FALSE, 1);

    /* Close file button*/
    GtkWidget *close_file_icon = gtk_image_new_from_file("./data/icons/file-minus.png");
    GtkWidget *close_file_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(close_file_btn), close_file_icon);
    gtk_widget_set_tooltip_text(close_file_btn, "Close file");
    atk_object_set_name(gtk_widget_get_accessible(close_file_btn), "Close file");
    gtk_box_pack_start(GTK_BOX(toolbar), close_file_btn, FALSE, FALSE, 1);

    /* Separator */
    GtkWidget *separator_b = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(toolbar), separator_b, FALSE, FALSE, 5);

    /* Page up button*/
    GtkWidget *page_up_icon = gtk_image_new_from_file("./data/icons/page-up.png");
    GtkWidget *page_up_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(page_up_btn), page_up_icon);
    gtk_widget_set_tooltip_text(page_up_btn, "Page up");
    atk_object_set_name(gtk_widget_get_accessible(page_up_btn), "Page up");
    gtk_box_pack_start(GTK_BOX(toolbar), page_up_btn, FALSE, FALSE, 1);

    /* Page down button*/
    GtkWidget *page_down_icon = gtk_image_new_from_file("./data/icons/page-down.png");
    GtkWidget *page_down_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(page_down_btn), page_down_icon);
    gtk_widget_set_tooltip_text(page_down_btn, "Page down");
    atk_object_set_name(gtk_widget_get_accessible(page_down_btn), "Page down");
    gtk_box_pack_start(GTK_BOX(toolbar), page_down_btn, FALSE, FALSE, 1);

    /* Page number jump: [spin] / total */
    GtkWidget *page_nav_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_halign(page_nav_box, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(page_nav_box, FALSE);

    page_entry = gtk_entry_new();
    gtk_widget_set_size_request(page_entry, 42, -1);
    gtk_entry_set_max_length(GTK_ENTRY(page_entry), 4);
    gtk_entry_set_width_chars(GTK_ENTRY(page_entry), 2);
    gtk_entry_set_max_width_chars(GTK_ENTRY(page_entry), 3);
    gtk_entry_set_input_purpose(GTK_ENTRY(page_entry), GTK_INPUT_PURPOSE_DIGITS);
    gtk_widget_set_tooltip_text(page_entry, "Current page (press Enter to jump)");
    atk_object_set_name(gtk_widget_get_accessible(page_entry), "Current page");

    page_total_label = gtk_label_new("/ 0");
    gtk_box_pack_start(GTK_BOX(page_nav_box), page_entry, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page_nav_box), page_total_label, FALSE, FALSE, 0);
    gtk_label_set_width_chars(GTK_LABEL(page_total_label), 4);
    gtk_label_set_xalign(GTK_LABEL(page_total_label), 0.5f);
    gtk_box_pack_start(GTK_BOX(toolbar), page_nav_box, FALSE, FALSE, 1);

    /* Allow only digits to be entered */
    g_signal_connect(page_entry, "insert-text", G_CALLBACK(on_page_entry_insert_text), NULL);

    /* Enter in spin jumps to page */
    g_signal_connect(page_entry, "activate", G_CALLBACK(on_page_entry_activate), NULL);

    /* Separator */
    GtkWidget *separator_c = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(toolbar), separator_c, FALSE, FALSE, 5);

    /* Zoom in button*/
    GtkWidget *zoom_in_icon = gtk_image_new_from_file("./data/icons/zoom-in.png");
    GtkWidget *zoom_in_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(zoom_in_btn), zoom_in_icon);
    gtk_widget_set_tooltip_text(zoom_in_btn, "Zoom in");
    atk_object_set_name(gtk_widget_get_accessible(zoom_in_btn), "Zoom in");
    gtk_box_pack_start(GTK_BOX(toolbar), zoom_in_btn, FALSE, FALSE, 1);
    g_signal_connect(G_OBJECT(zoom_in_btn), "clicked", G_CALLBACK(on_zoom_in_left), NULL);

    /* Zoom out button*/
    GtkWidget *zoom_out_icon = gtk_image_new_from_file("./data/icons/zoom-out.png");
    GtkWidget *zoom_out_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(zoom_out_btn), zoom_out_icon);
    gtk_widget_set_tooltip_text(zoom_out_btn, "Zoom out");
    atk_object_set_name(gtk_widget_get_accessible(zoom_out_btn), "Zoom out");
    gtk_box_pack_start(GTK_BOX(toolbar), zoom_out_btn, FALSE, FALSE, 1);
    g_signal_connect(G_OBJECT(zoom_out_btn), "clicked", G_CALLBACK(on_zoom_out_left), NULL);

    /* Separator */
    GtkWidget *separator_d = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(toolbar), separator_d, FALSE, FALSE, 5);

    /* Column view button*/
    GtkWidget *column_view_icon = gtk_image_new_from_file("./data/icons/column.png");
    left_column_btn = gtk_radio_button_new(NULL);
    gtk_button_set_image(GTK_BUTTON(left_column_btn), column_view_icon);
    gtk_widget_set_tooltip_text(left_column_btn, "Page column");
    atk_object_set_name(gtk_widget_get_accessible(left_column_btn), "Page column");
    g_object_set_data(G_OBJECT(left_column_btn), "layout-id", GINT_TO_POINTER(0 + 1));
    g_signal_connect(left_column_btn, "toggled", G_CALLBACK(on_layout_left_toggled), NULL);
    gtk_box_pack_start(GTK_BOX(toolbar), left_column_btn, FALSE, FALSE, 1);

    /* Double column view button*/
    GtkWidget *double_column_view_icon = gtk_image_new_from_file("./data/icons/double-column.png");
    left_double_column_btn = gtk_radio_button_new_from_widget(GTK_RADIO_BUTTON(left_column_btn));
    gtk_button_set_image(GTK_BUTTON(left_double_column_btn), double_column_view_icon);
    gtk_widget_set_tooltip_text(left_double_column_btn, "Page double column");
    atk_object_set_name(gtk_widget_get_accessible(left_double_column_btn), "Page double column");
    g_object_set_data(G_OBJECT(left_double_column_btn), "layout-id", GINT_TO_POINTER(1 + 1));
    g_signal_connect(left_double_column_btn, "toggled", G_CALLBACK(on_layout_left_toggled), NULL);
    gtk_box_pack_start(GTK_BOX(toolbar), left_double_column_btn, FALSE, FALSE, 1);

    /* Row view button*/
    GtkWidget *row_view_icon = gtk_image_new_from_file("./data/icons/row.png");
    left_row_btn = gtk_radio_button_new_from_widget(GTK_RADIO_BUTTON(left_column_btn));
    gtk_button_set_image(GTK_BUTTON(left_row_btn), row_view_icon);
    gtk_widget_set_tooltip_text(left_row_btn, "Page row");
    atk_object_set_name(gtk_widget_get_accessible(left_row_btn), "Page row");
    g_object_set_data(G_OBJECT(left_row_btn), "layout-id", GINT_TO_POINTER(2 + 1));
    g_signal_connect(left_row_btn, "toggled", G_CALLBACK(on_layout_left_toggled), NULL);
    gtk_box_pack_start(GTK_BOX(toolbar), left_row_btn, FALSE, FALSE, 1);

    /* Horizontal scroll toggle button*/
    GtkWidget *horiz_scroll_toggle_icon = gtk_image_new_from_file("./data/icons/horiz-scroll-off.png");
    GtkWidget *horiz_scroll_toggle_btn = gtk_toggle_button_new();
    gtk_button_set_image(GTK_BUTTON(horiz_scroll_toggle_btn), horiz_scroll_toggle_icon);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(horiz_scroll_toggle_btn), FALSE);
    gtk_widget_set_tooltip_text(horiz_scroll_toggle_btn, "Toggle horizontal scroll");
    atk_object_set_name(gtk_widget_get_accessible(horiz_scroll_toggle_btn), "Toggle horizontal scroll");
    g_signal_connect(horiz_scroll_toggle_btn, "toggled", G_CALLBACK(on_horiz_scroll_toggle), horiz_scroll_toggle_icon);
    gtk_box_pack_start(GTK_BOX(toolbar), horiz_scroll_toggle_btn, FALSE, FALSE, 1);

    /* Separator */
    GtkWidget *separator_e = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(toolbar), separator_e, FALSE, FALSE, 5);

    /* Title bar toggle button*/
    GtkWidget *title_bar_toggle_icon = gtk_image_new_from_file("./data/icons/title-bar-on.png");
    GtkWidget *title_bar_toggle_btn = gtk_toggle_button_new();
    gtk_button_set_image(GTK_BUTTON(title_bar_toggle_btn), title_bar_toggle_icon);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(title_bar_toggle_btn), TRUE);
    gtk_widget_set_tooltip_text(title_bar_toggle_btn, "Toggle title bar visibility");
    atk_object_set_name(gtk_widget_get_accessible(title_bar_toggle_btn), "Toggle title bar visibility");
    g_signal_connect(title_bar_toggle_btn, "toggled", G_CALLBACK(on_title_bar_toggle), title_bar_toggle_icon);
    gtk_box_pack_start(GTK_BOX(toolbar), title_bar_toggle_btn, FALSE, FALSE, 1);

    /* Helpers toggle button*/
    GtkWidget *helper_toggle_icon = gtk_image_new_from_file("./data/icons/sidebar-helper-off.png");
    GtkWidget *helper_toggle_btn = gtk_toggle_button_new();
    gtk_button_set_image(GTK_BUTTON(helper_toggle_btn), helper_toggle_icon);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(helper_toggle_btn), FALSE);
    gtk_widget_set_tooltip_text(helper_toggle_btn, "Helper files");
    atk_object_set_name(gtk_widget_get_accessible(helper_toggle_btn), "Helper files");
    g_signal_connect(helper_toggle_btn, "toggled", G_CALLBACK(on_helper_toggle), helper_toggle_icon);
    gtk_box_pack_start(GTK_BOX(toolbar), helper_toggle_btn, FALSE, FALSE, 1);

    /* Separator */
    GtkWidget *separator_f = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(toolbar), separator_f, FALSE, FALSE, 5);

    /* Close button*/
    GtkWidget *close_icon = gtk_image_new_from_file("./data/icons/plug.png");
    GtkWidget *close_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(close_btn), close_icon);
    gtk_widget_set_tooltip_text(close_btn, "Close");
    atk_object_set_name(gtk_widget_get_accessible(close_btn), "Close");
    g_signal_connect(close_btn, "clicked", G_CALLBACK(on_close_clicked), NULL);
    gtk_box_pack_end(GTK_BOX(toolbar), close_btn, FALSE, FALSE, 1);

    /* Maximize button*/
    GtkWidget *maximize_icon = gtk_image_new_from_file("./data/icons/maximize-2.png");
    GtkWidget *maximize_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(maximize_btn), maximize_icon);
    gtk_widget_set_tooltip_text(maximize_btn, "Maximize");
    atk_object_set_name(gtk_widget_get_accessible(maximize_btn), "Maximize");
    g_signal_connect(maximize_btn, "clicked", G_CALLBACK(on_maximize_clicked), window);
    gtk_box_pack_end(GTK_BOX(toolbar), maximize_btn, FALSE, FALSE, 1);

    /* Minimize button*/
    GtkWidget *minimize_icon = gtk_image_new_from_file("./data/icons/minimize-2.png");
    GtkWidget *minimize_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(minimize_btn), minimize_icon);
    gtk_widget_set_tooltip_text(minimize_btn, "Minimize");
    atk_object_set_name(gtk_widget_get_accessible(minimize_btn), "Minimize");
    g_signal_connect(minimize_btn, "clicked", G_CALLBACK(on_minimize_clicked), window);
    gtk_box_pack_end(GTK_BOX(toolbar), minimize_btn, FALSE, FALSE, 1);

    /* MAIN WINDOW PANED */
    /* Create a horizontal paned splitter containing two notebooks */
    paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(content_vbox), paned, TRUE, TRUE, 0);

    /* Left notebook (primary) */
    left_notebook = gtk_notebook_new();
    gtk_paned_pack1(GTK_PANED(paned), left_notebook, TRUE, FALSE);
    atk_object_set_name(gtk_widget_get_accessible(left_notebook), "Left Notebook");
    g_signal_connect(left_notebook, "switch-page", G_CALLBACK(on_left_notebook_switch_page), NULL);

    // Use last open session if available, otherwise default to "Default"
    const char *initial_session = "Default";
    if (sessions_model) {
        const char *last_session = sessions_model_get_last_open_session(sessions_model);
        if (last_session) {
            initial_session = last_session;
        }
    }

    current_selected_session = g_strdup(initial_session);
    update_window_title_for_session(current_selected_session);

    /* Right pane: container with notebook and toolbar */
    right_pane = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_paned_pack2(GTK_PANED(paned), right_pane, TRUE, FALSE);

    /* Right notebook (secondary) */
    right_notebook = gtk_notebook_new();
    gtk_box_pack_start(GTK_BOX(right_pane), right_notebook, TRUE, TRUE, 0);
    g_signal_connect(right_notebook, "switch-page", G_CALLBACK(on_right_notebook_switch_page), NULL);

    // Note: restore_open_tabs_for_session already handles both notebooks
    current_selected_session = g_strdup(initial_session);
    update_window_title_for_session(current_selected_session);

    /* Right pane toolbar (vertical) */
    GtkWidget *right_toolbar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(right_toolbar), "Toolbar");
    gtk_box_pack_start(GTK_BOX(right_pane), right_toolbar, FALSE, FALSE, 0);

    /* Right toolbar buttons - Open file */
    GtkWidget *right_open_file_icon = gtk_image_new_from_file("./data/icons/file-plus.png");
    GtkWidget *right_open_file_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(right_open_file_btn), right_open_file_icon);
    gtk_widget_set_tooltip_text(right_open_file_btn, "Open file");
    atk_object_set_name(gtk_widget_get_accessible(right_open_file_btn), "Open file");
    g_signal_connect(right_open_file_btn, "clicked", G_CALLBACK(on_open_helper_file_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(right_toolbar), right_open_file_btn, FALSE, FALSE, 1);

    /* Right toolbar - Close file */
    GtkWidget *right_close_file_icon = gtk_image_new_from_file("./data/icons/file-minus.png");
    GtkWidget *right_close_file_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(right_close_file_btn), right_close_file_icon);
    gtk_widget_set_tooltip_text(right_close_file_btn, "Close file");
    atk_object_set_name(gtk_widget_get_accessible(right_close_file_btn), "Close file");
    gtk_box_pack_start(GTK_BOX(right_toolbar), right_close_file_btn, FALSE, FALSE, 1);

    /* Right toolbar separator */
    GtkWidget *right_sep_a = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(right_toolbar), right_sep_a, FALSE, FALSE, 5);

    /* Right toolbar - Page up */
    GtkWidget *right_page_up_icon = gtk_image_new_from_file("./data/icons/page-up.png");
    GtkWidget *right_page_up_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(right_page_up_btn), right_page_up_icon);
    gtk_widget_set_tooltip_text(right_page_up_btn, "Page up");
    atk_object_set_name(gtk_widget_get_accessible(right_page_up_btn), "Page up");
    gtk_box_pack_start(GTK_BOX(right_toolbar), right_page_up_btn, FALSE, FALSE, 1);

    /* Right toolbar - Page down */
    GtkWidget *right_page_down_icon = gtk_image_new_from_file("./data/icons/page-down.png");
    GtkWidget *right_page_down_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(right_page_down_btn), right_page_down_icon);
    gtk_widget_set_tooltip_text(right_page_down_btn, "Page down");
    atk_object_set_name(gtk_widget_get_accessible(right_page_down_btn), "Page down");
    gtk_box_pack_start(GTK_BOX(right_toolbar), right_page_down_btn, FALSE, FALSE, 1);

    /* Right toolbar separator */
    GtkWidget *right_sep_b = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(right_toolbar), right_sep_b, FALSE, FALSE, 5);

    /* Right toolbar - Zoom in */
    GtkWidget *right_zoom_in_icon = gtk_image_new_from_file("./data/icons/zoom-in.png");
    GtkWidget *right_zoom_in_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(right_zoom_in_btn), right_zoom_in_icon);
    gtk_widget_set_tooltip_text(right_zoom_in_btn, "Zoom in");
    atk_object_set_name(gtk_widget_get_accessible(right_zoom_in_btn), "Zoom in");
    gtk_box_pack_start(GTK_BOX(right_toolbar), right_zoom_in_btn, FALSE, FALSE, 1);
    g_signal_connect(G_OBJECT(right_zoom_in_btn), "clicked", G_CALLBACK(on_zoom_in_right), NULL);

    /* Right toolbar - Zoom out */
    GtkWidget *right_zoom_out_icon = gtk_image_new_from_file("./data/icons/zoom-out.png");
    GtkWidget *right_zoom_out_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(right_zoom_out_btn), right_zoom_out_icon);
    gtk_widget_set_tooltip_text(right_zoom_out_btn, "Zoom out");
    atk_object_set_name(gtk_widget_get_accessible(right_zoom_out_btn), "Zoom out");
    gtk_box_pack_start(GTK_BOX(right_toolbar), right_zoom_out_btn, FALSE, FALSE, 1);
    g_signal_connect(G_OBJECT(right_zoom_out_btn), "clicked", G_CALLBACK(on_zoom_out_right), NULL);

    /* Right toolbar separator */
    GtkWidget *right_sep_c = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(right_toolbar), right_sep_c, FALSE, FALSE, 5);

    /* Right toolbar - Page column */
    GtkWidget *right_column_icon = gtk_image_new_from_file("./data/icons/column.png");
    right_column_btn = gtk_radio_button_new(NULL);
    gtk_button_set_image(GTK_BUTTON(right_column_btn), right_column_icon);
    gtk_widget_set_tooltip_text(right_column_btn, "Page column");
    atk_object_set_name(gtk_widget_get_accessible(right_column_btn), "Page column");
    g_object_set_data(G_OBJECT(right_column_btn), "layout-id", GINT_TO_POINTER(0 + 1));
    g_signal_connect(right_column_btn, "toggled", G_CALLBACK(on_layout_right_toggled), NULL);
    gtk_box_pack_start(GTK_BOX(right_toolbar), right_column_btn, FALSE, FALSE, 1);

    /* Right toolbar - Page double column */
    GtkWidget *right_double_column_icon = gtk_image_new_from_file("./data/icons/double-column.png");
    right_double_column_btn = gtk_radio_button_new_from_widget(GTK_RADIO_BUTTON(right_column_btn));
    gtk_button_set_image(GTK_BUTTON(right_double_column_btn), right_double_column_icon);
    gtk_widget_set_tooltip_text(right_double_column_btn, "Page double column");
    atk_object_set_name(gtk_widget_get_accessible(right_double_column_btn), "Page double column");
    g_object_set_data(G_OBJECT(right_double_column_btn), "layout-id", GINT_TO_POINTER(1 + 1));
    g_signal_connect(right_double_column_btn, "toggled", G_CALLBACK(on_layout_right_toggled), NULL);
    gtk_box_pack_start(GTK_BOX(right_toolbar), right_double_column_btn, FALSE, FALSE, 1);

    /* Right toolbar - Page row */
    GtkWidget *right_row_icon = gtk_image_new_from_file("./data/icons/row.png");
    right_row_btn = gtk_radio_button_new_from_widget(GTK_RADIO_BUTTON(right_column_btn));
    gtk_button_set_image(GTK_BUTTON(right_row_btn), right_row_icon);
    gtk_widget_set_tooltip_text(right_row_btn, "Page row");
    atk_object_set_name(gtk_widget_get_accessible(right_row_btn), "Page row");
    g_object_set_data(G_OBJECT(right_row_btn), "layout-id", GINT_TO_POINTER(2 + 1));
    g_signal_connect(right_row_btn, "toggled", G_CALLBACK(on_layout_right_toggled), NULL);
    gtk_box_pack_start(GTK_BOX(right_toolbar), right_row_btn, FALSE, FALSE, 1);

    /* Right toolbar separator */
    GtkWidget *right_sep_d = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(right_toolbar), right_sep_d, FALSE, FALSE, 5);

    /* Right toolbar - Horizontal scroll toggle */
    GtkWidget *right_horiz_scroll_icon = gtk_image_new_from_file("./data/icons/horiz-scroll-off.png");
    GtkWidget *right_horiz_scroll_btn = gtk_toggle_button_new();
    gtk_button_set_image(GTK_BUTTON(right_horiz_scroll_btn), right_horiz_scroll_icon);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(right_horiz_scroll_btn), FALSE);
    gtk_widget_set_tooltip_text(right_horiz_scroll_btn, "Toggle horizontal scroll");
    atk_object_set_name(gtk_widget_get_accessible(right_horiz_scroll_btn), "Toggle horizontal scroll");
    g_signal_connect(right_horiz_scroll_btn, "toggled", G_CALLBACK(on_horiz_scroll_toggle), right_horiz_scroll_icon);
    gtk_box_pack_start(GTK_BOX(right_toolbar), right_horiz_scroll_btn, FALSE, FALSE, 1);

    return window;
}

void hide_right_pane(void) {
    if (right_pane) {
        gtk_widget_hide(GTK_WIDGET(right_pane));
    }
}
