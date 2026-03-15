#ifndef DOCUMENT_MODEL_H
#define DOCUMENT_MODEL_H

#include <glib.h>

typedef struct {
    char *url;                    // Document URL
    double zoom;                  // Zoom level
    int page_count;               // Total number of pages
    int current_page;             // Current page number (1-based)
    char *visualization_type;     // "column", "double column", or "row"
    gboolean horizontal_scroll;   // Whether horizontal scroll is enabled
} document_model_t;

// Constructor and destructor
document_model_t* document_model_new();
void document_model_free(document_model_t* model);

// Getters
const char* document_model_get_url(document_model_t* model);
double document_model_get_zoom(document_model_t* model);
int document_model_get_page_count(document_model_t* model);
int document_model_get_current_page(document_model_t* model);
const char* document_model_get_visualization_type(document_model_t* model);
gboolean document_model_get_horizontal_scroll(document_model_t* model);

// Setters
void document_model_set_url(document_model_t* model, const char* url);
void document_model_set_zoom(document_model_t* model, double zoom);
void document_model_set_page_count(document_model_t* model, int count);
void document_model_set_current_page(document_model_t* model, int page);
void document_model_set_visualization_type(document_model_t* model, const char* type);
void document_model_set_horizontal_scroll(document_model_t* model, gboolean enabled);

#endif // DOCUMENT_MODEL_H