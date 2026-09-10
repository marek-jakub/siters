#include "notebook.h"
#include "app.h"
#include "state.h"
#include "sessions_tree.h"
#include "nav.h"
#include "ui/tab_lifecycle.h"
#include "ui/layout.h"
#include "fileinfo/fileinfo.h"
#include "session_model.h"
#include "sessions_model.h"
#include "pdf.h"
#include "settings/settings.h"
#include "view/view.h"
#include "view/render.h"
#include "view/scroll.h"

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
                    /* Newly opened documents are brand-new: never inherit
                       saved position/zoom state left over from a past use of
                       the same file or from another session. */
                    tab->fresh_open = TRUE;
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

TabData *get_current_left_tab(void) {
    if (!app.left_notebook) return NULL;
    int idx = gtk_notebook_get_current_page(GTK_NOTEBOOK(app.left_notebook));
    if (idx < 0) return NULL;

    GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(app.left_notebook), idx);
    if (!page) return NULL;

    return g_object_get_data(G_OBJECT(page), "tab-data");
}

TabData *get_current_right_tab(void) {
    if (!app.right_notebook) return NULL;
    int idx = gtk_notebook_get_current_page(GTK_NOTEBOOK(app.right_notebook));
    if (idx < 0) return NULL;

    GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(app.right_notebook), idx);
    if (!page) return NULL;

    return g_object_get_data(G_OBJECT(page), "tab-data");
}

static void on_tab_close_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    typedef struct {
        GtkNotebook *notebook;
        GtkWidget *page;
    } CloseInfo;
    CloseInfo *ci = user_data;
    if (!ci || !ci->notebook || !GTK_IS_NOTEBOOK(ci->notebook) || !ci->page) {
        if (ci) g_free(ci);
        return;
    }

    gboolean is_left = (ci->notebook == GTK_NOTEBOOK(app.left_notebook));
    gboolean is_right = (ci->notebook == GTK_NOTEBOOK(app.right_notebook));
    session_model_t *session = NULL;
    if (app.current_selected_session && app.session_models) {
        session = g_hash_table_lookup(app.session_models, app.current_selected_session);
    }

    char *closed_uri = NULL;
    int page_idx = gtk_notebook_page_num(ci->notebook, ci->page);
    if (page_idx >= 0) {
        GtkWidget *child = gtk_notebook_get_nth_page(ci->notebook, page_idx);
        if (child) {
            TabData *tab = g_object_get_data(G_OBJECT(child), "tab-data");
            if (tab && tab->current_file && session) {
                closed_uri = g_filename_to_uri(tab->current_file, NULL, NULL);
                // Remove from open documents list
                if (closed_uri) {
                    if (is_left) {
                        session_model_remove_document_url(session, closed_uri);
                    } else if (is_right) {
                        session_model_remove_helper_document_url(session, closed_uri);
                    }
                }
                // Remove document model so reopened file starts fresh (zoom, page, view, etc.)
                if (closed_uri && app.document_models) {
                    char *key = make_document_key(app.current_selected_session, closed_uri, tab->is_helper);
                    g_hash_table_remove(app.document_models, key);
                    g_free(key);
                }
            }
            /* The tab will be freed when its page widget is destroyed. */
        }
        gtk_notebook_remove_page(ci->notebook, page_idx);

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
                int cur = gtk_notebook_get_current_page(ci->notebook);
                if (cur >= 0) {
                    GtkWidget *new_page = gtk_notebook_get_nth_page(ci->notebook, cur);
                    if (new_page) {
                        update_last_read_for_notebook(ci->notebook, new_page, (guint)cur);
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

    g_free(ci);

    if (is_left) {
        populate_sessions_treeview();
    }
}

TabData *create_new_tab(GtkWidget *notebook) {
    TabData *tab = g_malloc0(sizeof(TabData));
    tab->zoom = 96.0;
    tab->layout_mode = 0;    /* single-column by default */
    tab->n_pages = 0;
    tab->cur_page = 0;
    tab->last_zoom = 96.0;
    tab->initial_scroll_pending = FALSE;
    tab->scroll_offset = -1.0;
    tab->is_helper = (notebook == app.right_notebook);
    tab->zoom_scroll_source_id = 0;
    tab->scroll_doc_debounce_id = 0;
    tab->last_cursor_type = GDK_LEFT_PTR;
    tab->last_cursor_check = 0;
    tab->last_known_page = -1;
    tab->last_known_page_start = 0.0;

    if (!notebook || !GTK_IS_NOTEBOOK(notebook)) {
        g_free(tab);
        return NULL;
    }

    gdk_rgba_parse(&tab->page_color, "white");
    /* Override with session's stored color if available */
    session_model_t *cur_session = get_current_session_model();
    if (cur_session) {
        const char *c = tab->is_helper
            ? session_model_get_helper_page_color(cur_session)
            : session_model_get_page_color(cur_session);
        if (c && *c) gdk_rgba_parse(&tab->page_color, c);
    }

    /* Create container for tab content */
    GtkWidget *tab_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    /* Create scrolled window and a single drawing area used for both single and continuous views */
    tab->scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_vexpand(tab->scrolled, TRUE);
    gtk_widget_set_hexpand(tab->scrolled, TRUE);
    /* connect scroll adjustments to update page display */
    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(tab->scrolled));
    g_signal_connect(G_OBJECT(vadj), "value-changed", G_CALLBACK(on_scroll_value_changed), tab);
    GtkAdjustment *hadj = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(tab->scrolled));
    g_signal_connect(G_OBJECT(hadj), "value-changed", G_CALLBACK(on_scroll_value_changed), tab);
    g_signal_connect(G_OBJECT(tab->scrolled), "size-allocate", G_CALLBACK(on_tab_scrolled_size_allocate), tab);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(tab->scrolled),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    tab->pages_drawing = gtk_drawing_area_new();
    gtk_widget_set_hexpand(tab->pages_drawing, TRUE);
    gtk_widget_set_vexpand(tab->pages_drawing, TRUE);
    g_signal_connect(G_OBJECT(tab->pages_drawing), "draw", G_CALLBACK(on_draw), tab);
    gtk_container_add(GTK_CONTAINER(tab->scrolled), tab->pages_drawing);
    gtk_box_pack_start(GTK_BOX(tab_box), tab->scrolled, TRUE, TRUE, 0);

    tab->h_scrollbar = gtk_scrollbar_new(GTK_ORIENTATION_HORIZONTAL, NULL);
    gtk_widget_set_no_show_all(tab->h_scrollbar, TRUE);
    gtk_widget_hide(tab->h_scrollbar);
    gtk_box_pack_start(GTK_BOX(tab_box), tab->h_scrollbar, FALSE, FALSE, 0);
    GtkAdjustment *scroll_adj = gtk_range_get_adjustment(GTK_RANGE(tab->h_scrollbar));
    g_signal_connect(G_OBJECT(scroll_adj), "value-changed", G_CALLBACK(on_scroll_value_changed), tab);
    g_signal_connect(G_OBJECT(tab->pages_drawing), "scroll-event", G_CALLBACK(on_drawing_scroll), tab);
    g_signal_connect(G_OBJECT(tab->pages_drawing), "button-press-event", G_CALLBACK(on_drawing_button_press), tab);
    g_signal_connect(G_OBJECT(tab->pages_drawing), "button-release-event", G_CALLBACK(on_drawing_button_release), tab);
    g_signal_connect(G_OBJECT(tab->pages_drawing), "motion-notify-event", G_CALLBACK(on_drawing_motion_notify), tab);
    g_signal_connect(G_OBJECT(tab->pages_drawing), "leave-notify-event", G_CALLBACK(on_drawing_leave), tab);
    gtk_widget_add_events(tab->pages_drawing, GDK_SCROLL_MASK | GDK_POINTER_MOTION_MASK | GDK_LEAVE_NOTIFY_MASK | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK);
    g_signal_connect(G_OBJECT(tab->h_scrollbar), "enter-notify-event", G_CALLBACK(on_h_scrollbar_enter), tab);
    g_signal_connect(G_OBJECT(tab->h_scrollbar), "leave-notify-event", G_CALLBACK(on_h_scrollbar_leave), tab);
    gtk_widget_add_events(tab->h_scrollbar, GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);

    gtk_widget_show_all(tab_box);

    /* Store tab data in the widget */
    g_object_set_data_full(G_OBJECT(tab_box), "tab-data", tab, destroy_tab_data);

    /* Determine box orientation based on tab position */
    GtkOrientation box_orientation = GTK_ORIENTATION_HORIZONTAL;
    if (app.sessions_model) {
        const char *pos = sessions_model_get_tabbar_position(app.sessions_model);
        if (g_strcmp0(pos, "left") == 0 || g_strcmp0(pos, "right") == 0)
            box_orientation = GTK_ORIENTATION_VERTICAL;
    }
    GtkWidget *label_box = gtk_box_new(box_orientation, 1);
    GtkWidget *label = gtk_label_new("New Document");
    if (app.sessions_model)
        gtk_label_set_angle(GTK_LABEL(label), get_angle_for_position(sessions_model_get_tabbar_position(app.sessions_model)));
    tab->tab_label = label;  /* Store reference to label for updates */
    tab->tab_label_box = label_box;  /* Store reference to container for orientation changes */
    GtkWidget *close_img = gtk_image_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_MENU);
    gtk_image_set_pixel_size(GTK_IMAGE(close_img), 8);
    GtkWidget *close_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(close_btn), close_img);
    gtk_widget_set_size_request(close_btn, 10, 10);
    tab->tab_label_close_btn = close_btn;
    gtk_widget_set_has_tooltip(close_btn, TRUE);
    gtk_widget_set_tooltip_text(close_btn, "Close tab");
    if (app.sessions_model && g_strcmp0(sessions_model_get_tabbar_position(app.sessions_model), "left") == 0) {
        gtk_box_pack_start(GTK_BOX(label_box), close_btn, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(label_box), label, FALSE, FALSE, 0);
    } else if (app.sessions_model && g_strcmp0(sessions_model_get_tabbar_position(app.sessions_model), "right") == 0) {
        gtk_box_pack_start(GTK_BOX(label_box), label, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(label_box), close_btn, FALSE, FALSE, 0);
    } else {
        gtk_box_pack_start(GTK_BOX(label_box), label, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(label_box), close_btn, FALSE, FALSE, 0);
    }
    /* allocate CloseInfo linking the notebook and this page so close removes correct page */
    typedef struct {
        GtkNotebook *notebook;
        GtkWidget *page;
    } CloseInfo;
    CloseInfo *ci = g_malloc(sizeof(CloseInfo));
    ci->notebook = GTK_NOTEBOOK(notebook);
    ci->page = tab_box;
    g_signal_connect(close_btn, "clicked", G_CALLBACK(on_tab_close_clicked), ci);
    gtk_widget_show_all(label_box);

    /* Add tab to notebook */
    int page_num = gtk_notebook_append_page(GTK_NOTEBOOK(notebook), tab_box, label_box);
    gtk_notebook_set_tab_reorderable(GTK_NOTEBOOK(notebook), tab_box, TRUE);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), page_num);

    return tab;
}
