#ifndef SITERS_FILEINFO_H
#define SITERS_FILEINFO_H

#include <gtk/gtk.h>
#include "tab.h"
#include "pdf.h"

/* Left file-info sidebar: refresh the Name/Path/Size/Pages labels for the
   current tab. Exported so the app hub can refresh it when the active tab
   changes. */
void update_file_info_labels(TabData *tab);

/* Right file-info popover: lazy-create and show the metadata popover. */
void on_right_file_info_clicked(GtkButton *button, gpointer user_data);

/* Shared byte-size formatting helper (also used by the app hub when it
   refreshes the right popover after closing a tab). */
gchar* format_file_size(goffset size);

/* Refresh the Name/Path/Size/Pages labels inside the right file-info
   popover for the current right-tab.  No-op when the popover is not
   mapped. */
void refresh_right_popover_labels(void);

#endif /* SITERS_FILEINFO_H */
