#include <gtk/gtk.h>
#include "siters.h"

GtkWidget* create_main_window(void) {
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "Siters");
    gtk_window_set_default_size(GTK_WINDOW(window), 400, 300);

    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    return window;
}