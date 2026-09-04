#ifndef SITERS_PDF_LINKS_H
#define SITERS_PDF_LINKS_H

#include <gtk/gtk.h>
#include "tab.h"

/* PDF link handling: lazy per-page link loading, hit-testing, and activation
   (internal GOTO/NAMED navigation and external URI opening with safe-scheme
   confirmation). Exported entry points are driven by the drawing-area mouse
   callbacks wired up in siters.c create_main_window. */
void     ensure_page_links_loaded(TabData *tab, int page);
gboolean has_link_at(TabData *tab, int page, double px, double py);
gboolean activate_link_at(TabData *tab, int page, double px, double py);

/* Safe-scheme check for external URIs (http/https/mailto). Exported for unit
   testing; returns the parsed scheme in *scheme_out (caller frees). */
gboolean is_safe_link_scheme(const char *uri, char **scheme_out);

#endif /* SITERS_PDF_LINKS_H */
