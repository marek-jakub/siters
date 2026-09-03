#ifndef SITERS_TAB_H
#define SITERS_TAB_H

#include <gtk/gtk.h>
#include "pdf.h"
#include "document_model.h"

/* Forward-declared opaque struct (defined in tab.h below) */
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

/* Per-tab state.  Every open document lives inside a TabData held by one of
   the two notebooks (left = primary, right = helper). */
struct TabDataStruct {
    PdfrDoc *doc;
    int n_pages;
    int cur_page;
    GtkWidget *drawing;
    double zoom;
    GdkRGBA page_color;
    /* per-document storage */
    char *current_file;
    GtkWidget *tab_label;           /* label widget in the tab header */
    GtkWidget *tab_label_box;       /* label container box */
    GtkWidget *tab_label_close_btn; /* close button */
    /* continuous view widgets */
    GtkWidget *scrolled;
    GtkWidget *pages_drawing;
    GtkWidget *h_scrollbar;
    int layout_mode;
    int built_layout_mode;
    double last_zoom;
    gboolean initial_scroll_pending;
    RestoreState *pending_restore;
    double scroll_offset;
    gboolean is_helper;
    guint zoom_scroll_source_id;
    guint h_scrollbar_timer_id;
    guint scroll_doc_debounce_id;
    int zoom_scroll_target_page;
    double zoom_scroll_fraction;
    gboolean dragging;
    double drag_start_x;
    double drag_start_y;
    double drag_scroll_x;
    double drag_scroll_y;
    double *cached_page_widths;
    double *cached_page_heights;
    double *cached_page_x0;
    double *cached_page_y0;
    double max_page_h;
    cairo_surface_t **page_cache;
    int total_cache_bytes;
    PdfrLink **page_links;
    int page_links_n;
    GdkCursorType last_cursor_type;
    gint64 last_cursor_check;
    /* Heuristic page-from-scroll cache */
    int last_known_page;
    double last_known_page_start;
    /* Search state */
    struct { int page; int n_matches; PdfrRect *rects; } *search_results;
    int search_results_n;
    int search_results_cap;
    char *search_text;
    gboolean search_cancelled;
    int search_page_idx;
    guint search_idle_id;
    guint load_idle_id;
    gboolean load_failed;
};

#endif /* SITERS_TAB_H */
