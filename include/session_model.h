#ifndef SESSION_MODEL_H
#define SESSION_MODEL_H

#include <glib.h>

typedef struct {
    GList *document_urls;           // List of document URL strings
    GList *helper_document_urls;    // List of helper document URL strings
    char *session_name;             // Name of the session
    char *last_read_document;       // URL of the last read document
    char *page_color;               // Page color (e.g., "#FFFFFF")
    char *last_read_help_document;  // URL of the last read help document
    char *helper_page_color;        // Helper document page color
} session_model_t;

// Constructor and destructor
session_model_t* session_model_new();
void session_model_free(session_model_t* model);

// Getters
const GList* session_model_get_document_urls(session_model_t* model);
const GList* session_model_get_helper_document_urls(session_model_t* model);
const char* session_model_get_session_name(session_model_t* model);
const char* session_model_get_last_read_document(session_model_t* model);
const char* session_model_get_page_color(session_model_t* model);
const char* session_model_get_last_read_help_document(session_model_t* model);
const char* session_model_get_helper_page_color(session_model_t* model);

// Setters
void session_model_set_document_urls(session_model_t* model, GList* urls);
void session_model_set_helper_document_urls(session_model_t* model, GList* urls);
void session_model_set_session_name(session_model_t* model, const char* name);
void session_model_set_last_read_document(session_model_t* model, const char* url);
void session_model_set_page_color(session_model_t* model, const char* color);
void session_model_set_last_read_help_document(session_model_t* model, const char* url);
void session_model_set_helper_page_color(session_model_t* model, const char* color);

// Utility functions
void session_model_add_document_url(session_model_t* model, const char* url);
void session_model_remove_document_url(session_model_t* model, const char* url);
void session_model_add_helper_document_url(session_model_t* model, const char* url);
void session_model_remove_helper_document_url(session_model_t* model, const char* url);

#endif // SESSION_MODEL_H