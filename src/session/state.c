/* Session/application state persistence.
 *
 * Serializes the whole application state (window geometry, settings and every
 * session's open documents + per-document model) to siters.json, and restores
 * it on startup.  Extracted verbatim from src/siters.c.
 */

#include <gtk/gtk.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <errno.h>
#include "app.h"
#include "document_model.h"
#include "session_model.h"
#include "sessions_model.h"
#include "log.h"
#include "state.h"

/* State management functions */
static void json_emit_document(JsonBuilder *builder, const char *uri, const char *side, document_model_t *dm) {
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "uri");
    json_builder_add_string_value(builder, uri);
    json_builder_set_member_name(builder, "side");
    json_builder_add_string_value(builder, side);
    json_builder_set_member_name(builder, "zoom");
    json_builder_add_double_value(builder, document_model_get_zoom(dm));
    json_builder_set_member_name(builder, "current_page");
    json_builder_add_int_value(builder, document_model_get_current_page(dm));
    json_builder_set_member_name(builder, "page_count");
    json_builder_add_int_value(builder, document_model_get_page_count(dm));
    json_builder_set_member_name(builder, "visualization_mode");
    json_builder_add_int_value(builder, document_model_get_visualization_mode(dm));
    json_builder_set_member_name(builder, "horizontal_scroll");
    json_builder_add_boolean_value(builder, document_model_get_horizontal_scroll(dm));
    json_builder_set_member_name(builder, "scroll_offset");
    json_builder_add_double_value(builder, document_model_get_scroll_offset(dm));
    json_builder_set_member_name(builder, "intra_page_fraction");
    json_builder_add_double_value(builder, document_model_get_intra_page_fraction(dm));
    json_builder_end_object(builder);
}

static void json_emit_session_docs(JsonBuilder *builder, const GList *uris, const char *side_prefix, GHashTable *models, const char *session_name) {
    json_builder_begin_array(builder);
    for (const GList *d = uris; d; d = d->next) {
        const char *uri = (const char *)d->data;
        const char *side = (strcmp(side_prefix, "right") == 0) ? "right" : "left";
        char *key = make_document_key(session_name, uri, side[0] == 'r');
        document_model_t *dm = models ? g_hash_table_lookup(models, key) : NULL;
        if (dm) {
            json_emit_document(builder, uri, side, dm);
        }
        g_free(key);
    }
    json_builder_end_array(builder);
}

static document_model_t* json_parse_document(JsonObject *obj) {
    const char *uri = json_object_get_string_member_with_default(obj, "uri", NULL);
    if (!uri || !*uri) return NULL;

    document_model_t *dm = document_model_new();
    document_model_set_url(dm, uri);
    document_model_set_zoom(dm, json_object_get_double_member_with_default(obj, "zoom", 1.0));
    document_model_set_current_page(dm, (int)json_object_get_int_member_with_default(obj, "current_page", 1));
    document_model_set_page_count(dm, (int)json_object_get_int_member_with_default(obj, "page_count", 0));
    document_model_set_visualization_mode(dm, (int)json_object_get_int_member_with_default(obj, "visualization_mode", 0));
    document_model_set_horizontal_scroll(dm, json_object_get_boolean_member_with_default(obj, "horizontal_scroll", FALSE));
    document_model_set_scroll_offset(dm, json_object_get_double_member_with_default(obj, "scroll_offset", 0.0));
    document_model_set_intra_page_fraction(dm, json_object_get_double_member_with_default(obj, "intra_page_fraction", 0.0));
    return dm;
}

void save_state(void) {
    // Save current session's open tabs before saving state
    if (app.current_selected_session) {
        save_open_tabs_for_session(app.current_selected_session);
    }

    const gchar *cfg_override = g_getenv("SITERS_CONFIG_DIR");
    const gchar *config_dir = cfg_override ? cfg_override : g_get_user_config_dir();
    gchar *app_config_dir = g_build_filename(config_dir, "siters", NULL);
    if (g_mkdir_with_parents(app_config_dir, 0755) != 0) {
        LOG_ERROR("Failed to create config directory %s: %s",
                  app_config_dir, g_strerror(errno));
    }

    JsonBuilder *builder = json_builder_new();
    json_builder_begin_object(builder);

    json_builder_set_member_name(builder, "window");
    json_builder_begin_object(builder);
    if (app.window) {
        json_builder_set_member_name(builder, "width");
        json_builder_add_int_value(builder, app.current_width);
        json_builder_set_member_name(builder, "height");
        json_builder_add_int_value(builder, app.current_height);
        json_builder_set_member_name(builder, "x");
        json_builder_add_int_value(builder, app.current_x);
        json_builder_set_member_name(builder, "y");
        json_builder_add_int_value(builder, app.current_y);
        json_builder_set_member_name(builder, "maximized");
        json_builder_add_boolean_value(builder, app.current_maximized);
    }
    if (app.sessions_model) {
        json_builder_set_member_name(builder, "tabbar_position");
        json_builder_add_string_value(builder, sessions_model_get_tabbar_position(app.sessions_model));
        json_builder_set_member_name(builder, "tab_width");
        json_builder_add_int_value(builder, sessions_model_get_tab_width(app.sessions_model));
        json_builder_set_member_name(builder, "theme");
        json_builder_add_string_value(builder, sessions_model_get_theme(app.sessions_model));
        json_builder_set_member_name(builder, "keep_dark");
        json_builder_add_boolean_value(builder, sessions_model_get_keep_dark(app.sessions_model));
    }

    if (app.last_open_dir) {
        json_builder_set_member_name(builder, "last_open_dir");
        json_builder_add_string_value(builder, app.last_open_dir);
    }

    json_builder_end_object(builder);

    json_builder_set_member_name(builder, "sessions");
    json_builder_begin_object(builder);
    if (app.sessions_model) {
        const GList *session_names = sessions_model_get_session_names(app.sessions_model);
        if (session_names) {
            json_builder_set_member_name(builder, "names");
            json_builder_begin_array(builder);
            for (const GList *iter = session_names; iter; iter = iter->next)
                json_builder_add_string_value(builder, (const char *)iter->data);
            json_builder_end_array(builder);

            const char *last_session = sessions_model_get_last_open_session(app.sessions_model);
            if (last_session) {
                json_builder_set_member_name(builder, "last_open_session");
                json_builder_add_string_value(builder, last_session);
            }

            json_builder_set_member_name(builder, "data");
            json_builder_begin_object(builder);
            for (const GList *iter = session_names; iter; iter = iter->next) {
                const char *session_name = (const char *)iter->data;
                json_builder_set_member_name(builder, session_name);
                json_builder_begin_object(builder);

                session_model_t *session = g_hash_table_lookup(app.session_models, session_name);
                if (session) {
                    json_builder_set_member_name(builder, "documents");
                    json_emit_session_docs(builder, session_model_get_document_urls(session), "left", app.document_models, session_name);

                    json_builder_set_member_name(builder, "helper_documents");
                    json_emit_session_docs(builder, session_model_get_helper_document_urls(session), "right", app.document_models, session_name);

                    const char *lr = session_model_get_last_read_document(session);
                    json_builder_set_member_name(builder, "last_read_document");
                    json_builder_add_string_value(builder, lr ? lr : "");

                    const char *pc = session_model_get_page_color(session);
                    json_builder_set_member_name(builder, "page_color");
                    json_builder_add_string_value(builder, pc ? pc : "#FFFFFF");

                    const char *lrh = session_model_get_last_read_help_document(session);
                    json_builder_set_member_name(builder, "last_read_help_document");
                    json_builder_add_string_value(builder, lrh ? lrh : "");

                    const char *hpc = session_model_get_helper_page_color(session);
                    json_builder_set_member_name(builder, "helper_page_color");
                    json_builder_add_string_value(builder, hpc ? hpc : "#FFFFFF");
                } else {
                    json_builder_set_member_name(builder, "documents");
                    json_builder_begin_array(builder); json_builder_end_array(builder);
                    json_builder_set_member_name(builder, "helper_documents");
                    json_builder_begin_array(builder); json_builder_end_array(builder);
                    json_builder_set_member_name(builder, "last_read_document");
                    json_builder_add_string_value(builder, "");
                    json_builder_set_member_name(builder, "page_color");
                    json_builder_add_string_value(builder, "#FFFFFF");
                    json_builder_set_member_name(builder, "last_read_help_document");
                    json_builder_add_string_value(builder, "");
                    json_builder_set_member_name(builder, "helper_page_color");
                    json_builder_add_string_value(builder, "#FFFFFF");
                }

                json_builder_end_object(builder);
            }
            json_builder_end_object(builder);
        }
    }
    json_builder_end_object(builder);

    json_builder_end_object(builder);

    JsonNode *root = json_builder_get_root(builder);
    JsonGenerator *gen = json_generator_new();
    json_generator_set_root(gen, root);
    json_generator_set_pretty(gen, TRUE);

    gchar *json_str = json_generator_to_data(gen, NULL);
    if (!json_str) {
        LOG_ERROR("JSON serialization failed");
    }
    gchar *config_file = g_build_filename(app_config_dir, "siters.json", NULL);
    if (json_str) {
        GError *fs_error = NULL;
        if (!g_file_set_contents(config_file, json_str, -1, &fs_error)) {
            LOG_ERROR("Failed to write state file %s: %s", config_file, fs_error->message);
            g_clear_error(&fs_error);
        }
        g_free(json_str);
    }
    g_free(config_file);
    g_free(app_config_dir);
    g_object_unref(gen);
    json_node_free(root);
    g_object_unref(builder);
}

static void load_session_doc_array(JsonArray *arr, session_model_t *session, const char *default_side, const char *session_name) {
    if (!arr) return;
    guint len = json_array_get_length(arr);
    for (guint i = 0; i < len; i++) {
        JsonObject *obj = json_array_get_object_element(arr, i);
        if (!obj) continue;
        const char *uri = json_object_get_string_member_with_default(obj, "uri", NULL);
        if (!uri || !*uri) continue;

        const char *side = json_object_get_string_member_with_default(obj, "side", default_side);
        gboolean is_helper = (g_strcmp0(side, "right") == 0);

        if (is_helper)
            session_model_add_helper_document_url(session, uri);
        else
            session_model_add_document_url(session, uri);

        document_model_t *dm = json_parse_document(obj);
        if (dm) {
            if (!app.document_models) {
                app.document_models = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                                       (GDestroyNotify)document_model_free);
            }
            char *key = make_document_key(session_name, uri, is_helper);
            g_hash_table_insert(app.document_models, key, dm);
        }
    }
}

void load_state(void) {
    const gchar *cfg_override = g_getenv("SITERS_CONFIG_DIR");
    const gchar *config_dir = cfg_override ? cfg_override : g_get_user_config_dir();
    gchar *app_config_dir = g_build_filename(config_dir, "siters", NULL);
    gchar *config_file = g_build_filename(app_config_dir, "siters.json", NULL);

    JsonParser *parser = json_parser_new();
    GError *error = NULL;

    if (!json_parser_load_from_file(parser, config_file, &error)) {
        g_clear_error(&error);
        g_object_unref(parser);
        g_free(config_file);
        g_free(app_config_dir);
        return;
    }

    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
        g_object_unref(parser);
        g_free(config_file);
        g_free(app_config_dir);
        return;
    }

    JsonObject *root_obj = json_node_get_object(root);

    JsonObject *win = json_object_get_object_member(root_obj, "window");
    if (win && app.window) {
        int w = (int)json_object_get_int_member_with_default(win, "width", 1000);
        int h = (int)json_object_get_int_member_with_default(win, "height", 800);
        int x = (int)json_object_get_int_member_with_default(win, "x", -1);
        int y = (int)json_object_get_int_member_with_default(win, "y", -1);
        gboolean max = json_object_get_boolean_member_with_default(win, "maximized", FALSE);

        gtk_window_set_default_size(GTK_WINDOW(app.window), w, h);
        if (x >= 0 && y >= 0)
            gtk_window_move(GTK_WINDOW(app.window), x, y);
        if (max)
            gtk_window_maximize(GTK_WINDOW(app.window));

        app.current_width = w;
        app.current_height = h;
        app.current_x = x;
        app.current_y = y;
        app.current_maximized = max;
    }

    g_free(app.last_open_dir);
    app.last_open_dir = g_strdup(json_object_get_string_member_with_default(root_obj, "last_open_dir", NULL));

    if (app.sessions_model && win) {
        const char *pos = json_object_get_string_member_with_default(win, "tabbar_position", "top");
        sessions_model_set_tabbar_position(app.sessions_model, pos);
        int tw = (int)json_object_get_int_member_with_default(win, "tab_width", 100);
        sessions_model_set_tab_width(app.sessions_model, tw);
        /* Theme is auto-detected in create_main_window; always keep the model
           in sync with auto-detection so create_toolbar_icon stays correct. */
        sessions_model_set_theme(app.sessions_model, app.is_dark_theme ? "dark" : "light");

        /* Restore keep_dark override */
        gboolean saved_keep = json_object_get_boolean_member_with_default(win, "keep_dark", FALSE);
        sessions_model_set_keep_dark(app.sessions_model, saved_keep);
        app.keep_dark_theme = saved_keep;
        if (app.keep_dark_check) {
            g_signal_handlers_block_by_func(app.keep_dark_check, G_CALLBACK(on_keep_dark_toggled), NULL);
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app.keep_dark_check), saved_keep);
            g_signal_handlers_unblock_by_func(app.keep_dark_check, G_CALLBACK(on_keep_dark_toggled), NULL);
        }
        if (saved_keep) {
            app.is_dark_theme = TRUE;
            sessions_model_set_theme(app.sessions_model, "dark");
            apply_dark_css(TRUE);
            recolor_all_toolbars();
        }
    }

    JsonObject *sessions_obj = json_object_get_object_member(root_obj, "sessions");
    if (sessions_obj) {
        if (!app.sessions_model)
            app.sessions_model = sessions_model_new();

        JsonArray *names_arr = json_object_get_array_member(sessions_obj, "names");
        if (names_arr) {
            guint nlen = json_array_get_length(names_arr);
            for (guint i = 0; i < nlen; i++) {
                const char *name = json_array_get_string_element(names_arr, i);
                if (!name || !*name) continue;
                sessions_model_add_session_name(app.sessions_model, name);

                session_model_t *session = session_model_new();
                session_model_set_session_name(session, name);

                JsonObject *sdata = json_object_get_object_member(sessions_obj, "data");
                if (sdata) {
                    JsonObject *sd = json_object_get_object_member(sdata, name);
                    if (sd) {
                        load_session_doc_array(json_object_get_array_member(sd, "documents"), session, "left", name);
                        load_session_doc_array(json_object_get_array_member(sd, "helper_documents"), session, "right", name);
                        session_model_set_last_read_document(session,
                            json_object_get_string_member_with_default(sd, "last_read_document", ""));
                        session_model_set_page_color(session,
                            json_object_get_string_member_with_default(sd, "page_color", "#FFFFFF"));
                        session_model_set_last_read_help_document(session,
                            json_object_get_string_member_with_default(sd, "last_read_help_document", ""));
                        session_model_set_helper_page_color(session,
                            json_object_get_string_member_with_default(sd, "helper_page_color", "#FFFFFF"));
                    }
                }

                g_hash_table_insert(app.session_models, g_strdup(name), session);
            }
        }

        const char *last_session = json_object_get_string_member_with_default(sessions_obj, "last_open_session", NULL);
        if (last_session && *last_session) {
            sessions_model_set_last_open_session(app.sessions_model, last_session);
        } else {
            sessions_model_set_last_open_session(app.sessions_model, "Default");
        }
    }

    populate_sessions_treeview();

    if (app.sessions_model && app.current_selected_session) {
        const char *loaded_session = sessions_model_get_last_open_session(app.sessions_model);
        if (loaded_session && strcmp(loaded_session, app.current_selected_session) != 0) {
            // Update current_selected_session and restore open tabs for the loaded session
            g_free(app.current_selected_session);
            app.current_selected_session = g_strdup(loaded_session);
            restore_open_tabs_for_session(loaded_session);
            sync_left_layout_buttons(get_current_left_tab());
            sync_right_layout_buttons(get_current_right_tab());
            sync_page_widget_from_tab(get_current_left_tab());
            update_window_title_for_session(app.current_selected_session);
        } else if (loaded_session) {
            restore_open_tabs_for_session(loaded_session);
            sync_left_layout_buttons(get_current_left_tab());
            sync_right_layout_buttons(get_current_right_tab());
            sync_page_widget_from_tab(get_current_left_tab());
        }
    }

    if (app.sessions_model) {
        apply_tabbar_position(sessions_model_get_tabbar_position(app.sessions_model));
        apply_tab_width(sessions_model_get_tab_width(app.sessions_model));
        /* Sync settings UI widgets with loaded values */
        if (app.tabbar_combo) {
            const char *pos = sessions_model_get_tabbar_position(app.sessions_model);
            if (g_strcmp0(pos, "left") == 0)
                gtk_combo_box_set_active(GTK_COMBO_BOX(app.tabbar_combo), 0);
            else if (g_strcmp0(pos, "right") == 0)
                gtk_combo_box_set_active(GTK_COMBO_BOX(app.tabbar_combo), 2);
            else
                gtk_combo_box_set_active(GTK_COMBO_BOX(app.tabbar_combo), 1);
        }
        if (app.tab_width_spin)
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(app.tab_width_spin),
                                       sessions_model_get_tab_width(app.sessions_model));

        /* Sync color buttons with current session's saved colors */
        session_model_t *cur = get_current_session_model();
        if (cur) {
            const char *pc = session_model_get_page_color(cur);
            const char *hpc = session_model_get_helper_page_color(cur);
            if (app.left_color_btn) {
                g_signal_handlers_block_by_func(app.left_color_btn, G_CALLBACK(on_left_color_set), NULL);
                if (pc) {
                    GdkRGBA c;
                    if (gdk_rgba_parse(&c, pc))
                        gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(app.left_color_btn), &c);
                }
                g_signal_handlers_unblock_by_func(app.left_color_btn, G_CALLBACK(on_left_color_set), NULL);
            }
            if (app.right_color_btn) {
                g_signal_handlers_block_by_func(app.right_color_btn, G_CALLBACK(on_right_color_set), NULL);
                if (hpc) {
                    GdkRGBA c;
                    if (gdk_rgba_parse(&c, hpc))
                        gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(app.right_color_btn), &c);
                }
                g_signal_handlers_unblock_by_func(app.right_color_btn, G_CALLBACK(on_right_color_set), NULL);
            }
            /* Re-apply colors to all tabs */
            apply_page_color_to_notebook(app.left_notebook, pc ? pc : "#FFFFFF");
            apply_page_color_to_notebook(app.right_notebook, hpc ? hpc : "#FFFFFF");
        }
    }

    g_object_unref(parser);
    g_free(config_file);
    g_free(app_config_dir);
}
