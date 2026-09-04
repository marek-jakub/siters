#include <gtk/gtk.h>
#include "app.h"
#include "pdf.h"
#include "tab.h"
#include "search.h"
#include "log.h"
#include "links.h"

/* PDF link handling: lazy per-page link loading, hit-testing, and activation.
   Internal GOTO/NAMED navigation drives the current document; external URIs
   are opened only after a safe-scheme check (and user confirmation otherwise).
   The exported entry points are invoked by the drawing-area mouse callbacks. */


/* Ensure link mappings are loaded for a specific page (lazy, per-page).
   Only allocates the array if needed; only fetches the requested page if not yet loaded. */

void ensure_page_links_loaded(TabData *tab, int page) {
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

/* Check if there is a clickable link at the given page-relative coordinates.
   px, py are in rendering space (y-down, 0 at top of page).
   Link rects from MuPDF are already in page/device space (y-down, 0=top),
   so we compare directly. */

gboolean has_link_at(TabData *tab, int page, double px, double py) {
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

/* Schemes considered safe to open without user confirmation. These are
   standard network/mail links whose handlers present no local attack surface
   comparable to launching arbitrary applications or opening local files. */

gboolean is_safe_link_scheme(const char *uri, char **scheme_out) {
    *scheme_out = NULL;
    const char *colon = strchr(uri, ':');
    if (!colon || colon == uri) return FALSE;

    char *scheme = g_ascii_strdown(uri, colon - uri);
    if (!g_ascii_isalpha(scheme[0])) {
        g_free(scheme);
        return FALSE;
    }
    for (const char *p = scheme + 1; *p; p++) {
        if (!g_ascii_isalnum(*p) && *p != '+' && *p != '-' && *p != '.') {
            g_free(scheme);
            return FALSE;
        }
    }

    *scheme_out = scheme;
    return g_str_equal(scheme, "http") || g_str_equal(scheme, "https") ||
           g_str_equal(scheme, "mailto");
}

static gboolean confirm_unsafe_link(const char *uri, const char *scheme) {
    GtkWidget *dialog = gtk_message_dialog_new(
        GTK_WINDOW(app.window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        GTK_MESSAGE_WARNING,
        GTK_BUTTONS_YES_NO,
        "This PDF contains a link using the \"%s\" scheme:\n\n%s\n\n"
        "Opening it may launch another application or open a local file.\n"
        "Do you want to open it anyway?",
        scheme, uri);
    gtk_window_set_title(GTK_WINDOW(dialog), "Confirm opening link");
    gint response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    return response == GTK_RESPONSE_YES;
}

/* Open an external link extracted from a PDF. Well-known network schemes
   (http/https/mailto) are opened directly; anything else — especially
   file: and custom schemes — requires explicit user confirmation, since a
   malicious PDF could otherwise force the launch of local handlers
   (e.g. opening arbitrary files or running registered applications). */

static gboolean open_external_link(const char *uri) {
    if (!uri || !*uri) return FALSE;

    char *scheme = NULL;
    gboolean safe = is_safe_link_scheme(uri, &scheme);
    if (!safe) {
        if (!scheme) {
            LOG_WARN("Refusing to open malformed link URI: %s", uri);
            return TRUE;
        }
        if (!confirm_unsafe_link(uri, scheme)) {
            g_free(scheme);
            return TRUE; /* user declined — consume the click */
        }
    }
    g_free(scheme);

    GError *err = NULL;
    gtk_show_uri_on_window(GTK_WINDOW(app.window), uri, GDK_CURRENT_TIME, &err);
    if (err) {
        g_warning("Failed to open URI: %s", err->message);
        g_clear_error(&err);
    }
    return TRUE;
}

/* Activate the link at the given page-relative coordinates (if any).
   px, py are in rendering space (y-down, 0 at top of page). */

gboolean activate_link_at(TabData *tab, int page, double px, double py) {
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
                        return open_external_link(link->uri);
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
