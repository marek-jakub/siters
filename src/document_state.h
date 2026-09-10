#ifndef SITERS_DOCUMENT_STATE_H
#define SITERS_DOCUMENT_STATE_H

#include "tab.h"

/* Debounced update of the document model (zoom, cur_page, page sizes)
   after the user scrolls. Shared by the scroll-value-changed handler and
   the zoom module. */
void schedule_doc_model_update(TabData *tab);

#endif /* SITERS_DOCUMENT_STATE_H */