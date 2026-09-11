#ifndef SITERS_UI_WINDOW_H
#define SITERS_UI_WINDOW_H

#include <gtk/gtk.h>

/* Main window factory (declared for main.c via the app hub header). */
GtkWidget* create_main_window(void);

/* Segment builders used by create_main_window. */
GtkWidget *window_create_main_toolbar(void);
void       window_fill_main_toolbar(GtkWidget *toolbar);
void       window_build_sidebar(void);
void       window_build_right_toolbar(void);

#endif /* SITERS_UI_WINDOW_H */
