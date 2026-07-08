#include <gtk/gtk.h>
#include <signal.h>
#include <glib.h>
#include <string.h>
#include "siters.h"
#include "log.h"

extern void load_state(void);

static GLogWriterOutput suppress_gtk_box_critical(GLogLevelFlags log_level,
                                                   const GLogField *fields,
                                                   gsize n_fields,
                                                   gpointer user_data) {
    (void)user_data;
    /* Suppress the known GTK3 assertion in side-tab overflow scenarios.
       It's non-fatal and safe to ignore. This writer is called at the lowest
       possible level, before any domain-specific handler runs. */
    if (log_level & G_LOG_LEVEL_CRITICAL) {
        for (gsize i = 0; i < n_fields; i++) {
            if (g_strcmp0(fields[i].key, "MESSAGE") == 0 &&
                fields[i].value &&
                strstr(fields[i].value, "gtk_box_gadget_distribute")) {
                return G_LOG_WRITER_HANDLED;
            }
        }
    }
    return G_LOG_WRITER_UNHANDLED;
}

void signal_handler(int sig) {
    (void)sig;  // Mark parameter as intentionally unused
    save_state();
    exit(0);
}

int main(int argc, char *argv[]) {
    /* Intercept Gtk criticals from a known GTK3 bug where side-tab
       box distribution asserts on negative remaining size. The assertion
       is harmless and the notebook handles it gracefully. */
    g_log_set_writer_func(suppress_gtk_box_critical, NULL, NULL);
    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);

    gtk_init(&argc, &argv);

    /* Remove GTK scrolled window overshoot/undershoot indicators (dashed lines at edges) */
    GtkCssProvider *css_provider = gtk_css_provider_new();
    GError *css_err = NULL;
    if (!gtk_css_provider_load_from_data(css_provider,
        "scrolledwindow overshoot, scrolledwindow undershoot { background: none; }\n"
        "#page-nav-overlay, #right-page-nav-overlay {\n"
        "    background: rgba(0, 0, 0, 0.6);\n"
        "    border-radius: 8px;\n"
        "    padding: 6px 10px;\n"
        "}\n"
        "notebook > header.left tab,\n"
        "notebook > header.right tab {\n"
        "    padding: 2px 2px;\n"
        "}\n"
        "#page-nav-overlay label, #right-page-nav-overlay label {\n"
        "    color: white;\n"
        "}\n"
        "#page-nav-overlay entry, #right-page-nav-overlay entry {\n"
        "    background: rgba(255, 255, 255, 0.9);\n"
        "    border: none;\n"
        "    border-radius: 4px;\n"
        "    color: black;\n"
        "}\n", -1, &css_err)) {
        LOG_WARN("Failed to load app CSS: %s", css_err->message);
        g_clear_error(&css_err);
    }
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
