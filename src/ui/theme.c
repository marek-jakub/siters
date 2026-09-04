#include <gtk/gtk.h>
#include <string.h>
#include "app.h"
#include "log.h"
#include "sessions_model.h"
#include "theme.h"

/* Dark-theme detection, toolbar icon recoloring and CSS styling. The toolbar
   icons are generated from SVG files recolored to match the current theme. */


/* Create a GtkImage from an SVG icon, recoloring it for the current theme.
   The 'name' parameter is the stem of the SVG file (e.g. "zoom-in" for zoom-in.svg). */

GtkWidget* create_toolbar_icon(const char *name) {
    char *path = g_strdup_printf(DATADIR "/data/icons/%s.svg", name);
    gchar *svg_content;
    gsize length;
    if (!g_file_get_contents(path, &svg_content, &length, NULL)) {
        LOG_WARN("Failed to read icon SVG %s", path);
        g_free(path);
        return gtk_image_new();
    }
    g_free(path);

    const char *target_color = app.is_dark_theme ? "#FFFFAD" : "#141400";

    /* Recolor all #XXXXXX values in the SVG to match the theme */
    char *p = svg_content;
    while ((p = strstr(p, "#FFFFAD")) != NULL) {
        memcpy(p, target_color, 7);
        p += 7;
    }
    p = svg_content;
    while ((p = strstr(p, "#ffffad")) != NULL) {
        memcpy(p, target_color, 7);
        p += 7;
    }

    GBytes *bytes = g_bytes_new_take(svg_content, length);
    GInputStream *stream = g_memory_input_stream_new_from_bytes(bytes);
    GError *pixbuf_err = NULL;
    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_stream_at_scale(stream, 20, 20, TRUE, NULL, &pixbuf_err);
    g_object_unref(stream);
    g_bytes_unref(bytes);

    GtkWidget *image;
    if (pixbuf) {
        image = gtk_image_new_from_pixbuf(pixbuf);
        g_object_unref(pixbuf);
    } else {
        LOG_WARN("Failed to render icon pixbuf: %s", pixbuf_err->message);
        g_clear_error(&pixbuf_err);
        image = gtk_image_new();
    }
    return image;
}

/* Refresh a single button's icon from its stored icon-name data.
   Toggle buttons use icon-on/icon-off based on their current state. */

static void recolor_toolbar_button(GtkWidget *btn) {
    const char *icon_on = g_object_get_data(G_OBJECT(btn), "icon-on");
    const char *icon_off = g_object_get_data(G_OBJECT(btn), "icon-off");
    if (icon_on && icon_off && GTK_IS_TOGGLE_BUTTON(btn)) {
        gboolean active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(btn));
        gtk_button_set_image(GTK_BUTTON(btn), create_toolbar_icon(active ? icon_on : icon_off));
        return;
    }
    const char *icon_name = g_object_get_data(G_OBJECT(btn), "icon-name");
    if (icon_name) {
        gtk_button_set_image(GTK_BUTTON(btn), create_toolbar_icon(icon_name));
    }
}

/* Recursively walk a container and recolor all buttons with icon data */

static void recolor_toolbar_children(GtkWidget *parent) {
    if (!parent) return;
    if (GTK_IS_BUTTON(parent)) {
        recolor_toolbar_button(parent);
        return;
    }
    if (GTK_IS_CONTAINER(parent)) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(parent));
        for (GList *iter = children; iter; iter = iter->next) {
            recolor_toolbar_children(GTK_WIDGET(iter->data));
        }
        g_list_free(children);
    }
}

/* Recolor all toolbar icons */

void recolor_all_toolbars(void) {
    if (app.main_hbox) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(app.main_hbox));
        for (GList *iter = children; iter; iter = iter->next) {
            recolor_toolbar_children(GTK_WIDGET(iter->data));
        }
        g_list_free(children);
    }
    if (app.right_pane) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(app.right_pane));
        for (GList *iter = children; iter; iter = iter->next) {
            recolor_toolbar_children(GTK_WIDGET(iter->data));
        }
        g_list_free(children);
    }
}

/* Detect whether the system theme is dark by checking gtk-application-prefer-dark-theme
   and gtk-theme-name for keywords. */

gboolean detect_system_dark_theme(void) {
    gboolean prefer_dark = FALSE;
    GtkSettings *settings = gtk_settings_get_default();
    if (settings) {
        g_object_get(settings, "gtk-application-prefer-dark-theme", &prefer_dark, NULL);
        gchar *theme_name = NULL;
        g_object_get(settings, "gtk-theme-name", &theme_name, NULL);
        if (theme_name) {
            gchar *lower = g_ascii_strdown(theme_name, -1);
            if (strstr(lower, "dark") || strstr(lower, "black") ||
                strstr(lower, "night") || strstr(lower, "nokto"))
                prefer_dark = TRUE;
            g_free(lower);
            g_free(theme_name);
        }
    }
    return prefer_dark;
}

/* Dark CSS theme applied via GtkCssProvider when keep_dark_theme is active.
   This overrides GTK widget colors to dark even when the system theme is light,
   working around the limitation of gtk-theme-name:dark and prefer-dark-theme
   not being reliably re-applied on system theme changes. */

void apply_dark_css(gboolean apply) {
    GdkScreen *screen = gdk_screen_get_default();
    if (!screen) return;
    if (apply) {
        if (!app.dark_css_provider) {
            app.dark_css_provider = gtk_css_provider_new();
            const char *css =
                "window, window.background, box, notebook, scrolledwindow,\n"
                "popover, popover.background, menubar, menu, .sidebar { background: #2e2e2e; }\n"
                "menubar, menu { color: #ffffff; }\n"
                "label, popover label { color: #ffffff; }\n"
                "button, combobox button, spinbutton button {\n"
                "    background: #3c3c3c; border: 1px solid #2F2F34;\n"
                "    color: #ffffff;\n"
                "}\n"
                "button:hover, combobox button:hover, spinbutton button:hover {\n"
                "    background: #4a4a4a;\n"
                "}\n"
                "button:checked {\n"
                "    background: #505050; border-color: #454655;\n"
                "}\n"
                "entry, spinbutton, spinbutton entry, treeview, treeview.view,\n"
                "drawingarea { background: #1e1e1e; color: #ffffff; }\n"
                "entry { border: 1px solid #2F2F34; }\n"
                "treeview:selected, treeview.view:selected {\n"
                "    background: #3584e4;\n"
                "}\n"
                "notebook > header { background: #353535; }\n"
                "notebook tab { background: #353535; color: #9a9a9a; }\n"
                "notebook tab:checked { background: #2e2e2e; color: #ffffff; }\n"
                "scrollbar { background: #2e2e2e; }\n"
                "scrollbar slider { background: #2F2F34; }\n"
                "paned > separator, separator { background: #2F2F34; }\n"
                "menu menuitem:hover { background: #3584e4; }\n"
                "dialog .background { background: #2e2e2e; }\n";
            GError *css_err = NULL;
            if (!gtk_css_provider_load_from_data(app.dark_css_provider, css, -1, &css_err)) {
                LOG_ERROR("Failed to load dark theme CSS: %s", css_err->message);
                g_clear_error(&css_err);
            }
        }
        gtk_style_context_add_provider_for_screen(screen,
            GTK_STYLE_PROVIDER(app.dark_css_provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    } else {
        if (app.dark_css_provider) {
            gtk_style_context_remove_provider_for_screen(screen,
                GTK_STYLE_PROVIDER(app.dark_css_provider));
        }
    }
}

/* Recolor all toolbar icons when the system theme changes.
   When keep_dark_theme is active, the CSS provider is the sole mechanism
   for dark widget styling — no GTK settings are touched. */

void on_theme_changed(GObject *gobject, GParamSpec *pspec, gpointer user_data) {
    (void)gobject;
    (void)pspec;
    (void)user_data;
    if (app.keep_dark_theme) {
        app.is_dark_theme = TRUE;
        apply_dark_css(TRUE);
    } else {
        apply_dark_css(FALSE);
        app.is_dark_theme = detect_system_dark_theme();
        if (app.sessions_model)
            sessions_model_set_theme(app.sessions_model, app.is_dark_theme ? "dark" : "light");
    }
    recolor_all_toolbars();
}
