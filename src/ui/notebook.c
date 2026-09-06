#include "notebook.h"
#include "app.h"
#include "state.h"
#include "nav.h"
#include "ui/tab_lifecycle.h"
#include "ui/layout.h"
#include "fileinfo/fileinfo.h"
#include "session_model.h"
#include "pdf.h"

extern App app;

int find_matching_tab_index(GtkNotebook *notebook, const char *target_uri) {
    if (!notebook || !target_uri || !*target_uri) return -1;
    int n_pages = gtk_notebook_get_n_pages(notebook);
    for (int i = 0; i < n_pages; i++) {
        GtkWidget *page = gtk_notebook_get_nth_page(notebook, i);
        if (!page) continue;
        TabData *tab = g_object_get_data(G_OBJECT(page), "tab-data");
        if (!tab || !tab->current_file) continue;
        char *uri = g_filename_to_uri(tab->current_file, NULL, NULL);
        if (!uri) continue;
        gboolean matched = (g_strcmp0(uri, target_uri) == 0);
        g_free(uri);
        if (matched) return i;
    }
    return -1;
}

void update_last_read_for_notebook(GtkNotebook *notebook, GtkWidget *page, guint page_num) {
    (void)page_num;
    if (app.is_restoring_session_tabs) return;
    if (!notebook || !page || !app.current_selected_session || !app.session_models) return;
    session_model_t *session = g_hash_table_lookup(app.session_models, app.current_selected_session);
    if (!session) return;
    TabData *tab = g_object_get_data(G_OBJECT(page), "tab-data");
    if (!tab || !tab->current_file) return;
    char *uri = g_filename_to_uri(tab->current_file, NULL, NULL);
    if (!uri) return;
    if (notebook == GTK_NOTEBOOK(app.left_notebook)) {
        session_model_set_last_read_document(session, uri);
    } else if (notebook == GTK_NOTEBOOK(app.right_notebook)) {
        session_model_set_last_read_help_document(session, uri);
    }
    g_free(uri);
}

void open_file_in_notebook(GtkWidget *notebook, gboolean is_helper) {
    if (!notebook) return;
    GtkWidget *dialog = gtk_file_chooser_dialog_new("Open PDF",
                                                   GTK_WINDOW(app.window),
                                                   GTK_FILE_CHOOSER_ACTION_OPEN,
                                                   "_Cancel", GTK_RESPONSE_CANCEL,
                                                   "_Open", GTK_RESPONSE_ACCEPT,
                                                   NULL);

    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_add_pattern(filter, "*.pdf");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);
    gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(dialog), TRUE);

    if (app.last_open_dir && g_file_test(app.last_open_dir, G_FILE_TEST_IS_DIR)) {
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), app.last_open_dir);
    }

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        GSList *filenames = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(dialog));
        if (filenames) {
            char *first_file = (char *)filenames->data;
            if (first_file) {
                char *dir = g_path_get_dirname(first_file);
                if (dir) {
                    g_free(app.last_open_dir);
                    app.last_open_dir = dir;
                }
            }
        }
        gboolean changed = FALSE;
        for (GSList *f = filenames; f; f = f->next) {
            char *fname = (char *)f->data;
            if (!fname) continue;
            char *uri = g_filename_to_uri(fname, NULL, NULL);
            int existing_idx = -1;
            if (uri) {
                existing_idx = find_matching_tab_index(GTK_NOTEBOOK(notebook), uri);
            }
            if (existing_idx >= 0) {
                /* Already open in this notebook: focus existing tab */
                gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), existing_idx);
            } else {
                /* Not open yet: create tab and load */
                TabData *tab = create_new_tab(notebook);
                if (tab) {
                    load_file_into_tab(tab, fname);
                    /* Add to session model only for newly opened docs */
                    if (app.current_selected_session) {
                        session_model_t *session = g_hash_table_lookup(app.session_models, app.current_selected_session);
                        if (session && uri) {
                            if (is_helper) {
                                session_model_add_helper_document_url(session, uri);
                            } else {
                                session_model_add_document_url(session, uri);
                                changed = TRUE;
                            }
                        }
                    }
                }
            }
            if (uri) g_free(uri);
            g_free(fname);
        }
        g_slist_free(filenames);
        if (changed) {
            populate_sessions_treeview();
        }
    }
    gtk_widget_destroy(dialog);
}

void close_tab_in_notebook(GtkNotebook *notebook) {
    if (!notebook) return;
    int page_idx = gtk_notebook_get_current_page(notebook);
    if (page_idx < 0) return;

    GtkWidget *page = gtk_notebook_get_nth_page(notebook, page_idx);
    if (!page) return;

    TabData *tab = g_object_get_data(G_OBJECT(page), "tab-data");
    if (!tab) {
        gtk_notebook_remove_page(notebook, page_idx);
        return;
    }

    gboolean is_left = (notebook == GTK_NOTEBOOK(app.left_notebook));
    gboolean is_right = (notebook == GTK_NOTEBOOK(app.right_notebook));

    char *closed_uri = NULL;
    session_model_t *session = NULL;
    if (app.current_selected_session && app.session_models) {
        session = g_hash_table_lookup(app.session_models, app.current_selected_session);
    }

    if (tab->current_file && session) {
        closed_uri = g_filename_to_uri(tab->current_file, NULL, NULL);
        if (closed_uri) {
            if (is_left) {
                session_model_remove_document_url(session, closed_uri);
            } else if (is_right) {
                session_model_remove_helper_document_url(session, closed_uri);
            }
        }
        if (closed_uri && app.document_models) {
            char *key = make_document_key(app.current_selected_session, closed_uri, tab->is_helper);
            g_hash_table_remove(app.document_models, key);
            g_free(key);
        }
    }

    gtk_notebook_remove_page(notebook, page_idx);

    if (session) {
        gboolean closed_was_last_read = FALSE;
        if (closed_uri) {
            if (is_left) {
                const char *last = session_model_get_last_read_document(session);
                closed_was_last_read = (g_strcmp0(last, closed_uri) == 0);
            } else if (is_right) {
                const char *last = session_model_get_last_read_help_document(session);
                closed_was_last_read = (g_strcmp0(last, closed_uri) == 0);
            }
        }
        if (closed_was_last_read) {
            int cur = gtk_notebook_get_current_page(notebook);
            if (cur >= 0) {
                GtkWidget *new_page = gtk_notebook_get_nth_page(notebook, cur);
                if (new_page) {
                    update_last_read_for_notebook(notebook, new_page, (guint)cur);
                }
            } else {
                if (is_left) {
                    session_model_set_last_read_document(session, "");
                } else if (is_right) {
                    session_model_set_last_read_help_document(session, "");
                }
            }
        }
    }

    if (closed_uri) g_free(closed_uri);

    if (is_left) {
        sync_page_widget_from_tab(get_current_left_tab());
    } else if (is_right) {
        sync_right_page_widget_from_tab(get_current_right_tab());
    }

    if (app.current_sidebar_mode == SIDEBAR_FILE_INFO) {
        update_file_info_labels(get_current_left_tab());
    }

    if (app.right_file_info_popover && gtk_widget_get_mapped(app.right_file_info_popover)) {
        TabData *rtab = get_current_right_tab();
        gchar *basename = rtab && rtab->current_file ? g_path_get_basename(rtab->current_file) : NULL;
        gchar *text = basename ? g_strdup_printf("Name: %s", basename) : g_strdup("Name: (no file)");
        gtk_label_set_text(GTK_LABEL(app.right_popover_name_label), text);
        g_free(text);
        g_free(basename);

        text = rtab && rtab->current_file ? g_strdup_printf("Path: %s", rtab->current_file) : g_strdup("Path: (none)");
        gtk_label_set_text(GTK_LABEL(app.right_popover_path_label), text);
        g_free(text);

        if (rtab && rtab->current_file) {
            GFile *gf = g_file_new_for_path(rtab->current_file);
            GFileInfo *info = g_file_query_info(gf, G_FILE_ATTRIBUTE_STANDARD_SIZE,
                                                 G_FILE_QUERY_INFO_NONE, NULL, NULL);
            if (info) {
                gchar *size_str = format_file_size(g_file_info_get_size(info));
                text = g_strdup_printf("Size: %s", size_str);
                gtk_label_set_text(GTK_LABEL(app.right_popover_size_label), text);
                g_free(text);
                g_free(size_str);
                g_object_unref(info);
            } else {
                gtk_label_set_text(GTK_LABEL(app.right_popover_size_label), "Size: Unknown");
            }
            g_object_unref(gf);

            if (rtab->doc) {
                gchar *pages_text = g_strdup_printf("Pages: %d", pdfr_count_pages(rtab->doc));
                gtk_label_set_text(GTK_LABEL(app.right_popover_pages_label), pages_text);
                g_free(pages_text);
            } else {
                gtk_label_set_text(GTK_LABEL(app.right_popover_pages_label), "Pages: N/A");
            }
        } else {
            gtk_label_set_text(GTK_LABEL(app.right_popover_size_label), "Size: (none)");
            gtk_label_set_text(GTK_LABEL(app.right_popover_pages_label), "Pages: (none)");
        }
    }

    if (is_left) {
        populate_sessions_treeview();
    }
}
