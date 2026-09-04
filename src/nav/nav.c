#include "nav.h"
#include "app.h"
#include "state.h"
#include "search.h"
#include "view.h"

#include <stdlib.h>

extern App app;

void sync_page_widget_from_tab(TabData *tab) {
    if (!app.page_entry || !app.page_total_label) return;

    int total = 0;
    int current = 0;

    if (tab && tab->n_pages > 0) {
        total = tab->n_pages;
        current = tab->cur_page + 1; /* UI is 1-based */
        if (current < 1) current = 1;
        if (current > total) current = total;
    }

    app.page_spin_syncing = TRUE;
    if (total == 0) {
        /* pick one of these two lines */
        gtk_entry_set_text(GTK_ENTRY(app.page_entry), "");   /* blank */
        /* gtk_entry_set_text(GTK_ENTRY(page_entry), "0"); */ /* or 0 */
    } else {
        gchar *cur_txt = g_strdup_printf("%d", current);
        gtk_entry_set_text(GTK_ENTRY(app.page_entry), cur_txt);
        g_free(cur_txt);
    }
    app.page_spin_syncing = FALSE;

    gchar *txt = g_strdup_printf("/ %d", total);
    gtk_label_set_text(GTK_LABEL(app.page_total_label), txt);
    g_free(txt);
}

void on_page_entry_insert_text(GtkEditable *editable,
                               const gchar *text,
                               gint length,
                               gint *position,
                               gpointer user_data) {
    (void)position;
    (void)user_data;

    /* Allow only ASCII digits to be inserted (typed or pasted). */
    for (gint i = 0; i < length; i++) {
        if (!g_ascii_isdigit((guchar)text[i])) {
            g_signal_stop_emission_by_name(editable, "insert-text");
            return;
        }
    }
}

void on_page_entry_activate(GtkEntry *entry, gpointer user_data) {
    (void)user_data;
    if (app.page_spin_syncing) return;

    TabData *tab = get_current_left_tab();
    if (!tab || tab->n_pages <= 0) return;

    const char *raw = gtk_entry_get_text(GTK_ENTRY(entry));
    char *endptr = NULL;
    long requested_ui = strtol(raw, &endptr, 10);
    if (endptr == raw || *endptr != '\0') {
        sync_page_widget_from_tab(tab);
        return;
    }

    if (requested_ui < 1 || requested_ui > tab->n_pages) return;

    int target_zero_based = requested_ui - 1;
    tab->cur_page = target_zero_based;
    scroll_to_page(tab, target_zero_based, -1);

    update_document_model_from_tab(tab);
}

void sync_right_page_widget_from_tab(TabData *tab) {
    if (!app.right_page_entry || !app.right_page_total_label) return;

    int total = 0;
    int current = 0;

    if (tab && tab->n_pages > 0) {
        total = tab->n_pages;
        current = tab->cur_page + 1;
        if (current < 1) current = 1;
        if (current > total) current = total;
    }

    app.right_page_spin_syncing = TRUE;
    if (total == 0) {
        gtk_entry_set_text(GTK_ENTRY(app.right_page_entry), "");
    } else {
        gchar *cur_txt = g_strdup_printf("%d", current);
        gtk_entry_set_text(GTK_ENTRY(app.right_page_entry), cur_txt);
        g_free(cur_txt);
    }
    app.right_page_spin_syncing = FALSE;

    gchar *txt = g_strdup_printf("/ %d", total);
    gtk_label_set_text(GTK_LABEL(app.right_page_total_label), txt);
    g_free(txt);

    /* Show/hide right nav based on whether there are pages */
    if (app.right_page_nav_overlay) {
        if (total > 0)
            gtk_widget_show(app.right_page_nav_overlay);
        else
            gtk_widget_hide(app.right_page_nav_overlay);
    }
}

void on_right_page_entry_activate(GtkEntry *entry, gpointer user_data) {
    (void)user_data;
    if (app.right_page_spin_syncing) return;

    TabData *tab = get_current_right_tab();
    if (!tab || tab->n_pages <= 0) return;

    const char *raw = gtk_entry_get_text(GTK_ENTRY(entry));
    char *endptr = NULL;
    long requested_ui = strtol(raw, &endptr, 10);
    if (endptr == raw || *endptr != '\0') {
        sync_right_page_widget_from_tab(tab);
        return;
    }

    if (requested_ui < 1 || requested_ui > tab->n_pages) return;

    int target_zero_based = requested_ui - 1;
    tab->cur_page = target_zero_based;
    scroll_to_page(tab, target_zero_based, -1);

    update_document_model_from_tab(tab);
}