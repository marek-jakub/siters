#ifndef DOCUMENT_MODEL_H
#define DOCUMENT_MODEL_H

#include <glib.h>

typedef struct {
    char *url;                    // Document URL
    double zoom;                  // Zoom level
    int page_count;               // Total number of pages
    int current_page;             // Current page number (1-based)
    int visualization_mode;       // 0=column, 1=double column, 2=row
    gboolean horizontal_scroll;   // Whether horizontal scroll is enabled
    double scroll_offset;         // Scroll offset for continuous view
    double intra_page_fraction;   // Fraction of the page that is visible (0.0 to 1.0)
    int target_width;           // Target width for column layout (optional, can be used for responsive design)
} document_model_t;

// Constructor and destructor
document_model_t* document_model_new();
void document_model_free(document_model_t* model);

// Getters
const char* document_model_get_url(document_model_t* model);
double document_model_get_zoom(document_model_t* model);
int document_model_get_page_count(document_model_t* model);
int document_model_get_current_page(document_model_t* model);
int document_model_get_visualization_mode(document_model_t* model);
gboolean document_model_get_horizontal_scroll(document_model_t* model);
double document_model_get_scroll_offset(document_model_t *model);
double document_model_get_intra_page_fraction(document_model_t* model);
int document_model_get_target_width(document_model_t* model);

// Setters
void document_model_set_url(document_model_t* model, const char* url);
void document_model_set_zoom(document_model_t* model, double zoom);
void document_model_set_page_count(document_model_t* model, int count);
void document_model_set_current_page(document_model_t* model, int page);
void document_model_set_visualization_mode(document_model_t* model, int mode);
void document_model_set_horizontal_scroll(document_model_t* model, gboolean enabled);
void document_model_set_scroll_offset(document_model_t *model, double offset);
void document_model_set_intra_page_fraction(document_model_t* model, double fraction);
void document_model_set_target_width(document_model_t* model, int target_width);

#endif // DOCUMENT_MODEL_H
