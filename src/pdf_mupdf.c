#include "pdf.h"
#include <mupdf/fitz.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <cairo.h>

#define MAX_STORE_BYTES (256 * 1024 * 1024)

/* ---- internal helpers ---- */

static PdfrRect fz_rect_to_pdf(fz_rect r) {
    PdfrRect pr;
    pr.x1 = r.x0;
    pr.y1 = r.y0;
    pr.x2 = r.x1;
    pr.y2 = r.y1;
    return pr;
}

static PdfrRect fz_quad_to_pdfr_rect(fz_quad q) {
    PdfrRect pr;
    pr.x1 = fminf(fminf(q.ul.x, q.ur.x), fminf(q.ll.x, q.lr.x));
    pr.y1 = fminf(fminf(q.ul.y, q.ur.y), fminf(q.ll.y, q.lr.y));
    pr.x2 = fmaxf(fmaxf(q.ul.x, q.ur.x), fmaxf(q.ll.x, q.lr.x));
    pr.y2 = fmaxf(fmaxf(q.ul.y, q.ur.y), fmaxf(q.ll.y, q.lr.y));
    return pr;
}

/* ---- Internal: suppress MuPDF diagnostics ---- */

static void silent_warning(void *user, const char *message) {
    (void)user; (void)message;
}

static void silent_error(void *user, const char *message) {
    (void)user; (void)message;
}

/* ---- Document lifecycle ---- */

struct PdfrDoc {
    fz_context *ctx;
    fz_document *doc;
};

PdfrDoc *pdfr_open(const char *path, char **error) {
    fz_context *ctx = fz_new_context(NULL, NULL, MAX_STORE_BYTES);
    if (!ctx) {
        if (error) *error = strdup("Failed to create MuPDF context");
        return NULL;
    }
    fz_set_warning_callback(ctx, silent_warning, NULL);
    fz_set_error_callback(ctx, silent_error, NULL);
    fz_register_document_handlers(ctx);

    PdfrDoc *pd = calloc(1, sizeof(PdfrDoc));
    pd->ctx = ctx;

    fz_try(ctx) {
        pd->doc = fz_open_document(ctx, path);
    }
    fz_catch(ctx) {
        fz_drop_context(ctx);
        free(pd);
        if (error) *error = strdup("Failed to open document with MuPDF");
        return NULL;
    }

    int n = pdfr_count_pages(pd);
    if (n < 1) {
        pdfr_close(pd);
        if (error) *error = strdup("Document has no accessible pages");
        return NULL;
    }

    return pd;
}

int pdfr_count_pages(PdfrDoc *doc) {
    int n = 0;
    fz_try(doc->ctx) {
        n = fz_count_pages(doc->ctx, doc->doc);
    }
    fz_catch(doc->ctx) {}
    return n;
}

void pdfr_close(PdfrDoc *doc) {
    if (!doc) return;
    fz_try(doc->ctx) {
        fz_drop_document(doc->ctx, doc->doc);
    }
    fz_catch(doc->ctx) { }
    fz_drop_context(doc->ctx);
    free(doc);
}

/* ---- Page lifecycle ---- */

struct PdfrPage {
    fz_page *page;
};

PdfrPage *pdfr_load_page(PdfrDoc *doc, int page_idx) {
    PdfrPage *pd = NULL;
    fz_try(doc->ctx) {
        fz_page *page = fz_load_page(doc->ctx, doc->doc, page_idx);
        if (page) {
            pd = calloc(1, sizeof(PdfrPage));
            pd->page = page;
        }
    }
    fz_catch(doc->ctx) {
        free(pd);
        pd = NULL;
    }
    return pd;
}

void pdfr_page_size(PdfrDoc *doc, PdfrPage *page, double *w, double *h) {
    fz_try(doc->ctx) {
        fz_rect r = fz_bound_page(doc->ctx, page->page);
        *w = r.x1 - r.x0;
        *h = r.y1 - r.y0;
    }
    fz_catch(doc->ctx) {
        *w = *h = 0;
    }
}

void pdfr_free_page(PdfrDoc *doc, PdfrPage *page) {
    if (!page) return;
    fz_try(doc->ctx) {
        fz_drop_page(doc->ctx, page->page);
    }
    fz_catch(doc->ctx) { }
    free(page);
}

/* ---- Rendering ---- */

void pdfr_render(PdfrDoc *doc, PdfrPage *page, cairo_t *cr) {
    fz_context *ctx = doc->ctx;

    cairo_surface_t *surface = cairo_get_target(cr);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS ||
        cairo_image_surface_get_format(surface) != CAIRO_FORMAT_ARGB32)
        return;

    int w = cairo_image_surface_get_width(surface);
    int h = cairo_image_surface_get_height(surface);
    int stride = cairo_image_surface_get_stride(surface);
    unsigned char *data = cairo_image_surface_get_data(surface);

    double dsx, dsy;
    cairo_surface_get_device_scale(surface, &dsx, &dsy);

    cairo_matrix_t cm;
    cairo_get_matrix(cr, &cm);
    double scale_x = cm.xx;
    double scale_y = cm.yy;

    double sx = scale_x * dsx;
    double sy = scale_y * dsy;
    fz_matrix ctm = fz_scale(sx, sy);

    fz_try(ctx) {
        fz_colorspace *cs = fz_device_bgr(ctx);
        fz_pixmap *pix = fz_new_pixmap_from_page(ctx, page->page, ctm, cs, 1);
        if (!pix) {
            fz_throw(ctx, FZ_ERROR_GENERIC, "pixmap is NULL");
        }

        int pw = pix->w;
        int ph = pix->h;
        int src_stride = pix->stride;
        int copy_w = pw < w ? pw : w;
        int copy_h = ph < h ? ph : h;
        int copy_bytes = copy_w * 4;

        for (int y = 0; y < copy_h; y++) {
            memcpy(data + y * stride,
                   pix->samples + y * src_stride,
                   copy_bytes);
        }

        fz_drop_pixmap(ctx, pix);
        cairo_surface_mark_dirty_rectangle(surface, 0, 0, copy_w, copy_h);
    }
    fz_catch(ctx) {
        /* fallback: nothing drawn */
    }
}

/* ---- Links ---- */

PdfrLink *pdfr_load_links(PdfrDoc *doc, PdfrPage *page) {
    fz_context *ctx = doc->ctx;
    PdfrLink *head = NULL, *tail = NULL;

    fz_try(ctx) {
        fz_link *links = fz_load_links(ctx, page->page);
        for (fz_link *l = links; l; l = l->next) {
            PdfrLink *link = calloc(1, sizeof(PdfrLink));
            link->rect = fz_rect_to_pdf(l->rect);
            link->next = NULL;

            if (l->uri) {
                if (strncmp(l->uri, "#page=", 6) == 0) {
                    link->type = PDF_LINK_GOTO;
                    link->page_num = atoi(l->uri + 6);
                } else if (strncmp(l->uri, "#nameddest=", 11) == 0) {
                    link->type = PDF_LINK_NAMED;
                    link->named_dest = strdup(l->uri + 11);
                } else if (strchr(l->uri, ':') != NULL) {
                    link->type = PDF_LINK_URI;
                    link->uri = strdup(l->uri);
                } else {
                    /* Internal link with unknown format; try as named dest */
                    link->type = PDF_LINK_NAMED;
                    link->named_dest = strdup(l->uri);
                }
            } else {
                link->type = PDF_LINK_UNKNOWN;
            }

            if (tail) { tail->next = link; tail = link; }
            else { head = tail = link; }
        }
        fz_drop_link(ctx, links);
    }
    fz_catch(ctx) {
    }
    return head;
}

void pdfr_free_links(PdfrDoc *doc, PdfrLink *links) {
    (void)doc;
    while (links) {
        PdfrLink *next = links->next;
        free(links->named_dest);
        free(links->uri);
        free(links);
        links = next;
    }
}

int pdfr_resolve_named_dest(PdfrDoc *doc, const char *name) {
    fz_context *ctx = doc->ctx;
    /* Build a URI of the form #nameddest=... and resolve it */
    char uri[1024];
    int n = snprintf(uri, sizeof(uri), "#nameddest=%s", name);
    if (n >= (int)sizeof(uri)) return 0;

    int page = 0;
    fz_try(ctx) {
        float xp, yp;
        fz_location loc = fz_resolve_link(ctx, doc->doc, uri, &xp, &yp);
        page = loc.page + 1; /* Convert to 1-based */
    }
    fz_catch(ctx) { }
    return page;
}

/* ---- Search ---- */

int pdfr_search_page(PdfrDoc *doc, PdfrPage *page,
                    const char *text, PdfrRect *matches, int max_matches) {
    fz_context *ctx = doc->ctx;
    int hit_mark = 0;
    fz_quad quads[100];
    int n = 0;

    fz_try(ctx) {
        n = fz_search_page(ctx, page->page, text, &hit_mark, quads, max_matches);
    }
    fz_catch(ctx) { }

    if (n > max_matches) n = max_matches;
    for (int i = 0; i < n; i++) {
        matches[i] = fz_quad_to_pdfr_rect(quads[i]);
    }
    return n;
}

/* ---- Outline (TOC) ---- */

static PdfrOutline *build_outline_tree(fz_context *ctx, fz_outline *entry) {
    if (!entry) return NULL;

    PdfrOutline *head = NULL, *tail = NULL;

    for (fz_outline *cur = entry; cur; cur = cur->next) {
        PdfrOutline *node = calloc(1, sizeof(PdfrOutline));
        node->title = cur->title ? strdup(cur->title) : strdup("");

        if (cur->page.page >= 0) {
            /* fz_outline uses 0-based pages in .page, but URI might contain
               the actual page reference. Use the URI if available for more
               precision. */
            if (cur->uri && strncmp(cur->uri, "#page=", 6) == 0) {
                node->page = atoi(cur->uri + 6); /* 1-based in URI */
            } else {
                node->page = cur->page.page + 1; /* convert to 1-based */
            }
        } else {
            node->page = 0;
        }

        node->down = build_outline_tree(ctx, cur->down);
        node->next = NULL;

        if (tail) { tail->next = node; tail = node; }
        else { head = tail = node; }
    }
    return head;
}

PdfrOutline *pdfr_load_outline(PdfrDoc *doc) {
    fz_context *ctx = doc->ctx;
    fz_outline *root = NULL;

    fz_try(ctx) {
        root = fz_load_outline(ctx, doc->doc);
    }
    fz_catch(ctx) {
        return NULL;
    }

    if (!root) return NULL;

    PdfrOutline *outline = build_outline_tree(ctx, root);
    fz_drop_outline(ctx, root);
    return outline;
}

void pdfr_free_outline(PdfrDoc *doc, PdfrOutline *outline) {
    (void)doc;
    while (outline) {
        PdfrOutline *next = outline->next;
        free(outline->title);
        if (outline->down) pdfr_free_outline(doc, outline->down);
        free(outline);
        outline = next;
    }
}
