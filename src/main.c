#include <gtk/gtk.h>
#include <signal.h>
#include "siters.h"

extern void load_state(void);

void signal_handler(int sig) {
    (void)sig;  // Mark parameter as intentionally unused
    save_state();
    exit(0);
}

int main(int argc, char *argv[]) {
    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);
    
    gtk_init(&argc, &argv);

    GtkWidget *window = create_main_window();
    
    // Load saved state and apply to window
    load_state();

    gtk_widget_show_all(window);
    hide_right_pane();

    gtk_main();
    return 0;
}