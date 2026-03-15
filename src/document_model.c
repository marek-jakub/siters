#include "document_model.h"
#include <string.h>

document_model_t* document_model_new() {
    document_model_t* model = g_new(document_model_t, 1);
    model->url = NULL;
    model->zoom = 1.0; // default zoom
    model->page_count = 0;
    model->current_page = 1; // default to first page
    model->visualization_type = g_strdup("column"); // default
    model->horizontal_scroll = FALSE; // default
    return model;
}

void document_model_free(document_model_t* model) {
    if (!model) return;

    // Free strings
    g_free(model->url);
    g_free(model->visualization_type);

    g_free(model);
}

// Getters
const char* document_model_get_url(document_model_t* model) {
    return model->url;
}

double document_model_get_zoom(document_model_t* model) {
    return model->zoom;
}

int document_model_get_page_count(document_model_t* model) {
    return model->page_count;
}

int document_model_get_current_page(document_model_t* model) {
    return model->current_page;
}

const char* document_model_get_visualization_type(document_model_t* model) {
    return model->visualization_type;
}

gboolean document_model_get_horizontal_scroll(document_model_t* model) {
    return model->horizontal_scroll;
}

// Setters
void document_model_set_url(document_model_t* model, const char* url) {
    g_free(model->url);
    model->url = g_strdup(url);
}

void document_model_set_zoom(document_model_t* model, double zoom) {
    model->zoom = zoom;
}

void document_model_set_page_count(document_model_t* model, int count) {
    model->page_count = count;
}

void document_model_set_current_page(document_model_t* model, int page) {
    model->current_page = page;
}

void document_model_set_visualization_type(document_model_t* model, const char* type) {
    g_free(model->visualization_type);
    model->visualization_type = g_strdup(type);
}

void document_model_set_horizontal_scroll(document_model_t* model, gboolean enabled) {
    model->horizontal_scroll = enabled;
}