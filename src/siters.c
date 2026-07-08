#include <gtk/gtk.h>
#include <atk/atk.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include "pdf.h"
#include "log.h"
#include <math.h>
#include "siters.h"
#include "sessions_model.h"
#include "session_model.h"
#include "document_model.h"

#include "mem_debug.h"

/* To limit RAM use as large zoom takes many MB of resources */
#define MAX_SURFACE_DIM 3500
#define MAX_CACHE_BYTES (80 * 1024 * 1024)

/* DATADIR is normally defined by -DDATADIR=... at build time.
   This fallback lets clang-based tools parse the file without flags. */
#ifndef DATADIR
#define DATADIR "."
#endif

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
    PdfrDoc *doc;
    int n_pages;
    int cur_page;
    GtkWidget *drawing;
    double zoom;
    GdkRGBA page_color;
    /* per-document storage */
    char *current_file;
    GtkWidget *tab_label;  /* reference to the label widget in the tab */
    GtkWidget *tab_label_box; /* reference to the label container box */
    GtkWidget *tab_label_close_btn; /* reference to the close button */
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
    guint zoom_scroll_source_id; /* pending zoom-scroll idle callback source id */
    guint h_scrollbar_timer_id;  /* auto-hide timer for horizontal scrollbar */
    guint scroll_doc_debounce_id; /* debounce timer for document model update on scroll */
    int zoom_scroll_target_page; /* target page for post-zoom scroll restore */
    double zoom_scroll_fraction; /* fractional offset within target page */
    gboolean dragging;          /* drag-to-scroll in progress */
    double drag_start_x;        /* cursor X at drag start in widget coords */
    double drag_start_y;        /* cursor Y at drag start in widget coords */
    double drag_scroll_x;       /* h_scrollbar value at drag start */
    double drag_scroll_y;       /* vadjustment value at drag start */
    double *cached_page_widths;  /* raw page widths from pdfr_page_size */
    double *cached_page_heights; /* raw page heights from pdfr_page_size */
    double *cached_page_x0;      /* page bounds origin x0 from pdfr_page_size */
    double *cached_page_y0;      /* page bounds origin y0 from pdfr_page_size */
    double max_page_h;           /* tallest page height (scaled) for row view sizing */
    cairo_surface_t **page_cache; /* cached rendered page surfaces (NULL = not cached) */
    int total_cache_bytes;        /* sum of pixel-buffer bytes across all cached surfaces */
    PdfrLink **page_links;     /* array[n_pages]: head of PerPageLink list, NULL = not loaded */
    int page_links_n;       /* size of page_links array (== n_pages) */
    GdkCursorType last_cursor_type; /* last cursor set on drawing area (to avoid redundant X11 calls) */
    gint64 last_cursor_check;       /* monotonic time of last link cursor check (for throttling) */
} TabData;

typedef enum {
    SIDEBAR_NONE,
    SIDEBAR_SESSIONS,
    SIDEBAR_TOC,
    SIDEBAR_SETTINGS,
    SIDEBAR_FILE_INFO
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

/* Debounce ID for saving state after rapid zoom events */
static guint zoom_save_debounce_id = 0;

/* TOC sidebar components */
static GtkWidget *toc_container;
static GtkWidget *toc_tree_view;
static GtkTreeStore *toc_tree_store;
static gboolean toc_tree_syncing = FALSE;

/* Last directory used in file chooser */
static char *last_open_dir = NULL;

/* Tracks the last page selected in the TOC tree to avoid redundant updates */
static int last_toc_selected_page = -1;

/* Settings sidebar components */
static GtkWidget *settings_container;
static GtkWidget *tabbar_combo;
static GtkWidget *tab_width_spin;
static GtkWidget *left_color_btn;
static GtkWidget *right_color_btn;

/* Sidebar toggle buttons */
static GtkWidget *sessions_btn = NULL;
static GtkWidget *toc_btn = NULL;
static GtkWidget *settings_btn = NULL;
static GtkWidget *file_info_btn = NULL;

/* File info sidebar */
static GtkWidget *file_info_container;
static GtkWidget *file_info_name_label;
static GtkWidget *file_info_path_label;
static GtkWidget *file_info_size_label;
static GtkWidget *file_info_pages_label;
static GtkWidget *file_info_search_entry;
static GtkWidget *file_info_search_btn;
static GtkWidget *file_info_search_results_view;
static GtkListStore *file_info_search_results_store;
static GtkWidget *file_info_search_no_results;
static GtkWidget *file_info_search_overflow_label;

enum {
    SEARCH_COL_PAGE = 0,
    SEARCH_COL_COUNT,
    SEARCH_COL_LABEL,
    SEARCH_COL_NCOL
};

#define SEARCH_MAX_PER_PAGE 10
#define SEARCH_MAX_PAGES 200

typedef struct {
    int page;
    int n_matches;
    PdfrRect rects[SEARCH_MAX_PER_PAGE];
} SearchPageResult;

static SearchPageResult search_page_results[SEARCH_MAX_PAGES];
static int search_page_results_n = 0;

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

typedef enum {
    TOC_COL_LABEL = 0,
    TOC_COL_PAGE,
    TOC_COL_NAMED_DEST,
    TOC_COL_COUNT
} TOCTreeCols;

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
static GtkWidget *page_nav_overlay = NULL;
static gboolean page_spin_syncing = FALSE;

/* Right notebook page jump widget */
static GtkWidget *right_page_entry = NULL;
static GtkWidget *right_page_total_label = NULL;
static GtkWidget *right_page_nav_overlay = NULL;
static gboolean right_page_spin_syncing = FALSE;

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
static void sync_right_page_widget_from_tab(TabData *tab);
static void on_page_entry_activate(GtkEntry *entry, gpointer user_data);
static void on_right_page_entry_activate(GtkEntry *entry, gpointer user_data);
static void on_page_up_left(GtkButton *btn, gpointer user_data);
static void on_page_down_left(GtkButton *btn, gpointer user_data);
static void on_page_up_right(GtkButton *btn, gpointer user_data);
static void on_page_down_right(GtkButton *btn, gpointer user_data);

static void cancel_tab_restore(TabData *tab);
static void cancel_doc_model_debounce(TabData *tab);
static void destroy_tab_data(gpointer data);
static void apply_layout_to_tab(TabData *tab, int layout);
static void on_layout_left_toggled(GtkToggleButton *btn, gpointer user_data);
static void on_layout_right_toggled(GtkToggleButton *btn, gpointer user_data);
static void sync_left_layout_buttons(TabData *tab);
static void sync_right_layout_buttons(TabData *tab);

/* Function prototypes */
void save_state(void);
void populate_sessions_treeview(void);
void populate_toc_treeview(void);
static void update_sessions_tree_document_selection_for_tab(TabData *tab);
static void update_sessions_tree_document_selection(void);
static void update_toc_selection_for_current_page(TabData *tab);

static void on_toc_row_activated(GtkTreeView *tree_view, GtkTreePath *path, GtkTreeViewColumn *column, gpointer user_data);

void hide_right_pane(void);
static void set_right_notebook_session(const gchar *session_name);
static void on_tab_close_clicked(GtkButton *btn, gpointer user_data);
static void on_page_entry_insert_text(GtkEditable *editable,
                                      const gchar *text,
                                      gint length,
                                      gint *position,
                                      gpointer user_data);
static void on_tab_scrolled_size_allocate(GtkWidget *widget, GdkRectangle *allocation, gpointer user_data);

/* Settings apply functions */
static void apply_tabbar_position(const char *pos);
static void apply_tab_width(int width);

/* Session tab persistence functions */
static void save_open_tabs_for_session(const char *session_name);
static void restore_open_tabs_for_session(const char *session_name);

/* PDF handling function prototypes */
static TabData *create_new_tab(GtkWidget *notebook);
static void load_file_into_tab(TabData *tab, const char *filename);
static void queue_draw(TabData *tab);
static gboolean on_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data);
static void build_continuous_view(TabData *tab);
static void invalidate_page_cache(TabData *tab);
static void scroll_to_page(TabData *tab, int page, double target_y);
static void on_scroll_value_changed(GtkAdjustment *adj, gpointer user_data);
static gboolean on_drawing_scroll(GtkWidget *widget, GdkEventScroll *event, gpointer user_data);
static gboolean on_drawing_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
static gboolean on_drawing_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
static gboolean restore_zoom_scroll_cb(gpointer user_data);
static void open_file_in_notebook(GtkWidget *notebook, gboolean is_helper);
static void on_open_file_clicked(GtkButton *button, gpointer user_data);
static void on_open_helper_file_clicked(GtkButton *button, gpointer user_data);
static void on_close_file_clicked(GtkButton *btn, gpointer user_data);
static void on_close_helper_file_clicked(GtkButton *btn, gpointer user_data);
static void update_last_read_for_notebook(GtkNotebook *notebook, GtkWidget *page, guint page_num);
static session_model_t *get_current_session_model(void);
static void apply_page_color_to_notebook(GtkWidget *notebook, const char *color_str);
static void on_left_color_set(GtkColorButton *btn, gpointer user_data);
static void on_right_color_set(GtkColorButton *btn, gpointer user_data);
static gboolean detect_system_dark_theme(void);
static void apply_dark_css(gboolean apply);
static void on_keep_dark_toggled(GtkToggleButton *btn, gpointer user_data);
static void on_sessions_toggled(GtkToggleButton *btn, gpointer user_data);
static void on_toc_toggled(GtkToggleButton *btn, gpointer user_data);
static void on_settings_toggled(GtkToggleButton *btn, gpointer user_data);
static void on_left_file_info_toggled(GtkToggleButton *btn, gpointer user_data);
static void on_right_file_info_clicked(GtkButton *btn, gpointer user_data);
static void clear_file_info_search_results(void);
static SearchPageResult* find_search_result_for_page(int page_1based);
static void on_file_info_search_activated(GtkEntry *entry, gpointer user_data);
static void on_file_info_search_clicked(GtkButton *btn, gpointer user_data);
static void on_file_info_search_row_activated(GtkTreeView *tree_view, GtkTreePath *path, GtkTreeViewColumn *column, gpointer user_data);
static void on_left_notebook_switch_page(GtkNotebook *notebook, GtkWidget *page, guint page_num, gpointer user_data);
static void on_right_notebook_switch_page(GtkNotebook *notebook, GtkWidget *page, guint page_num, gpointer user_data);
static int find_matching_tab_index(GtkNotebook *notebook, const char *target_uri);
static void on_notebook_page_reordered(GtkNotebook *notebook, GtkWidget *page, guint page_num, gpointer user_data);
static void start_initial_scroll_restore(TabData *tab, int target_page, double target_zoom, double target_fraction);
static char* make_document_key(const char *session_name, const char *uri, gboolean is_helper);

/* Theme-aware icon color: dark theme uses yellow, light theme uses dark brown */
static gboolean is_dark_theme = TRUE;

/* User override to force dark theme regardless of system */
static gboolean keep_dark_theme = FALSE;
static GtkWidget *keep_dark_check = NULL;
static GtkCssProvider *dark_css_provider = NULL;

/* Detect whether the system theme is dark by checking gtk-application-prefer-dark-theme
   and gtk-theme-name for keywords. */
static gboolean detect_system_dark_theme(void) {
    gboolean prefer_dark = FALSE;
    GtkSettings *settings = gtk_settings_get_default();
    if (settings) {
        g_object_get(settings, "gtk-application-prefer-dark-theme", &prefer_dark, NULL);
        gchar *theme_name = NULL;
        g_object_get(settings, "gtk-theme-name", &theme_name, NULL);
        if (theme_name) {
            gchar *lower = g_ascii_strdown(theme_name, -1);
            if (strstr(lower, "dark") || strstr(lower, "black") ||
                strstr(lower, "night") || strstr(lower, "nokto"))
                prefer_dark = TRUE;
            g_free(lower);
            g_free(theme_name);
        }
    }
    return prefer_dark;
}

/* Create a GtkImage from an SVG icon, recoloring it for the current theme.
   The 'name' parameter is the stem of the SVG file (e.g. "zoom-in" for zoom-in.svg). */
static GtkWidget* create_toolbar_icon(const char *name) {
    char *path = g_strdup_printf(DATADIR "/data/icons/%s.svg", name);
    gchar *svg_content;
    gsize length;
    if (!g_file_get_contents(path, &svg_content, &length, NULL)) {
        LOG_WARN("Failed to read icon SVG %s", path);
        g_free(path);
        return gtk_image_new();
    }
    g_free(path);

    const char *target_color = is_dark_theme ? "#FFFFAD" : "#141400";

    /* Recolor all #XXXXXX values in the SVG to match the theme */
    char *p = svg_content;
    while ((p = strstr(p, "#FFFFAD")) != NULL) {
        memcpy(p, target_color, 7);
        p += 7;
    }
    p = svg_content;
    while ((p = strstr(p, "#ffffad")) != NULL) {
        memcpy(p, target_color, 7);
        p += 7;
    }

    GBytes *bytes = g_bytes_new_take(svg_content, length);
    GInputStream *stream = g_memory_input_stream_new_from_bytes(bytes);
    GError *pixbuf_err = NULL;
    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_stream_at_scale(stream, 20, 20, TRUE, NULL, &pixbuf_err);
    g_object_unref(stream);
    g_bytes_unref(bytes);

    GtkWidget *image;
    if (pixbuf) {
        image = gtk_image_new_from_pixbuf(pixbuf);
        g_object_unref(pixbuf);
    } else {
        LOG_WARN("Failed to render icon pixbuf: %s", pixbuf_err->message);
        g_clear_error(&pixbuf_err);
        image = gtk_image_new();
    }
    return image;
}

static gchar* format_file_size(goffset size) {
    if (size < 1024)
        return g_strdup_printf("%lld B", (long long)size);
    else if (size < 1024 * 1024)
        return g_strdup_printf("%.1f KB", size / 1024.0);
    else if (size < 1024 * 1024 * 1024)
        return g_strdup_printf("%.1f MB", size / (1024.0 * 1024.0));
    else
        return g_strdup_printf("%.1f GB", size / (1024.0 * 1024.0 * 1024.0));
}

static void update_file_info_labels(TabData *tab) {
    clear_file_info_search_results();
    if (file_info_search_entry) {
        gtk_entry_set_text(GTK_ENTRY(file_info_search_entry), "");
    }
    if (!tab || !tab->current_file) {
        gtk_label_set_text(GTK_LABEL(file_info_name_label), "Name: (no file)");
        gtk_label_set_text(GTK_LABEL(file_info_path_label), "Path: (none)");
        gtk_label_set_text(GTK_LABEL(file_info_size_label), "Size: (none)");
        gtk_label_set_text(GTK_LABEL(file_info_pages_label), "Pages: (none)");
        return;
    }

    gchar *basename = g_path_get_basename(tab->current_file);
    gchar *name_text = g_strdup_printf("Name: %s", basename);
    g_free(basename);
    gtk_label_set_text(GTK_LABEL(file_info_name_label), name_text);
    g_free(name_text);

    gchar *path_text = g_strdup_printf("Path: %s", tab->current_file);
    gtk_label_set_text(GTK_LABEL(file_info_path_label), path_text);
    g_free(path_text);

    GFile *gf = g_file_new_for_path(tab->current_file);
    GFileInfo *info = g_file_query_info(gf, G_FILE_ATTRIBUTE_STANDARD_SIZE,
                                         G_FILE_QUERY_INFO_NONE, NULL, NULL);
    if (info) {
        goffset size = g_file_info_get_size(info);
        gchar *size_str = format_file_size(size);
        gchar *size_text = g_strdup_printf("Size: %s", size_str);
        gtk_label_set_text(GTK_LABEL(file_info_size_label), size_text);
        g_free(size_text);
        g_free(size_str);
        g_object_unref(info);
    } else {
        gtk_label_set_text(GTK_LABEL(file_info_size_label), "Size: Unknown");
    }
    g_object_unref(gf);

    if (tab->doc) {
        int n_pages = pdfr_count_pages(tab->doc);
        gchar *pages_text = g_strdup_printf("Pages: %d", n_pages);
        gtk_label_set_text(GTK_LABEL(file_info_pages_label), pages_text);
        g_free(pages_text);
    } else {
        gtk_label_set_text(GTK_LABEL(file_info_pages_label), "Pages: N/A");
    }
}


/* Refresh a single button's icon from its stored icon-name data.
   Toggle buttons use icon-on/icon-off based on their current state. */
static void recolor_toolbar_button(GtkWidget *btn) {
    const char *icon_on = g_object_get_data(G_OBJECT(btn), "icon-on");
    const char *icon_off = g_object_get_data(G_OBJECT(btn), "icon-off");
    if (icon_on && icon_off && GTK_IS_TOGGLE_BUTTON(btn)) {
        gboolean active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(btn));
        gtk_button_set_image(GTK_BUTTON(btn), create_toolbar_icon(active ? icon_on : icon_off));
        return;
    }
    const char *icon_name = g_object_get_data(G_OBJECT(btn), "icon-name");
    if (icon_name) {
        gtk_button_set_image(GTK_BUTTON(btn), create_toolbar_icon(icon_name));
    }
}

/* Recursively walk a container and recolor all buttons with icon data */
static void recolor_toolbar_children(GtkWidget *parent) {
    if (!parent) return;
    if (GTK_IS_BUTTON(parent)) {
        recolor_toolbar_button(parent);
        return;
    }
    if (GTK_IS_CONTAINER(parent)) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(parent));
        for (GList *iter = children; iter; iter = iter->next) {
            recolor_toolbar_children(GTK_WIDGET(iter->data));
        }
        g_list_free(children);
    }
}

/* Recolor all toolbar icons */
static void recolor_all_toolbars(void) {
    if (main_hbox) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(main_hbox));
        for (GList *iter = children; iter; iter = iter->next) {
            recolor_toolbar_children(GTK_WIDGET(iter->data));
        }
        g_list_free(children);
    }
    if (right_pane) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(right_pane));
        for (GList *iter = children; iter; iter = iter->next) {
            recolor_toolbar_children(GTK_WIDGET(iter->data));
        }
        g_list_free(children);
    }
}

/* Recolor all toolbar icons when the system theme changes.
   When keep_dark_theme is active, the CSS provider is the sole mechanism
   for dark widget styling — no GTK settings are touched. */
static void on_theme_changed(GObject *gobject, GParamSpec *pspec, gpointer user_data) {
    (void)gobject;
    (void)pspec;
    (void)user_data;
    if (keep_dark_theme) {
        is_dark_theme = TRUE;
        apply_dark_css(TRUE);
    } else {
        apply_dark_css(FALSE);
        is_dark_theme = detect_system_dark_theme();
        if (sessions_model)
            sessions_model_set_theme(sessions_model, is_dark_theme ? "dark" : "light");
    }
    recolor_all_toolbars();
}

/* Dark CSS theme applied via GtkCssProvider when keep_dark_theme is active.
   This overrides GTK widget colors to dark even when the system theme is light,
   working around the limitation of gtk-theme-name:dark and prefer-dark-theme
   not being reliably re-applied on system theme changes. */
static void apply_dark_css(gboolean apply) {
    GdkScreen *screen = gdk_screen_get_default();
    if (!screen) return;
    if (apply) {
        if (!dark_css_provider) {
            dark_css_provider = gtk_css_provider_new();
            const char *css =
                "window, window.background, box, notebook, scrolledwindow,\n"
                "popover, popover.background, menubar, menu, .sidebar { background: #2e2e2e; }\n"
                "menubar, menu { color: #ffffff; }\n"
                "label, popover label { color: #ffffff; }\n"
                "button, combobox button, spinbutton button {\n"
                "    background: #3c3c3c; border: 1px solid #2F2F34;\n"
                "    color: #ffffff;\n"
                "}\n"
                "button:hover, combobox button:hover, spinbutton button:hover {\n"
                "    background: #4a4a4a;\n"
                "}\n"
                "button:checked {\n"
                "    background: #505050; border-color: #454655;\n"
                "}\n"
                "entry, spinbutton, spinbutton entry, treeview, treeview.view,\n"
                "drawingarea { background: #1e1e1e; color: #ffffff; }\n"
                "entry { border: 1px solid #2F2F34; }\n"
                "treeview:selected, treeview.view:selected {\n"
                "    background: #3584e4;\n"
                "}\n"
                "notebook > header { background: #353535; }\n"
                "notebook tab { background: #353535; color: #9a9a9a; }\n"
                "notebook tab:checked { background: #2e2e2e; color: #ffffff; }\n"
                "scrollbar { background: #2e2e2e; }\n"
                "scrollbar slider { background: #2F2F34; }\n"
                "paned > separator, separator { background: #2F2F34; }\n"
                "menu menuitem:hover { background: #3584e4; }\n"
                "dialog .background { background: #2e2e2e; }\n";
            GError *css_err = NULL;
            if (!gtk_css_provider_load_from_data(dark_css_provider, css, -1, &css_err)) {
                LOG_ERROR("Failed to load dark theme CSS: %s", css_err->message);
                g_clear_error(&css_err);
            }
        }
        gtk_style_context_add_provider_for_screen(screen,
            GTK_STYLE_PROVIDER(dark_css_provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    } else {
        if (dark_css_provider) {
            gtk_style_context_remove_provider_for_screen(screen,
                GTK_STYLE_PROVIDER(dark_css_provider));
        }
    }
}

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
    if (tab->doc) {
        pdfr_close(tab->doc);
    }
    g_free(tab->current_file);
    invalidate_page_cache(tab);
    g_free(tab->page_cache);
    g_free(tab->cached_page_widths);
    g_free(tab->cached_page_heights);
    g_free(tab->cached_page_x0);
    g_free(tab->cached_page_y0);
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

static void json_emit_session_docs(JsonBuilder *builder, const GList *uris, const char *side_prefix, GHashTable *models, const char *session_name) {
    json_builder_begin_array(builder);
    for (const GList *d = uris; d; d = d->next) {
        const char *uri = (const char *)d->data;
        const char *side = (strcmp(side_prefix, "right") == 0) ? "right" : "left";
        char *key = make_document_key(session_name, uri, side[0] == 'r');
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

    const gchar *cfg_override = g_getenv("SITERS_CONFIG_DIR");
    const gchar *config_dir = cfg_override ? cfg_override : g_get_user_config_dir();
    gchar *app_config_dir = g_build_filename(config_dir, "siters", NULL);
    if (g_mkdir_with_parents(app_config_dir, 0755) != 0) {
        LOG_ERROR("Failed to create config directory %s: %s",
                  app_config_dir, g_strerror(errno));
    }

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
    if (sessions_model) {
        json_builder_set_member_name(builder, "tabbar_position");
        json_builder_add_string_value(builder, sessions_model_get_tabbar_position(sessions_model));
        json_builder_set_member_name(builder, "tab_width");
        json_builder_add_int_value(builder, sessions_model_get_tab_width(sessions_model));
        json_builder_set_member_name(builder, "theme");
        json_builder_add_string_value(builder, sessions_model_get_theme(sessions_model));
        json_builder_set_member_name(builder, "keep_dark");
        json_builder_add_boolean_value(builder, sessions_model_get_keep_dark(sessions_model));
    }

    if (last_open_dir) {
        json_builder_set_member_name(builder, "last_open_dir");
        json_builder_add_string_value(builder, last_open_dir);
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
                    json_emit_session_docs(builder, session_model_get_document_urls(session), "left", document_models, session_name);

                    json_builder_set_member_name(builder, "helper_documents");
                    json_emit_session_docs(builder, session_model_get_helper_document_urls(session), "right", document_models, session_name);

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
    if (!json_str) {
        LOG_ERROR("JSON serialization failed");
    }
    gchar *config_file = g_build_filename(app_config_dir, "siters.json", NULL);
    if (json_str) {
        GError *fs_error = NULL;
        if (!g_file_set_contents(config_file, json_str, -1, &fs_error)) {
            LOG_ERROR("Failed to write state file %s: %s", config_file, fs_error->message);
            g_clear_error(&fs_error);
        }
        g_free(json_str);
    }
    g_free(config_file);
    g_free(app_config_dir);
    g_object_unref(gen);
    json_node_free(root);
    g_object_unref(builder);
}

static void load_session_doc_array(JsonArray *arr, session_model_t *session, const char *default_side, const char *session_name) {
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
            char *key = make_document_key(session_name, uri, is_helper);
            g_hash_table_insert(document_models, key, dm);
        }
    }
}

void load_state(void) {
    const gchar *cfg_override = g_getenv("SITERS_CONFIG_DIR");
    const gchar *config_dir = cfg_override ? cfg_override : g_get_user_config_dir();
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

    g_free(last_open_dir);
    last_open_dir = g_strdup(json_object_get_string_member_with_default(root_obj, "last_open_dir", NULL));

    if (sessions_model && win) {
        const char *pos = json_object_get_string_member_with_default(win, "tabbar_position", "top");
        sessions_model_set_tabbar_position(sessions_model, pos);
        int tw = (int)json_object_get_int_member_with_default(win, "tab_width", 100);
        sessions_model_set_tab_width(sessions_model, tw);
        /* Theme is auto-detected in create_main_window; always keep the model
           in sync with auto-detection so create_toolbar_icon stays correct. */
        sessions_model_set_theme(sessions_model, is_dark_theme ? "dark" : "light");

        /* Restore keep_dark override */
        gboolean saved_keep = json_object_get_boolean_member_with_default(win, "keep_dark", FALSE);
        sessions_model_set_keep_dark(sessions_model, saved_keep);
        keep_dark_theme = saved_keep;
        if (keep_dark_check) {
            g_signal_handlers_block_by_func(keep_dark_check, G_CALLBACK(on_keep_dark_toggled), NULL);
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(keep_dark_check), saved_keep);
            g_signal_handlers_unblock_by_func(keep_dark_check, G_CALLBACK(on_keep_dark_toggled), NULL);
        }
        if (saved_keep) {
            is_dark_theme = TRUE;
            sessions_model_set_theme(sessions_model, "dark");
            apply_dark_css(TRUE);
            recolor_all_toolbars();
        }
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
                        load_session_doc_array(json_object_get_array_member(sd, "documents"), session, "left", name);
                        load_session_doc_array(json_object_get_array_member(sd, "helper_documents"), session, "right", name);
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
            sync_left_layout_buttons(get_current_left_tab());
            sync_right_layout_buttons(get_current_right_tab());
            sync_page_widget_from_tab(get_current_left_tab());
            update_window_title_for_session(current_selected_session);
        } else if (loaded_session) {
            restore_open_tabs_for_session(loaded_session);
            sync_left_layout_buttons(get_current_left_tab());
            sync_right_layout_buttons(get_current_right_tab());
            sync_page_widget_from_tab(get_current_left_tab());
        }
    }

    if (sessions_model) {
        apply_tabbar_position(sessions_model_get_tabbar_position(sessions_model));
        apply_tab_width(sessions_model_get_tab_width(sessions_model));
        /* Sync settings UI widgets with loaded values */
        if (tabbar_combo) {
            const char *pos = sessions_model_get_tabbar_position(sessions_model);
            if (g_strcmp0(pos, "left") == 0)
                gtk_combo_box_set_active(GTK_COMBO_BOX(tabbar_combo), 0);
            else if (g_strcmp0(pos, "right") == 0)
                gtk_combo_box_set_active(GTK_COMBO_BOX(tabbar_combo), 2);
            else
                gtk_combo_box_set_active(GTK_COMBO_BOX(tabbar_combo), 1);
        }
        if (tab_width_spin)
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(tab_width_spin),
                                       sessions_model_get_tab_width(sessions_model));

        /* Sync color buttons with current session's saved colors */
        session_model_t *cur = get_current_session_model();
        if (cur) {
            const char *pc = session_model_get_page_color(cur);
            const char *hpc = session_model_get_helper_page_color(cur);
            if (left_color_btn) {
                g_signal_handlers_block_by_func(left_color_btn, G_CALLBACK(on_left_color_set), NULL);
                if (pc) {
                    GdkRGBA c;
                    if (gdk_rgba_parse(&c, pc))
                        gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(left_color_btn), &c);
                }
                g_signal_handlers_unblock_by_func(left_color_btn, G_CALLBACK(on_left_color_set), NULL);
            }
            if (right_color_btn) {
                g_signal_handlers_block_by_func(right_color_btn, G_CALLBACK(on_right_color_set), NULL);
                if (hpc) {
                    GdkRGBA c;
                    if (gdk_rgba_parse(&c, hpc))
                        gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(right_color_btn), &c);
                }
                g_signal_handlers_unblock_by_func(right_color_btn, G_CALLBACK(on_right_color_set), NULL);
            }
            /* Re-apply colors to all tabs */
            apply_page_color_to_notebook(left_notebook, pc ? pc : "#FFFFFF");
            apply_page_color_to_notebook(right_notebook, hpc ? hpc : "#FFFFFF");
        }
    }

    g_object_unref(parser);
    g_free(config_file);
    g_free(app_config_dir);
}

/* Helper: Calculate PPI-based scale for a tab */
static double get_ppi_scale(TabData *tab) {
    double eff = tab->zoom > 0 ? tab->zoom : 96.0;
    return eff / 72.0;
}

/* Helper: Calculate offset to top of a given page at current PPI zoom */
static double calculate_page_top_offset_ppi(TabData *tab, int page_idx) {
    if (!tab || !tab->cached_page_widths || page_idx < 0 || page_idx >= tab->n_pages) {
        return 0.0;
    }

    const double spacing = 6.0;
    double scale = get_ppi_scale(tab);

    double y_offset = spacing;
    if (tab->layout_mode == 2) {
        for (int i = 0; i < page_idx; ++i) {
            y_offset += tab->cached_page_widths[i] * scale + spacing;
        }
    } else if (tab->layout_mode == 1) {
        int row = page_idx / 2;
        for (int r = 0; r < row; r++) {
            double row_h = 0.0;
            for (int p = 0; p < 2; p++) {
                int idx = r * 2 + p;
                if (idx >= tab->n_pages) break;
                double h = tab->cached_page_heights[idx] * scale;
                if (h > row_h) row_h = h;
            }
            if (row_h < 1.0) row_h = 1.0;
            y_offset += row_h + spacing;
        }
    } else {
        for (int i = 0; i < page_idx; ++i) {
            y_offset += tab->cached_page_heights[i] * scale + spacing;
        }
    }
    return y_offset;
}

/* Helper: Get the height of a specific page at current PPI zoom */
static double get_page_height_ppi(TabData *tab, int page_idx) {
    if (!tab || !tab->cached_page_heights || page_idx < 0 || page_idx >= tab->n_pages) {
        return 0.0;
    }

    double ph = tab->cached_page_heights[page_idx];
    if (ph <= 0.0) return 0.0;

    double eff_zoom = tab->zoom > 0 ? tab->zoom : 96.0;
    double scale = eff_zoom / 72.0;
    return ph * scale;
}

/* Return pixel-buffer byte count for a cached surface (0 if NULL). */
static inline int surface_byte_size(cairo_surface_t *s) {
    if (!s) return 0;
    return cairo_image_surface_get_width(s) * cairo_image_surface_get_height(s) * 4;
}

/* Destroy a single cached surface and update the tab's byte counter. */
static void cache_evict_idx(TabData *tab, int idx) {
    if (!tab || idx < 0 || !tab->page_cache || !tab->page_cache[idx]) return;
    tab->total_cache_bytes -= surface_byte_size(tab->page_cache[idx]);
    cairo_surface_destroy(tab->page_cache[idx]);
    MEM_SURFACE_DESTROYED();
    tab->page_cache[idx] = NULL;
}

static void cache_page_dimensions(TabData *tab) {
    if (!tab || !tab->doc || tab->n_pages <= 0) {
        if (tab) {
            invalidate_page_cache(tab);
            g_free(tab->page_cache);
            tab->page_cache = NULL;
            g_free(tab->cached_page_widths);
            g_free(tab->cached_page_heights);
            tab->cached_page_widths = NULL;
            tab->cached_page_heights = NULL;
            g_free(tab->cached_page_x0);
            g_free(tab->cached_page_y0);
            tab->cached_page_x0 = NULL;
            tab->cached_page_y0 = NULL;
        }
        return;
    }
    invalidate_page_cache(tab);
    g_free(tab->page_cache);
    g_free(tab->cached_page_widths);
    g_free(tab->cached_page_heights);
    g_free(tab->cached_page_x0);
    g_free(tab->cached_page_y0);
    tab->page_cache = g_malloc0(sizeof(cairo_surface_t *) * tab->n_pages);
    tab->cached_page_widths = g_malloc(sizeof(double) * tab->n_pages);
    tab->cached_page_heights = g_malloc(sizeof(double) * tab->n_pages);
    tab->cached_page_x0 = g_malloc0(sizeof(double) * tab->n_pages);
    tab->cached_page_y0 = g_malloc0(sizeof(double) * tab->n_pages);
    for (int i = 0; i < tab->n_pages; ++i) {
        PdfrPage *page = pdfr_load_page(tab->doc, i);
        double pw = 0, ph = 0, px0 = 0, py0 = 0;
        if (page) {
            pdfr_page_size(tab->doc, page, &pw, &ph, &px0, &py0);
            pdfr_free_page(tab->doc, page);
        }
        tab->cached_page_widths[i] = pw > 0 ? pw : 1.0;
        tab->cached_page_heights[i] = ph > 0 ? ph : 1.0;
        tab->cached_page_x0[i] = px0;
        tab->cached_page_y0[i] = py0;

    }
}

static void invalidate_page_cache(TabData *tab) {
    if (!tab || !tab->page_cache) return;
    for (int i = 0; i < tab->n_pages; ++i)
        cache_evict_idx(tab, i);
}

static gboolean ensure_tab_doc_loaded(TabData *tab) {
    if (!tab || !tab->current_file) return FALSE;
    if (tab->doc) return TRUE;

    char *open_error = NULL;
    PdfrDoc *doc = pdfr_open(tab->current_file, &open_error);
    if (!doc) {
        LOG_ERROR("Failed to reopen PDF: %s", open_error ? open_error : "unknown error");
        free(open_error);
        return FALSE;
    }
    free(open_error);

    if (tab->doc) {
        pdfr_close(tab->doc);
    }
    tab->doc = doc;
    tab->n_pages = pdfr_count_pages(doc);
    cache_page_dimensions(tab);
    build_continuous_view(tab);
    queue_draw(tab);
    return TRUE;
}

/* Build a compound key "side:uri" to differentiate left vs right notebook state */
static char* make_document_key(const char *session_name, const char *uri, gboolean is_helper) {
    const char *side = is_helper ? "right" : "left";
    return g_strdup_printf("%s:%s:%s", session_name ? session_name : "Unknown", side, uri);
}

static void update_document_model_from_tab(TabData *tab) {
    if (!tab || !tab->current_file || !document_models) return;
    if (!tab->cached_page_widths) return;

    char *uri = g_filename_to_uri(tab->current_file, NULL, NULL);
    if (!uri) return;

    char *key = make_document_key(current_selected_session, uri, tab->is_helper);

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

static void restore_document_model_to_tab(TabData *tab) {
    if (!tab || !tab->current_file || !document_models) return;

    char *uri = g_filename_to_uri(tab->current_file, NULL, NULL);
    if (!uri) return;

    char *key = make_document_key(current_selected_session, uri, tab->is_helper);
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
        start_initial_scroll_restore(tab, 0, 96.0, 0.0);
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

    /* Sync color buttons to switched session's colors */
    session_model_t *session = get_current_session_model();
    if (session) {
        const char *pc = session_model_get_page_color(session);
        const char *hpc = session_model_get_helper_page_color(session);

        if (left_color_btn) {
            g_signal_handlers_block_by_func(left_color_btn, G_CALLBACK(on_left_color_set), NULL);
            if (pc) {
                GdkRGBA c;
                if (gdk_rgba_parse(&c, pc))
                    gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(left_color_btn), &c);
            }
            g_signal_handlers_unblock_by_func(left_color_btn, G_CALLBACK(on_left_color_set), NULL);
            apply_page_color_to_notebook(left_notebook, pc ? pc : "#FFFFFF");
        }
        if (right_color_btn) {
            g_signal_handlers_block_by_func(right_color_btn, G_CALLBACK(on_right_color_set), NULL);
            if (hpc) {
                GdkRGBA c;
                if (gdk_rgba_parse(&c, hpc))
                    gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(right_color_btn), &c);
            }
            g_signal_handlers_unblock_by_func(right_color_btn, G_CALLBACK(on_right_color_set), NULL);
            apply_page_color_to_notebook(right_notebook, hpc ? hpc : "#FFFFFF");
        }
    }
}

static void reset_sessions_tree_selection_guard(void) {
    g_clear_pointer(&last_tree_selection_key, g_free);
}

void populate_sessions_treeview(void) {
    if (!sessions_tree_store || !sessions_model) return;

    sessions_tree_syncing = TRUE;
    reset_sessions_tree_selection_guard();

    /* Save expanded session rows before clearing */
    GPtrArray *expanded = g_ptr_array_new_with_free_func(g_free);
    GtkTreeIter iter;
    if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(sessions_tree_store), &iter)) {
        do {
            char *name = NULL;
            gtk_tree_model_get(GTK_TREE_MODEL(sessions_tree_store), &iter,
                               SESSION_COL_SESSION_NAME, &name, -1);
            if (name) {
                GtkTreePath *path = gtk_tree_model_get_path(GTK_TREE_MODEL(sessions_tree_store), &iter);
                if (path) {
                    if (gtk_tree_view_row_expanded(GTK_TREE_VIEW(sessions_tree_view), path)) {
                        g_ptr_array_add(expanded, name);
                    } else {
                        g_free(name);
                    }
                    gtk_tree_path_free(path);
                } else {
                    g_free(name);
                }
            }
        } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(sessions_tree_store), &iter));
    }

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

    /* Restore expanded session rows */
    for (guint i = 0; i < expanded->len; i++) {
        const char *name = g_ptr_array_index(expanded, i);
        GtkTreeIter row;
        if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(sessions_tree_store), &row)) {
            do {
                char *row_name = NULL;
                gtk_tree_model_get(GTK_TREE_MODEL(sessions_tree_store), &row,
                                   SESSION_COL_SESSION_NAME, &row_name, -1);
                if (row_name && strcmp(row_name, name) == 0) {
                    GtkTreePath *path = gtk_tree_model_get_path(GTK_TREE_MODEL(sessions_tree_store), &row);
                    if (path) {
                        gtk_tree_view_expand_row(GTK_TREE_VIEW(sessions_tree_view), path, FALSE);
                        gtk_tree_path_free(path);
                    }
                    g_free(row_name);
                    break;
                }
                g_free(row_name);
            } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(sessions_tree_store), &row));
        }
    }
    g_ptr_array_unref(expanded);

    /* Auto-select current session + document when sidebar is visible */
    if (current_sidebar_mode == SIDEBAR_SESSIONS && current_selected_session) {
        reset_sessions_tree_selection_guard();
        GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(sessions_tree_view));
        GtkTreeIter iter;
        if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(sessions_tree_store), &iter)) {
            do {
                gchar *name = NULL;
                gtk_tree_model_get(GTK_TREE_MODEL(sessions_tree_store), &iter,
                                   SESSION_COL_SESSION_NAME, &name, -1);
                if (!name) continue;
                if (g_strcmp0(name, current_selected_session) != 0) {
                    g_free(name);
                    continue;
                }
                GtkTreePath *path = gtk_tree_model_get_path(GTK_TREE_MODEL(sessions_tree_store), &iter);
                if (path) {
                    gtk_tree_selection_select_path(sel, path);
                    gtk_tree_view_expand_row(GTK_TREE_VIEW(sessions_tree_view), path, FALSE);
                    gtk_tree_path_free(path);
                }
                TabData *tab = get_current_left_tab();
                if (tab && tab->current_file) {
                    char *uri = g_filename_to_uri(tab->current_file, NULL, NULL);
                    if (uri) {
                        GtkTreeIter child;
                        if (gtk_tree_model_iter_children(GTK_TREE_MODEL(sessions_tree_store), &child, &iter)) {
                            do {
                                gchar *doc_uri = NULL;
                                gtk_tree_model_get(GTK_TREE_MODEL(sessions_tree_store), &child,
                                                   SESSION_COL_DOC_URI, &doc_uri, -1);
                                if (doc_uri && g_strcmp0(doc_uri, uri) == 0) {
                                    GtkTreePath *cp = gtk_tree_model_get_path(GTK_TREE_MODEL(sessions_tree_store), &child);
                                    if (cp) {
                                        gtk_tree_selection_select_path(sel, cp);
                                        gtk_tree_path_free(cp);
                                    }
                                    g_free(doc_uri);
                                    break;
                                }
                                g_free(doc_uri);
                            } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(sessions_tree_store), &child));
                        }
                        g_free(uri);
                        }
                    }
                g_free(name);
                break;
            } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(sessions_tree_store), &iter));
        }
    }

    sessions_tree_syncing = FALSE;
}

/* Helper: Determine current page from scroll position */
static int compute_page_from_scroll(TabData *tab, double scroll_y) {
    if (!tab || !tab->cached_page_widths || tab->n_pages <= 0) return 0;

    const double spacing = 6.0;
    double scale = get_ppi_scale(tab);
    double y = spacing;
    int visible_page = tab->n_pages - 1;

    if (tab->layout_mode == 2) {
        for (int i = 0; i < tab->n_pages; ++i) {
            double page_w = tab->cached_page_widths[i] * scale;
            if (scroll_y >= y && scroll_y < y + page_w) {
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
        gtk_window_set_decorated(GTK_WINDOW(window), TRUE);
    } else {
        gtk_button_set_image(GTK_BUTTON(btn), create_toolbar_icon("title-bar-off"));
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
    (void)user_data;
    gboolean active = gtk_toggle_button_get_active(button);
    GtkWidget *btn = GTK_WIDGET(button);

    if (active) {
        gtk_button_set_image(GTK_BUTTON(btn), create_toolbar_icon("sidebar-helper-on"));
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
        gtk_button_set_image(GTK_BUTTON(btn), create_toolbar_icon("sidebar-helper-off"));
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

static guint maximize_pending_id = 0;

static gboolean defer_maximize_toggle(gpointer user_data) {
    GtkWindow *window = GTK_WINDOW(user_data);
    maximize_pending_id = 0;
    if (window) {
        if (gtk_window_is_maximized(window))
            gtk_window_unmaximize(window);
        else
            gtk_window_maximize(window);
        if (left_notebook) gtk_widget_queue_resize(left_notebook);
        if (right_notebook) gtk_widget_queue_resize(right_notebook);
    }
    g_object_unref(window);
    return FALSE;
}

static void on_maximize_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    GtkWindow *window = GTK_WINDOW(user_data);
    if (!window || maximize_pending_id) return;
    g_object_ref(window);
    maximize_pending_id = g_idle_add(defer_maximize_toggle, window);
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

        // Remove stale document models for this session
        if (session_models && document_models) {
            session_model_t *sm = g_hash_table_lookup(session_models, session_name);
            if (sm) {
                const GList *iter;
                for (iter = session_model_get_document_urls(sm); iter; iter = iter->next) {
                    char *key = make_document_key(session_name, (const char *)iter->data, FALSE);
                    g_hash_table_remove(document_models, key);
                    g_free(key);
                }
                for (iter = session_model_get_helper_document_urls(sm); iter; iter = iter->next) {
                    char *key = make_document_key(session_name, (const char *)iter->data, TRUE);
                    g_hash_table_remove(document_models, key);
                    g_free(key);
                }
            }
        }

        // Remove from session_models hash table so stale data doesn't persist
        if (session_models)
            g_hash_table_remove(session_models, session_name);

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

            // Migrate the session model in the hash table to the new key
            session_model_t *session_model = g_hash_table_lookup(session_models, old_name);
            if (session_model) {
                session_model_set_session_name(session_model, new_name);
                g_hash_table_steal(session_models, old_name);
                g_hash_table_insert(session_models, g_strdup(new_name), session_model);

                // Re-key all document_models entries that embed the old session name
                if (document_models) {
                    GList *keys = g_hash_table_get_keys(document_models);
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
                        document_model_t *doc = g_hash_table_lookup(document_models, old_key);
                        if (doc) {
                            char *new_key = g_strdup_printf("%s%s", new_name, old_key + old_len);
                            g_hash_table_steal(document_models, old_key);
                            g_hash_table_insert(document_models, new_key, doc);
                            g_free(old_key);
                        }
                    }
                    g_list_free(to_rekey);
                }
            }

            // Update in sessions_model
            sessions_model_remove_session_name(sessions_model, old_name);
            sessions_model_add_session_name(sessions_model, new_name);

            // Update current_selected_session BEFORE tree repopulation so the
            // cursor-changed handler sees it matches and avoids switch_to_session.
            gboolean was_current = (current_selected_session &&
                                    strcmp(current_selected_session, old_name) == 0);
            if (was_current) {
                g_free(current_selected_session);
                current_selected_session = g_strdup(new_name);
            }

            // Update tree view
            populate_sessions_treeview();

            // Re-select the renamed session in the tree
            sessions_tree_syncing = TRUE;
            {
                GtkTreeIter si;
                if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(sessions_tree_store), &si)) {
                    do {
                        char *sn = NULL;
                        gtk_tree_model_get(GTK_TREE_MODEL(sessions_tree_store), &si,
                                           SESSION_COL_SESSION_NAME, &sn, -1);
                        if (sn && strcmp(sn, new_name) == 0) {
                            GtkTreeSelection *sel = gtk_tree_view_get_selection(
                                GTK_TREE_VIEW(sessions_tree_view));
                            GtkTreePath *path = gtk_tree_model_get_path(
                                GTK_TREE_MODEL(sessions_tree_store), &si);
                            gtk_tree_selection_select_path(sel, path);
                            gtk_tree_path_free(path);
                            g_free(sn);
                            break;
                        }
                        g_free(sn);
                    } while (gtk_tree_model_iter_next(
                        GTK_TREE_MODEL(sessions_tree_store), &si));
                }
            }
            sessions_tree_syncing = FALSE;

            // Update window title for the renamed session
            if (was_current) {
                if (sessions_model) {
                    sessions_model_set_last_open_session(sessions_model, current_selected_session);
                }
                update_window_title_for_session(current_selected_session);
            }

            // Clear entry
            gtk_entry_set_text(GTK_ENTRY(sessions_entry), "");

            // Persist the rename
            save_state();

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
    gtk_widget_hide(right_page_nav_overlay);

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

static void on_sessions_toggled(GtkToggleButton *btn, gpointer user_data) {
    (void)user_data;

    if (!gtk_toggle_button_get_active(btn)) {
        /* Toggled off: close sidebar if we are the active mode */
        if (current_sidebar_mode == SIDEBAR_SESSIONS) {
            gtk_container_remove(GTK_CONTAINER(main_hbox), sidebar);
            gtk_box_reorder_child(GTK_BOX(main_hbox), content_vbox, 1);
            current_sidebar_mode = SIDEBAR_NONE;
        }
        return;
    }

    /* Toggled on: open sessions sidebar, deactivate other buttons */
    if (gtk_widget_get_parent(sidebar) != NULL) {
        gtk_container_remove(GTK_CONTAINER(main_hbox), sidebar);
    }

    /* Deactivate other toggle buttons */
    g_signal_handlers_block_by_func(toc_btn, G_CALLBACK(on_toc_toggled), NULL);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toc_btn), FALSE);
    g_signal_handlers_unblock_by_func(toc_btn, G_CALLBACK(on_toc_toggled), NULL);

    g_signal_handlers_block_by_func(settings_btn, G_CALLBACK(on_settings_toggled), NULL);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(settings_btn), FALSE);
    g_signal_handlers_unblock_by_func(settings_btn, G_CALLBACK(on_settings_toggled), NULL);

    g_signal_handlers_block_by_func(file_info_btn, G_CALLBACK(on_left_file_info_toggled), NULL);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(file_info_btn), FALSE);
    g_signal_handlers_unblock_by_func(file_info_btn, G_CALLBACK(on_left_file_info_toggled), NULL);

    /* Hide other sidebar contents */
    gtk_widget_hide(sidebar_label);
    gtk_widget_hide(sessions_container);
    gtk_widget_hide(settings_container);
    gtk_widget_hide(toc_container);
    gtk_widget_hide(file_info_container);
    gtk_tree_store_clear(toc_tree_store);

    /* Show sessions container */
    gtk_widget_show_all(sessions_container);

    gtk_box_pack_start(GTK_BOX(main_hbox), sidebar, FALSE, FALSE, 0);
    gtk_box_reorder_child(GTK_BOX(main_hbox), content_vbox, 2);
    gtk_widget_set_size_request(sidebar, 300, -1);
    gtk_widget_show(sidebar);
    current_sidebar_mode = SIDEBAR_SESSIONS;

    /* Auto-select current session and document in the tree */
    if (sessions_tree_store && current_selected_session) {
        reset_sessions_tree_selection_guard();
        sessions_tree_syncing = TRUE;
        GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(sessions_tree_view));
        GtkTreeIter iter;
        if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(sessions_tree_store), &iter)) {
            do {
                gchar *name = NULL;
                gtk_tree_model_get(GTK_TREE_MODEL(sessions_tree_store), &iter,
                                   SESSION_COL_SESSION_NAME, &name, -1);
                if (!name) continue;
                if (g_strcmp0(name, current_selected_session) != 0) {
                    g_free(name);
                    continue;
                }
                /* Found matching session row — select and expand it */
                GtkTreePath *path = gtk_tree_model_get_path(GTK_TREE_MODEL(sessions_tree_store), &iter);
                if (path) {
                    gtk_tree_selection_select_path(sel, path);
                    gtk_tree_view_expand_row(GTK_TREE_VIEW(sessions_tree_view), path, FALSE);
                    gtk_tree_path_free(path);
                }
                update_sessions_tree_document_selection();
                g_free(name);
                break;
            } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(sessions_tree_store), &iter));
        }
        sessions_tree_syncing = FALSE;
    }
}

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
        gtk_tree_store_append(toc_tree_store, &child, parent);
        gtk_tree_store_set(toc_tree_store, &child,
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

static void populate_toc_treeview_for_tab(TabData *tab) {
    last_toc_selected_page = -1;
    gtk_tree_store_clear(toc_tree_store);
    if (!tab) return;
    if (!tab->doc) ensure_tab_doc_loaded(tab);
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

static void update_toc_selection_for_current_page(TabData *tab) {
    if (!toc_tree_store || !toc_tree_view || !tab) return;

    int target_page = tab->cur_page + 1;
    if (target_page < 1) target_page = 1;

    if (target_page == last_toc_selected_page) return;

    TocFindData find = { .target_page = target_page, .best_page = 0, .best_path = NULL };
    gtk_tree_model_foreach(GTK_TREE_MODEL(toc_tree_store), toc_find_nearest_cb, &find);

    if (find.best_path) {
        toc_tree_syncing = TRUE;
        GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(toc_tree_view));
        gtk_tree_selection_select_path(sel, find.best_path);
        gtk_tree_view_expand_to_path(GTK_TREE_VIEW(toc_tree_view), find.best_path);
        gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(toc_tree_view), find.best_path, NULL, FALSE, 0, 0);
        last_toc_selected_page = target_page;
        gtk_tree_path_free(find.best_path);
        toc_tree_syncing = FALSE;
    }
}

static void update_sessions_tree_document_selection_for_tab(TabData *tab) {
    if (!sessions_tree_store || !sessions_tree_view || !current_selected_session) return;

    if (!tab || !tab->current_file) return;

    char *uri = g_filename_to_uri(tab->current_file, NULL, NULL);
    if (!uri) return;

    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(sessions_tree_view));
    GtkTreeIter iter;
    if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(sessions_tree_store), &iter)) {
        do {
            gchar *name = NULL;
            gtk_tree_model_get(GTK_TREE_MODEL(sessions_tree_store), &iter,
                               SESSION_COL_SESSION_NAME, &name, -1);
            if (!name) continue;
            if (g_strcmp0(name, current_selected_session) != 0) {
                g_free(name);
                continue;
            }
            g_free(name);
            /* Found the session row. Select the matching document child. */
            GtkTreeIter child;
            if (gtk_tree_model_iter_children(GTK_TREE_MODEL(sessions_tree_store), &child, &iter)) {
                do {
                    gchar *doc_uri = NULL;
                    gtk_tree_model_get(GTK_TREE_MODEL(sessions_tree_store), &child,
                                       SESSION_COL_DOC_URI, &doc_uri, -1);
                    if (doc_uri && g_strcmp0(doc_uri, uri) == 0) {
                        GtkTreePath *cp = gtk_tree_model_get_path(GTK_TREE_MODEL(sessions_tree_store), &child);
                        if (cp) {
                            gtk_tree_selection_select_path(sel, cp);
                            gtk_tree_path_free(cp);
                        }
                        g_free(doc_uri);
                        g_free(uri);
                        return;
                    }
                    g_free(doc_uri);
                } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(sessions_tree_store), &child));
            }
            break;
        } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(sessions_tree_store), &iter));
    }
    g_free(uri);
}

static void update_sessions_tree_document_selection(void) {
    update_sessions_tree_document_selection_for_tab(get_current_left_tab());
}

static void on_toc_row_activated(GtkTreeView *tree_view, GtkTreePath *path, GtkTreeViewColumn *column, gpointer user_data) {
    (void)column;
    (void)user_data;

    if (toc_tree_syncing) return;
    if (!gtk_widget_get_visible(toc_container)) return;

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
            last_toc_selected_page = page;
            tab->cur_page = page - 1;
            scroll_to_page(tab, page - 1, -1);
            update_document_model_from_tab(tab);
        }
    }
    g_free(named_dest);
}

static void on_toc_toggled(GtkToggleButton *btn, gpointer user_data) {
    (void)user_data;

    if (!gtk_toggle_button_get_active(btn)) {
        if (current_sidebar_mode == SIDEBAR_TOC) {
            gtk_container_remove(GTK_CONTAINER(main_hbox), sidebar);
            gtk_box_reorder_child(GTK_BOX(main_hbox), content_vbox, 1);
            current_sidebar_mode = SIDEBAR_NONE;
        }
        return;
    }

    if (gtk_widget_get_parent(sidebar) != NULL) {
        gtk_container_remove(GTK_CONTAINER(main_hbox), sidebar);
    }

    /* Deactivate other toggle buttons */
    g_signal_handlers_block_by_func(sessions_btn, G_CALLBACK(on_sessions_toggled), NULL);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(sessions_btn), FALSE);
    g_signal_handlers_unblock_by_func(sessions_btn, G_CALLBACK(on_sessions_toggled), NULL);

    g_signal_handlers_block_by_func(settings_btn, G_CALLBACK(on_settings_toggled), NULL);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(settings_btn), FALSE);
    g_signal_handlers_unblock_by_func(settings_btn, G_CALLBACK(on_settings_toggled), NULL);

    g_signal_handlers_block_by_func(file_info_btn, G_CALLBACK(on_left_file_info_toggled), NULL);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(file_info_btn), FALSE);
    g_signal_handlers_unblock_by_func(file_info_btn, G_CALLBACK(on_left_file_info_toggled), NULL);

    /* Hide other sidebar contents */
    gtk_widget_hide(sidebar_label);
    gtk_widget_hide(sessions_container);
    gtk_widget_hide(settings_container);
    gtk_widget_hide(file_info_container);

    populate_toc_treeview();

    gtk_widget_show_all(toc_container);
    update_toc_selection_for_current_page(get_current_left_tab());

    gtk_box_pack_start(GTK_BOX(main_hbox), sidebar, FALSE, FALSE, 0);
    gtk_box_reorder_child(GTK_BOX(main_hbox), content_vbox, 2);
    gtk_widget_set_size_request(sidebar, 300, -1);
    gtk_widget_show(sidebar);
    current_sidebar_mode = SIDEBAR_TOC;
}

static double get_angle_for_position(const char *pos) {
    if (g_strcmp0(pos, "left") == 0) return 90.0;
    if (g_strcmp0(pos, "right") == 0) return -90.0;
    return 0.0;
}

static void apply_tabbar_position(const char *pos) {
    GtkPositionType gpos = GTK_POS_TOP;
    if (g_strcmp0(pos, "left") == 0) gpos = GTK_POS_LEFT;
    else if (g_strcmp0(pos, "right") == 0) gpos = GTK_POS_RIGHT;
    if (left_notebook)
        gtk_notebook_set_tab_pos(GTK_NOTEBOOK(left_notebook), gpos);
    if (right_notebook)
        gtk_notebook_set_tab_pos(GTK_NOTEBOOK(right_notebook), gpos);

    gboolean is_side = (g_strcmp0(pos, "left") == 0 || g_strcmp0(pos, "right") == 0);
    if (left_notebook) {
        gtk_notebook_set_show_border(GTK_NOTEBOOK(left_notebook), !is_side);
        gtk_widget_set_size_request(left_notebook, is_side ? -1 : 70, is_side ? 200 : -1);
    }
    if (right_notebook) {
        gtk_notebook_set_show_border(GTK_NOTEBOOK(right_notebook), !is_side);
        gtk_widget_set_size_request(right_notebook, -1, is_side ? 200 : -1);
    }

    /* Adjust page nav overlay so it doesn't sit over a left-side tab bar */
    if (page_nav_overlay) {
        if (g_strcmp0(pos, "left") == 0) {
            gtk_widget_set_margin_start(page_nav_overlay, 60);
        } else {
            gtk_widget_set_margin_start(page_nav_overlay, 16);
        }
    }
    /* Adjust right page nav overlay for right-side tab bar */
    if (right_page_nav_overlay) {
        if (g_strcmp0(pos, "right") == 0) {
            gtk_widget_set_margin_end(right_page_nav_overlay, 60);
        } else {
            gtk_widget_set_margin_end(right_page_nav_overlay, 8);
        }
    }

    double angle = get_angle_for_position(pos);
    GtkOrientation box_orientation = is_side ? GTK_ORIENTATION_VERTICAL : GTK_ORIENTATION_HORIZONTAL;
    GtkWidget *notebooks[] = {left_notebook, right_notebook};
    for (int n = 0; n < 2; n++) {
        if (!notebooks[n]) continue;
        int np = gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebooks[n]));
        for (int i = 0; i < np; i++) {
            GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(notebooks[n]), i);
            TabData *tab = g_object_get_data(G_OBJECT(page), "tab-data");
            if (tab && tab->tab_label)
                gtk_label_set_angle(GTK_LABEL(tab->tab_label), angle);
            if (tab && tab->tab_label_box) {
                gtk_orientable_set_orientation(GTK_ORIENTABLE(tab->tab_label_box), box_orientation);
                if (g_strcmp0(pos, "left") == 0)
                    gtk_box_reorder_child(GTK_BOX(tab->tab_label_box), tab->tab_label, 1);
                else
                    gtk_box_reorder_child(GTK_BOX(tab->tab_label_box), tab->tab_label, 0);
            }
            if (tab && tab->tab_label_close_btn) {
                gtk_widget_set_has_tooltip(tab->tab_label_close_btn, TRUE);
                gtk_widget_set_tooltip_text(tab->tab_label_close_btn, "Close tab");
            }
        }
    }
}

static void apply_tab_width(int width) {
    /* Truncate all existing tab labels to 'width' characters */
    GtkWidget *notebooks[] = {left_notebook, right_notebook};
    for (int n = 0; n < 2; n++) {
        if (!notebooks[n]) continue;
        int np = gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebooks[n]));
        for (int i = 0; i < np; i++) {
            GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(notebooks[n]), i);
            TabData *tab = g_object_get_data(G_OBJECT(page), "tab-data");
            if (!tab || !tab->tab_label || !tab->current_file) continue;
            char *basename = g_path_get_basename(tab->current_file);
            const char *label_text = basename;
            char *truncated = NULL;
            if (width > 0 && (int)strlen(basename) > width) {
                truncated = g_strndup(basename, width);
                label_text = truncated;
            }
            gtk_label_set_text(GTK_LABEL(tab->tab_label), label_text);
            g_free(truncated);
            g_free(basename);
        }
    }
}

static void on_tabbar_combo_changed(GtkComboBox *combo, gpointer user_data) {
    (void)user_data;
    if (!sessions_model) return;
    const char *pos = gtk_combo_box_get_active_id(GTK_COMBO_BOX(combo));
    if (!pos) return;
    sessions_model_set_tabbar_position(sessions_model, pos);
    apply_tabbar_position(pos);
    save_state();
}

static void on_tab_width_spin_changed(GtkSpinButton *spin, gpointer user_data) {
    (void)user_data;
    if (!sessions_model) return;
    int w = (int)gtk_spin_button_get_value(spin);
    sessions_model_set_tab_width(sessions_model, w);
    apply_tab_width(w);
    save_state();
}

static void on_keep_dark_toggled(GtkToggleButton *btn, gpointer user_data) {
    (void)user_data;
    keep_dark_theme = gtk_toggle_button_get_active(btn);
    if (keep_dark_theme) {
        is_dark_theme = TRUE;
        apply_dark_css(TRUE);
    } else {
        apply_dark_css(FALSE);
        is_dark_theme = detect_system_dark_theme();
    }
    recolor_all_toolbars();
    if (sessions_model)
        sessions_model_set_keep_dark(sessions_model, keep_dark_theme);
    save_state();
}

static session_model_t *get_current_session_model(void) {
    if (!current_selected_session || !session_models) return NULL;
    return g_hash_table_lookup(session_models, current_selected_session);
}

static void apply_page_color_to_notebook(GtkWidget *notebook, const char *color_str) {
    if (!notebook) return;
    GdkRGBA color;
    if (!gdk_rgba_parse(&color, color_str)) return;
    int np = gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook));
    for (int i = 0; i < np; i++) {
        GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(notebook), i);
        TabData *tab = g_object_get_data(G_OBJECT(page), "tab-data");
        if (tab) {
            tab->page_color = color;
            queue_draw(tab);
        }
    }
}

static void on_left_color_set(GtkColorButton *btn, gpointer user_data) {
    (void)user_data;
    session_model_t *session = get_current_session_model();
    if (!session) return;
    GdkRGBA color;
    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(btn), &color);
    char *str = gdk_rgba_to_string(&color);
    session_model_set_page_color(session, str);
    apply_page_color_to_notebook(left_notebook, str);
    g_free(str);
    save_state();
}

static void on_right_color_set(GtkColorButton *btn, gpointer user_data) {
    (void)user_data;
    session_model_t *session = get_current_session_model();
    if (!session) return;
    GdkRGBA color;
    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(btn), &color);
    char *str = gdk_rgba_to_string(&color);
    session_model_set_helper_page_color(session, str);
    apply_page_color_to_notebook(right_notebook, str);
    g_free(str);
    save_state();
}

static void on_settings_toggled(GtkToggleButton *btn, gpointer user_data) {
    (void)user_data;

    if (!gtk_toggle_button_get_active(btn)) {
        if (current_sidebar_mode == SIDEBAR_SETTINGS) {
            gtk_container_remove(GTK_CONTAINER(main_hbox), sidebar);
            gtk_box_reorder_child(GTK_BOX(main_hbox), content_vbox, 1);
            current_sidebar_mode = SIDEBAR_NONE;
        }
        return;
    }

    if (gtk_widget_get_parent(sidebar) != NULL) {
        gtk_container_remove(GTK_CONTAINER(main_hbox), sidebar);
    }

    /* Deactivate other toggle buttons */
    g_signal_handlers_block_by_func(sessions_btn, G_CALLBACK(on_sessions_toggled), NULL);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(sessions_btn), FALSE);
    g_signal_handlers_unblock_by_func(sessions_btn, G_CALLBACK(on_sessions_toggled), NULL);

    g_signal_handlers_block_by_func(toc_btn, G_CALLBACK(on_toc_toggled), NULL);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toc_btn), FALSE);
    g_signal_handlers_unblock_by_func(toc_btn, G_CALLBACK(on_toc_toggled), NULL);

    g_signal_handlers_block_by_func(file_info_btn, G_CALLBACK(on_left_file_info_toggled), NULL);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(file_info_btn), FALSE);
    g_signal_handlers_unblock_by_func(file_info_btn, G_CALLBACK(on_left_file_info_toggled), NULL);

    /* Hide other sidebar contents */
    gtk_widget_hide(sidebar_label);
    gtk_widget_hide(sessions_container);
    gtk_widget_hide(toc_container);
    gtk_widget_hide(file_info_container);
    gtk_tree_store_clear(toc_tree_store);

    /* Show settings container */
    gtk_widget_show_all(settings_container);

    gtk_box_pack_start(GTK_BOX(main_hbox), sidebar, FALSE, FALSE, 0);
    gtk_box_reorder_child(GTK_BOX(main_hbox), content_vbox, 2);
    gtk_widget_set_size_request(sidebar, 300, -1);
    gtk_widget_show(sidebar);
    current_sidebar_mode = SIDEBAR_SETTINGS;
}

static SearchPageResult* find_search_result_for_page(int page_1based) {
    for (int i = 0; i < search_page_results_n; i++) {
        if (search_page_results[i].page == page_1based)
            return &search_page_results[i];
    }
    return NULL;
}

static void clear_file_info_search_results(void) {
    search_page_results_n = 0;
    if (file_info_search_results_store) {
        gtk_list_store_clear(file_info_search_results_store);
    }
    if (file_info_search_no_results) {
        gtk_widget_hide(file_info_search_no_results);
    }
    if (file_info_search_overflow_label) {
        gtk_widget_hide(file_info_search_overflow_label);
    }
    TabData *tab = get_current_left_tab();
    if (tab) queue_draw(tab);
}

static void perform_file_info_search(const char *text) {
    clear_file_info_search_results();
    if (!text || !*text) {
        TabData *tab = get_current_left_tab();
        if (tab) queue_draw(tab);
        return;
    }

    TabData *tab = get_current_left_tab();
    if (!tab || !ensure_tab_doc_loaded(tab)) return;
    if (!tab->doc) return;

    int n_pages = pdfr_count_pages(tab->doc);
    if (n_pages <= 0) return;

    for (int i = 0; i < n_pages && search_page_results_n < SEARCH_MAX_PAGES; i++) {
        PdfrPage *page = pdfr_load_page(tab->doc, i);
        if (!page) continue;

        PdfrRect matches[SEARCH_MAX_PER_PAGE];
        int n_matches = pdfr_search_page(tab->doc, page, text, matches, SEARCH_MAX_PER_PAGE);

        if (n_matches > 0) {
            SearchPageResult *r = &search_page_results[search_page_results_n];
            r->page = i + 1;
            r->n_matches = n_matches;
            memcpy(r->rects, matches, sizeof(PdfrRect) * n_matches);
            search_page_results_n++;

            GtkTreeIter tree_iter;
            const char *plural = n_matches == 1 ? "" : "es";
            gchar *label = g_strdup_printf("Page %d (%d match%s)", i + 1, n_matches, plural);
            gtk_list_store_append(file_info_search_results_store, &tree_iter);
            gtk_list_store_set(file_info_search_results_store, &tree_iter,
                               SEARCH_COL_PAGE, i + 1,
                               SEARCH_COL_COUNT, n_matches,
                               SEARCH_COL_LABEL, label, -1);
            g_free(label);
        }

        pdfr_free_page(tab->doc, page);
    }

    if (search_page_results_n == 0) {
        gtk_widget_show(file_info_search_no_results);
    }

    if (search_page_results_n >= SEARCH_MAX_PAGES) {
        gchar *overflow = g_strdup_printf("Results limited to %d pages", SEARCH_MAX_PAGES);
        gtk_label_set_text(GTK_LABEL(file_info_search_overflow_label), overflow);
        g_free(overflow);
        gtk_widget_show(file_info_search_overflow_label);
    } else {
        gtk_widget_hide(file_info_search_overflow_label);
    }

    queue_draw(tab);
}

static void on_file_info_search_activated(GtkEntry *entry, gpointer user_data) {
    (void)user_data;
    const char *text = gtk_entry_get_text(entry);
    perform_file_info_search(text);
}

static void on_file_info_search_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    (void)user_data;
    const char *text = gtk_entry_get_text(GTK_ENTRY(file_info_search_entry));
    perform_file_info_search(text);
}

static void on_file_info_search_row_activated(GtkTreeView *tree_view, GtkTreePath *path, GtkTreeViewColumn *column, gpointer user_data) {
    (void)tree_view;
    (void)column;
    (void)user_data;

    GtkTreeModel *model = GTK_TREE_MODEL(file_info_search_results_store);
    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter(model, &iter, path)) return;

    int page = 0;
    gtk_tree_model_get(model, &iter, SEARCH_COL_PAGE, &page, -1);

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

static GtkWidget *right_file_info_popover = NULL;
static GtkWidget *right_popover_name_label;
static GtkWidget *right_popover_path_label;
static GtkWidget *right_popover_size_label;
static GtkWidget *right_popover_pages_label;

static void on_left_file_info_toggled(GtkToggleButton *btn, gpointer user_data) {
    (void)user_data;

    if (!gtk_toggle_button_get_active(btn)) {
        if (current_sidebar_mode == SIDEBAR_FILE_INFO) {
            gtk_container_remove(GTK_CONTAINER(main_hbox), sidebar);
            gtk_box_reorder_child(GTK_BOX(main_hbox), content_vbox, 1);
            current_sidebar_mode = SIDEBAR_NONE;
        }
        return;
    }

    if (gtk_widget_get_parent(sidebar) != NULL) {
        gtk_container_remove(GTK_CONTAINER(main_hbox), sidebar);
    }

    /* Deactivate other toggle buttons */
    g_signal_handlers_block_by_func(sessions_btn, G_CALLBACK(on_sessions_toggled), NULL);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(sessions_btn), FALSE);
    g_signal_handlers_unblock_by_func(sessions_btn, G_CALLBACK(on_sessions_toggled), NULL);

    g_signal_handlers_block_by_func(toc_btn, G_CALLBACK(on_toc_toggled), NULL);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toc_btn), FALSE);
    g_signal_handlers_unblock_by_func(toc_btn, G_CALLBACK(on_toc_toggled), NULL);

    g_signal_handlers_block_by_func(settings_btn, G_CALLBACK(on_settings_toggled), NULL);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(settings_btn), FALSE);
    g_signal_handlers_unblock_by_func(settings_btn, G_CALLBACK(on_settings_toggled), NULL);

    gtk_widget_hide(sidebar_label);
    gtk_widget_hide(sessions_container);
    gtk_widget_hide(toc_container);
    gtk_widget_hide(settings_container);
    gtk_tree_store_clear(toc_tree_store);

    update_file_info_labels(get_current_left_tab());
    gtk_widget_show_all(file_info_container);
    gtk_widget_hide(file_info_search_no_results);
    gtk_widget_hide(file_info_search_overflow_label);

    gtk_box_pack_start(GTK_BOX(main_hbox), sidebar, FALSE, FALSE, 0);
    gtk_box_reorder_child(GTK_BOX(main_hbox), content_vbox, 2);
    gtk_widget_set_size_request(sidebar, 300, -1);
    gtk_widget_show(sidebar);
    current_sidebar_mode = SIDEBAR_FILE_INFO;
}

static void on_right_file_info_popover_closed(GtkPopover *popover, gpointer user_data);

static void on_right_file_info_clicked(GtkButton *button, gpointer user_data) {
    (void)user_data;
    GtkWidget *btn = GTK_WIDGET(button);

    if (!right_file_info_popover) {
        right_file_info_popover = gtk_popover_new(btn);
        gtk_popover_set_position(GTK_POPOVER(right_file_info_popover), GTK_POS_LEFT);
        gtk_popover_set_modal(GTK_POPOVER(right_file_info_popover), FALSE);

        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_container_set_border_width(GTK_CONTAINER(box), 8);

        right_popover_name_label = gtk_label_new("Name: (no file)");
        gtk_widget_set_halign(right_popover_name_label, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(box), right_popover_name_label, FALSE, FALSE, 0);

        right_popover_path_label = gtk_label_new("Path: (none)");
        gtk_widget_set_halign(right_popover_path_label, GTK_ALIGN_FILL);
        gtk_label_set_line_wrap(GTK_LABEL(right_popover_path_label), TRUE);
        gtk_label_set_line_wrap_mode(GTK_LABEL(right_popover_path_label), PANGO_WRAP_WORD_CHAR);
        gtk_label_set_max_width_chars(GTK_LABEL(right_popover_path_label), 60);
        gtk_box_pack_start(GTK_BOX(box), right_popover_path_label, FALSE, FALSE, 0);

        right_popover_size_label = gtk_label_new("Size: (none)");
        gtk_widget_set_halign(right_popover_size_label, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(box), right_popover_size_label, FALSE, FALSE, 0);

        right_popover_pages_label = gtk_label_new("Pages: (none)");
        gtk_widget_set_halign(right_popover_pages_label, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(box), right_popover_pages_label, FALSE, FALSE, 0);

        gtk_container_add(GTK_CONTAINER(right_file_info_popover), box);
        gtk_widget_show_all(box);
        g_signal_connect(right_file_info_popover, "closed",
                         G_CALLBACK(on_right_file_info_popover_closed), btn);
    }

    if (gtk_widget_get_mapped(right_file_info_popover)) {
        gtk_popover_popdown(GTK_POPOVER(right_file_info_popover));
    } else {
        TabData *tab = get_current_right_tab();

        if (tab && tab->current_file) {
            gchar *basename = g_path_get_basename(tab->current_file);
            gchar *text = g_strdup_printf("Name: %s", basename);
            gtk_label_set_text(GTK_LABEL(right_popover_name_label), text);
            g_free(text);
            g_free(basename);

            text = g_strdup_printf("Path: %s", tab->current_file);
            gtk_label_set_text(GTK_LABEL(right_popover_path_label), text);
            g_free(text);

            GFile *gf = g_file_new_for_path(tab->current_file);
            GFileInfo *info = g_file_query_info(gf, G_FILE_ATTRIBUTE_STANDARD_SIZE,
                                                 G_FILE_QUERY_INFO_NONE, NULL, NULL);
            if (info) {
                gchar *size_str = format_file_size(g_file_info_get_size(info));
                gchar *size_text = g_strdup_printf("Size: %s", size_str);
                gtk_label_set_text(GTK_LABEL(right_popover_size_label), size_text);
                g_free(size_text);
                g_free(size_str);
                g_object_unref(info);
            } else {
                gtk_label_set_text(GTK_LABEL(right_popover_size_label), "Size: Unknown");
            }
            g_object_unref(gf);

            if (tab->doc) {
                gchar *pages_text = g_strdup_printf("Pages: %d", pdfr_count_pages(tab->doc));
                gtk_label_set_text(GTK_LABEL(right_popover_pages_label), pages_text);
                g_free(pages_text);
            } else {
                gtk_label_set_text(GTK_LABEL(right_popover_pages_label), "Pages: N/A");
            }
        } else {
            gtk_label_set_text(GTK_LABEL(right_popover_name_label), "Name: (no file)");
            gtk_label_set_text(GTK_LABEL(right_popover_path_label), "Path: (none)");
            gtk_label_set_text(GTK_LABEL(right_popover_size_label), "Size: (none)");
            gtk_label_set_text(GTK_LABEL(right_popover_pages_label), "Pages: (none)");
        }

        gtk_popover_popup(GTK_POPOVER(right_file_info_popover));
    }
}

static void on_right_file_info_popover_closed(GtkPopover *popover, gpointer user_data) {
    (void)popover;
    GtkWidget *btn = GTK_WIDGET(user_data);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(btn), FALSE);
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

static void scroll_to_page(TabData *tab, int page, double target_y) {
    if (!tab || !tab->scrolled || !tab->pages_drawing || !tab->cached_page_widths) return;
    if (page < 0 || page >= tab->n_pages) return;

    const double spacing = 6.0;
    double scale = get_ppi_scale(tab);

    if (tab->layout_mode == 2) {
        double x = spacing;
        for (int i = 0; i < page; ++i) {
            x += tab->cached_page_widths[i] * scale + spacing;
        }
        if (tab->h_scrollbar) {
            GtkAdjustment *sadj = gtk_range_get_adjustment(GTK_RANGE(tab->h_scrollbar));
            gtk_adjustment_set_value(sadj, x);
        }
        return;
    }

    double y = spacing;
    if (tab->layout_mode == 0) {
        for (int i = 0; i < page; ++i) {
            y += tab->cached_page_heights[i] * scale + spacing;
        }
        if (target_y >= 0) {
            y += (tab->cached_page_heights[page] - target_y) * scale;
        }
    } else if (tab->layout_mode == 1) {
        int row = page / 2;
        for (int i = 0; i < row; ++i) {
            double row_h = 0.0;
            double h1 = tab->cached_page_heights[i * 2] * scale;
            if (h1 > row_h) row_h = h1;
            if (i * 2 + 1 < tab->n_pages) {
                double h2 = tab->cached_page_heights[i * 2 + 1] * scale;
                if (h2 > row_h) row_h = h2;
            }
            y += row_h + spacing;
        }
        if (target_y >= 0) {
            y += (tab->cached_page_heights[page] - target_y) * scale;
        }
    }

    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(tab->scrolled));
    gtk_adjustment_set_value(vadj, y);
}

static gboolean deferred_update_document_model(gpointer data) {
    TabData *tab = data;
    tab->scroll_doc_debounce_id = 0;
    if (!tab || !tab->cached_page_widths) return G_SOURCE_REMOVE;
    update_document_model_from_tab(tab);
    if (current_sidebar_mode == SIDEBAR_TOC) {
        update_toc_selection_for_current_page(tab);
    }
    return G_SOURCE_REMOVE;
}

static void cancel_doc_model_debounce(TabData *tab) {
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
        if (!ensure_tab_doc_loaded(tab)) return;
    }

    double zoom = tab->zoom > 0 ? tab->zoom : 96.0;
    if (zoom != tab->last_zoom) {
        tab->last_zoom = zoom;
        build_continuous_view(tab);
    }

    /* For row view (mode 2), the size_request width is clamped to page_width_px
       to prevent the window from growing.  GTK's internal handler set the
       hadjustment upper from the clamped size_request, so we must extend it
       here to the full total_w BEFORE scroll_to_page runs below. */
    if (tab->layout_mode == 2 && tab->cached_page_widths && tab->n_pages > 0 && tab->h_scrollbar) {
        int vp_w = allocation ? allocation->width : 200;
        int vp_h = allocation ? allocation->height : 200;
        GtkAdjustment *sadj = gtk_range_get_adjustment(GTK_RANGE(tab->h_scrollbar));
        gtk_adjustment_set_page_size(sadj, vp_w > 0 ? vp_w : 200);
        gtk_adjustment_set_step_increment(sadj, vp_w > 0 ? vp_w * 0.1 : 20);
        gtk_adjustment_set_page_increment(sadj, vp_w * 0.9);
        double upper = gtk_adjustment_get_upper(sadj);
        if (upper < 1.0) upper = 1.0;
        gtk_adjustment_set_upper(sadj, upper);
        double target_h = MAX(vp_h, tab->max_page_h);
        gtk_widget_set_size_request(tab->pages_drawing, -1, (int)ceil(target_h));
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

/* Ensure link mappings are loaded for a specific page (lazy, per-page).
   Only allocates the array if needed; only fetches the requested page if not yet loaded. */
static void ensure_page_links_loaded(TabData *tab, int page) {
    if (!tab || !tab->doc) return;
    if (!tab->page_links) {
        tab->page_links_n = tab->n_pages;
        tab->page_links = g_new0(PdfrLink*, tab->n_pages);
    }
    if (page >= 0 && page < tab->page_links_n && !tab->page_links[page]) {
        PdfrPage *ppage = pdfr_load_page(tab->doc, page);
        if (ppage) {
            tab->page_links[page] = pdfr_load_links(tab->doc, ppage);
            pdfr_free_page(tab->doc, ppage);
        }
    }
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

/* Check if there is a clickable link at the given page-relative coordinates.
   px, py are in rendering space (y-down, 0 at top of page).
   Link rects from MuPDF are already in page/device space (y-down, 0=top),
   so we compare directly. */
static gboolean has_link_at(TabData *tab, int page, double px, double py) {
    if (!tab || page < 0 || page >= tab->page_links_n || !tab->page_links) return FALSE;
    PdfrLink *link = tab->page_links[page];
    while (link) {
        PdfrRect *a = &link->rect;
        if (px >= a->x1 && px <= a->x2 && py >= a->y1 && py <= a->y2) {
            return TRUE;
        }
        link = link->next;
    }
    return FALSE;
}

/* Debug helper — print link rects on a page for diagnostic purposes. */
/* Activate the link at the given page-relative coordinates (if any).
   px, py are in rendering space (y-down, 0 at top of page). */
static gboolean activate_link_at(TabData *tab, int page, double px, double py) {
    if (!tab || page < 0 || page >= tab->page_links_n || !tab->page_links) return FALSE;
    PdfrLink *link = tab->page_links[page];
    while (link) {
        PdfrRect *a = &link->rect;
        /* Link rects from MuPDF are in page/device space (y-down, 0=top),
           matching rendering space coordinates. */
        if (px >= a->x1 && px <= a->x2 && py >= a->y1 && py <= a->y2) {
            switch (link->type) {
                case PDF_LINK_URI:
                    if (link->uri) {
                        GError *err = NULL;
                        gtk_show_uri_on_window(GTK_WINDOW(window),
                            link->uri, GDK_CURRENT_TIME, &err);
                        if (err) {
                            g_warning("Failed to open URI: %s", err->message);
                            g_clear_error(&err);
                        }
                        return TRUE;
                    }
                    break;
                case PDF_LINK_GOTO:
                    if (link->page_num > 0) {
                        int dest = link->page_num - 1;
                        if (dest >= 0 && dest < tab->n_pages) {
                             tab->cur_page = dest;
                             scroll_to_page(tab, dest, link->y > 0 ? link->y : -1);
                             update_document_model_from_tab(tab);
                             return TRUE;
                        }
                    }
                    break;
                case PDF_LINK_NAMED:
                    if (link->named_dest) {
                        const char *name = link->named_dest;
                        if (g_strcmp0(name, "FirstPage") == 0) {
                            tab->cur_page = 0;
                            scroll_to_page(tab, 0, -1);
                            update_document_model_from_tab(tab);
                            return TRUE;
                        } else if (g_strcmp0(name, "LastPage") == 0) {
                            tab->cur_page = tab->n_pages - 1;
                            scroll_to_page(tab, tab->n_pages - 1, -1);
                            update_document_model_from_tab(tab);
                            return TRUE;
                        } else if (g_strcmp0(name, "NextPage") == 0) {
                            int p = MIN(tab->cur_page + 1, tab->n_pages - 1);
                            tab->cur_page = p;
                            scroll_to_page(tab, p, -1);
                            update_document_model_from_tab(tab);
                            return TRUE;
                        } else if (g_strcmp0(name, "PrevPage") == 0) {
                            int p = MAX(tab->cur_page - 1, 0);
                            tab->cur_page = p;
                            scroll_to_page(tab, p, -1);
                            update_document_model_from_tab(tab);
                            return TRUE;
                        } else if (g_strcmp0(name, "GoBack") == 0 || g_strcmp0(name, "GoForward") == 0) {
                            return TRUE;
                        } else {
                            int resolved;
                            double ny = -1;
                            if (link->page_num > 0) {
                                resolved = link->page_num;
                                ny = link->y;
                            } else {
                                double nx_discard;
                                resolved = pdfr_resolve_named_dest(tab->doc, name, &nx_discard, &ny);
                            }
                            if (resolved > 0) {
                                int dest = resolved - 1;
                                if (dest >= 0 && dest < tab->n_pages) {
                                    tab->cur_page = dest;
                                    scroll_to_page(tab, dest, ny > 0 ? ny : -1);
                                    update_document_model_from_tab(tab);
                                    return TRUE;
                                }
                            }
                        }
                    }
                    break;
                default:
                    break;
            }
        }
        link = link->next;
    }
    return FALSE;
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
                PdfrPage *page = pdfr_load_page(tab->doc, i);
                if (!page) { LOG_ERROR("Failed to load page %d", i); y += page_h + spacing; continue; }

                /* draw background rectangle */
                cairo_save(cr);
                cairo_set_source_rgba(cr, tab->page_color.red, tab->page_color.green, tab->page_color.blue, tab->page_color.alpha);
                cairo_rectangle(cr, off_x, off_y, page_w, page_h);
                cairo_fill(cr);
                cairo_restore(cr);
                int iw = (int)(tab->cached_page_widths[i] * scale * dsx + 0.5);
                int ih = (int)(tab->cached_page_heights[i] * scale * dsy + 0.5);
                if (iw > MAX_SURFACE_DIM) iw = MAX_SURFACE_DIM;
                if (ih > MAX_SURFACE_DIM) ih = MAX_SURFACE_DIM;
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
                        cairo_surface_t *pimg = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, iw, ih);
                        cairo_surface_set_device_scale(pimg, dsx, dsy);
                        cairo_t *picr = cairo_create(pimg);
                        cairo_set_font_options(picr, fo);
                        cairo_set_antialias(picr, CAIRO_ANTIALIAS_BEST);
                        cairo_scale(picr, scale, scale);
                        pdfr_render(tab->doc, page, picr);
                        cairo_destroy(picr);
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
                    if (first_visible == -1) first_visible = i;
                    last_visible = i;
                }
                if (search_page_results_n > 0) {
                    SearchPageResult *sr = find_search_result_for_page(i + 1);
                    if (sr) {
                        double ph_pts = tab->cached_page_heights[i];
                        cairo_save(cr);
                        cairo_set_source_rgba(cr, 1.0, 1.0, 0.0, 0.3);
                        for (int m = 0; m < sr->n_matches; m++) {
                            PdfrRect *r = &sr->rects[m];
                            cairo_rectangle(cr,
                                off_x + r->x1 * scale,
                                off_y + (ph_pts - r->y2) * scale,
                                (r->x2 - r->x1) * scale,
                                (r->y2 - r->y1) * scale);
                        }
                        cairo_fill(cr);
                        cairo_restore(cr);
                    }
                }
                pdfr_free_page(tab->doc, page);
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
                PdfrPage *p1 = pdfr_load_page(tab->doc, i);
                if (p1) {
                    cairo_save(cr);
                    cairo_set_source_rgba(cr, tab->page_color.red, tab->page_color.green, tab->page_color.blue, tab->page_color.alpha);
                    cairo_rectangle(cr, left_x, y, page_w1, page_h1);
                    cairo_fill(cr);
                    cairo_restore(cr);
                    int iw1 = (int)(tab->cached_page_widths[i] * scale * dsx2 + 0.5);
                    int ih1 = (int)(tab->cached_page_heights[i] * scale * dsy2 + 0.5);
                    if (iw1 > MAX_SURFACE_DIM) iw1 = MAX_SURFACE_DIM;
                    if (ih1 > MAX_SURFACE_DIM) ih1 = MAX_SURFACE_DIM;
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
                            cairo_surface_t *pimg = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, iw1, ih1);
                            cairo_surface_set_device_scale(pimg, dsx2, dsy2);
                            cairo_t *picr = cairo_create(pimg);
                            cairo_set_font_options(picr, fo2);
                            cairo_set_antialias(picr, CAIRO_ANTIALIAS_BEST);
                            cairo_scale(picr, scale, scale);
                            pdfr_render(tab->doc, p1, picr);
                            cairo_destroy(picr);
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
                    if (first_visible == -1) first_visible = i;
                        last_visible = i;
                    }
                    if (search_page_results_n > 0) {
                        SearchPageResult *sr = find_search_result_for_page(i + 1);
                        if (sr) {
                            double ph_pts = tab->cached_page_heights[i];
                            cairo_save(cr);
                            cairo_set_source_rgba(cr, 1.0, 1.0, 0.0, 0.3);
                            for (int m = 0; m < sr->n_matches; m++) {
                                PdfrRect *r = &sr->rects[m];
                                cairo_rectangle(cr,
                                    left_x + r->x1 * scale,
                                    y + (ph_pts - r->y2) * scale,
                                    (r->x2 - r->x1) * scale,
                                    (r->y2 - r->y1) * scale);
                            }
                            cairo_fill(cr);
                            cairo_restore(cr);
                        }
                    }
                    pdfr_free_page(tab->doc, p1);
                }
            }

            /* draw right page if visible */
            if (page_h2 > 0 && !(y + page_h2 < clip_y1 || y > clip_y2)) {
                PdfrPage *p2 = pdfr_load_page(tab->doc, i + 1);
                if (p2) {
                    cairo_save(cr);
                    cairo_set_source_rgba(cr, tab->page_color.red, tab->page_color.green, tab->page_color.blue, tab->page_color.alpha);
                    cairo_rectangle(cr, right_x, y, page_w2, page_h2);
                    cairo_fill(cr);
                    cairo_restore(cr);
                    int iw2 = (int)(tab->cached_page_widths[i + 1] * scale * dsx2 + 0.5);
                    int ih2 = (int)(tab->cached_page_heights[i + 1] * scale * dsy2 + 0.5);
                    if (iw2 > MAX_SURFACE_DIM) iw2 = MAX_SURFACE_DIM;
                    if (ih2 > MAX_SURFACE_DIM) ih2 = MAX_SURFACE_DIM;
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
                            cairo_surface_t *pimg = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, iw2, ih2);
                            cairo_surface_set_device_scale(pimg, dsx2, dsy2);
                            cairo_t *picr = cairo_create(pimg);
                            cairo_set_font_options(picr, fo2);
                            cairo_set_antialias(picr, CAIRO_ANTIALIAS_BEST);
                            cairo_scale(picr, scale, scale);
                            pdfr_render(tab->doc, p2, picr);
                            cairo_destroy(picr);
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
                    if (first_visible == -1) first_visible = i;
                    last_visible = i;
                }
                if (search_page_results_n > 0) {
                        SearchPageResult *sr = find_search_result_for_page(i + 1);
                        if (sr) {
                            double ph_pts = tab->cached_page_heights[i];
                            cairo_save(cr);
                            cairo_set_source_rgba(cr, 1.0, 1.0, 0.0, 0.3);
                            for (int m = 0; m < sr->n_matches; m++) {
                                PdfrRect *r = &sr->rects[m];
                                cairo_rectangle(cr,
                                    right_x + r->x1 * scale,
                                    y + (ph_pts - r->y2) * scale,
                                    (r->x2 - r->x1) * scale,
                                    (r->y2 - r->y1) * scale);
                            }
                            cairo_fill(cr);
                            cairo_restore(cr);
                        }
                    }
                    pdfr_free_page(tab->doc, p2);
                }
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
                PdfrPage *page = pdfr_load_page(tab->doc, i);
                if (!page) { x += page_w + spacing; continue; }
                cairo_save(cr);
                cairo_set_source_rgba(cr, tab->page_color.red, tab->page_color.green, tab->page_color.blue, tab->page_color.alpha);
                cairo_rectangle(cr, dev_x, off_y, page_w, page_h);
                cairo_fill(cr);
                cairo_restore(cr);
                int iwh = (int)(tab->cached_page_widths[i] * scale * dsxh + 0.5);
                int ihh = (int)(tab->cached_page_heights[i] * scale * dsyh + 0.5);
                if (iwh > MAX_SURFACE_DIM) iwh = MAX_SURFACE_DIM;
                if (ihh > MAX_SURFACE_DIM) ihh = MAX_SURFACE_DIM;
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
                        cairo_surface_t *pimg = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, iwh, ihh);
                        cairo_surface_set_device_scale(pimg, dsxh, dsyh);
                        cairo_t *picr = cairo_create(pimg);
                        cairo_set_font_options(picr, foh);
                        cairo_set_antialias(picr, CAIRO_ANTIALIAS_BEST);
                        cairo_scale(picr, scale, scale);
                        pdfr_render(tab->doc, page, picr);
                        cairo_destroy(picr);
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
                    if (first_visible == -1) first_visible = i;
                    last_visible = i;
                }
                if (search_page_results_n > 0) {
                    SearchPageResult *sr = find_search_result_for_page(i + 1);
                        if (sr) {
                            double ph_pts = tab->cached_page_heights[i];
                        cairo_save(cr);
                        cairo_set_source_rgba(cr, 1.0, 1.0, 0.0, 0.3);
                        for (int m = 0; m < sr->n_matches; m++) {
                            PdfrRect *r = &sr->rects[m];
                            cairo_rectangle(cr,
                                dev_x + r->x1 * scale,
                                off_y + (ph_pts - r->y2) * scale,
                                (r->x2 - r->x1) * scale,
                                (r->y2 - r->y1) * scale);
                        }
                        cairo_fill(cr);
                        cairo_restore(cr);
                    }
                }
                pdfr_free_page(tab->doc, page);
            }
            x += page_w + spacing;
        }
        cairo_font_options_destroy(foh);
    }

    /* Prune cache: keep only pages within margin of the visible range */
    if (first_visible >= 0 && last_visible >= 0) {
        int margin = 2;
        for (int i = 0; i < tab->n_pages; ++i) {
            if (tab->page_cache[i] && (i < first_visible - margin || i > last_visible + margin))
                cache_evict_idx(tab, i);
        }
    }

    MEM_REPORT_DRAW();
    return FALSE;
}

static void build_continuous_view(TabData *tab) {
    if (!tab || !tab->cached_page_widths || !tab->pages_drawing) return;
    invalidate_page_cache(tab);
    const double spacing = 6.0;
    double scale = get_ppi_scale(tab);
    int page_width_px = tab->n_pages > 0 ? (int)ceil(tab->cached_page_widths[0] * scale) : 800;
    if (page_width_px < 1) page_width_px = 800;

    if (tab->layout_mode == 0) {
        double total_h = spacing;
        for (int i = 0; i < tab->n_pages; ++i) {
            total_h += tab->cached_page_heights[i] * scale + spacing;
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
            double pw1 = tab->cached_page_widths[i];
            double ph1 = tab->cached_page_heights[i];
            double page_w1 = pw1 * scale;
            double page_h1 = ph1 * scale;
            double row_w = page_w1;
            double row_h = page_h1;
            double page_w2 = 0, page_h2 = 0;
            if (i + 1 < n) {
                page_w2 = tab->cached_page_widths[i + 1] * scale;
                page_h2 = tab->cached_page_heights[i + 1] * scale;
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
            double page_w = tab->cached_page_widths[i] * scale;
            double page_h = tab->cached_page_heights[i] * scale;
            total_w += page_w + spacing;
            if (page_h > max_h) max_h = page_h;
        }
        if (total_w < 1.0) total_w = 1.0;
        if (max_h < 1.0) max_h = 1.0;
        tab->max_page_h = max_h;
        if (tab->h_scrollbar) {
            GtkAdjustment *adj = gtk_range_get_adjustment(GTK_RANGE(tab->h_scrollbar));
            gtk_adjustment_set_lower(adj, 0.0);
            gtk_adjustment_set_upper(adj, total_w);
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
    scroll_to_page(tab, target_zero_based, -1);

    update_document_model_from_tab(tab);
}

static void sync_right_page_widget_from_tab(TabData *tab) {
    if (!right_page_entry || !right_page_total_label) return;

    int total = 0;
    int current = 0;

    if (tab && tab->n_pages > 0) {
        total = tab->n_pages;
        current = tab->cur_page + 1;
        if (current < 1) current = 1;
        if (current > total) current = total;
    }

    right_page_spin_syncing = TRUE;
    if (total == 0) {
        gtk_entry_set_text(GTK_ENTRY(right_page_entry), "");
    } else {
        gchar *cur_txt = g_strdup_printf("%d", current);
        gtk_entry_set_text(GTK_ENTRY(right_page_entry), cur_txt);
        g_free(cur_txt);
    }
    right_page_spin_syncing = FALSE;

    gchar *txt = g_strdup_printf("/ %d", total);
    gtk_label_set_text(GTK_LABEL(right_page_total_label), txt);
    g_free(txt);

    /* Show/hide right nav based on whether there are pages */
    if (right_page_nav_overlay) {
        if (total > 0)
            gtk_widget_show(right_page_nav_overlay);
        else
            gtk_widget_hide(right_page_nav_overlay);
    }
}

static void on_right_page_entry_activate(GtkEntry *entry, gpointer user_data) {
    (void)user_data;
    if (right_page_spin_syncing) return;

    TabData *tab = get_current_right_tab();
    if (!tab || tab->n_pages <= 0) return;

    const char *raw = gtk_entry_get_text(GTK_ENTRY(entry));
    char *endptr = NULL;
    long requested_ui = strtol(raw, &endptr, 10);
    if (endptr == raw || *endptr != '\0') {
        sync_right_page_widget_from_tab(tab);
        return;
    }

    if (requested_ui < 1 || requested_ui > tab->n_pages) return;

    int target_zero_based = requested_ui - 1;
    tab->cur_page = target_zero_based;
    scroll_to_page(tab, target_zero_based, -1);

    update_document_model_from_tab(tab);
}

static void load_file_into_tab(TabData *tab, const char *filename) {
    if (!tab || !filename) return;
    char *open_error = NULL;
    PdfrDoc *doc = pdfr_open(filename, &open_error);
    if (!doc) {
        LOG_ERROR("Failed to open PDF: %s", open_error ? open_error : "unknown error");
        free(open_error);
        return;
    }
    free(open_error);

    if (tab->doc)
        pdfr_close(tab->doc);

    tab->doc = doc;
    tab->n_pages = pdfr_count_pages(doc);
    cache_page_dimensions(tab);
    tab->cur_page = 0;
    tab->zoom = 96.0;
    /* track current filename for per-document settings */
    if (tab->current_file)
        g_free(tab->current_file);
    tab->current_file = g_strdup(filename);

    /* Update the tab's label with filename */
    if (tab->tab_label) {
        char *basename = g_path_get_basename(filename);
        const char *label_text = basename;
        char *truncated = NULL;
        if (sessions_model) {
            int max_chars = sessions_model_get_tab_width(sessions_model);
            if (max_chars > 0 && (int)strlen(basename) > max_chars) {
                truncated = g_strndup(basename, max_chars);
                label_text = truncated;
            }
        }
        gtk_label_set_text(GTK_LABEL(tab->tab_label), label_text);
        g_free(truncated);
        g_free(basename);
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
            sync_page_widget_from_tab(tab);
        }
        if (tab == get_current_right_tab()) {
            sync_right_layout_buttons(tab);
            sync_right_page_widget_from_tab(tab);
        }
    }
    if (current_sidebar_mode == SIDEBAR_TOC) populate_toc_treeview();
    if (current_sidebar_mode == SIDEBAR_FILE_INFO) update_file_info_labels(get_current_left_tab());
    if (right_file_info_popover && gtk_widget_get_mapped(right_file_info_popover)) {
        TabData *rtab = get_current_right_tab();
        gchar *basename = rtab && rtab->current_file ? g_path_get_basename(rtab->current_file) : NULL;
        gchar *text = basename ? g_strdup_printf("Name: %s", basename) : g_strdup("Name: (no file)");
        gtk_label_set_text(GTK_LABEL(right_popover_name_label), text);
        g_free(text);
        g_free(basename);

        text = rtab && rtab->current_file ? g_strdup_printf("Path: %s", rtab->current_file) : g_strdup("Path: (none)");
        gtk_label_set_text(GTK_LABEL(right_popover_path_label), text);
        g_free(text);

        if (rtab && rtab->current_file) {
            GFile *gf = g_file_new_for_path(rtab->current_file);
            GFileInfo *info = g_file_query_info(gf, G_FILE_ATTRIBUTE_STANDARD_SIZE,
                                                 G_FILE_QUERY_INFO_NONE, NULL, NULL);
            if (info) {
                gchar *size_str = format_file_size(g_file_info_get_size(info));
                gchar *size_text = g_strdup_printf("Size: %s", size_str);
                gtk_label_set_text(GTK_LABEL(right_popover_size_label), size_text);
                g_free(size_text);
                g_free(size_str);
                g_object_unref(info);
            } else {
                gtk_label_set_text(GTK_LABEL(right_popover_size_label), "Size: Unknown");
            }
            g_object_unref(gf);

            if (rtab->doc) {
                gchar *pages_text = g_strdup_printf("Pages: %d", pdfr_count_pages(rtab->doc));
                gtk_label_set_text(GTK_LABEL(right_popover_pages_label), pages_text);
                g_free(pages_text);
            } else {
                gtk_label_set_text(GTK_LABEL(right_popover_pages_label), "Pages: N/A");
            }
        } else {
            gtk_label_set_text(GTK_LABEL(right_popover_size_label), "Size: (none)");
            gtk_label_set_text(GTK_LABEL(right_popover_pages_label), "Pages: (none)");
        }
    }
}

static TabData *create_new_tab(GtkWidget *notebook) {
    TabData *tab = g_malloc0(sizeof(TabData));
    tab->zoom = 96.0;
    tab->layout_mode = 0;    /* single-column by default */
    tab->n_pages = 0;
    tab->cur_page = 0;
    tab->last_zoom = 96.0;
    tab->initial_scroll_pending = FALSE;
    tab->scroll_offset = -1.0;
    tab->is_helper = (notebook == right_notebook);
    tab->zoom_scroll_source_id = 0;
    tab->scroll_doc_debounce_id = 0;
    tab->last_cursor_type = GDK_LEFT_PTR;
    tab->last_cursor_check = 0;

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
    if (sessions_model) {
        const char *pos = sessions_model_get_tabbar_position(sessions_model);
        if (g_strcmp0(pos, "left") == 0 || g_strcmp0(pos, "right") == 0)
            box_orientation = GTK_ORIENTATION_VERTICAL;
    }
    GtkWidget *label_box = gtk_box_new(box_orientation, 1);
    GtkWidget *label = gtk_label_new("New Document");
    if (sessions_model)
        gtk_label_set_angle(GTK_LABEL(label), get_angle_for_position(sessions_model_get_tabbar_position(sessions_model)));
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
    if (sessions_model && g_strcmp0(sessions_model_get_tabbar_position(sessions_model), "left") == 0) {
        gtk_box_pack_start(GTK_BOX(label_box), close_btn, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(label_box), label, FALSE, FALSE, 0);
    } else if (sessions_model && g_strcmp0(sessions_model_get_tabbar_position(sessions_model), "right") == 0) {
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
    gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(dialog), TRUE);

    if (last_open_dir && g_file_test(last_open_dir, G_FILE_TEST_IS_DIR)) {
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), last_open_dir);
    }

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        GSList *filenames = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(dialog));
        if (filenames) {
            char *first_file = (char *)filenames->data;
            if (first_file) {
                char *dir = g_path_get_dirname(first_file);
                if (dir) {
                    g_free(last_open_dir);
                    last_open_dir = dir;
                }
            }
        }
        gboolean changed = FALSE;
        for (GSList *f = filenames; f; f = f->next) {
            char *fname = (char *)f->data;
            if (!fname) continue;
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
                                changed = TRUE;
                            }
                        }
                    }
                }
            }
            if (uri) g_free(uri);
            g_free(fname);
        }
        g_slist_free(filenames);
        if (changed) {
            populate_sessions_treeview();
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

static void close_tab_in_notebook(GtkNotebook *notebook) {
    if (!notebook) return;
    int page_idx = gtk_notebook_get_current_page(notebook);
    if (page_idx < 0) return;

    GtkWidget *page = gtk_notebook_get_nth_page(notebook, page_idx);
    if (!page) return;

    TabData *tab = g_object_get_data(G_OBJECT(page), "tab-data");
    if (!tab) {
        gtk_notebook_remove_page(notebook, page_idx);
        return;
    }

    gboolean is_left = (notebook == GTK_NOTEBOOK(left_notebook));
    gboolean is_right = (notebook == GTK_NOTEBOOK(right_notebook));

    char *closed_uri = NULL;
    session_model_t *session = NULL;
    if (current_selected_session && session_models) {
        session = g_hash_table_lookup(session_models, current_selected_session);
    }

    if (tab->current_file && session) {
        closed_uri = g_filename_to_uri(tab->current_file, NULL, NULL);
        if (closed_uri) {
            if (is_left) {
                session_model_remove_document_url(session, closed_uri);
            } else if (is_right) {
                session_model_remove_helper_document_url(session, closed_uri);
            }
        }
        if (closed_uri && document_models) {
            char *key = make_document_key(current_selected_session, closed_uri, tab->is_helper);
            g_hash_table_remove(document_models, key);
            g_free(key);
        }
    }

    gtk_notebook_remove_page(notebook, page_idx);

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
            int cur = gtk_notebook_get_current_page(notebook);
            if (cur >= 0) {
                GtkWidget *new_page = gtk_notebook_get_nth_page(notebook, cur);
                if (new_page) {
                    update_last_read_for_notebook(notebook, new_page, (guint)cur);
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

    if (closed_uri) g_free(closed_uri);

    if (is_left) {
        sync_page_widget_from_tab(get_current_left_tab());
    } else if (is_right) {
        sync_right_page_widget_from_tab(get_current_right_tab());
    }

    if (current_sidebar_mode == SIDEBAR_FILE_INFO) {
        update_file_info_labels(get_current_left_tab());
    }

    if (right_file_info_popover && gtk_widget_get_mapped(right_file_info_popover)) {
        TabData *rtab = get_current_right_tab();
        gchar *basename = rtab && rtab->current_file ? g_path_get_basename(rtab->current_file) : NULL;
        gchar *text = basename ? g_strdup_printf("Name: %s", basename) : g_strdup("Name: (no file)");
        gtk_label_set_text(GTK_LABEL(right_popover_name_label), text);
        g_free(text);
        g_free(basename);

        text = rtab && rtab->current_file ? g_strdup_printf("Path: %s", rtab->current_file) : g_strdup("Path: (none)");
        gtk_label_set_text(GTK_LABEL(right_popover_path_label), text);
        g_free(text);

        if (rtab && rtab->current_file) {
            GFile *gf = g_file_new_for_path(rtab->current_file);
            GFileInfo *info = g_file_query_info(gf, G_FILE_ATTRIBUTE_STANDARD_SIZE,
                                                 G_FILE_QUERY_INFO_NONE, NULL, NULL);
            if (info) {
                gchar *size_str = format_file_size(g_file_info_get_size(info));
                text = g_strdup_printf("Size: %s", size_str);
                gtk_label_set_text(GTK_LABEL(right_popover_size_label), text);
                g_free(text);
                g_free(size_str);
                g_object_unref(info);
            } else {
                gtk_label_set_text(GTK_LABEL(right_popover_size_label), "Size: Unknown");
            }
            g_object_unref(gf);

            if (rtab->doc) {
                gchar *pages_text = g_strdup_printf("Pages: %d", pdfr_count_pages(rtab->doc));
                gtk_label_set_text(GTK_LABEL(right_popover_pages_label), pages_text);
                g_free(pages_text);
            } else {
                gtk_label_set_text(GTK_LABEL(right_popover_pages_label), "Pages: N/A");
            }
        } else {
            gtk_label_set_text(GTK_LABEL(right_popover_size_label), "Size: (none)");
            gtk_label_set_text(GTK_LABEL(right_popover_pages_label), "Pages: (none)");
        }
    }

    if (is_left) {
        populate_sessions_treeview();
    }
}

static void on_close_file_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    (void)user_data;
    close_tab_in_notebook(GTK_NOTEBOOK(left_notebook));
}

static void on_close_helper_file_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    (void)user_data;
    close_tab_in_notebook(GTK_NOTEBOOK(right_notebook));
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

    /* Flush state for all left tabs with loaded docs before switching */
    int n_left = gtk_notebook_get_n_pages(notebook);
    for (int i = 0; i < n_left; i++) {
        GtkWidget *p = gtk_notebook_get_nth_page(notebook, i);
        TabData *t = g_object_get_data(G_OBJECT(p), "tab-data");
        if (t && t->doc) update_document_model_from_tab(t);
    }

    /* Unload docs from all non-current tabs in this notebook to save RAM */
    TabData *tab = g_object_get_data(G_OBJECT(page), "tab-data");
    for (int i = 0; i < n_left; i++) {
        GtkWidget *p = gtk_notebook_get_nth_page(notebook, i);
        TabData *t = g_object_get_data(G_OBJECT(p), "tab-data");
        if (t && t != tab && t->doc) {
            cancel_tab_restore(t);
            cancel_doc_model_debounce(t);
            if (t->zoom_scroll_source_id) {
                g_source_remove(t->zoom_scroll_source_id);
                t->zoom_scroll_source_id = 0;
            }
            /* Free link mappings while the doc is still alive */
            if (t->page_links) {
                for (int j = 0; j < t->page_links_n; j++) {
                    if (t->page_links[j])
                        pdfr_free_links(t->doc, t->page_links[j]);
                }
                g_free(t->page_links);
                t->page_links = NULL;
                t->page_links_n = 0;
            }
            pdfr_close(t->doc);
            t->doc = NULL;
            g_free(t->cached_page_widths);
            g_free(t->cached_page_heights);
            g_free(t->cached_page_x0);
            g_free(t->cached_page_y0);
            t->cached_page_widths = NULL;
            t->cached_page_heights = NULL;
            t->cached_page_x0 = NULL;
            t->cached_page_y0 = NULL;
            invalidate_page_cache(t);
            g_free(t->page_cache);
            t->page_cache = NULL;
        }
    }

    update_last_read_for_notebook(notebook, page, page_num);

    /* Load current tab's doc if needed */
    if (tab) ensure_tab_doc_loaded(tab);

    // RESTORE STATE WHEN SWITCHING TO THIS TAB
    if (tab) {
        if (tab->initial_scroll_pending) {
            // First time seeing this tab after startup - do full restoration
            restore_document_model_to_tab(tab);
            tab->initial_scroll_pending = FALSE;
        } else {
            if (tab->current_file && document_models && tab->cached_page_widths) {
                char *uri = g_filename_to_uri(tab->current_file, NULL, NULL);
                if (uri) {
                    char *key = make_document_key(current_selected_session, uri, tab->is_helper);
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
                        scroll_to_page(tab, tab->cur_page, -1);
                    }
                    g_free(key);
                    g_free(uri);
                }
            }
        }
    }

    sync_left_layout_buttons(tab);
    sync_page_widget_from_tab(tab);
    if (current_sidebar_mode == SIDEBAR_TOC) {
        populate_toc_treeview_for_tab(tab);
        update_toc_selection_for_current_page(tab);
    }
    if (current_sidebar_mode == SIDEBAR_SESSIONS) {
        sessions_tree_syncing = TRUE;
        update_sessions_tree_document_selection_for_tab(tab);
        sessions_tree_syncing = FALSE;
    }
    if (current_sidebar_mode == SIDEBAR_FILE_INFO) update_file_info_labels(tab);
}

static void on_right_notebook_switch_page(GtkNotebook *notebook, GtkWidget *page, guint page_num, gpointer user_data) {
    (void)user_data;

    /* Unload docs from all non-current tabs in this notebook */
    TabData *tab = g_object_get_data(G_OBJECT(page), "tab-data");
    int n_right = gtk_notebook_get_n_pages(notebook);
    for (int i = 0; i < n_right; i++) {
        GtkWidget *p = gtk_notebook_get_nth_page(notebook, i);
        TabData *t = g_object_get_data(G_OBJECT(p), "tab-data");
        if (t && t != tab && t->doc) {
            cancel_tab_restore(t);
            cancel_doc_model_debounce(t);
            if (t->zoom_scroll_source_id) {
                g_source_remove(t->zoom_scroll_source_id);
                t->zoom_scroll_source_id = 0;
            }
            /* Free link mappings while the doc is still alive */
            if (t->page_links) {
                for (int j = 0; j < t->page_links_n; j++) {
                    if (t->page_links[j])
                        pdfr_free_links(t->doc, t->page_links[j]);
                }
                g_free(t->page_links);
                t->page_links = NULL;
                t->page_links_n = 0;
            }
            pdfr_close(t->doc);
            t->doc = NULL;
            g_free(t->cached_page_widths);
            g_free(t->cached_page_heights);
            g_free(t->cached_page_x0);
            g_free(t->cached_page_y0);
            t->cached_page_widths = NULL;
            t->cached_page_heights = NULL;
            t->cached_page_x0 = NULL;
            t->cached_page_y0 = NULL;
            invalidate_page_cache(t);
            g_free(t->page_cache);
            t->page_cache = NULL;
        }
    }

    update_last_read_for_notebook(notebook, page, page_num);

    /* Load current tab's doc if needed */
    if (tab) ensure_tab_doc_loaded(tab);

    // RESTORE STATE WHEN SWITCHING TO THIS TAB
    if (tab) {
        restore_document_model_to_tab(tab);
    }

    sync_right_layout_buttons(tab);
    /* keep left widget tied to primary (left) document */
    sync_page_widget_from_tab(get_current_left_tab());
    sync_right_page_widget_from_tab(tab);

    if (right_file_info_popover && gtk_widget_get_mapped(right_file_info_popover)) {
        TabData *rtab = tab;
        gchar *basename = rtab && rtab->current_file ? g_path_get_basename(rtab->current_file) : NULL;
        gchar *text = basename ? g_strdup_printf("Name: %s", basename) : g_strdup("Name: (no file)");
        gtk_label_set_text(GTK_LABEL(right_popover_name_label), text);
        g_free(text);
        g_free(basename);

        text = rtab && rtab->current_file ? g_strdup_printf("Path: %s", rtab->current_file) : g_strdup("Path: (none)");
        gtk_label_set_text(GTK_LABEL(right_popover_path_label), text);
        g_free(text);

        if (rtab && rtab->current_file) {
            GFile *gf = g_file_new_for_path(rtab->current_file);
            GFileInfo *info = g_file_query_info(gf, G_FILE_ATTRIBUTE_STANDARD_SIZE,
                                                 G_FILE_QUERY_INFO_NONE, NULL, NULL);
            if (info) {
                gchar *size_str = format_file_size(g_file_info_get_size(info));
                gchar *size_text = g_strdup_printf("Size: %s", size_str);
                gtk_label_set_text(GTK_LABEL(right_popover_size_label), size_text);
                g_free(size_text);
                g_free(size_str);
                g_object_unref(info);
            } else {
                gtk_label_set_text(GTK_LABEL(right_popover_size_label), "Size: Unknown");
            }
            g_object_unref(gf);

            if (rtab->doc) {
                gchar *pages_text = g_strdup_printf("Pages: %d", pdfr_count_pages(rtab->doc));
                gtk_label_set_text(GTK_LABEL(right_popover_pages_label), pages_text);
                g_free(pages_text);
            } else {
                gtk_label_set_text(GTK_LABEL(right_popover_pages_label), "Pages: N/A");
            }
        } else {
            gtk_label_set_text(GTK_LABEL(right_popover_size_label), "Size: (none)");
            gtk_label_set_text(GTK_LABEL(right_popover_pages_label), "Pages: (none)");
        }
    }
}

static void on_notebook_page_reordered(GtkNotebook *notebook, GtkWidget *page, guint page_num, gpointer user_data) {
    (void)notebook;
    (void)page;
    (void)page_num;
    (void)user_data;
    if (current_selected_session) {
        save_open_tabs_for_session(current_selected_session);
        populate_sessions_treeview();
    }
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
                // Remove document model so reopened file starts fresh (zoom, page, view, etc.)
                if (closed_uri && document_models) {
                    char *key = make_document_key(current_selected_session, closed_uri, tab->is_helper);
                    g_hash_table_remove(document_models, key);
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

    if (current_sidebar_mode == SIDEBAR_FILE_INFO) {
        update_file_info_labels(get_current_left_tab());
    }

    if (right_file_info_popover && gtk_widget_get_mapped(right_file_info_popover)) {
        TabData *rtab = get_current_right_tab();
        gchar *basename = rtab && rtab->current_file ? g_path_get_basename(rtab->current_file) : NULL;
        gchar *text = basename ? g_strdup_printf("Name: %s", basename) : g_strdup("Name: (no file)");
        gtk_label_set_text(GTK_LABEL(right_popover_name_label), text);
        g_free(text);
        g_free(basename);

        text = rtab && rtab->current_file ? g_strdup_printf("Path: %s", rtab->current_file) : g_strdup("Path: (none)");
        gtk_label_set_text(GTK_LABEL(right_popover_path_label), text);
        g_free(text);

        if (rtab && rtab->current_file) {
            GFile *gf = g_file_new_for_path(rtab->current_file);
            GFileInfo *info = g_file_query_info(gf, G_FILE_ATTRIBUTE_STANDARD_SIZE,
                                                 G_FILE_QUERY_INFO_NONE, NULL, NULL);
            if (info) {
                gchar *size_str = format_file_size(g_file_info_get_size(info));
                text = g_strdup_printf("Size: %s", size_str);
                gtk_label_set_text(GTK_LABEL(right_popover_size_label), text);
                g_free(text);
                g_free(size_str);
                g_object_unref(info);
            } else {
                gtk_label_set_text(GTK_LABEL(right_popover_size_label), "Size: Unknown");
            }
            g_object_unref(gf);

            if (rtab->doc) {
                gchar *pages_text = g_strdup_printf("Pages: %d", pdfr_count_pages(rtab->doc));
                gtk_label_set_text(GTK_LABEL(right_popover_pages_label), pages_text);
                g_free(pages_text);
            } else {
                gtk_label_set_text(GTK_LABEL(right_popover_pages_label), "Pages: N/A");
            }
        } else {
            gtk_label_set_text(GTK_LABEL(right_popover_size_label), "Size: (none)");
            gtk_label_set_text(GTK_LABEL(right_popover_pages_label), "Pages: (none)");
        }
    }

    g_free(ci);

    if (is_left) {
        populate_sessions_treeview();
    }
}

/* =============== Layout button management =============== */

/* Idle callback: restore scroll position after zoom view rebuild */
static gboolean restore_zoom_scroll_cb(gpointer user_data) {
    TabData *tab = user_data;
    if (!tab || !tab->cached_page_widths || !tab->scrolled || !tab->zoom_scroll_source_id) {
        return FALSE;
    }
    tab->zoom_scroll_source_id = 0;

    int page = tab->zoom_scroll_target_page;
    double fraction = tab->zoom_scroll_fraction;
    if (page < 0 || page >= tab->n_pages) return FALSE;

    const double spacing = 6.0;
    double scale = get_ppi_scale(tab);

    if (tab->layout_mode == 2) {
        if (!tab->h_scrollbar) return FALSE;
        double x = spacing;
        for (int i = 0; i < page; ++i) {
            x += tab->cached_page_widths[i] * scale + spacing;
        }
        double page_w = tab->cached_page_widths[page] * scale;
        double target = x + fraction * page_w;
        GtkAdjustment *sadj = gtk_range_get_adjustment(GTK_RANGE(tab->h_scrollbar));
        gtk_adjustment_set_value(sadj, target);
    } else if (tab->layout_mode == 1) {
        int row = page / 2;
        double y = spacing;
        for (int i = 0; i < row; ++i) {
            double row_h = 0.0;
            for (int p = 0; p < 2; p++) {
                int idx = i * 2 + p;
                if (idx >= tab->n_pages) break;
                double h = tab->cached_page_heights[idx] * scale;
                if (h > row_h) row_h = h;
            }
            if (row_h < 1.0) row_h = 1.0;
            y += row_h + spacing;
        }
        double curr_row_h = 0.0;
        for (int p = 0; p < 2; p++) {
            int idx = row * 2 + p;
            if (idx >= tab->n_pages) break;
            double h = tab->cached_page_heights[idx] * scale;
            if (h > curr_row_h) curr_row_h = h;
        }
        if (curr_row_h < 1.0) curr_row_h = 1.0;
        double target = y + fraction * curr_row_h;
        GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(tab->scrolled));
        gtk_adjustment_set_value(vadj, target);
    } else {
        double y = spacing;
        for (int i = 0; i < page; ++i) {
            y += tab->cached_page_heights[i] * scale + spacing;
        }
        double page_h = get_page_height_ppi(tab, page);
        double target = y + fraction * page_h;
        GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(tab->scrolled));
        gtk_adjustment_set_value(vadj, target);
    }
    return FALSE;
}

static gboolean deferred_zoom_save(gpointer data) {
    (void)data;
    zoom_save_debounce_id = 0;
    save_state();
    return FALSE;
}

static void schedule_zoom_save(void) {
    if (zoom_save_debounce_id)
        g_source_remove(zoom_save_debounce_id);
    zoom_save_debounce_id = g_timeout_add(200, deferred_zoom_save, NULL);
}

static void apply_zoom_to_tab(TabData *tab, int direction) {
    if (!tab) return;
    if (!tab->doc && !ensure_tab_doc_loaded(tab)) return;
    double new_zoom = tab->zoom + (direction > 0 ? 2.0 : -2.0);
    if (new_zoom < 10.0) new_zoom = 10.0;
    if (new_zoom > 500.0) new_zoom = 500.0;

    /* Save current scroll state (using pre-zoom page sizes) */
    if (tab->cached_page_widths && tab->scrolled && tab->n_pages > 0) {
        int page = 0;
        double fraction = 0.0;
        const double spacing = 6.0;
        double old_scale = get_ppi_scale(tab);

        if (tab->layout_mode == 2) {
            if (tab->h_scrollbar) {
                GtkAdjustment *sadj = gtk_range_get_adjustment(GTK_RANGE(tab->h_scrollbar));
                double scroll_x = gtk_adjustment_get_value(sadj);
                double x = spacing;
                for (int i = 0; i < tab->n_pages; ++i) {
                    double page_w = tab->cached_page_widths[i] * old_scale;
                    if (page_w <= 0) continue;
                    if (scroll_x >= x && (scroll_x < x + page_w || i == tab->n_pages - 1)) {
                        page = i;
                        fraction = (scroll_x - x) / page_w;
                        break;
                    }
                    x += page_w + spacing;
                }
            }
        } else if (tab->layout_mode == 1) {
            GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(tab->scrolled));
            double scroll_y = gtk_adjustment_get_value(vadj);
            double row_y = spacing;
            int n_rows = (tab->n_pages + 1) / 2;
            for (int r = 0; r < n_rows; ++r) {
                double row_h = 0.0;
                for (int p = 0; p < 2; p++) {
                    int idx = r * 2 + p;
                    if (idx >= tab->n_pages) break;
                    double h = tab->cached_page_heights[idx] * old_scale;
                    if (h > row_h) row_h = h;
                }
                if (row_h < 1.0) { row_y += spacing; continue; }
                if (scroll_y >= row_y && (scroll_y < row_y + row_h || r == n_rows - 1)) {
                    page = r * 2;
                    fraction = (scroll_y - row_y) / row_h;
                    break;
                }
                row_y += row_h + spacing;
            }
        } else {
            GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(tab->scrolled));
            double scroll_y = gtk_adjustment_get_value(vadj);
            double y = spacing;
            for (int i = 0; i < tab->n_pages; ++i) {
                double page_h = tab->cached_page_heights[i] * old_scale;
                if (page_h <= 0) { y += spacing; continue; }
                if (scroll_y >= y && (scroll_y < y + page_h || i == tab->n_pages - 1)) {
                    page = i;
                    fraction = (scroll_y - y) / page_h;
                    break;
                }
                y += page_h + spacing;
            }
        }

        tab->zoom_scroll_target_page = page;
        tab->zoom_scroll_fraction = CLAMP(fraction, 0.0, 1.0);
    }

    tab->zoom = new_zoom;
    tab->last_zoom = new_zoom;
    build_continuous_view(tab);
    gtk_widget_queue_resize(tab->scrolled);

    /* Schedule deferred scroll restore after layout settles */
    if (tab->doc && tab->n_pages > 0) {
        if (tab->zoom_scroll_source_id)
            g_source_remove(tab->zoom_scroll_source_id);
        tab->zoom_scroll_source_id = g_idle_add(restore_zoom_scroll_cb, tab);
    }

    update_document_model_from_tab(tab);
    schedule_zoom_save();
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

static void on_page_up_left(GtkButton *btn, gpointer user_data) {
    (void)btn;
    (void)user_data;
    TabData *tab = get_current_left_tab();
    if (!tab || tab->n_pages <= 0 || tab->cur_page <= 0) return;
    tab->cur_page--;
    scroll_to_page(tab, tab->cur_page, -1);
}

static void on_page_down_left(GtkButton *btn, gpointer user_data) {
    (void)btn;
    (void)user_data;
    TabData *tab = get_current_left_tab();
    if (!tab || tab->n_pages <= 0 || tab->cur_page >= tab->n_pages - 1) return;
    tab->cur_page++;
    scroll_to_page(tab, tab->cur_page, -1);
}

static void on_page_up_right(GtkButton *btn, gpointer user_data) {
    (void)btn;
    (void)user_data;
    TabData *tab = get_current_right_tab();
    if (!tab || tab->n_pages <= 0 || tab->cur_page <= 0) return;
    tab->cur_page--;
    scroll_to_page(tab, tab->cur_page, -1);
}

static void on_page_down_right(GtkButton *btn, gpointer user_data) {
    (void)btn;
    (void)user_data;
    TabData *tab = get_current_right_tab();
    if (!tab || tab->n_pages <= 0 || tab->cur_page >= tab->n_pages - 1) return;
    tab->cur_page++;
    scroll_to_page(tab, tab->cur_page, -1);
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

    /* Auto-detect dark vs light theme: check both the prefer-dark setting
       and the theme name for known dark-theme keywords. */
    {
        GtkSettings *settings = gtk_settings_get_default();
        is_dark_theme = detect_system_dark_theme();
        sessions_model_set_theme(sessions_model, is_dark_theme ? "dark" : "light");
        g_signal_connect(settings, "notify::gtk-theme-name", G_CALLBACK(on_theme_changed), NULL);
        g_signal_connect(settings, "notify::gtk-application-prefer-dark-theme", G_CALLBACK(on_theme_changed), NULL);
    }

    g_signal_connect(window, "destroy", G_CALLBACK(on_window_destroy), NULL);
    g_signal_connect(window, "configure-event", G_CALLBACK(on_window_configure), NULL);

    /* Main horizontal container: toolbar on left, content on right */
    main_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_container_add(GTK_CONTAINER(window), main_hbox);

    /* Left sidebar: main toolbar */
    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(toolbar), "Toolbar");
    gtk_widget_set_size_request(toolbar, 36, -1);
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
    atk_object_set_name(gtk_widget_get_accessible(sessions_tree_view), "Sessions tree");

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

    /* TOC container */
    toc_container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_set_border_width(GTK_CONTAINER(toc_container), 5);
    g_object_ref(toc_container);

    GtkWidget *toc_title = gtk_label_new("Table of Contents");
    gtk_widget_set_halign(toc_title, GTK_ALIGN_START);
    PangoAttrList *toc_attr = pango_attr_list_new();
    pango_attr_list_insert(toc_attr, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    pango_attr_list_insert(toc_attr, pango_attr_scale_new(PANGO_SCALE_LARGE));
    gtk_label_set_attributes(GTK_LABEL(toc_title), toc_attr);
    pango_attr_list_unref(toc_attr);
    gtk_box_pack_start(GTK_BOX(toc_container), toc_title, FALSE, FALSE, 0);

    toc_tree_store = gtk_tree_store_new(TOC_COL_COUNT, G_TYPE_STRING, G_TYPE_INT, G_TYPE_STRING);

    toc_tree_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(toc_tree_store));
    g_object_unref(toc_tree_store);

    GtkCellRenderer *toc_renderer = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *toc_col = gtk_tree_view_column_new_with_attributes("Section", toc_renderer, "text", TOC_COL_LABEL, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(toc_tree_view), toc_col);

    gtk_tree_view_set_activate_on_single_click(GTK_TREE_VIEW(toc_tree_view), TRUE);
    g_signal_connect(toc_tree_view, "row-activated", G_CALLBACK(on_toc_row_activated), NULL);

    GtkWidget *toc_scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(toc_scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(toc_scrolled), toc_tree_view);
    gtk_box_pack_start(GTK_BOX(toc_container), toc_scrolled, TRUE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(sidebar), toc_container, TRUE, TRUE, 0);
    gtk_widget_hide(toc_container);

    /* Settings container */
    settings_container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_set_border_width(GTK_CONTAINER(settings_container), 5);
    g_object_ref(settings_container);

    GtkWidget *settings_title = gtk_label_new("Reader Preferences");
    gtk_widget_set_halign(settings_title, GTK_ALIGN_START);
    PangoAttrList *sattr = pango_attr_list_new();
    pango_attr_list_insert(sattr, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    pango_attr_list_insert(sattr, pango_attr_scale_new(PANGO_SCALE_LARGE));
    gtk_label_set_attributes(GTK_LABEL(settings_title), sattr);
    pango_attr_list_unref(sattr);
    gtk_box_pack_start(GTK_BOX(settings_container), settings_title, FALSE, FALSE, 0);

    /* Tab bar position */
    GtkWidget *tabbar_label = gtk_label_new("Tab bar position:");
    gtk_widget_set_halign(tabbar_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(settings_container), tabbar_label, FALSE, FALSE, 0);

    tabbar_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(tabbar_combo), "left", "Left");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(tabbar_combo), "top", "Top");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(tabbar_combo), "right", "Right");
    if (sessions_model) {
        const char *cur = sessions_model_get_tabbar_position(sessions_model);
        if (g_strcmp0(cur, "left") == 0)
            gtk_combo_box_set_active(GTK_COMBO_BOX(tabbar_combo), 0);
        else if (g_strcmp0(cur, "right") == 0)
            gtk_combo_box_set_active(GTK_COMBO_BOX(tabbar_combo), 2);
        else
            gtk_combo_box_set_active(GTK_COMBO_BOX(tabbar_combo), 1);
    } else {
        gtk_combo_box_set_active(GTK_COMBO_BOX(tabbar_combo), 1);
    }
    gtk_box_pack_start(GTK_BOX(settings_container), tabbar_combo, FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(tabbar_combo), "changed", G_CALLBACK(on_tabbar_combo_changed), NULL);

    /* Tab text width */
    GtkWidget *tab_width_label = gtk_label_new("Tab text width:");
    gtk_widget_set_halign(tab_width_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(settings_container), tab_width_label, FALSE, FALSE, 0);

    tab_width_spin = gtk_spin_button_new_with_range(5, 50, 1);
    if (sessions_model)
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(tab_width_spin), sessions_model_get_tab_width(sessions_model));
    else
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(tab_width_spin), 50);
    gtk_box_pack_start(GTK_BOX(settings_container), tab_width_spin, FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(tab_width_spin), "value-changed", G_CALLBACK(on_tab_width_spin_changed), NULL);

    /* Page colors */
    GtkWidget *color_title = gtk_label_new("Page Colors");
    gtk_widget_set_halign(color_title, GTK_ALIGN_START);
    PangoAttrList *cat = pango_attr_list_new();
    pango_attr_list_insert(cat, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    gtk_label_set_attributes(GTK_LABEL(color_title), cat);
    pango_attr_list_unref(cat);
    gtk_box_pack_start(GTK_BOX(settings_container), color_title, FALSE, FALSE, 0);

    GtkWidget *left_color_label = gtk_label_new("Left notebook:");
    gtk_widget_set_halign(left_color_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(settings_container), left_color_label, FALSE, FALSE, 0);

    left_color_btn = gtk_color_button_new_with_rgba(&(GdkRGBA){1.0, 1.0, 1.0, 1.0});
    gtk_box_pack_start(GTK_BOX(settings_container), left_color_btn, FALSE, FALSE, 0);
    g_signal_connect(left_color_btn, "color-set", G_CALLBACK(on_left_color_set), NULL);

    GtkWidget *right_color_label = gtk_label_new("Right notebook:");
    gtk_widget_set_halign(right_color_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(settings_container), right_color_label, FALSE, FALSE, 0);

    right_color_btn = gtk_color_button_new_with_rgba(&(GdkRGBA){1.0, 1.0, 1.0, 1.0});
    gtk_box_pack_start(GTK_BOX(settings_container), right_color_btn, FALSE, FALSE, 0);
    g_signal_connect(right_color_btn, "color-set", G_CALLBACK(on_right_color_set), NULL);

    /* Keep dark theme toggle */
    keep_dark_check = gtk_check_button_new_with_label("Keep dark theme");
    gtk_box_pack_start(GTK_BOX(settings_container), keep_dark_check, FALSE, FALSE, 0);
    g_signal_connect(keep_dark_check, "toggled", G_CALLBACK(on_keep_dark_toggled), NULL);

    gtk_box_pack_start(GTK_BOX(sidebar), settings_container, TRUE, TRUE, 0);
    gtk_widget_hide(settings_container);

    /* File info container */
    file_info_container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(file_info_container), 8);
    g_object_ref(file_info_container);

    GtkWidget *file_info_title = gtk_label_new("File Information");
    gtk_widget_set_halign(file_info_title, GTK_ALIGN_START);
    PangoAttrList *fi_attr = pango_attr_list_new();
    pango_attr_list_insert(fi_attr, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    pango_attr_list_insert(fi_attr, pango_attr_scale_new(PANGO_SCALE_LARGE));
    gtk_label_set_attributes(GTK_LABEL(file_info_title), fi_attr);
    pango_attr_list_unref(fi_attr);
    gtk_box_pack_start(GTK_BOX(file_info_container), file_info_title, FALSE, FALSE, 0);

    file_info_name_label = gtk_label_new("Name: (no file)");
    gtk_widget_set_halign(file_info_name_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(file_info_container), file_info_name_label, FALSE, FALSE, 0);

    file_info_path_label = gtk_label_new("Path: (none)");
    gtk_widget_set_halign(file_info_path_label, GTK_ALIGN_FILL);
    gtk_label_set_xalign(GTK_LABEL(file_info_path_label), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(file_info_path_label), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(file_info_path_label), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(file_info_path_label), 30);
    gtk_box_pack_start(GTK_BOX(file_info_container), file_info_path_label, FALSE, FALSE, 0);

    file_info_size_label = gtk_label_new("Size: (none)");
    gtk_widget_set_halign(file_info_size_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(file_info_container), file_info_size_label, FALSE, FALSE, 0);

    file_info_pages_label = gtk_label_new("Pages: (none)");
    gtk_widget_set_halign(file_info_pages_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(file_info_container), file_info_pages_label, FALSE, FALSE, 0);

    /* Separator before search */
    GtkWidget *search_sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(file_info_container), search_sep, FALSE, FALSE, 5);

    /* Search title */
    GtkWidget *search_title = gtk_label_new("Search in Document");
    gtk_widget_set_halign(search_title, GTK_ALIGN_START);
    PangoAttrList *sa = pango_attr_list_new();
    pango_attr_list_insert(sa, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    pango_attr_list_insert(sa, pango_attr_scale_new(PANGO_SCALE_LARGE));
    gtk_label_set_attributes(GTK_LABEL(search_title), sa);
    pango_attr_list_unref(sa);
    gtk_box_pack_start(GTK_BOX(file_info_container), search_title, FALSE, FALSE, 0);

    /* Search entry + button row */
    GtkWidget *search_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    file_info_search_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(file_info_search_entry), "Search text...");
    gtk_box_pack_start(GTK_BOX(search_hbox), file_info_search_entry, TRUE, TRUE, 0);
    g_signal_connect(file_info_search_entry, "activate", G_CALLBACK(on_file_info_search_activated), NULL);

    file_info_search_btn = gtk_button_new_with_label("Search");
    gtk_box_pack_start(GTK_BOX(search_hbox), file_info_search_btn, FALSE, FALSE, 0);
    g_signal_connect(file_info_search_btn, "clicked", G_CALLBACK(on_file_info_search_clicked), NULL);

    gtk_box_pack_start(GTK_BOX(file_info_container), search_hbox, FALSE, FALSE, 0);

    /* Results tree view */
    file_info_search_results_store = gtk_list_store_new(SEARCH_COL_NCOL, G_TYPE_INT, G_TYPE_INT, G_TYPE_STRING);
    file_info_search_results_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(file_info_search_results_store));
    g_object_unref(file_info_search_results_store);

    GtkCellRenderer *sr = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *sc = gtk_tree_view_column_new_with_attributes("Page", sr, "text", SEARCH_COL_LABEL, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(file_info_search_results_view), sc);

    gtk_tree_view_set_activate_on_single_click(GTK_TREE_VIEW(file_info_search_results_view), TRUE);
    g_signal_connect(file_info_search_results_view, "row-activated", G_CALLBACK(on_file_info_search_row_activated), NULL);

    GtkWidget *search_scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(search_scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(search_scrolled), 80);
    gtk_container_add(GTK_CONTAINER(search_scrolled), file_info_search_results_view);
    gtk_box_pack_start(GTK_BOX(file_info_container), search_scrolled, TRUE, TRUE, 0);

    /* No results label */
    file_info_search_no_results = gtk_label_new("No results found");
    gtk_widget_set_halign(file_info_search_no_results, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(file_info_container), file_info_search_no_results, FALSE, FALSE, 0);
    gtk_widget_hide(file_info_search_no_results);

    /* Overflow label */
    file_info_search_overflow_label = gtk_label_new(NULL);
    gtk_widget_set_halign(file_info_search_overflow_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(file_info_container), file_info_search_overflow_label, FALSE, FALSE, 0);
    gtk_widget_hide(file_info_search_overflow_label);

    gtk_widget_set_size_request(file_info_container, 300, -1);
    gtk_box_pack_start(GTK_BOX(sidebar), file_info_container, TRUE, TRUE, 0);
    gtk_widget_hide(file_info_container);

    /* Content area on the right*/
    content_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(main_hbox), content_vbox, TRUE, TRUE, 0);

    /* Buttons*/
    /* Sessions button */
    GtkWidget *sessions_icon = create_toolbar_icon("sessions");
    sessions_btn = gtk_toggle_button_new();
    gtk_button_set_image(GTK_BUTTON(sessions_btn), sessions_icon);
    g_object_set_data_full(G_OBJECT(sessions_btn), "icon-name", g_strdup("sessions"), g_free);
    gtk_widget_set_tooltip_text(sessions_btn, "Sessions");
    atk_object_set_name(gtk_widget_get_accessible(sessions_btn), "Sessions");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(sessions_btn), FALSE);
    g_signal_connect(sessions_btn, "toggled", G_CALLBACK(on_sessions_toggled), NULL);
    gtk_box_pack_start(GTK_BOX(toolbar), sessions_btn, FALSE, FALSE, 1);

    /* Table of contents button */
    GtkWidget *toc_icon = create_toolbar_icon("toc");
    toc_btn = gtk_toggle_button_new();
    gtk_button_set_image(GTK_BUTTON(toc_btn), toc_icon);
    g_object_set_data_full(G_OBJECT(toc_btn), "icon-name", g_strdup("toc"), g_free);
    gtk_widget_set_tooltip_text(toc_btn, "Table of contents");
    atk_object_set_name(gtk_widget_get_accessible(toc_btn), "Table of contents");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toc_btn), FALSE);
    g_signal_connect(toc_btn, "toggled", G_CALLBACK(on_toc_toggled), NULL);
    gtk_box_pack_start(GTK_BOX(toolbar), toc_btn, FALSE, FALSE, 1);

    /* Settings button */
    GtkWidget *settings_icon = create_toolbar_icon("settings");
    settings_btn = gtk_toggle_button_new();
    gtk_button_set_image(GTK_BUTTON(settings_btn), settings_icon);
    g_object_set_data_full(G_OBJECT(settings_btn), "icon-name", g_strdup("settings"), g_free);
    gtk_widget_set_tooltip_text(settings_btn, "Settings");
    atk_object_set_name(gtk_widget_get_accessible(settings_btn), "Settings");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(settings_btn), FALSE);
    g_signal_connect(settings_btn, "toggled", G_CALLBACK(on_settings_toggled), NULL);
    gtk_box_pack_start(GTK_BOX(toolbar), settings_btn, FALSE, FALSE, 1);

    /* File information button */
    GtkWidget *file_info_icon = create_toolbar_icon("file");
    file_info_btn = gtk_toggle_button_new();
    gtk_button_set_image(GTK_BUTTON(file_info_btn), file_info_icon);
    g_object_set_data_full(G_OBJECT(file_info_btn), "icon-name", g_strdup("file"), g_free);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(file_info_btn), FALSE);
    gtk_widget_set_tooltip_text(file_info_btn, "File information");
    atk_object_set_name(gtk_widget_get_accessible(file_info_btn), "File information");
    g_signal_connect(file_info_btn, "toggled", G_CALLBACK(on_left_file_info_toggled), NULL);
    gtk_box_pack_start(GTK_BOX(toolbar), file_info_btn, FALSE, FALSE, 1);

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
    page_entry = gtk_entry_new();
    gtk_widget_set_size_request(page_entry, 42, -1);
    gtk_entry_set_max_length(GTK_ENTRY(page_entry), 4);
    gtk_entry_set_width_chars(GTK_ENTRY(page_entry), 2);
    gtk_entry_set_max_width_chars(GTK_ENTRY(page_entry), 3);
    gtk_entry_set_input_purpose(GTK_ENTRY(page_entry), GTK_INPUT_PURPOSE_DIGITS);
    gtk_widget_set_tooltip_text(page_entry, "Current page (press Enter to jump)");
    atk_object_set_name(gtk_widget_get_accessible(page_entry), "Current page");

    page_total_label = gtk_label_new("/ 0");
    gtk_label_set_width_chars(GTK_LABEL(page_total_label), 4);
    gtk_label_set_xalign(GTK_LABEL(page_total_label), 0.0f);

    /* Allow only digits to be entered */
    g_signal_connect(page_entry, "insert-text", G_CALLBACK(on_page_entry_insert_text), NULL);

    /* Enter in spin jumps to page */
    g_signal_connect(page_entry, "activate", G_CALLBACK(on_page_entry_activate), NULL);

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
    left_column_btn = gtk_radio_button_new(NULL);
    gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(left_column_btn), FALSE);
    gtk_button_set_image(GTK_BUTTON(left_column_btn), column_view_icon);
    g_object_set_data_full(G_OBJECT(left_column_btn), "icon-name", g_strdup("column"), g_free);
    gtk_widget_set_tooltip_text(left_column_btn, "Page column");
    atk_object_set_name(gtk_widget_get_accessible(left_column_btn), "Page column");
    g_object_set_data(G_OBJECT(left_column_btn), "layout-id", GINT_TO_POINTER(0 + 1));
    g_signal_connect(left_column_btn, "toggled", G_CALLBACK(on_layout_left_toggled), NULL);
    gtk_box_pack_start(GTK_BOX(middle_box), left_column_btn, FALSE, FALSE, 1);

    /* Double column view button*/
    GtkWidget *double_column_view_icon = create_toolbar_icon("double-column");
    left_double_column_btn = gtk_radio_button_new_from_widget(GTK_RADIO_BUTTON(left_column_btn));
    gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(left_double_column_btn), FALSE);
    gtk_button_set_image(GTK_BUTTON(left_double_column_btn), double_column_view_icon);
    g_object_set_data_full(G_OBJECT(left_double_column_btn), "icon-name", g_strdup("double-column"), g_free);
    gtk_widget_set_tooltip_text(left_double_column_btn, "Page double column");
    atk_object_set_name(gtk_widget_get_accessible(left_double_column_btn), "Page double column");
    g_object_set_data(G_OBJECT(left_double_column_btn), "layout-id", GINT_TO_POINTER(1 + 1));
    g_signal_connect(left_double_column_btn, "toggled", G_CALLBACK(on_layout_left_toggled), NULL);
    gtk_box_pack_start(GTK_BOX(middle_box), left_double_column_btn, FALSE, FALSE, 1);

    /* Row view button*/
    GtkWidget *row_view_icon = create_toolbar_icon("row");
    left_row_btn = gtk_radio_button_new_from_widget(GTK_RADIO_BUTTON(left_column_btn));
    gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(left_row_btn), FALSE);
    gtk_button_set_image(GTK_BUTTON(left_row_btn), row_view_icon);
    g_object_set_data_full(G_OBJECT(left_row_btn), "icon-name", g_strdup("row"), g_free);
    gtk_widget_set_tooltip_text(left_row_btn, "Page row");
    atk_object_set_name(gtk_widget_get_accessible(left_row_btn), "Page row");
    g_object_set_data(G_OBJECT(left_row_btn), "layout-id", GINT_TO_POINTER(2 + 1));
    g_signal_connect(left_row_btn, "toggled", G_CALLBACK(on_layout_left_toggled), NULL);
    gtk_box_pack_start(GTK_BOX(middle_box), left_row_btn, FALSE, FALSE, 1);

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
    g_signal_connect(maximize_btn, "clicked", G_CALLBACK(on_maximize_clicked), window);
    gtk_box_pack_end(GTK_BOX(toolbar), maximize_btn, FALSE, FALSE, 1);

    /* Minimize button*/
    GtkWidget *minimize_icon = create_toolbar_icon("minimize-2");
    GtkWidget *minimize_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(minimize_btn), minimize_icon);
    g_object_set_data_full(G_OBJECT(minimize_btn), "icon-name", g_strdup("minimize-2"), g_free);
    gtk_widget_set_tooltip_text(minimize_btn, "Minimize");
    atk_object_set_name(gtk_widget_get_accessible(minimize_btn), "Minimize");
    g_signal_connect(minimize_btn, "clicked", G_CALLBACK(on_minimize_clicked), window);
    gtk_box_pack_end(GTK_BOX(toolbar), minimize_btn, FALSE, FALSE, 1);

    /* MAIN WINDOW PANED */
    /* Create a horizontal paned splitter containing two notebooks */
    paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);

    /* Wrap paned in an overlay for floating page navigation widget */
    GtkWidget *overlay = gtk_overlay_new();
    gtk_box_pack_start(GTK_BOX(content_vbox), overlay, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(overlay), paned);

    /* Floating page navigation overlay (lower-left corner) */
    page_nav_overlay = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_name(page_nav_overlay, "page-nav-overlay");
    gtk_widget_set_halign(page_nav_overlay, GTK_ALIGN_START);
    gtk_widget_set_valign(page_nav_overlay, GTK_ALIGN_END);
    gtk_widget_set_margin_start(page_nav_overlay, 8);
    gtk_widget_set_margin_bottom(page_nav_overlay, 8);
    gtk_box_pack_start(GTK_BOX(page_nav_overlay), page_entry, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page_nav_overlay), page_total_label, FALSE, FALSE, 0);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), page_nav_overlay);

    /* Left notebook (primary) */
    left_notebook = gtk_notebook_new();
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(left_notebook), TRUE);
    gtk_widget_set_size_request(left_notebook, 70, -1);
    gtk_paned_pack1(GTK_PANED(paned), left_notebook, TRUE, TRUE);
    atk_object_set_name(gtk_widget_get_accessible(left_notebook), "Left Notebook");
    g_signal_connect(left_notebook, "switch-page", G_CALLBACK(on_left_notebook_switch_page), NULL);
    g_signal_connect(left_notebook, "page-reordered", G_CALLBACK(on_notebook_page_reordered), NULL);

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
    gtk_widget_set_size_request(right_pane, 36, -1);
    gtk_paned_pack2(GTK_PANED(paned), right_pane, TRUE, TRUE);
    gtk_paned_set_position(GTK_PANED(paned), 500);

    /* Right notebook wrapper with overlay for floating page nav */
    GtkWidget *right_nb_overlay = gtk_overlay_new();
    gtk_box_pack_start(GTK_BOX(right_pane), right_nb_overlay, TRUE, TRUE, 0);

    /* Right notebook (secondary) */
    right_notebook = gtk_notebook_new();
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(right_notebook), TRUE);
    gtk_container_add(GTK_CONTAINER(right_nb_overlay), right_notebook);
    g_signal_connect(right_notebook, "switch-page", G_CALLBACK(on_right_notebook_switch_page), NULL);
    g_signal_connect(right_notebook, "page-reordered", G_CALLBACK(on_notebook_page_reordered), NULL);

    /* Right page navigation entry + label */
    right_page_entry = gtk_entry_new();
    gtk_widget_set_size_request(right_page_entry, 42, -1);
    gtk_entry_set_max_length(GTK_ENTRY(right_page_entry), 4);
    gtk_entry_set_width_chars(GTK_ENTRY(right_page_entry), 2);
    gtk_entry_set_max_width_chars(GTK_ENTRY(right_page_entry), 3);
    gtk_entry_set_input_purpose(GTK_ENTRY(right_page_entry), GTK_INPUT_PURPOSE_DIGITS);
    gtk_widget_set_tooltip_text(right_page_entry, "Current page (press Enter to jump)");
    atk_object_set_name(gtk_widget_get_accessible(right_page_entry), "Current page");

    right_page_total_label = gtk_label_new("/ 0");
    gtk_label_set_width_chars(GTK_LABEL(right_page_total_label), 4);
    gtk_label_set_xalign(GTK_LABEL(right_page_total_label), 0.0f);

    g_signal_connect(right_page_entry, "insert-text", G_CALLBACK(on_page_entry_insert_text), NULL);
    g_signal_connect(right_page_entry, "activate", G_CALLBACK(on_right_page_entry_activate), NULL);

    /* Floating page navigation overlay (lower-right corner of right notebook) */
    right_page_nav_overlay = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_name(right_page_nav_overlay, "right-page-nav-overlay");
    gtk_widget_set_halign(right_page_nav_overlay, GTK_ALIGN_END);
    gtk_widget_set_valign(right_page_nav_overlay, GTK_ALIGN_END);
    gtk_widget_set_margin_end(right_page_nav_overlay, 8);
    gtk_widget_set_margin_bottom(right_page_nav_overlay, 8);
    gtk_box_pack_start(GTK_BOX(right_page_nav_overlay), right_page_entry, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(right_page_nav_overlay), right_page_total_label, FALSE, FALSE, 0);
    gtk_overlay_add_overlay(GTK_OVERLAY(right_nb_overlay), right_page_nav_overlay);
    /* Initially hidden until there are documents */
    gtk_widget_hide(right_page_nav_overlay);

    // Note: restore_open_tabs_for_session already handles both notebooks
    current_selected_session = g_strdup(initial_session);
    update_window_title_for_session(current_selected_session);

    /* Right pane toolbar (vertical) */
    GtkWidget *right_toolbar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(right_toolbar), "Toolbar");
    gtk_box_pack_start(GTK_BOX(right_pane), right_toolbar, FALSE, FALSE, 0);

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
    right_column_btn = gtk_radio_button_new(NULL);
    gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(right_column_btn), FALSE);
    gtk_button_set_image(GTK_BUTTON(right_column_btn), right_column_icon);
    g_object_set_data_full(G_OBJECT(right_column_btn), "icon-name", g_strdup("column"), g_free);
    gtk_widget_set_tooltip_text(right_column_btn, "Page column");
    atk_object_set_name(gtk_widget_get_accessible(right_column_btn), "Page column");
    g_object_set_data(G_OBJECT(right_column_btn), "layout-id", GINT_TO_POINTER(0 + 1));
    g_signal_connect(right_column_btn, "toggled", G_CALLBACK(on_layout_right_toggled), NULL);
    gtk_box_pack_start(GTK_BOX(right_middle_box), right_column_btn, FALSE, FALSE, 1);

    /* Right toolbar - Page double column */
    GtkWidget *right_double_column_icon = create_toolbar_icon("double-column");
    right_double_column_btn = gtk_radio_button_new_from_widget(GTK_RADIO_BUTTON(right_column_btn));
    gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(right_double_column_btn), FALSE);
    gtk_button_set_image(GTK_BUTTON(right_double_column_btn), right_double_column_icon);
    g_object_set_data_full(G_OBJECT(right_double_column_btn), "icon-name", g_strdup("double-column"), g_free);
    gtk_widget_set_tooltip_text(right_double_column_btn, "Page double column");
    atk_object_set_name(gtk_widget_get_accessible(right_double_column_btn), "Page double column");
    g_object_set_data(G_OBJECT(right_double_column_btn), "layout-id", GINT_TO_POINTER(1 + 1));
    g_signal_connect(right_double_column_btn, "toggled", G_CALLBACK(on_layout_right_toggled), NULL);
    gtk_box_pack_start(GTK_BOX(right_middle_box), right_double_column_btn, FALSE, FALSE, 1);

    /* Right toolbar - Page row */
    GtkWidget *right_row_icon = create_toolbar_icon("row");
    right_row_btn = gtk_radio_button_new_from_widget(GTK_RADIO_BUTTON(right_column_btn));
    gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(right_row_btn), FALSE);
    gtk_button_set_image(GTK_BUTTON(right_row_btn), right_row_icon);
    g_object_set_data_full(G_OBJECT(right_row_btn), "icon-name", g_strdup("row"), g_free);
    gtk_widget_set_tooltip_text(right_row_btn, "Page row");
    atk_object_set_name(gtk_widget_get_accessible(right_row_btn), "Page row");
    g_object_set_data(G_OBJECT(right_row_btn), "layout-id", GINT_TO_POINTER(2 + 1));
    g_signal_connect(right_row_btn, "toggled", G_CALLBACK(on_layout_right_toggled), NULL);
    gtk_box_pack_start(GTK_BOX(right_middle_box), right_row_btn, FALSE, FALSE, 1);

    /* Right toolbar separator */
    GtkWidget *right_sep_d = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(right_middle_box), right_sep_d, FALSE, FALSE, 5);

    return window;
}

void hide_right_pane(void) {
    if (right_pane) {
        gtk_widget_hide(GTK_WIDGET(right_pane));
    }
}
