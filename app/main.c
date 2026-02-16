#include <gtk/gtk.h>
#include "siters.h"

int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);

    GtkWidget *window = create_main_window();
    gtk_widget_show_all(window);

    return gtk_main();
}