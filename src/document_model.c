#include "document_model.h"

document_model_t* document_model_new() {
    document_model_t* model = g_new(document_model_t, 1);
    model->url = NULL;
    model->zoom = 1.0; // default zoom
    model->page_count = 0;
    model->current_page = 1; // default to first page
    model->visualization_mode = 0; // default 0 (0=column, 1=double column, 2=row)
    model->horizontal_scroll = FALSE; // default
    model->scroll_offset = 0.0; // default
    model->intra_page_fraction = 0.0; // default
    model->target_width = 800; // default
    return model;
}

void document_model_free(document_model_t* model) {
    if (!model) return;

    // Free strings
    g_free(model->url);

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

int document_model_get_visualization_mode(document_model_t* model) {
    return model->visualization_mode;
}

gboolean document_model_get_horizontal_scroll(document_model_t* model) {
    return model->horizontal_scroll;
}

double document_model_get_scroll_offset(document_model_t *model) {
    return model ? model->scroll_offset : 0.0;
}

double document_model_get_intra_page_fraction(document_model_t *model) {
    return model ? model->intra_page_fraction : 0.0;
}

int document_model_get_target_width(document_model_t *doc_model) {
    if (!doc_model) return 800;
    return doc_model->target_width;
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

void document_model_set_visualization_mode(document_model_t* model, int mode) {
    model->visualization_mode = mode;
}

void document_model_set_horizontal_scroll(document_model_t* model, gboolean enabled) {
    model->horizontal_scroll = enabled;
}

void document_model_set_scroll_offset(document_model_t *model, double offset) {
    if (!model) return;
    model->scroll_offset = offset;
}

void document_model_set_intra_page_fraction(document_model_t *model, double fraction) {
    if (!model) return;
    model->intra_page_fraction = fraction;
}

void document_model_set_target_width(document_model_t *doc_model, int target_width) {
    if (doc_model) {
        doc_model->target_width = target_width;
    }
}
