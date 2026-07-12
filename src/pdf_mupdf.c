#include "pdf.h"
#include "log.h"
#include <mupdf/fitz.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <cairo.h>
#include <stdio.h>

#define MAX_STORE_BYTES (64 * 1024 * 1024)

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

/* ---- Internal: MuPDF diagnostics --- */

static void log_warning(void *user, const char *message) {
    (void)user;
    LOG_WARN("MuPDF: %s", message);
}

static void log_error(void *user, const char *message) {
    (void)user;
    LOG_ERROR("MuPDF: %s", message);
}

/* ---- Global MuPDF context ---- */

static fz_context *gctx = NULL;

static fz_context *pdfr_get_context(void) {
    if (!gctx) {
        gctx = fz_new_context(NULL, NULL, MAX_STORE_BYTES);
        fz_set_warning_callback(gctx, log_warning, NULL);
        fz_set_error_callback(gctx, log_error, NULL);
        fz_register_document_handlers(gctx);
    }
    return gctx;
}

void pdfr_shutdown(void) {
    if (gctx) {
        fz_drop_context(gctx);
        gctx = NULL;
    }
}

void pdfr_purge_store(void) {
    if (!gctx) return;
    fz_empty_store(gctx);
}

/* ---- Document lifecycle ---- */

struct PdfrDoc {
    fz_document *doc;
};

PdfrDoc *pdfr_open(const char *path, char **error) {
    fz_context *ctx = pdfr_get_context();

    PdfrDoc *pd = calloc(1, sizeof(PdfrDoc));

    fz_try(ctx) {
        pd->doc = fz_open_document(ctx, path);
    }
    fz_catch(ctx) {
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
    fz_context *ctx = pdfr_get_context();
    int n = 0;
    fz_var(n);
    fz_try(ctx) {
        n = fz_count_pages(ctx, doc->doc);
    }
    fz_catch(ctx) {
        LOG_ERROR("MuPDF: %s", fz_caught_message(ctx));
    }
    return n;
}

void pdfr_close(PdfrDoc *doc) {
    if (!doc) return;
    fz_context *ctx = pdfr_get_context();
    fz_try(ctx) {
        fz_drop_document(ctx, doc->doc);
    }
    fz_catch(ctx) {
        LOG_ERROR("MuPDF: %s", fz_caught_message(ctx));
    }
    free(doc);
}

/* ---- Page lifecycle ---- */

struct PdfrPage {
    fz_page *page;
};

PdfrPage *pdfr_load_page(PdfrDoc *doc, int page_idx) {
    fz_context *ctx = pdfr_get_context();
    PdfrPage *pd = NULL;
    fz_var(pd);
    fz_try(ctx) {
        fz_page *page = fz_load_page(ctx, doc->doc, page_idx);
        if (page) {
            pd = calloc(1, sizeof(PdfrPage));
            pd->page = page;
        }
    }
    fz_catch(ctx) {
        LOG_ERROR("MuPDF: %s", fz_caught_message(ctx));
        free(pd);
        pd = NULL;
    }
    return pd;
}

void pdfr_page_size(PdfrDoc *doc, PdfrPage *page, double *w, double *h, double *x0, double *y0) {
    (void)doc;
    fz_context *ctx = pdfr_get_context();
    fz_try(ctx) {
        fz_rect r = fz_bound_page(ctx, page->page);
        *w = r.x1 - r.x0;
        *h = r.y1 - r.y0;
        if (x0) *x0 = r.x0;
        if (y0) *y0 = r.y0;
    }
    fz_catch(ctx) {
        LOG_ERROR("MuPDF: %s", fz_caught_message(ctx));
        *w = *h = 0;
        if (x0) *x0 = 0;
        if (y0) *y0 = 0;
    }
}

void pdfr_free_page(PdfrDoc *doc, PdfrPage *page) {
    (void)doc;
    if (!page) return;
    fz_context *ctx = pdfr_get_context();
    fz_try(ctx) {
        fz_drop_page(ctx, page->page);
    }
    fz_catch(ctx) {
        LOG_ERROR("MuPDF: %s", fz_caught_message(ctx));
    }
    free(page);
}

/* ---- Rendering ---- */

void pdfr_render(PdfrDoc *doc, PdfrPage *page, cairo_t *cr) {
    (void)doc;
    fz_context *ctx = pdfr_get_context();

    cairo_surface_t *surface = cairo_get_target(cr);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        LOG_WARN("Cairo surface in invalid state: %s",
                 cairo_status_to_string(cairo_surface_status(surface)));
        return;
    }
    if (cairo_image_surface_get_format(surface) != CAIRO_FORMAT_ARGB32) {
        LOG_WARN("Cairo surface format is not ARGB32");
        return;
    }

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
        LOG_ERROR("MuPDF render failed: %s", fz_caught_message(ctx));
    }
}

/* ---- Links ---- */

PdfrLink *pdfr_load_links(PdfrDoc *doc, PdfrPage *page) {
    (void)doc;
    fz_context *ctx = pdfr_get_context();
    PdfrLink *head = NULL, *tail = NULL;

    fz_try(ctx) {
        fz_link *links = fz_load_links(ctx, page->page);
        for (fz_link *l = links; l; l = l->next) {
            PdfrLink *link = calloc(1, sizeof(PdfrLink));
            link->rect = fz_rect_to_pdf(l->rect);
            link->x = -1;
            link->y = -1;
            link->next = NULL;

            if (l->uri) {
                if (strncmp(l->uri, "#page=", 6) == 0) {
                    link->type = PDF_LINK_GOTO;
                    /* Use fz_resolve_link_dest for authoritative page number
                       (handles page labels, indirect page refs, etc.).
                       Fall back to raw atoi if resolution fails. */
                    fz_try(ctx) {
                        fz_link_dest ld = fz_resolve_link_dest(ctx, doc->doc, l->uri);
                        int abs_page = fz_page_number_from_location(ctx, doc->doc, ld.loc);
                        link->page_num = abs_page + 1; /* 0-based → 1-based */
                        if (ld.type == FZ_LINK_DEST_XYZ && isfinite(ld.zoom)) {
                            link->x = ld.x;
                            link->y = ld.y;
                        }
                    }
                    fz_catch(ctx) {
                        LOG_WARN("MuPDF: %s", fz_caught_message(ctx));
                        link->page_num = atoi(l->uri + 6);
                    }
                } else if (strncmp(l->uri, "#nameddest=", 11) == 0) {
                    link->type = PDF_LINK_NAMED;
                    link->named_dest = strdup(l->uri + 11);
                    /* Resolve page number at load time (consistent with GOTO links) */
                    fz_try(ctx) {
                        fz_link_dest ld = fz_resolve_link_dest(ctx, doc->doc, l->uri);
                        int abs_page = fz_page_number_from_location(ctx, doc->doc, ld.loc);
                        if (abs_page >= 0)
                            link->page_num = abs_page + 1; /* 0-based → 1-based */
                        if (ld.type == FZ_LINK_DEST_XYZ && isfinite(ld.zoom)) {
                            link->x = ld.x;
                            link->y = ld.y;
                        }
                    }
                    fz_catch(ctx) {
                        LOG_WARN("MuPDF: %s", fz_caught_message(ctx));
                    }
                } else if (strchr(l->uri, ':') != NULL) {
                    link->type = PDF_LINK_URI;
                    link->uri = strdup(l->uri);
                } else {
                    /* Internal link with unknown format; try as named dest */
                    link->type = PDF_LINK_NAMED;
                    const char *s = l->uri;
                    if (*s == '#') s++;
                    link->named_dest = strdup(s);
                    fz_try(ctx) {
                        fz_link_dest ld = fz_resolve_link_dest(ctx, doc->doc, l->uri);
                        int abs_page = fz_page_number_from_location(ctx, doc->doc, ld.loc);
                        if (abs_page >= 0)
                            link->page_num = abs_page + 1;
                        if (ld.type == FZ_LINK_DEST_XYZ && isfinite(ld.zoom)) {
                            link->x = ld.x;
                            link->y = ld.y;
                        }
                    }
                    fz_catch(ctx) {
                        LOG_WARN("MuPDF: %s", fz_caught_message(ctx));
                    }
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
        LOG_ERROR("MuPDF: %s", fz_caught_message(ctx));
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

int pdfr_resolve_named_dest(PdfrDoc *doc, const char *name, double *out_x, double *out_y) {
    fz_context *ctx = pdfr_get_context();
    /* Build a URI of the form #nameddest=... and resolve it */
    char uri[1024];
    int n = snprintf(uri, sizeof(uri), "#nameddest=%s", name);
    if (n >= (int)sizeof(uri)) return 0;

    int page = 0;
    fz_var(page);
    if (out_x) *out_x = -1;
    if (out_y) *out_y = -1;
    fz_try(ctx) {
        float xp, yp;
        fz_location loc = fz_resolve_link(ctx, doc->doc, uri, &xp, &yp);
        int abs_page = fz_page_number_from_location(ctx, doc->doc, loc);
        page = abs_page + 1; /* Convert to 1-based */
        if (out_x) *out_x = xp;
        if (out_y) *out_y = yp;
    }
    fz_catch(ctx) {
        LOG_WARN("MuPDF: %s", fz_caught_message(ctx));
        page = 0;
    }
    return page;
}

/* ---- Search ---- */

int pdfr_search_page(PdfrDoc *doc, int page_idx,
                    const char *text, PdfrRect *matches, int max_matches) {
    if (!doc || !doc->doc || !text || max_matches <= 0) return 0;
    fz_context *ctx = pdfr_get_context();
    if (!ctx) return 0;
    fz_quad quads[100];
    int hit_mark = 0;
    int n = 0;
    fz_var(n);
    fz_try(ctx) {
        n = fz_search_page_number(ctx, doc->doc, page_idx, text, &hit_mark, quads, max_matches);
    }
    fz_catch(ctx) {
        LOG_WARN("MuPDF: %s", fz_caught_message(ctx));
    }
    if (n > max_matches) n = max_matches;
    for (int i = 0; i < n; i++)
        matches[i] = fz_quad_to_pdfr_rect(quads[i]);
    return n;
}

/* ---- Outline (TOC) ---- */

static PdfrOutline *build_outline_tree(fz_context *ctx, fz_document *doc, fz_outline *entry) {
    if (!entry) return NULL;

    PdfrOutline *head = NULL, *tail = NULL;

    for (fz_outline *cur = entry; cur; cur = cur->next) {
        PdfrOutline *node = calloc(1, sizeof(PdfrOutline));
        node->title = cur->title ? strdup(cur->title) : strdup("");

        if (cur->page.page >= 0) {
            /* Convert fz_location (chapter + page-within-chapter) to
               absolute 0-based page number, then store as 1-based. */
            int abs_page = fz_page_number_from_location(ctx, doc, cur->page);
            if (abs_page >= 0) {
                node->page = abs_page + 1; /* 0-based → 1-based */
            } else {
                node->page = 0;
            }
        } else {
            node->page = 0;
        }

        node->down = build_outline_tree(ctx, doc, cur->down);
        node->next = NULL;

        if (tail) { tail->next = node; tail = node; }
        else { head = tail = node; }
    }
    return head;
}

PdfrOutline *pdfr_load_outline(PdfrDoc *doc) {
    (void)doc;
    fz_context *ctx = pdfr_get_context();
    fz_outline *root = NULL;
    fz_var(root);

    fz_try(ctx) {
        root = fz_load_outline(ctx, doc->doc);
    }
    fz_catch(ctx) {
        LOG_ERROR("MuPDF: %s", fz_caught_message(ctx));
        return NULL;
    }

    if (!root) return NULL;

    PdfrOutline *outline = build_outline_tree(ctx, doc->doc, root);
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
