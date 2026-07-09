#ifndef PDF_H
#define PDF_H

#include <cairo.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque types */
typedef struct PdfrDoc   PdfrDoc;
typedef struct PdfrPage  PdfrPage;

/* Rectangle for search hits and link areas (PDF user-space coordinates) */
typedef struct {
    double x1, y1, x2, y2;
} PdfrRect;

/* --- Link types --- */

typedef enum {
    PDF_LINK_GOTO,      /* internal page link */
    PDF_LINK_NAMED,     /* named destination (resolve with pdfr_resolve_named_dest) */
    PDF_LINK_URI,       /* external URI */
    PDF_LINK_LAUNCH,    /* launch / file */
    PDF_LINK_UNKNOWN
} PdfrLinkType;

typedef struct PdfrLink {
    PdfrRect        rect;
    PdfrLinkType    type;
    int            page_num;     /* for PDF_LINK_GOTO (1-based) */
    double         x, y;         /* target position within page (PDF points), -1 if unspecified */
    char          *named_dest;   /* for PDF_LINK_NAMED */
    char          *uri;          /* for PDF_LINK_URI */
    struct PdfrLink *next;
} PdfrLink;

/* --- Outline (Table of Contents) --- */

typedef struct PdfrOutline {
    char              *title;
    int                page;      /* 1-based page number, 0 if unresolved */
    struct PdfrOutline *down;     /* first child */
    struct PdfrOutline *next;     /* next sibling */
} PdfrOutline;

/* --- Document lifecycle --- */

PdfrDoc *pdfr_open(const char *path, char **error);
int     pdfr_count_pages(PdfrDoc *doc);
void    pdfr_close(PdfrDoc *doc);
void    pdfr_shutdown(void);

/* --- Page lifecycle --- */

PdfrPage *pdfr_load_page(PdfrDoc *doc, int page_idx);
void     pdfr_page_size(PdfrDoc *doc, PdfrPage *page, double *w, double *h, double *x0, double *y0);
void     pdfr_free_page(PdfrDoc *doc, PdfrPage *page);

/* --- Rendering --- */

/* Render a page into a cairo context at the given PPI scale and device scale.
   The caller should set up the cairo context with appropriate transforms
   (cairo_scale for the zoom, cairo_surface_set_device_scale for HiDPI)
   before calling. */
void pdfr_render(PdfrDoc *doc, PdfrPage *page, cairo_t *cr);

/* --- Links --- */

PdfrLink *pdfr_load_links(PdfrDoc *doc, PdfrPage *page);
void     pdfr_free_links(PdfrDoc *doc, PdfrLink *links);
int      pdfr_resolve_named_dest(PdfrDoc *doc, const char *name, double *out_x, double *out_y);

/* --- Search --- */

/* Search for 'text' on a page by page index (0-based).
   Fills at most max_matches rectangles and returns the number of matches
   found (may exceed max_matches). */
int pdfr_search_page(PdfrDoc *doc, int page_idx,
                    const char *text, PdfrRect *matches, int max_matches);

/* --- Outline (TOC) --- */

PdfrOutline *pdfr_load_outline(PdfrDoc *doc);
void        pdfr_free_outline(PdfrDoc *doc, PdfrOutline *outline);

#ifdef __cplusplus
}
#endif

#endif /* PDF_H */
