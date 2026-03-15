#include "sessions_model.h"
#include <string.h>

static void* copy_string(const void* src, void* data) {
    (void)data;
    return g_strdup((const char*)src);
}

sessions_model_t* sessions_model_new() {
    sessions_model_t* model = g_new(sessions_model_t, 1);
    model->session_names = NULL;
    model->last_open_session = NULL;
    model->tab_width = 100; // default
    model->last_opened_directory = NULL;
    model->theme = g_strdup("light"); // default
    return model;
}

void sessions_model_free(sessions_model_t* model) {
    if (!model) return;

    // Free session names list
    g_list_free_full(model->session_names, g_free);

    // Free strings
    g_free(model->last_open_session);
    g_free(model->last_opened_directory);
    g_free(model->theme);

    g_free(model);
}

// Getters
const GList* sessions_model_get_session_names(sessions_model_t* model) {
    return model->session_names;
}

const char* sessions_model_get_last_open_session(sessions_model_t* model) {
    return model->last_open_session;
}

int sessions_model_get_tab_width(sessions_model_t* model) {
    return model->tab_width;
}

const char* sessions_model_get_last_opened_directory(sessions_model_t* model) {
    return model->last_opened_directory;
}

const char* sessions_model_get_theme(sessions_model_t* model) {
    return model->theme;
}

// Setters
void sessions_model_set_session_names(sessions_model_t* model, GList* names) {
    if (model->session_names) {
        g_list_free_full(model->session_names, g_free);
    }
    model->session_names = g_list_copy_deep(names, copy_string, NULL);
}

void sessions_model_set_last_open_session(sessions_model_t* model, const char* session) {
    g_free(model->last_open_session);
    model->last_open_session = g_strdup(session);
}

void sessions_model_set_tab_width(sessions_model_t* model, int width) {
    model->tab_width = width;
}

void sessions_model_set_last_opened_directory(sessions_model_t* model, const char* directory) {
    g_free(model->last_opened_directory);
    model->last_opened_directory = g_strdup(directory);
}

void sessions_model_set_theme(sessions_model_t* model, const char* theme) {
    g_free(model->theme);
    model->theme = g_strdup(theme);
}

// Utility functions
void sessions_model_add_session_name(sessions_model_t* model, const char* name) {
    model->session_names = g_list_append(model->session_names, g_strdup(name));
}

void sessions_model_remove_session_name(sessions_model_t* model, const char* name) {
    GList* found = g_list_find_custom(model->session_names, name, (GCompareFunc)strcmp);
    if (found) {
        g_free(found->data);
        model->session_names = g_list_delete_link(model->session_names, found);
    }
}