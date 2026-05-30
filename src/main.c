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

    /* Remove GTK scrolled window overshoot/undershoot indicators (dashed lines at edges) */
    GtkCssProvider *css_provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css_provider,
        "scrolledwindow overshoot, scrolledwindow undershoot { background: none; }\n"
        "#page-nav-overlay {\n"
        "    background: rgba(0, 0, 0, 0.6);\n"
        "    border-radius: 8px;\n"
        "    padding: 6px 10px;\n"
        "}\n"
        "#page-nav-overlay label {\n"
        "    color: white;\n"
        "}\n"
        "#page-nav-overlay entry {\n"
        "    background: rgba(255, 255, 255, 0.9);\n"
        "    border: none;\n"
        "    border-radius: 4px;\n"
        "    color: black;\n"
        "}\n", -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(css_provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css_provider);

    GtkWidget *window = create_main_window();
    
    // Load saved state and apply to window
    load_state();

    gtk_widget_show_all(window);
    hide_right_pane();

    gtk_main();
    return 0;
}
