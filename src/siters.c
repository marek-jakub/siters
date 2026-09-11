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
#include "session/sessions_actions.h"
#include "view.h"
#include "render.h"
#include "document_state.h"
#include "view/scroll.h"
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

/* DATADIR is normally defined by -DDATADIR=... at build time.
   This fallback lets clang-based tools parse the file without flags. */
#ifndef DATADIR
#define DATADIR "."
#endif

/* Single application-wide state object. All former module-level statics now
   live as fields of this struct (defined in app.h). */
App app;



/* Function prototypes */
void save_state(void);


void hide_right_pane(void);

/* PDF handling function prototypes */
void queue_draw(TabData *tab);
void scroll_to_page(TabData *tab, int page, double target_y);
/* Build a compound key "side:uri" to differentiate left vs right notebook state */

void hide_right_pane(void) {
    if (app.right_pane) {
        gtk_widget_hide(GTK_WIDGET(app.right_pane));
    }
}
