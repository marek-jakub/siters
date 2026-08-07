#include <gtk/gtk.h>
#include <glib.h>
#include <glib-unix.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include "siters.h"
#include "log.h"
#include "pdf.h"

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

/* Self-pipe: the raw signal handler writes one byte (async-signal-safe), and
   the GTK main loop watches the read end. This delivers SIGTERM/SIGINT to the
   main-loop context, where GLib/GTK calls such as save_state() are safe.
   The handler is never uninstalled, so the process can never be terminated
   mid-shutdown by a stray signal. */
static int signal_pipe[2] = {-1, -1};

static void raw_signal_handler(int signo) {
    (void)signo;
    char byte = 1;
    ssize_t r;
    do {
        r = write(signal_pipe[1], &byte, 1);
    } while (r < 0 && errno == EINTR);
}

static gboolean force_exit(gpointer user_data) {
    (void)user_data;
    exit(0);
    return G_SOURCE_REMOVE;
}

static void terminate_requested(void) {
    save_state();
    if (gtk_main_level() > 0) {
        /* Exit the innermost gtk_main(); the fallback timeout guarantees
           termination even if nested loops (dialogs) are still active. */
        gtk_main_quit();
        g_timeout_add(250, (GSourceFunc)force_exit, NULL);
    } else {
        /* No main loop running yet — nothing to unwind, exit directly. */
        exit(0);
    }
}

static gboolean on_terminate_signal(int fd, GIOCondition condition, gpointer user_data) {
    (void)condition;
    (void)user_data;
    char buf[64];
    while (read(fd, buf, sizeof buf) > 0) { /* drain the pipe */ }
    terminate_requested();
    return G_SOURCE_CONTINUE;
}

static gboolean on_terminate_signal_fallback(gpointer user_data) {
    (void)user_data;
    terminate_requested();
    return G_SOURCE_REMOVE;
}

static void install_terminate_handlers(void) {
    if (pipe(signal_pipe) != 0) {
        /* Fall back to GLib's own unix signal source. */
        GSource *source = g_unix_signal_source_new(SIGTERM);
        g_source_set_callback(source, (GSourceFunc)on_terminate_signal_fallback, NULL, NULL);
        g_source_attach(source, NULL);
        g_source_unref(source);
        source = g_unix_signal_source_new(SIGINT);
        g_source_set_callback(source, (GSourceFunc)on_terminate_signal_fallback, NULL, NULL);
        g_source_attach(source, NULL);
        g_source_unref(source);
        return;
    }

    for (int i = 0; i < 2; i++) {
        int flags = fcntl(signal_pipe[i], F_GETFL, 0);
        fcntl(signal_pipe[i], F_SETFL, flags | O_NONBLOCK);
        int fdflags = fcntl(signal_pipe[i], F_GETFD, 0);
        fcntl(signal_pipe[i], F_SETFD, fdflags | FD_CLOEXEC);
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = raw_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    g_unix_fd_add(signal_pipe[0], G_IO_IN, on_terminate_signal, NULL);
}

int main(int argc, char *argv[]) {
    /* Intercept Gtk criticals from a known GTK3 bug where side-tab
       box distribution asserts on negative remaining size. The assertion
       is harmless and the notebook handles it gracefully. */
    g_log_set_writer_func(suppress_gtk_box_critical, NULL, NULL);
    install_terminate_handlers();

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
    pdfr_shutdown();
    return 0;
}
