#ifndef SESSIONS_MODEL_H
#define SESSIONS_MODEL_H

#include <glib.h>

typedef struct {
    GList *session_names;        // List of session name strings
    char *last_open_session;     // Name of the last opened session
    int tab_width;               // Width of tabs for opened PDF files
    char *last_opened_directory; // Path to the last opened directory
    char *theme;                 // Theme: "dark" or "light"
} sessions_model_t;

// Constructor and destructor
sessions_model_t* sessions_model_new();
void sessions_model_free(sessions_model_t* model);

// Getters
const GList* sessions_model_get_session_names(sessions_model_t* model);
const char* sessions_model_get_last_open_session(sessions_model_t* model);
int sessions_model_get_tab_width(sessions_model_t* model);
const char* sessions_model_get_last_opened_directory(sessions_model_t* model);
const char* sessions_model_get_theme(sessions_model_t* model);

// Setters
void sessions_model_set_session_names(sessions_model_t* model, GList* names);
void sessions_model_set_last_open_session(sessions_model_t* model, const char* session);
void sessions_model_set_tab_width(sessions_model_t* model, int width);
void sessions_model_set_last_opened_directory(sessions_model_t* model, const char* directory);
void sessions_model_set_theme(sessions_model_t* model, const char* theme);

// Utility functions
void sessions_model_add_session_name(sessions_model_t* model, const char* name);
void sessions_model_remove_session_name(sessions_model_t* model, const char* name);

#endif // SESSIONS_MODEL_H