#include <gtk/gtk.h>
#include <atk/atk.h>
#include <glib/gkeyfile.h>
#include <glib/gstdio.h>
#include <poppler.h>
#include <math.h>
#include "siters.h"
#include "sessions_model.h"
#include "session_model.h"
#include "document_model.h"

#define MAX_SURFACE_DIM 5000

typedef struct {
    PopplerDocument *doc;
    int n_pages;
    int cur_page;
    GtkWidget *drawing;
    double zoom;
    GdkRGBA page_color;
    /* per-document storage */
    char *current_file;
    GtkWidget *tab_label;  /* reference to the label widget in the tab */
    /* continuous view widgets */
    GtkWidget *scrolled;
    GtkWidget *pages_drawing; /* single drawing area used for both single and continuous views */
    int layout_mode; /* 0=single-column,1=two-column,2=horizontal */
} TabData;

typedef enum {
    SIDEBAR_NONE,
    SIDEBAR_SESSIONS,
    SIDEBAR_TOC,
    SIDEBAR_SETTINGS
} SidebarMode;

/* Current window geometry */
static gint current_width = 1000;
static gint current_height = 800;
static gint current_x = -1;
static gint current_y = -1;
static gboolean current_maximized = FALSE;

/* Sidebar for sessions, toc and settings */
static SidebarMode current_sidebar_mode = SIDEBAR_NONE;
static GtkWidget *sidebar;
static GtkWidget *sidebar_label;
static GtkWidget *main_hbox;
static GtkWidget *content_vbox;
static GtkWidget *window;

/* Sessions sidebar components */
static GtkWidget *sessions_container;
static GtkWidget *sessions_title;
static GtkWidget *sessions_entry;
static GtkWidget *sessions_add_btn;
static GtkWidget *sessions_remove_btn;
static GtkWidget *sessions_update_btn;
static GtkWidget *sessions_tree_view;
static GtkListStore *sessions_list_store;
static sessions_model_t *sessions_model;

/* Session models - map from session name to session_model_t */
static GHashTable *session_models;

/* Paned/Notebook components */
static GtkWidget *paned;
static GtkWidget *right_pane;
static GtkWidget *left_notebook;
static GtkWidget *right_notebook;
static gchar *current_selected_session = NULL;
static gboolean is_restoring_session_tabs = FALSE;

/* Function prototypes */
void save_state(void);
void populate_sessions_treeview(void);
void hide_right_pane(void);
static void set_right_notebook_session(const gchar *session_name);
static void on_tab_close_clicked(GtkButton *btn, gpointer user_data);

/* Session tab persistence functions */
static void save_open_tabs_for_session(const char *session_name);
static void restore_open_tabs_for_session(const char *session_name);

/* PDF handling function prototypes */
static TabData *create_new_tab(GtkWidget *notebook);
static void load_file_into_tab(TabData *tab, const char *filename);
static void queue_draw(TabData *tab);
static gboolean on_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data);
static void build_continuous_view(TabData *tab, int target_width);
static void scroll_to_page(TabData *tab, int page);
static void on_scroll_value_changed(GtkAdjustment *adj, gpointer user_data);
static void open_file_in_notebook(GtkWidget *notebook, gboolean is_helper);
static void on_open_file_clicked(GtkButton *button, gpointer user_data);
static void on_open_helper_file_clicked(GtkButton *button, gpointer user_data);
static void update_last_read_for_notebook(GtkNotebook *notebook, GtkWidget *page, guint page_num);
static void on_left_notebook_switch_page(GtkNotebook *notebook, GtkWidget *page, guint page_num, gpointer user_data);
static void on_right_notebook_switch_page(GtkNotebook *notebook, GtkWidget *page, guint page_num, gpointer user_data);
static int find_matching_tab_index(GtkNotebook *notebook, const char *target_uri);

/* Save state on closing app */
static void on_window_destroy(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    (void)user_data;
    save_state();
    gtk_main_quit();
}

static gboolean on_window_configure(GtkWidget *widget, GdkEventConfigure *event, gpointer user_data) {
    (void)user_data;
    
    // Update current geometry
    current_width = event->width;
    current_height = event->height;
    current_x = event->x;
    current_y = event->y;
    current_maximized = gtk_window_is_maximized(GTK_WINDOW(widget));
    
    return FALSE; // Allow further processing
}

static void update_window_title_for_session(const char *session_name) {
    if (!window) return;

    const char *name = (session_name && *session_name) ? session_name : "Default";
    gchar *title = g_strdup_printf("Siters - %s", name);
    gtk_window_set_title(GTK_WINDOW(window), title);
    g_free(title);
}

/* State management functions */
void save_state(void) {
    // Save current session's open tabs before saving state
    if (current_selected_session) {
        save_open_tabs_for_session(current_selected_session);
    }
    
    GKeyFile *key_file = g_key_file_new();
    GError *error = NULL;
    
    // Create config directory if it doesn't exist
    const gchar *config_dir = g_get_user_config_dir();
    gchar *app_config_dir = g_build_filename(config_dir, "siters", NULL);
    g_mkdir_with_parents(app_config_dir, 0755);
    
    // Save window state
    if (window) {
        g_key_file_set_integer(key_file, "Window", "width", current_width);
        g_key_file_set_integer(key_file, "Window", "height", current_height);
        g_key_file_set_integer(key_file, "Window", "x", current_x);
        g_key_file_set_integer(key_file, "Window", "y", current_y);
        g_key_file_set_boolean(key_file, "Window", "maximized", current_maximized);
    }
    
    // Save sessions
    if (sessions_model) {
        const GList *session_names = sessions_model_get_session_names(sessions_model);
        if (session_names) {
            // Build comma-separated list of session names
            GString *names_str = g_string_new("");
            for (const GList *iter = session_names; iter != NULL; iter = iter->next) {
                if (names_str->len > 0) {
                    g_string_append(names_str, ",");
                }
                g_string_append(names_str, (const char*)iter->data);
            }
            g_key_file_set_string(key_file, "Sessions", "names", names_str->str);
            g_string_free(names_str, TRUE);
            
            // Save the last open session
            const char *last_session = sessions_model_get_last_open_session(sessions_model);
            if (last_session) {
                g_key_file_set_string(key_file, "Sessions", "last_open_session", last_session);
            }
            
            // For each session, save its data (currently placeholder, as session_model not fully integrated)
            for (const GList *iter = session_names; iter != NULL; iter = iter->next) {
                const char *session_name = (const char*)iter->data;
                gchar *section_name = g_strdup_printf("Session_%s", session_name);
                
                // Get session model for this session
                session_model_t *session = g_hash_table_lookup(session_models, session_name);
                if (session) {
                    // Save document URLs
                    const GList *docs = session_model_get_document_urls(session);
                    GString *docs_str = g_string_new("");
                    for (const GList *doc_iter = docs; doc_iter != NULL; doc_iter = doc_iter->next) {
                        if (docs_str->len > 0) g_string_append(docs_str, ",");
                        g_string_append(docs_str, (const char*)doc_iter->data);
                    }
                    g_key_file_set_string(key_file, section_name, "documents", docs_str->str);
                    g_string_free(docs_str, TRUE);
                    
                    // Save helper document URLs
                    const GList *helper_docs = session_model_get_helper_document_urls(session);
                    GString *helper_docs_str = g_string_new("");
                    for (const GList *doc_iter = helper_docs; doc_iter != NULL; doc_iter = doc_iter->next) {
                        if (helper_docs_str->len > 0) g_string_append(helper_docs_str, ",");
                        g_string_append(helper_docs_str, (const char*)doc_iter->data);
                    }
                    g_key_file_set_string(key_file, section_name, "helper_documents", helper_docs_str->str);
                    g_string_free(helper_docs_str, TRUE);
                    
                    // Save other session properties
                    const char *last_read = session_model_get_last_read_document(session);
                    g_key_file_set_string(key_file, section_name, "last_read_document", last_read ? last_read : "");
                    
                    const char *page_color = session_model_get_page_color(session);
                    g_key_file_set_string(key_file, section_name, "page_color", page_color ? page_color : "#FFFFFF");
                    
                    const char *last_read_help = session_model_get_last_read_help_document(session);
                    g_key_file_set_string(key_file, section_name, "last_read_help_document", last_read_help ? last_read_help : "");
                    
                    const char *helper_page_color = session_model_get_helper_page_color(session);
                    g_key_file_set_string(key_file, section_name, "helper_page_color", helper_page_color ? helper_page_color : "#FFFFFF");
                } else {
                    // Placeholder: save empty document lists
                    g_key_file_set_string(key_file, section_name, "documents", "");
                    g_key_file_set_string(key_file, section_name, "helper_documents", "");
                    g_key_file_set_string(key_file, section_name, "last_read_document", "");
                    g_key_file_set_string(key_file, section_name, "page_color", "#FFFFFF");
                    g_key_file_set_string(key_file, section_name, "last_read_help_document", "");
                    g_key_file_set_string(key_file, section_name, "helper_page_color", "#FFFFFF");
                }
                
                g_free(section_name);
            }
        }
    }
    
    // Save to file
    gchar *config_file = g_build_filename(app_config_dir, "siters.ini", NULL);
    if (!g_key_file_save_to_file(key_file, config_file, &error)) {
        g_warning("Failed to save config: %s", error->message);
        g_error_free(error);
    }
    
    g_key_file_free(key_file);
    g_free(config_file);
    g_free(app_config_dir);
}

void load_state(void) {
    GKeyFile *key_file = g_key_file_new();
    GError *error = NULL;
    
    const gchar *config_dir = g_get_user_config_dir();
    gchar *config_file = g_build_filename(config_dir, "siters", "siters.ini", NULL);
    
    if (!g_key_file_load_from_file(key_file, config_file, G_KEY_FILE_NONE, &error)) {
        // File doesn't exist or can't be read, use defaults
        g_key_file_free(key_file);
        g_free(config_file);
        return;
    }
    
    // Load window state
    gint saved_width = g_key_file_get_integer(key_file, "Window", "width", &error);
    if (error) { saved_width = 1000; g_error_free(error); error = NULL; }
    
    gint saved_height = g_key_file_get_integer(key_file, "Window", "height", &error);
    if (error) { saved_height = 800; g_error_free(error); error = NULL; }
    
    gint saved_x = g_key_file_get_integer(key_file, "Window", "x", &error);
    if (error) { saved_x = -1; g_error_free(error); error = NULL; }
    
    gint saved_y = g_key_file_get_integer(key_file, "Window", "y", &error);
    if (error) { saved_y = -1; g_error_free(error); error = NULL; }
    
    gboolean saved_maximized = g_key_file_get_boolean(key_file, "Window", "maximized", &error);
    if (error) { saved_maximized = FALSE; g_error_free(error); error = NULL; }
    
    // Apply window state if window exists
    if (window) {
        gtk_window_set_default_size(GTK_WINDOW(window), saved_width, saved_height);
        if (saved_x >= 0 && saved_y >= 0) {
            gtk_window_move(GTK_WINDOW(window), saved_x, saved_y);
        }
        if (saved_maximized) {
            gtk_window_maximize(GTK_WINDOW(window));
        }
        
        // Update current geometry variables
        current_width = saved_width;
        current_height = saved_height;
        current_x = saved_x;
        current_y = saved_y;
        current_maximized = saved_maximized;
    }
    
    // Load sessions
    gchar *names_str = g_key_file_get_string(key_file, "Sessions", "names", &error);
    if (!error && names_str) {
        // Initialize sessions_model if not done
        if (!sessions_model) {
            sessions_model = sessions_model_new();
        }
        
        // Parse comma-separated names and load session data
        gchar **names_array = g_strsplit(names_str, ",", -1);
        for (gchar **name = names_array; *name != NULL; name++) {
            g_strstrip(*name); // Remove whitespace
            if (strlen(*name) > 0) {
                sessions_model_add_session_name(sessions_model, *name);
                
                // Load session data for this session
                gchar *section_name = g_strdup_printf("Session_%s", *name);
                
                // Create session model for this session
                session_model_t *session = session_model_new();
                session_model_set_session_name(session, *name);
                
                // Load documents
                gchar *docs_str = g_key_file_get_string(key_file, section_name, "documents", NULL);
                if (docs_str) {
                    gchar **docs_array = g_strsplit(docs_str, ",", -1);
                    for (gchar **doc = docs_array; *doc != NULL; doc++) {
                        g_strstrip(*doc);
                        if (strlen(*doc) > 0) {
                            session_model_add_document_url(session, *doc);
                        }
                    }
                    g_strfreev(docs_array);
                    g_free(docs_str);
                }
                
                // Load helper documents
                gchar *helper_docs_str = g_key_file_get_string(key_file, section_name, "helper_documents", NULL);
                if (helper_docs_str) {
                    gchar **helper_docs_array = g_strsplit(helper_docs_str, ",", -1);
                    for (gchar **doc = helper_docs_array; *doc != NULL; doc++) {
                        g_strstrip(*doc);
                        if (strlen(*doc) > 0) {
                            session_model_add_helper_document_url(session, *doc);
                        }
                    }
                    g_strfreev(helper_docs_array);
                    g_free(helper_docs_str);
                }
                
                // Load other properties
                gchar *last_read = g_key_file_get_string(key_file, section_name, "last_read_document", NULL);
                if (last_read) {
                    session_model_set_last_read_document(session, last_read);
                    g_free(last_read);
                }
                
                gchar *page_color = g_key_file_get_string(key_file, section_name, "page_color", NULL);
                if (page_color) {
                    session_model_set_page_color(session, page_color);
                    g_free(page_color);
                }
                
                gchar *last_read_help = g_key_file_get_string(key_file, section_name, "last_read_help_document", NULL);
                if (last_read_help) {
                    session_model_set_last_read_help_document(session, last_read_help);
                    g_free(last_read_help);
                }
                
                gchar *helper_page_color = g_key_file_get_string(key_file, section_name, "helper_page_color", NULL);
                if (helper_page_color) {
                    session_model_set_helper_page_color(session, helper_page_color);
                    g_free(helper_page_color);
                }
                
                // Store session model
                g_hash_table_insert(session_models, g_strdup(*name), session);
                
                g_free(section_name);
            }
        }
        g_strfreev(names_array);
        g_free(names_str);
    }
    if (error) { g_error_free(error); error = NULL; }
    
    // Load last open session
    gchar *last_session = g_key_file_get_string(key_file, "Sessions", "last_open_session", &error);
    if (!error && last_session && sessions_model) {
        sessions_model_set_last_open_session(sessions_model, last_session);
        g_free(last_session);
    } else if (sessions_model) {
        // If no last session was saved, default to "Default"
        sessions_model_set_last_open_session(sessions_model, "Default");
    }
    if (error) { g_error_free(error); }
    
    // Update the sessions treeview with loaded sessions
    populate_sessions_treeview();
    
    // If the loaded last_open_session is different from current_selected_session, update the UI
    if (sessions_model && current_selected_session) {
        const char *loaded_session = sessions_model_get_last_open_session(sessions_model);
        if (loaded_session && strcmp(loaded_session, current_selected_session) != 0) {
            // Update current_selected_session and restore open tabs for the loaded session
            g_free(current_selected_session);
            current_selected_session = g_strdup(loaded_session);
            restore_open_tabs_for_session(loaded_session);
            update_window_title_for_session(current_selected_session);
        }
    }
    
    g_key_file_free(key_file);
    g_free(config_file);
}

void populate_sessions_treeview(void) {
    if (!sessions_list_store) return;
    
    // Clear existing items
    gtk_list_store_clear(sessions_list_store);
    
    // Populate tree view with existing sessions
    const GList *session_names = sessions_model_get_session_names(sessions_model);
    for (const GList *iter = session_names; iter != NULL; iter = iter->next) {
        GtkTreeIter tree_iter;
        gtk_list_store_append(sessions_list_store, &tree_iter);
        gtk_list_store_set(sessions_list_store, &tree_iter, 0, (const char*)iter->data, -1);
    }
}

static void on_horiz_scroll_toggle(GtkToggleButton *button, gpointer user_data) {
    GtkImage *image = GTK_IMAGE(user_data);
    if (gtk_toggle_button_get_active(button)) {
        gtk_image_set_from_file(image, "./data/icons/horiz-scroll-on.png");
    } else {
        gtk_image_set_from_file(image, "./data/icons/horiz-scroll-off.png");
    }
}

static void on_title_bar_toggle(GtkToggleButton *button, gpointer user_data) {
    GtkImage *image = GTK_IMAGE(user_data);
    gboolean active = gtk_toggle_button_get_active(button);
    if (active) {
        gtk_image_set_from_file(image, "./data/icons/title-bar-on.png");
        gtk_window_set_decorated(GTK_WINDOW(window), TRUE);
    } else {
        gtk_image_set_from_file(image, "./data/icons/title-bar-off.png");
        // If window is maximized, need to unmaximize, remove decoration, then re-maximize
        // to ensure it fills the screen properly
        gboolean was_maximized = gtk_window_is_maximized(GTK_WINDOW(window));
        if (was_maximized) {
            gtk_window_unmaximize(GTK_WINDOW(window));
        }
        gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
        if (was_maximized) {
            gtk_window_maximize(GTK_WINDOW(window));
        }
    }
    // Force layout update after changing decoration
    gtk_widget_queue_resize(window);
}

static void on_helper_toggle(GtkToggleButton *button, gpointer user_data) {
    GtkImage *image = GTK_IMAGE(user_data);
    gboolean active = gtk_toggle_button_get_active(button);

    if (active) {
        gtk_image_set_from_file(image, "./data/icons/sidebar-helper-on.png");
        if (right_pane) {
            gtk_widget_show_all(GTK_WIDGET(right_pane));
            set_right_notebook_session(current_selected_session ? current_selected_session : "Default");
        }
    } else {
        // Save current helper tabs before hiding
        if (current_selected_session) {
            save_open_tabs_for_session(current_selected_session);
        }
        gtk_image_set_from_file(image, "./data/icons/sidebar-helper-off.png");
        if (right_pane) {
            gtk_widget_hide(GTK_WIDGET(right_pane));
        }
    }
}

static void on_minimize_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    GtkWindow *window = GTK_WINDOW(user_data);
    gtk_window_iconify(window);
}

static void on_maximize_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    GtkWindow *window = GTK_WINDOW(user_data);
    if (gtk_window_is_maximized(window)) {
        gtk_window_unmaximize(window);
    } else {
        gtk_window_maximize(window);
    }
}

static void on_close_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    save_state();
    gtk_main_quit();
}

static void on_sessions_add_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    
    const char *session_name = gtk_entry_get_text(GTK_ENTRY(sessions_entry));
    if (session_name && strlen(session_name) > 0) {
        // Check if session name already exists
        const GList *existing_sessions = sessions_model_get_session_names(sessions_model);
        for (const GList *iter = existing_sessions; iter != NULL; iter = iter->next) {
            if (strcmp((const char*)iter->data, session_name) == 0) {
                // Show error dialog
                GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(window),
                    GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                    GTK_MESSAGE_ERROR,
                    GTK_BUTTONS_OK,
                    "A session with the name '%s' already exists. Please choose a different name.",
                    session_name);
                gtk_dialog_run(GTK_DIALOG(dialog));
                gtk_widget_destroy(dialog);
                return;
            }
        }
        
        // Add to model
        sessions_model_add_session_name(sessions_model, session_name);

        // Create per-session model so notebook restore can find it
        if (session_models && !g_hash_table_lookup(session_models, session_name)) {
            session_model_t *session = session_model_new();
            session_model_set_session_name(session, session_name);
            g_hash_table_insert(session_models, g_strdup(session_name), session);
        }
        
        // Update tree view
        populate_sessions_treeview();
        
        // Clear entry
        gtk_entry_set_text(GTK_ENTRY(sessions_entry), "");

        // Save state to persist the new session
        save_state();
    }
}

static void on_sessions_remove_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(sessions_tree_view));
    GtkTreeIter iter;
    GtkTreeModel *model;
    
    if (gtk_tree_selection_get_selected(selection, &model, &iter)) {
        gchar *session_name;
        gtk_tree_model_get(model, &iter, 0, &session_name, -1);
        
        // Prevent removing the "Default" session
        if (strcmp(session_name, "Default") == 0) {
            // Show a message dialog that Default session cannot be removed
            GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(window),
                GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                GTK_MESSAGE_INFO,
                GTK_BUTTONS_OK,
                "The 'Default' session cannot be removed.");
            gtk_dialog_run(GTK_DIALOG(dialog));
            gtk_widget_destroy(dialog);
            g_free(session_name);
            return;
        }
        
        // Remove from model
        sessions_model_remove_session_name(sessions_model, session_name);
        
        // Update tree view
        populate_sessions_treeview();

        // If the removed session is currently selected, switch to "Default" or first remaining session
        if (current_selected_session && strcmp(current_selected_session, session_name) == 0) {
            g_free(current_selected_session);
            current_selected_session = g_strdup("Default"); // or first remaining session

            restore_open_tabs_for_session(current_selected_session);

            if (sessions_model) {
                sessions_model_set_last_open_session(sessions_model, current_selected_session);
            }

            update_window_title_for_session(current_selected_session);
        }
        
        g_free(session_name);

        // Save state to persist the removed session
        save_state();
    }
}

static void on_sessions_update_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(sessions_tree_view));
    GtkTreeIter iter;
    GtkTreeModel *model;
    
    if (gtk_tree_selection_get_selected(selection, &model, &iter)) {
        const char *new_name = gtk_entry_get_text(GTK_ENTRY(sessions_entry));
        if (new_name && strlen(new_name) > 0) {
            gchar *old_name;
            gtk_tree_model_get(model, &iter, 0, &old_name, -1);
            
            // Prevent renaming the "Default" session
            if (strcmp(old_name, "Default") == 0) {
                GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(window),
                    GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                    GTK_MESSAGE_INFO,
                    GTK_BUTTONS_OK,
                    "The 'Default' session cannot be renamed.");
                gtk_dialog_run(GTK_DIALOG(dialog));
                gtk_widget_destroy(dialog);
                g_free(old_name);
                return;
            }
            
            // Check if new name already exists (excluding the current session)
            const GList *existing_sessions = sessions_model_get_session_names(sessions_model);
            for (const GList *iter_check = existing_sessions; iter_check != NULL; iter_check = iter_check->next) {
                if (strcmp((const char*)iter_check->data, old_name) != 0 && 
                    strcmp((const char*)iter_check->data, new_name) == 0) {
                    // Show error dialog
                    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(window),
                        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                        GTK_MESSAGE_ERROR,
                        GTK_BUTTONS_OK,
                        "A session with the name '%s' already exists. Please choose a different name.",
                        new_name);
                    gtk_dialog_run(GTK_DIALOG(dialog));
                    gtk_widget_destroy(dialog);
                    g_free(old_name);
                    return;
                }
            }
            
            // Update in model
            sessions_model_remove_session_name(sessions_model, old_name);
            sessions_model_add_session_name(sessions_model, new_name);
            
            // Update tree view
            populate_sessions_treeview();

            // If the updated session is currently selected, update current_selected_session and window title
            if (current_selected_session && strcmp(current_selected_session, old_name) == 0) {
                g_free(current_selected_session);
                current_selected_session = g_strdup(new_name);
                if (sessions_model) {
                    sessions_model_set_last_open_session(sessions_model, current_selected_session);
                }
                update_window_title_for_session(current_selected_session);
            }
            
            // Clear entry
            gtk_entry_set_text(GTK_ENTRY(sessions_entry), "");
            
            g_free(old_name);
        }
    }
}

static void set_right_notebook_session(const gchar *session_name) {
    if (!right_notebook || !session_name || !session_models) return;

    // Clear current right notebook
    while (gtk_notebook_get_n_pages(GTK_NOTEBOOK(right_notebook)) > 0) {
        gtk_notebook_remove_page(GTK_NOTEBOOK(right_notebook), 0);
    }

    // Get the session model
    session_model_t *session = g_hash_table_lookup(session_models, session_name);
    if (!session) return;

    // Restore saved helper documents in right notebook
    const GList *helper_docs = session_model_get_helper_document_urls(session);
    for (const GList *iter = helper_docs; iter != NULL; iter = iter->next) {
        const char *uri = (const char*)iter->data;
        char *filename = g_filename_from_uri(uri, NULL, NULL);
        if (filename) {
            TabData *tab = create_new_tab(right_notebook);
            if (tab) {
                load_file_into_tab(tab, filename);
            }
            g_free(filename);
        }
    }

    // If no helper documents, show an empty right notebook
    if (gtk_notebook_get_n_pages(GTK_NOTEBOOK(right_notebook)) == 0) {
        gtk_widget_show_all(right_notebook);
    }
}

static gboolean on_sessions_treeview_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data) {
    (void)user_data;
    if (event->type == GDK_BUTTON_PRESS && event->button == GDK_BUTTON_PRIMARY) {
        GtkTreePath *path = NULL;
        if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(widget), (gint)event->x, (gint)event->y, &path, NULL, NULL, NULL)) {
            GtkTreeIter iter;
            GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(widget));
            if (gtk_tree_model_get_iter(model, &iter, path)) {
                gchar *session_name = NULL;
                gtk_tree_model_get(model, &iter, 0, &session_name, -1);
                if (session_name) {
                    // No-op if clicking the already selected session
                    if (current_selected_session && g_strcmp0(current_selected_session, session_name) == 0) {
                        g_free(session_name);
                        gtk_tree_path_free(path);
                        return FALSE;
                    }

                    // Save current session's open tabs before switching
                    if (current_selected_session) {
                        save_open_tabs_for_session(current_selected_session);
                    }

                    // Switch selected session FIRST so notebook callbacks use the right session
                    if (current_selected_session) {
                        g_free(current_selected_session);
                    }
                    current_selected_session = g_strdup(session_name);

                    // Restore tabs for the new active session
                    restore_open_tabs_for_session(session_name);

                    // Update window title
                    update_window_title_for_session(current_selected_session);

                    // Persist "last open session" in sessions model
                    if (sessions_model) {
                        sessions_model_set_last_open_session(sessions_model, session_name);
                    }

                    g_free(session_name);
                }
            }
            gtk_tree_path_free(path);
        }
    }
    return FALSE;
}

static void on_sessions_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;

    if (current_sidebar_mode == SIDEBAR_SESSIONS) {
        gtk_container_remove(GTK_CONTAINER(main_hbox), sidebar);
        gtk_box_reorder_child(GTK_BOX(main_hbox), content_vbox, 1);
        current_sidebar_mode = SIDEBAR_NONE;
    } else {
        if (gtk_widget_get_parent(sidebar) != NULL) {
            gtk_container_remove(GTK_CONTAINER(main_hbox), sidebar);
        }
        
        // Hide other sidebar contents
        gtk_widget_hide(sidebar_label);
        gtk_widget_hide(sessions_container);
        
        // Show sessions container
        gtk_widget_show_all(sessions_container);
        
        gtk_box_pack_start(GTK_BOX(main_hbox), sidebar, FALSE, FALSE, 0);
        gtk_box_reorder_child(GTK_BOX(main_hbox), content_vbox, 2);
        gtk_widget_show(sidebar);
        current_sidebar_mode = SIDEBAR_SESSIONS;
    }
}

static void on_toc_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    if (current_sidebar_mode == SIDEBAR_TOC) {
        gtk_container_remove(GTK_CONTAINER(main_hbox), sidebar);
        gtk_box_reorder_child(GTK_BOX(main_hbox), content_vbox, 1);
        current_sidebar_mode = SIDEBAR_NONE;
    } else {
        if (gtk_widget_get_parent(sidebar) != NULL) {
            gtk_container_remove(GTK_CONTAINER(main_hbox), sidebar);
        }
        
        // Hide other sidebar contents
        gtk_widget_hide(sidebar_label);
        gtk_widget_hide(sessions_container);
        
        // Show sidebar_label with TOC text
        gtk_label_set_text(GTK_LABEL(sidebar_label), "Table of Contents\n\n• Chapter 1\n• Chapter 2\n• Chapter 3\n\nSelect a section to navigate.");
        gtk_widget_show_all(sidebar_label);
        
        gtk_box_pack_start(GTK_BOX(main_hbox), sidebar, FALSE, FALSE, 0);
        gtk_box_reorder_child(GTK_BOX(main_hbox), content_vbox, 2);
        gtk_widget_show(sidebar);
        current_sidebar_mode = SIDEBAR_TOC;
    }
}

static void on_settings_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    if (current_sidebar_mode == SIDEBAR_SETTINGS) {
        gtk_container_remove(GTK_CONTAINER(main_hbox), sidebar);
        gtk_box_reorder_child(GTK_BOX(main_hbox), content_vbox, 1);
        current_sidebar_mode = SIDEBAR_NONE;
    } else {
        if (gtk_widget_get_parent(sidebar) != NULL) {
            gtk_container_remove(GTK_CONTAINER(main_hbox), sidebar);
        }
        
        // Hide other sidebar contents
        gtk_widget_hide(sidebar_label);
        gtk_widget_hide(sessions_container);
        
        // Show sidebar_label with settings text
        gtk_label_set_text(GTK_LABEL(sidebar_label), "Settings\n\n• Display options\n• Keyboard shortcuts\n• Preferences\n\nConfigure application settings.");
        gtk_widget_show_all(sidebar_label);
        
        gtk_box_pack_start(GTK_BOX(main_hbox), sidebar, FALSE, FALSE, 0);
        gtk_box_reorder_child(GTK_BOX(main_hbox), content_vbox, 2);
        gtk_widget_show(sidebar);
        current_sidebar_mode = SIDEBAR_SETTINGS;
    }
}

static void save_open_tabs_for_session(const char *session_name) {
    if (!session_name || !session_models) return;
    
    session_model_t *session = g_hash_table_lookup(session_models, session_name);
    if (!session) return;
    
    // Replace current saved document URLs with the currently open tabs.
    if (session->document_urls) {
        g_list_free_full(session->document_urls, g_free);
        session->document_urls = NULL;
    }
    if (session->helper_document_urls) {
        g_list_free_full(session->helper_document_urls, g_free);
        session->helper_document_urls = NULL;
    }
    
    // Save open tabs from left notebook
    if (left_notebook) {
        int n_pages = gtk_notebook_get_n_pages(GTK_NOTEBOOK(left_notebook));
        for (int i = 0; i < n_pages; i++) {
            GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(left_notebook), i);
            if (page) {
                TabData *tab = g_object_get_data(G_OBJECT(page), "tab-data");
                if (tab && tab->current_file) {
                    char *uri = g_filename_to_uri(tab->current_file, NULL, NULL);
                    if (uri) {
                        session_model_add_document_url(session, uri);
                        g_free(uri);
                    }
                }
            }
        }
    }
    
    // Save open tabs from right notebook
    if (right_notebook) {
        int n_pages = gtk_notebook_get_n_pages(GTK_NOTEBOOK(right_notebook));
        for (int i = 0; i < n_pages; i++) {
            GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(right_notebook), i);
            if (page) {
                TabData *tab = g_object_get_data(G_OBJECT(page), "tab-data");
                if (tab && tab->current_file) {
                    char *uri = g_filename_to_uri(tab->current_file, NULL, NULL);
                    if (uri) {
                        session_model_add_helper_document_url(session, uri);
                        g_free(uri);
                    }
                }
            }
        }
    }

    /* Also snapshot the currently focused tabs as last-read markers */
    if (left_notebook) {
        int cur = gtk_notebook_get_current_page(GTK_NOTEBOOK(left_notebook));
        if (cur >= 0) {
            GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(left_notebook), cur);
            if (page) {
                update_last_read_for_notebook(GTK_NOTEBOOK(left_notebook), page, (guint)cur);
            }
        } else {
            session_model_set_last_read_document(session, "");
        }
    }

    if (right_notebook) {
        int cur = gtk_notebook_get_current_page(GTK_NOTEBOOK(right_notebook));
        if (cur >= 0) {
            GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(right_notebook), cur);
            if (page) {
                update_last_read_for_notebook(GTK_NOTEBOOK(right_notebook), page, (guint)cur);
            }
        } else {
            session_model_set_last_read_help_document(session, "");
        }
    }
}

static void restore_open_tabs_for_session(const char *session_name) {
    if (!session_name || !session_models) return;

    session_model_t *session = g_hash_table_lookup(session_models, session_name);
    if (!session) {
        session = session_model_new();
        session_model_set_session_name(session, session_name);
        g_hash_table_insert(session_models, g_strdup(session_name), session);
    }

    is_restoring_session_tabs = TRUE;

    // Clear current notebooks
    if (left_notebook) {
        int n_pages = gtk_notebook_get_n_pages(GTK_NOTEBOOK(left_notebook));
        for (int i = n_pages - 1; i >= 0; i--) {
            gtk_notebook_remove_page(GTK_NOTEBOOK(left_notebook), i);
        }
    }

    if (right_notebook) {
        int n_pages = gtk_notebook_get_n_pages(GTK_NOTEBOOK(right_notebook));
        for (int i = n_pages - 1; i >= 0; i--) {
            gtk_notebook_remove_page(GTK_NOTEBOOK(right_notebook), i);
        }
    }

    const char *last_read_uri = session_model_get_last_read_document(session);
    const char *last_read_help_uri = session_model_get_last_read_help_document(session);

    int matched_left_index = -1;
    int matched_right_index = -1;
    int left_index = 0;
    int right_index = 0;

    // Restore saved documents in left notebook
    const GList *docs = session_model_get_document_urls(session);
    for (const GList *iter = docs; iter != NULL; iter = iter->next) {
        const char *uri = (const char*)iter->data;
        char *filename = g_filename_from_uri(uri, NULL, NULL);
        if (filename) {
            TabData *tab = create_new_tab(left_notebook);
            if (tab) {
                load_file_into_tab(tab, filename);
            }
            g_free(filename);
        }

        if (last_read_uri && *last_read_uri && g_strcmp0(uri, last_read_uri) == 0) {
            matched_left_index = left_index;
        }
        left_index++;
    }

    // Restore saved helper documents in right notebook
    const GList *helper_docs = session_model_get_helper_document_urls(session);
    for (const GList *iter = helper_docs; iter != NULL; iter = iter->next) {
        const char *uri = (const char*)iter->data;
        char *filename = g_filename_from_uri(uri, NULL, NULL);
        if (filename) {
            TabData *tab = create_new_tab(right_notebook);
            if (tab) {
                load_file_into_tab(tab, filename);
            }
            g_free(filename);
        }

        if (last_read_help_uri && *last_read_help_uri && g_strcmp0(uri, last_read_help_uri) == 0) {
            matched_right_index = right_index;
        }
        right_index++;
    }

    // Re-focus last-read tabs if present
    if (left_notebook && matched_left_index >= 0 &&
        matched_left_index < gtk_notebook_get_n_pages(GTK_NOTEBOOK(left_notebook))) {
        gtk_notebook_set_current_page(GTK_NOTEBOOK(left_notebook), matched_left_index);
    }

    if (right_notebook && matched_right_index >= 0 &&
        matched_right_index < gtk_notebook_get_n_pages(GTK_NOTEBOOK(right_notebook))) {
        gtk_notebook_set_current_page(GTK_NOTEBOOK(right_notebook), matched_right_index);
    }

    is_restoring_session_tabs = FALSE;
}

static void queue_draw(TabData *tab) {
    if (tab && tab->pages_drawing)
        gtk_widget_queue_draw(tab->pages_drawing);
}

static gboolean on_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data) {
    TabData *tab = user_data;
    GtkAllocation alloc;
    gtk_widget_get_allocation(widget, &alloc);

    if (!tab || !tab->doc) {
        return FALSE;
    }

    /* continuous mode: draw multiple pages vertically inside this drawing area
       Render only pages intersecting the current clip extents to save work. */
    double clip_x1, clip_y1, clip_x2, clip_y2;
    cairo_clip_extents(cr, &clip_x1, &clip_y1, &clip_x2, &clip_y2);

    const double spacing = 6.0;
    if (tab->layout_mode == 0) {
        int target_w = alloc.width - 40;
        if (target_w < 100) target_w = alloc.width;
        double y = spacing; /* top padding */
        for (int i = 0; i < tab->n_pages; ++i) {
            PopplerPage *page = poppler_document_get_page(tab->doc, i);
            if (!page) continue;
            double pw, ph;
            poppler_page_get_size(page, &pw, &ph);
            if (pw <= 0 || ph <= 0) { g_object_unref(page); continue; }
            double scale = (double)target_w / pw * (tab->zoom > 0 ? tab->zoom : 1.0);
            double page_w = pw * scale;
            double page_h = ph * scale;
            double off_x = (alloc.width - page_w) / 2.0;
            double off_y = y;

            /* skip if page is outside clip */
            if (!(off_y + page_h < clip_y1 || off_y > clip_y2)) {
                /* draw background rectangle */
                cairo_save(cr);
                cairo_set_source_rgba(cr, tab->page_color.red, tab->page_color.green, tab->page_color.blue, tab->page_color.alpha);
                cairo_rectangle(cr, off_x, off_y, page_w, page_h);
                cairo_fill(cr);
                cairo_restore(cr);
                cairo_save(cr);
                cairo_translate(cr, off_x, off_y);
                cairo_scale(cr, scale, scale);
                poppler_page_render(page, cr);
                cairo_restore(cr);
            }

            y += page_h + spacing;
            g_object_unref(page);
        }
    }

    return FALSE;
}

static void build_continuous_view(TabData *tab, int target_width) {
    if (!tab || !tab->doc || !tab->pages_drawing) return;
    const double spacing = 6.0;
    if (tab->layout_mode == 0) {
        /* single-column (top-to-bottom) */
        double total_h = 0.0;
        for (int i = 0; i < tab->n_pages; ++i) {
            PopplerPage *page = poppler_document_get_page(tab->doc, i);
            if (!page) continue;
            double pw, ph;
            poppler_page_get_size(page, &pw, &ph);
            double scale = (double)target_width / pw * (tab->zoom > 0 ? tab->zoom : 1.0);
            double page_h = ph * scale;
            total_h += page_h + spacing;
            g_object_unref(page);
        }
        if (total_h < 1.0) total_h = 1.0;
        gtk_widget_set_size_request(tab->pages_drawing, target_width, (int)ceil(total_h));
    }
    gtk_widget_queue_draw(tab->pages_drawing);
}

static void scroll_to_page(TabData *tab, int page) {
    if (!tab || !tab->scrolled || !tab->pages_drawing || !tab->doc) return;
    if (page < 0 || page >= tab->n_pages) return;
    /* compute vertical offset of requested page */
    int target_w = 800;
    GtkAllocation salloc;
    gtk_widget_get_allocation(tab->scrolled, &salloc);
    if (salloc.width > 100) target_w = salloc.width - 40;

    double y = 0.0;
    for (int i = 0; i < page; ++i) {
        PopplerPage *p = poppler_document_get_page(tab->doc, i);
        if (!p) continue;
        double pw, ph;
        poppler_page_get_size(p, &pw, &ph);
        double scale = (double)target_w / pw * (tab->zoom > 0 ? tab->zoom : 1.0);
        double page_h = ph * scale;
        y += page_h + 6;
        g_object_unref(p);
    }
    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(tab->scrolled));
    gtk_adjustment_set_value(vadj, y);
}

static void on_scroll_value_changed(GtkAdjustment *adj, gpointer user_data) {
    TabData *tab = user_data;
    if (!tab || !tab->doc) return;
    
    /* Calculate which page is at the top of the visible area */
    double scroll_y = gtk_adjustment_get_value(adj);
    const double spacing = 6.0;
    int target_w = 800;
    if (tab->scrolled) {
        GtkAllocation alloc;
        gtk_widget_get_allocation(tab->scrolled, &alloc);
        if (alloc.width > 100) target_w = alloc.width - 40;
    }
    
    double y = spacing;
    int visible_page = 0;
    
    if (tab->layout_mode == 0) {
        /* single-column layout */
        for (int i = 0; i < tab->n_pages; ++i) {
            PopplerPage *page = poppler_document_get_page(tab->doc, i);
            if (!page) continue;
            double pw, ph;
            poppler_page_get_size(page, &pw, &ph);
            if (pw <= 0 || ph <= 0) { g_object_unref(page); continue; }
            double scale = (double)target_w / pw * (tab->zoom > 0 ? tab->zoom : 1.0);
            double page_h = ph * scale;
            if (y + page_h / 2.0 > scroll_y) {
                visible_page = i;
                g_object_unref(page);
                break;
            }
            y += page_h + spacing;
            g_object_unref(page);
        }
    }
    
    tab->cur_page = visible_page;
}

static void load_file_into_tab(TabData *tab, const char *filename) {
    if (!tab || !filename) return;
    GError *error = NULL;
    char *uri = g_filename_to_uri(filename, NULL, &error);
    if (!uri) {
        g_warning("Failed to convert filename to URI: %s", error->message);
        g_clear_error(&error);
        return;
    }

    PopplerDocument *doc = poppler_document_new_from_file(uri, NULL, &error);
    g_free(uri);

    if (!doc) {
        g_warning("Failed to open PDF: %s", error->message);
        g_clear_error(&error);
        return;
    }

    if (tab->doc)
        g_object_unref(tab->doc);

    tab->doc = doc;
    tab->n_pages = poppler_document_get_n_pages(doc);
    tab->cur_page = 0;
    tab->zoom = 1.0;
    /* track current filename for per-document settings */
    if (tab->current_file)
        g_free(tab->current_file);
    tab->current_file = g_strdup(filename);

    /* Update the tab's label with filename */
    if (tab->tab_label) {
        const char *basename = g_path_get_basename(filename);
        gtk_label_set_text(GTK_LABEL(tab->tab_label), basename);
    }

    queue_draw(tab);

    {
        /* rebuild continuous pages using scrolled allocation if available */
        int target_w = 800;
        if (tab->scrolled) {
            GtkAllocation alloc;
            gtk_widget_get_allocation(tab->scrolled, &alloc);
            if (alloc.width > 100) target_w = alloc.width - 40;
        }
        build_continuous_view(tab, target_w);
        scroll_to_page(tab, tab->cur_page);
    }
}

static TabData *create_new_tab(GtkWidget *notebook) {
    TabData *tab = g_malloc0(sizeof(TabData));
    tab->zoom = 1.0;
    tab->layout_mode = 0;    /* single-column by default */
    tab->n_pages = 0;
    tab->cur_page = 0;
    
    if (!notebook || !GTK_IS_NOTEBOOK(notebook)) {
        g_free(tab);
        return NULL;
    }

    gdk_rgba_parse(&tab->page_color, "white");
    
    /* Create container for tab content */
    GtkWidget *tab_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    
    /* Create scrolled window and a single drawing area used for both single and continuous views */
    tab->scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_vexpand(tab->scrolled, TRUE);
    gtk_widget_set_hexpand(tab->scrolled, TRUE);
    /* connect scroll adjustment to update page display */
    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(tab->scrolled));
    g_signal_connect(G_OBJECT(vadj), "value-changed", G_CALLBACK(on_scroll_value_changed), tab);
    tab->pages_drawing = gtk_drawing_area_new();
    gtk_widget_set_hexpand(tab->pages_drawing, TRUE);
    gtk_widget_set_vexpand(tab->pages_drawing, TRUE);
    g_signal_connect(G_OBJECT(tab->pages_drawing), "draw", G_CALLBACK(on_draw), tab);
    gtk_container_add(GTK_CONTAINER(tab->scrolled), tab->pages_drawing);
    gtk_box_pack_start(GTK_BOX(tab_box), tab->scrolled, TRUE, TRUE, 0);
    
    gtk_widget_show_all(tab_box);
    
    /* Store tab data in the widget */
    g_object_set_data(G_OBJECT(tab_box), "tab-data", tab);
    
    /* Create tab label with close button */
    GtkWidget *label_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *label = gtk_label_new("New Document");
    gtk_box_pack_start(GTK_BOX(label_box), label, FALSE, FALSE, 0);
    tab->tab_label = label;  /* Store reference to label for updates */
    GtkWidget *close_btn = gtk_button_new_with_label("×");
    gtk_widget_set_size_request(close_btn, 30, -1);
    /* allocate CloseInfo linking the notebook and this page so close removes correct page */
    typedef struct {
        GtkNotebook *notebook;
        GtkWidget *page;
    } CloseInfo;
    CloseInfo *ci = g_malloc(sizeof(CloseInfo));
    ci->notebook = GTK_NOTEBOOK(notebook);
    ci->page = tab_box;
    g_signal_connect(close_btn, "clicked", G_CALLBACK(on_tab_close_clicked), ci);
    gtk_box_pack_start(GTK_BOX(label_box), close_btn, FALSE, FALSE, 0);
    gtk_widget_show_all(label_box);
    
    /* Add tab to notebook */
    int page_num = gtk_notebook_append_page(GTK_NOTEBOOK(notebook), tab_box, label_box);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), page_num);
    
    return tab;
}

static void open_file_in_notebook(GtkWidget *notebook, gboolean is_helper) {
    if (!notebook) return;
    GtkWidget *dialog = gtk_file_chooser_dialog_new("Open PDF",
                                                   GTK_WINDOW(window),
                                                   GTK_FILE_CHOOSER_ACTION_OPEN,
                                                   "_Cancel", GTK_RESPONSE_CANCEL,
                                                   "_Open", GTK_RESPONSE_ACCEPT,
                                                   NULL);

    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_add_pattern(filter, "*.pdf");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
    char *fname = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
    if (fname) {
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
                if (current_selected_session) {
                    session_model_t *session = g_hash_table_lookup(session_models, current_selected_session);
                    if (session && uri) {
                        if (is_helper) {
                            session_model_add_helper_document_url(session, uri);
                        } else {
                            session_model_add_document_url(session, uri);
                            /* TODO: Implement adding to tree view */
                        }
                    }
                }
            }
        }
        if (uri) g_free(uri);
        g_free(fname);
        }
    }
    gtk_widget_destroy(dialog);
}

static void on_open_file_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    open_file_in_notebook(left_notebook, FALSE);
}

static void on_open_helper_file_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    open_file_in_notebook(right_notebook, TRUE);
}

static int find_matching_tab_index(GtkNotebook *notebook, const char *target_uri) {
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

static void update_last_read_for_notebook(GtkNotebook *notebook, GtkWidget *page, guint page_num) {
    (void)page_num;
    if (is_restoring_session_tabs) return;
    if (!notebook || !page || !current_selected_session || !session_models) return;
    session_model_t *session = g_hash_table_lookup(session_models, current_selected_session);
    if (!session) return;
    TabData *tab = g_object_get_data(G_OBJECT(page), "tab-data");
    if (!tab || !tab->current_file) return;
    char *uri = g_filename_to_uri(tab->current_file, NULL, NULL);
    if (!uri) return;
    if (notebook == GTK_NOTEBOOK(left_notebook)) {
        session_model_set_last_read_document(session, uri);
    } else if (notebook == GTK_NOTEBOOK(right_notebook)) {
        session_model_set_last_read_help_document(session, uri);
    }
    g_free(uri);
}

static void on_left_notebook_switch_page(GtkNotebook *notebook, GtkWidget *page, guint page_num, gpointer user_data) {
    (void)user_data;
    update_last_read_for_notebook(notebook, page, page_num);
}

static void on_right_notebook_switch_page(GtkNotebook *notebook, GtkWidget *page, guint page_num, gpointer user_data) {
    (void)user_data;
    update_last_read_for_notebook(notebook, page, page_num);
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

    gboolean is_left = (ci->notebook == GTK_NOTEBOOK(left_notebook));
    gboolean is_right = (ci->notebook == GTK_NOTEBOOK(right_notebook));
    session_model_t *session = NULL;
    if (current_selected_session && session_models) {
        session = g_hash_table_lookup(session_models, current_selected_session);
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
                if (tab->doc) g_object_unref(tab->doc);
                if (tab->current_file) g_free(tab->current_file);
                g_free(tab);
            }
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
    g_free(ci);
}

GtkWidget* create_main_window(void) {
    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "Siters");
    gtk_window_set_default_size(GTK_WINDOW(window), 1000, 800);

    // Set minimal size to prevent unusable layouts
    GdkGeometry hints = {0};
    hints.min_width = 1000;
    hints.min_height = 800;
    gtk_window_set_geometry_hints(GTK_WINDOW(window), NULL, &hints, GDK_HINT_MIN_SIZE);

    // Initialize sessions model
    if (!sessions_model) {
        sessions_model = sessions_model_new();
        // Initialize session models hash table
        session_models = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)session_model_free);
        // Always ensure "Default" session exists
        const GList *existing_sessions = sessions_model_get_session_names(sessions_model);
        gboolean has_default = FALSE;
        for (const GList *iter = existing_sessions; iter != NULL; iter = iter->next) {
            if (strcmp((const char*)iter->data, "Default") == 0) {
                has_default = TRUE;
                break;
            }
        }
        if (!has_default) {
            sessions_model_add_session_name(sessions_model, "Default");
            // Create session model for Default
            session_model_t *default_session = session_model_new();
            session_model_set_session_name(default_session, "Default");
            g_hash_table_insert(session_models, g_strdup("Default"), default_session);
        } else {
            // Ensure we have a session model for Default
            if (!g_hash_table_lookup(session_models, "Default")) {
                session_model_t *default_session = session_model_new();
                session_model_set_session_name(default_session, "Default");
                g_hash_table_insert(session_models, g_strdup("Default"), default_session);
            }
        }
    }

    g_signal_connect(window, "destroy", G_CALLBACK(on_window_destroy), NULL);
    g_signal_connect(window, "configure-event", G_CALLBACK(on_window_configure), NULL);

    /* Main horizontal container: toolbar on left, content on right */
    main_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_container_add(GTK_CONTAINER(window), main_hbox);

    /* Left sidebar: main toolbar */
    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(toolbar), "Toolbar");
    gtk_box_pack_start(GTK_BOX(main_hbox), toolbar, FALSE, FALSE, 0);

    /* Sidebar for sessions, toc, settings */
    sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    g_object_ref(sidebar);  /* Keep a reference to prevent destruction when removed */
    gtk_widget_set_size_request(sidebar, 200, -1);
    atk_object_set_name(gtk_widget_get_accessible(sidebar), "Sidebar");

    /* Content for sidebar */
    sidebar_label = gtk_label_new("");
    gtk_label_set_justify(GTK_LABEL(sidebar_label), GTK_JUSTIFY_LEFT);
    atk_object_set_name(gtk_widget_get_accessible(sidebar_label), "Sidebar label");
    g_object_ref(sidebar_label);  /* Keep a reference to prevent destruction when removed */
    gtk_box_pack_start(GTK_BOX(sidebar), sidebar_label, TRUE, TRUE, 0);
    gtk_widget_hide(sidebar_label);

    /* Sessions container */
    sessions_container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_set_border_width(GTK_CONTAINER(sessions_container), 5);
    g_object_ref(sessions_container);  /* Keep a reference */
    
    // Title
    sessions_title = gtk_label_new("Sessions");
    gtk_widget_set_halign(sessions_title, GTK_ALIGN_START);
    PangoAttrList *attr_list = pango_attr_list_new();
    PangoAttribute *attr = pango_attr_weight_new(PANGO_WEIGHT_BOLD);
    pango_attr_list_insert(attr_list, attr);
    attr = pango_attr_scale_new(PANGO_SCALE_LARGE);
    pango_attr_list_insert(attr_list, attr);
    gtk_label_set_attributes(GTK_LABEL(sessions_title), attr_list);
    pango_attr_list_unref(attr_list);
    gtk_box_pack_start(GTK_BOX(sessions_container), sessions_title, FALSE, FALSE, 0);
    
    // Entry field
    sessions_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(sessions_entry), "Enter session name...");
    gtk_box_pack_start(GTK_BOX(sessions_container), sessions_entry, FALSE, FALSE, 0);
    atk_object_set_name(gtk_widget_get_accessible(sessions_entry), "Sessions entry");
    
    // Buttons box
    GtkWidget *buttons_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_pack_start(GTK_BOX(sessions_container), buttons_box, FALSE, FALSE, 0);
    
    sessions_add_btn = gtk_button_new_with_label("Add");
    gtk_widget_set_tooltip_text(sessions_add_btn, "Add new session");
    atk_object_set_name(gtk_widget_get_accessible(sessions_add_btn), "Add session");
    g_signal_connect(sessions_add_btn, "clicked", G_CALLBACK(on_sessions_add_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(buttons_box), sessions_add_btn, TRUE, TRUE, 0);
    
    sessions_remove_btn = gtk_button_new_with_label("Remove");
    gtk_widget_set_tooltip_text(sessions_remove_btn, "Remove selected session");
    atk_object_set_name(gtk_widget_get_accessible(sessions_remove_btn), "Remove session");
    g_signal_connect(sessions_remove_btn, "clicked", G_CALLBACK(on_sessions_remove_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(buttons_box), sessions_remove_btn, TRUE, TRUE, 0);
    
    sessions_update_btn = gtk_button_new_with_label("Update");
    gtk_widget_set_tooltip_text(sessions_update_btn, "Update selected session name");
    atk_object_set_name(gtk_widget_get_accessible(sessions_update_btn), "Update session");
    g_signal_connect(sessions_update_btn, "clicked", G_CALLBACK(on_sessions_update_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(buttons_box), sessions_update_btn, TRUE, TRUE, 0);
    
    // Tree view
    sessions_list_store = gtk_list_store_new(1, G_TYPE_STRING);
    sessions_tree_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(sessions_list_store));
    g_object_unref(sessions_list_store);
    
    GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes("Session Name", renderer, "text", 0, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(sessions_tree_view), column);

    g_signal_connect(sessions_tree_view, "button-press-event", G_CALLBACK(on_sessions_treeview_button_press), NULL);
    
    // Scrolled window for tree view
    GtkWidget *scrolled_window = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scrolled_window), sessions_tree_view);
    gtk_box_pack_start(GTK_BOX(sessions_container), scrolled_window, TRUE, TRUE, 0);
    
    // Populate tree view with existing sessions
    populate_sessions_treeview();
    
    gtk_box_pack_start(GTK_BOX(sidebar), sessions_container, TRUE, TRUE, 0);
    gtk_widget_hide(sessions_container);

    /* Content area on the right*/
    content_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(main_hbox), content_vbox, TRUE, TRUE, 0);

    /* Buttons*/
    /* Sessions button */
    GtkWidget *sessions_icon = gtk_image_new_from_file("./data/icons/sessions.png");
    GtkWidget *sessions_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(sessions_btn), sessions_icon);
    gtk_widget_set_tooltip_text(sessions_btn, "Sessions");
    atk_object_set_name(gtk_widget_get_accessible(sessions_btn), "Sessions");
    g_signal_connect(sessions_btn, "clicked", G_CALLBACK(on_sessions_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(toolbar), sessions_btn, FALSE, FALSE, 1);

    /* Table of contents button */
    GtkWidget *toc_icon = gtk_image_new_from_file("./data/icons/toc.png");
    GtkWidget *toc_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(toc_btn), toc_icon);
    gtk_widget_set_tooltip_text(toc_btn, "Table of contents");
    atk_object_set_name(gtk_widget_get_accessible(toc_btn), "Table of contents");
    g_signal_connect(toc_btn, "clicked", G_CALLBACK(on_toc_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(toolbar), toc_btn, FALSE, FALSE, 1);

    /* Settings button */
    GtkWidget *settings_icon = gtk_image_new_from_file("./data/icons/settings.png");
    GtkWidget *settings_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(settings_btn), settings_icon);
    gtk_widget_set_tooltip_text(settings_btn, "Settings");
    atk_object_set_name(gtk_widget_get_accessible(settings_btn), "Settings");
    g_signal_connect(settings_btn, "clicked", G_CALLBACK(on_settings_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(toolbar), settings_btn, FALSE, FALSE, 1);

    /* Separator */
    GtkWidget *separator_a = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(toolbar), separator_a, FALSE, FALSE, 5);

    /* Open file button */
    GtkWidget *open_file_icon = gtk_image_new_from_file("./data/icons/file-plus.png");
    GtkWidget *open_file_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(open_file_btn), open_file_icon);
    gtk_widget_set_tooltip_text(open_file_btn, "Open file");
    atk_object_set_name(gtk_widget_get_accessible(open_file_btn), "Open file");
    g_signal_connect(open_file_btn, "clicked", G_CALLBACK(on_open_file_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(toolbar), open_file_btn, FALSE, FALSE, 1);

    /* Close file button*/
    GtkWidget *close_file_icon = gtk_image_new_from_file("./data/icons/file-minus.png");
    GtkWidget *close_file_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(close_file_btn), close_file_icon);
    gtk_widget_set_tooltip_text(close_file_btn, "Close file");
    atk_object_set_name(gtk_widget_get_accessible(close_file_btn), "Close file");
    gtk_box_pack_start(GTK_BOX(toolbar), close_file_btn, FALSE, FALSE, 1);

    /* Separator */
    GtkWidget *separator_b = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(toolbar), separator_b, FALSE, FALSE, 5);

    /* Page up button*/
    GtkWidget *page_up_icon = gtk_image_new_from_file("./data/icons/page-up.png");
    GtkWidget *page_up_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(page_up_btn), page_up_icon);
    gtk_widget_set_tooltip_text(page_up_btn, "Page up");
    atk_object_set_name(gtk_widget_get_accessible(page_up_btn), "Page up");
    gtk_box_pack_start(GTK_BOX(toolbar), page_up_btn, FALSE, FALSE, 1);

    /* Page down button*/
    GtkWidget *page_down_icon = gtk_image_new_from_file("./data/icons/page-down.png");
    GtkWidget *page_down_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(page_down_btn), page_down_icon);
    gtk_widget_set_tooltip_text(page_down_btn, "Page down");
    atk_object_set_name(gtk_widget_get_accessible(page_down_btn), "Page down");
    gtk_box_pack_start(GTK_BOX(toolbar), page_down_btn, FALSE, FALSE, 1);

    /* Separator */
    GtkWidget *separator_c = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(toolbar), separator_c, FALSE, FALSE, 5);

    /* Zoom in button*/
    GtkWidget *zoom_in_icon = gtk_image_new_from_file("./data/icons/zoom-in.png");
    GtkWidget *zoom_in_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(zoom_in_btn), zoom_in_icon);
    gtk_widget_set_tooltip_text(zoom_in_btn, "Zoom in");
    atk_object_set_name(gtk_widget_get_accessible(zoom_in_btn), "Zoom in");
    gtk_box_pack_start(GTK_BOX(toolbar), zoom_in_btn, FALSE, FALSE, 1);

    /* Zoom out button*/
    GtkWidget *zoom_out_icon = gtk_image_new_from_file("./data/icons/zoom-out.png");
    GtkWidget *zoom_out_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(zoom_out_btn), zoom_out_icon);
    gtk_widget_set_tooltip_text(zoom_out_btn, "Zoom out");
    atk_object_set_name(gtk_widget_get_accessible(zoom_out_btn), "Zoom out");
    gtk_box_pack_start(GTK_BOX(toolbar), zoom_out_btn, FALSE, FALSE, 1);

    /* Separator */
    GtkWidget *separator_d = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(toolbar), separator_d, FALSE, FALSE, 5);

    /* Column view button*/
    GtkWidget *column_view_icon = gtk_image_new_from_file("./data/icons/column.png");
    GtkWidget *column_view_btn = gtk_radio_button_new(NULL);
    gtk_button_set_image(GTK_BUTTON(column_view_btn), column_view_icon);
    gtk_widget_set_tooltip_text(column_view_btn, "Page column");
    atk_object_set_name(gtk_widget_get_accessible(column_view_btn), "Page column");
    gtk_box_pack_start(GTK_BOX(toolbar), column_view_btn, FALSE, FALSE, 1);

    /* Double column view button*/
    GtkWidget *double_column_view_icon = gtk_image_new_from_file("./data/icons/double-column.png");
    GtkWidget *double_column_view_btn = gtk_radio_button_new_from_widget(GTK_RADIO_BUTTON(column_view_btn));
    gtk_button_set_image(GTK_BUTTON(double_column_view_btn), double_column_view_icon);
    gtk_widget_set_tooltip_text(double_column_view_btn, "Page double column");
    atk_object_set_name(gtk_widget_get_accessible(double_column_view_btn), "Page double column");
    gtk_box_pack_start(GTK_BOX(toolbar), double_column_view_btn, FALSE, FALSE, 1);

    /* Row view button*/
    GtkWidget *row_view_icon = gtk_image_new_from_file("./data/icons/row.png");
    GtkWidget *row_view_btn = gtk_radio_button_new_from_widget(GTK_RADIO_BUTTON(column_view_btn));
    gtk_button_set_image(GTK_BUTTON(row_view_btn), row_view_icon);
    gtk_widget_set_tooltip_text(row_view_btn, "Page row");
    atk_object_set_name(gtk_widget_get_accessible(row_view_btn), "Page row");
    gtk_box_pack_start(GTK_BOX(toolbar), row_view_btn, FALSE, FALSE, 1);

    /* Horizontal scroll toggle button*/
    GtkWidget *horiz_scroll_toggle_icon = gtk_image_new_from_file("./data/icons/horiz-scroll-off.png");
    GtkWidget *horiz_scroll_toggle_btn = gtk_toggle_button_new();
    gtk_button_set_image(GTK_BUTTON(horiz_scroll_toggle_btn), horiz_scroll_toggle_icon);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(horiz_scroll_toggle_btn), FALSE);
    gtk_widget_set_tooltip_text(horiz_scroll_toggle_btn, "Toggle horizontal scroll");
    atk_object_set_name(gtk_widget_get_accessible(horiz_scroll_toggle_btn), "Toggle horizontal scroll");
    g_signal_connect(horiz_scroll_toggle_btn, "toggled", G_CALLBACK(on_horiz_scroll_toggle), horiz_scroll_toggle_icon);
    gtk_box_pack_start(GTK_BOX(toolbar), horiz_scroll_toggle_btn, FALSE, FALSE, 1);

    /* Separator */
    GtkWidget *separator_e = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(toolbar), separator_e, FALSE, FALSE, 5);

    /* Title bar toggle button*/
    GtkWidget *title_bar_toggle_icon = gtk_image_new_from_file("./data/icons/title-bar-on.png");
    GtkWidget *title_bar_toggle_btn = gtk_toggle_button_new();
    gtk_button_set_image(GTK_BUTTON(title_bar_toggle_btn), title_bar_toggle_icon);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(title_bar_toggle_btn), TRUE);
    gtk_widget_set_tooltip_text(title_bar_toggle_btn, "Toggle title bar visibility");
    atk_object_set_name(gtk_widget_get_accessible(title_bar_toggle_btn), "Toggle title bar visibility");
    g_signal_connect(title_bar_toggle_btn, "toggled", G_CALLBACK(on_title_bar_toggle), title_bar_toggle_icon);
    gtk_box_pack_start(GTK_BOX(toolbar), title_bar_toggle_btn, FALSE, FALSE, 1);

    /* Helpers toggle button*/
    GtkWidget *helper_toggle_icon = gtk_image_new_from_file("./data/icons/sidebar-helper-off.png");
    GtkWidget *helper_toggle_btn = gtk_toggle_button_new();
    gtk_button_set_image(GTK_BUTTON(helper_toggle_btn), helper_toggle_icon);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(helper_toggle_btn), FALSE);
    gtk_widget_set_tooltip_text(helper_toggle_btn, "Helper files");
    atk_object_set_name(gtk_widget_get_accessible(helper_toggle_btn), "Helper files");
    g_signal_connect(helper_toggle_btn, "toggled", G_CALLBACK(on_helper_toggle), helper_toggle_icon);
    gtk_box_pack_start(GTK_BOX(toolbar), helper_toggle_btn, FALSE, FALSE, 1);

    /* Separator */
    GtkWidget *separator_f = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(toolbar), separator_f, FALSE, FALSE, 5);

    /* Close button*/
    GtkWidget *close_icon = gtk_image_new_from_file("./data/icons/plug.png");
    GtkWidget *close_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(close_btn), close_icon);
    gtk_widget_set_tooltip_text(close_btn, "Close");
    atk_object_set_name(gtk_widget_get_accessible(close_btn), "Close");
    g_signal_connect(close_btn, "clicked", G_CALLBACK(on_close_clicked), NULL);
    gtk_box_pack_end(GTK_BOX(toolbar), close_btn, FALSE, FALSE, 1);

    /* Maximize button*/
    GtkWidget *maximize_icon = gtk_image_new_from_file("./data/icons/maximize-2.png");
    GtkWidget *maximize_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(maximize_btn), maximize_icon);
    gtk_widget_set_tooltip_text(maximize_btn, "Maximize");
    atk_object_set_name(gtk_widget_get_accessible(maximize_btn), "Maximize");
    g_signal_connect(maximize_btn, "clicked", G_CALLBACK(on_maximize_clicked), window);
    gtk_box_pack_end(GTK_BOX(toolbar), maximize_btn, FALSE, FALSE, 1);

    /* Minimize button*/
    GtkWidget *minimize_icon = gtk_image_new_from_file("./data/icons/minimize-2.png");
    GtkWidget *minimize_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(minimize_btn), minimize_icon);
    gtk_widget_set_tooltip_text(minimize_btn, "Minimize");
    atk_object_set_name(gtk_widget_get_accessible(minimize_btn), "Minimize");
    g_signal_connect(minimize_btn, "clicked", G_CALLBACK(on_minimize_clicked), window);
    gtk_box_pack_end(GTK_BOX(toolbar), minimize_btn, FALSE, FALSE, 1);

    /* MAIN WINDOW PANED */
    /* Create a horizontal paned splitter containing two notebooks */
    paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(content_vbox), paned, TRUE, TRUE, 0);

    /* Left notebook (primary) */
    left_notebook = gtk_notebook_new();
    gtk_paned_pack1(GTK_PANED(paned), left_notebook, TRUE, FALSE);
    atk_object_set_name(gtk_widget_get_accessible(left_notebook), "Left Notebook");
    g_signal_connect(left_notebook, "switch-page", G_CALLBACK(on_left_notebook_switch_page), NULL);

    // Add initial placeholder page so it is visible immediately
    // Use last open session if available, otherwise default to "Default"
    const char *initial_session = "Default";
    if (sessions_model) {
        const char *last_session = sessions_model_get_last_open_session(sessions_model);
        if (last_session) {
            initial_session = last_session;
        }
    }
    restore_open_tabs_for_session(initial_session);

    /* Right pane: container with notebook and toolbar */
    right_pane = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_paned_pack2(GTK_PANED(paned), right_pane, TRUE, FALSE);

    /* Right notebook (secondary) */
    right_notebook = gtk_notebook_new();
    gtk_box_pack_start(GTK_BOX(right_pane), right_notebook, TRUE, TRUE, 0);
    g_signal_connect(right_notebook, "switch-page", G_CALLBACK(on_right_notebook_switch_page), NULL);

    /* Add initial content so right notebook is visible when shown */
    // Note: restore_open_tabs_for_session already handles both notebooks
    current_selected_session = g_strdup(initial_session);
    update_window_title_for_session(current_selected_session);

    /* Right pane toolbar (vertical) */
    GtkWidget *right_toolbar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(right_toolbar), "Toolbar");
    gtk_box_pack_start(GTK_BOX(right_pane), right_toolbar, FALSE, FALSE, 0);

    /* Right toolbar buttons - Open file */
    GtkWidget *right_open_file_icon = gtk_image_new_from_file("./data/icons/file-plus.png");
    GtkWidget *right_open_file_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(right_open_file_btn), right_open_file_icon);
    gtk_widget_set_tooltip_text(right_open_file_btn, "Open file");
    atk_object_set_name(gtk_widget_get_accessible(right_open_file_btn), "Open file");
    g_signal_connect(right_open_file_btn, "clicked", G_CALLBACK(on_open_helper_file_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(right_toolbar), right_open_file_btn, FALSE, FALSE, 1);

    /* Right toolbar - Close file */
    GtkWidget *right_close_file_icon = gtk_image_new_from_file("./data/icons/file-minus.png");
    GtkWidget *right_close_file_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(right_close_file_btn), right_close_file_icon);
    gtk_widget_set_tooltip_text(right_close_file_btn, "Close file");
    atk_object_set_name(gtk_widget_get_accessible(right_close_file_btn), "Close file");
    gtk_box_pack_start(GTK_BOX(right_toolbar), right_close_file_btn, FALSE, FALSE, 1);

    /* Right toolbar separator */
    GtkWidget *right_sep_a = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(right_toolbar), right_sep_a, FALSE, FALSE, 5);

    /* Right toolbar - Page up */
    GtkWidget *right_page_up_icon = gtk_image_new_from_file("./data/icons/page-up.png");
    GtkWidget *right_page_up_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(right_page_up_btn), right_page_up_icon);
    gtk_widget_set_tooltip_text(right_page_up_btn, "Page up");
    atk_object_set_name(gtk_widget_get_accessible(right_page_up_btn), "Page up");
    gtk_box_pack_start(GTK_BOX(right_toolbar), right_page_up_btn, FALSE, FALSE, 1);

    /* Right toolbar - Page down */
    GtkWidget *right_page_down_icon = gtk_image_new_from_file("./data/icons/page-down.png");
    GtkWidget *right_page_down_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(right_page_down_btn), right_page_down_icon);
    gtk_widget_set_tooltip_text(right_page_down_btn, "Page down");
    atk_object_set_name(gtk_widget_get_accessible(right_page_down_btn), "Page down");
    gtk_box_pack_start(GTK_BOX(right_toolbar), right_page_down_btn, FALSE, FALSE, 1);

    /* Right toolbar separator */
    GtkWidget *right_sep_b = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(right_toolbar), right_sep_b, FALSE, FALSE, 5);

    /* Right toolbar - Zoom in */
    GtkWidget *right_zoom_in_icon = gtk_image_new_from_file("./data/icons/zoom-in.png");
    GtkWidget *right_zoom_in_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(right_zoom_in_btn), right_zoom_in_icon);
    gtk_widget_set_tooltip_text(right_zoom_in_btn, "Zoom in");
    atk_object_set_name(gtk_widget_get_accessible(right_zoom_in_btn), "Zoom in");
    gtk_box_pack_start(GTK_BOX(right_toolbar), right_zoom_in_btn, FALSE, FALSE, 1);

    /* Right toolbar - Zoom out */
    GtkWidget *right_zoom_out_icon = gtk_image_new_from_file("./data/icons/zoom-out.png");
    GtkWidget *right_zoom_out_btn = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(right_zoom_out_btn), right_zoom_out_icon);
    gtk_widget_set_tooltip_text(right_zoom_out_btn, "Zoom out");
    atk_object_set_name(gtk_widget_get_accessible(right_zoom_out_btn), "Zoom out");
    gtk_box_pack_start(GTK_BOX(right_toolbar), right_zoom_out_btn, FALSE, FALSE, 1);

    /* Right toolbar separator */
    GtkWidget *right_sep_c = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(right_toolbar), right_sep_c, FALSE, FALSE, 5);

    /* Right toolbar - Page column */
    GtkWidget *right_column_icon = gtk_image_new_from_file("./data/icons/column.png");
    GtkWidget *right_column_btn = gtk_radio_button_new(NULL);
    gtk_button_set_image(GTK_BUTTON(right_column_btn), right_column_icon);
    gtk_widget_set_tooltip_text(right_column_btn, "Page column");
    atk_object_set_name(gtk_widget_get_accessible(right_column_btn), "Page column");
    gtk_box_pack_start(GTK_BOX(right_toolbar), right_column_btn, FALSE, FALSE, 1);

    /* Right toolbar - Page double column */
    GtkWidget *right_double_column_icon = gtk_image_new_from_file("./data/icons/double-column.png");
    GtkWidget *right_double_column_btn = gtk_radio_button_new_from_widget(GTK_RADIO_BUTTON(right_column_btn));
    gtk_button_set_image(GTK_BUTTON(right_double_column_btn), right_double_column_icon);
    gtk_widget_set_tooltip_text(right_double_column_btn, "Page double column");
    atk_object_set_name(gtk_widget_get_accessible(right_double_column_btn), "Page double column");
    gtk_box_pack_start(GTK_BOX(right_toolbar), right_double_column_btn, FALSE, FALSE, 1);

    /* Right toolbar - Page row */
    GtkWidget *right_row_icon = gtk_image_new_from_file("./data/icons/row.png");
    GtkWidget *right_row_btn = gtk_radio_button_new_from_widget(GTK_RADIO_BUTTON(right_column_btn));
    gtk_button_set_image(GTK_BUTTON(right_row_btn), right_row_icon);
    gtk_widget_set_tooltip_text(right_row_btn, "Page row");
    atk_object_set_name(gtk_widget_get_accessible(right_row_btn), "Page row");
    gtk_box_pack_start(GTK_BOX(right_toolbar), right_row_btn, FALSE, FALSE, 1);

    /* Right toolbar separator */
    GtkWidget *right_sep_d = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(right_toolbar), right_sep_d, FALSE, FALSE, 5);

    /* Right toolbar - Horizontal scroll toggle */
    GtkWidget *right_horiz_scroll_icon = gtk_image_new_from_file("./data/icons/horiz-scroll-off.png");
    GtkWidget *right_horiz_scroll_btn = gtk_toggle_button_new();
    gtk_button_set_image(GTK_BUTTON(right_horiz_scroll_btn), right_horiz_scroll_icon);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(right_horiz_scroll_btn), FALSE);
    gtk_widget_set_tooltip_text(right_horiz_scroll_btn, "Toggle horizontal scroll");
    atk_object_set_name(gtk_widget_get_accessible(right_horiz_scroll_btn), "Toggle horizontal scroll");
    g_signal_connect(right_horiz_scroll_btn, "toggled", G_CALLBACK(on_horiz_scroll_toggle), right_horiz_scroll_icon);
    gtk_box_pack_start(GTK_BOX(right_toolbar), right_horiz_scroll_btn, FALSE, FALSE, 1);

    return window;
}

void hide_right_pane(void) {
    if (right_pane) {
        gtk_widget_hide(GTK_WIDGET(right_pane));
    }
}
