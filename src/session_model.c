#include "session_model.h"
#include <string.h>

static void* copy_string(const void* src, void* data) {
    (void)data;
    return g_strdup((const char*)src);
}

session_model_t* session_model_new() {
    session_model_t* model = g_new(session_model_t, 1);
    model->document_urls = NULL;
    model->helper_document_urls = NULL;
    model->session_name = NULL;
    model->last_read_document = NULL;
    model->page_color = g_strdup("#FFFFFF"); // default white
    model->last_read_help_document = NULL;
    model->helper_page_color = g_strdup("#FFFFFF"); // default white
    return model;
}

void session_model_free(session_model_t* model) {
    if (!model) return;

    // Free URL lists
    g_list_free_full(model->document_urls, g_free);
    g_list_free_full(model->helper_document_urls, g_free);

    // Free strings
    g_free(model->session_name);
    g_free(model->last_read_document);
    g_free(model->page_color);
    g_free(model->last_read_help_document);
    g_free(model->helper_page_color);

    g_free(model);
}

// Getters
const GList* session_model_get_document_urls(session_model_t* model) {
    return model->document_urls;
}

const GList* session_model_get_helper_document_urls(session_model_t* model) {
    return model->helper_document_urls;
}

const char* session_model_get_session_name(session_model_t* model) {
    return model->session_name;
}

const char* session_model_get_last_read_document(session_model_t* model) {
    return model->last_read_document;
}

const char* session_model_get_page_color(session_model_t* model) {
    return model->page_color;
}

const char* session_model_get_last_read_help_document(session_model_t* model) {
    return model->last_read_help_document;
}

const char* session_model_get_helper_page_color(session_model_t* model) {
    return model->helper_page_color;
}

// Setters
void session_model_set_document_urls(session_model_t* model, GList* urls) {
    if (model->document_urls) {
        g_list_free_full(model->document_urls, g_free);
    }
    model->document_urls = g_list_copy_deep(urls, copy_string, NULL);
}

void session_model_set_helper_document_urls(session_model_t* model, GList* urls) {
    if (model->helper_document_urls) {
        g_list_free_full(model->helper_document_urls, g_free);
    }
    model->helper_document_urls = g_list_copy_deep(urls, copy_string, NULL);
}

void session_model_set_session_name(session_model_t* model, const char* name) {
    g_free(model->session_name);
    model->session_name = g_strdup(name);
}

void session_model_set_last_read_document(session_model_t* model, const char* url) {
    g_free(model->last_read_document);
    model->last_read_document = g_strdup(url);
}

void session_model_set_page_color(session_model_t* model, const char* color) {
    g_free(model->page_color);
    model->page_color = g_strdup(color);
}

void session_model_set_last_read_help_document(session_model_t* model, const char* url) {
    g_free(model->last_read_help_document);
    model->last_read_help_document = g_strdup(url);
}

void session_model_set_helper_page_color(session_model_t* model, const char* color) {
    g_free(model->helper_page_color);
    model->helper_page_color = g_strdup(color);
}

// Utility functions
void session_model_add_document_url(session_model_t* model, const char* url) {
    model->document_urls = g_list_append(model->document_urls, g_strdup(url));
}

void session_model_remove_document_url(session_model_t* model, const char* url) {
    GList* found = g_list_find_custom(model->document_urls, url, (GCompareFunc)strcmp);
    if (found) {
        g_free(found->data);
        model->document_urls = g_list_delete_link(model->document_urls, found);
    }
}

void session_model_add_helper_document_url(session_model_t* model, const char* url) {
    model->helper_document_urls = g_list_append(model->helper_document_urls, g_strdup(url));
}

void session_model_remove_helper_document_url(session_model_t* model, const char* url) {
    GList* found = g_list_find_custom(model->helper_document_urls, url, (GCompareFunc)strcmp);
    if (found) {
        g_free(found->data);
        model->helper_document_urls = g_list_delete_link(model->helper_document_urls, found);
    }
}