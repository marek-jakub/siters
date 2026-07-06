# Top 10 Harshest Bugs & Solutions

> **Note:** The original Poppler backend was abandoned in favour of MuPDF during the life of this project.  
> See [Why Poppler Was Abandoned](#why-poppler-was-abandoned) below.

## Why Poppler Was Abandoned

The original backend used **Poppler** (`libpoppler-glib`) for PDF loading, rendering, and text extraction. Poppler was abandoned for the following reasons:

| Factor | Poppler | MuPDF |
|---|---|---|
| **Memory growth** | Internal per‑page resource cache (`arena` heap) grew from ~500 MB → 2.9 GB with no eviction API. Only a full process restart could reclaim it. | `fz_store` has a fixed byte budget (configurable, default 256 MB). Resources are evicted automatically when the budget is exceeded (LRU). |
| **RAM target** | Blew past the project's 200 MB RSS target by an order of magnitude. | Stays within budget; no unbounded growth observed. |
| **Cairo integration** | `poppler_page_render()` draws directly to a `cairo_t`. Rendering is tightly coupled to Poppler's internal device. | `fz_new_pixmap_from_page()` returns a raw `fz_pixmap` that can be `memcpy`'d into a Cairo image surface. Slightly more glue code but fully deterministic. |
| **API stability** | Stable GLib‑style API, but no cache control at all. | Stable C API. `fz_store` can be tuned, flushed, or walked. |
| **License** | LGPLv2.1+ (compatible). | AGPLv3 (compatible for this open‑source project). |

The decision was made after `MEM_DEBUG` instrumentation (see commit history) confirmed that Poppler's `arena` heap was the sole source of unbounded RSS growth. Switching to MuPDF eliminated the memory regression without changing the rendering pipeline architecture (both backends use an intermediate `cairo_image_surface_create()` + blit pattern, see Bug #1).

The switch was done in two phases:
1. **Abstract API** (`include/pdf.h`): defined `PdfrDoc`, `PdfrPage`, `PdfrLink`, `PdfrRect`, `PdfrLinkType`, and `PdfrOutline` — a backend‑neutral interface.
2. **MuPDF backend** (`src/pdf_mupdf.c`): implemented the abstract API using MuPDF's `fz_*` functions. Poppler backend (`src/pdf_poppler.c`) was deleted after the switch.

---

1. [PDF Text Not Rendering on X11 (and Ragged Text Fix)](#1-pdf-text-not-rendering-on-x11-and-ragged-text-fix)
2. [Session Rename Destroys All Per-Document Settings](#2-session-rename-destroys-all-per-document-settings)
3. [CPU at 25% Idle Due to Per-Scroll Document Model Update](#3-cpu-at-25-idle-due-to-per-scroll-document-model-update)
4. [View-Mode Radio Buttons Fail to Sync on Startup](#4-view-mode-radio-buttons-fail-to-sync-on-startup)
5. [TOC Chapter Click Scrolls to Wrong Page (Off-by-One)](#5-toc-chapter-click-scrolls-to-wrong-page-off-by-one)
6. [TOC Sidebar Stale on Tab Switch](#6-toc-sidebar-stale-on-tab-switch)
7. [TOC Navigation Causes Page Jump When Switching Sidebar](#7-toc-navigation-causes-page-jump-when-switching-sidebar)
8. [Horizontal Scrollbar Always Visible in Row View](#8-horizontal-scrollbar-always-visible-in-row-view)
9. [File Chooser Forgets Last Used Directory](#9-file-chooser-forgets-last-used-directory)
10. [Dev Build and Installed .deb Share Config File](#10-dev-build-and-installed-deb-share-config-file)
11. [TOC Clicking Does Nothing on Some PDFs (Named Destinations)](#11-toc-clicking-does-nothing-on-some-pdfs-named-destinations)
12. [Blank Pages After Scrolling (MuPDF Error Stack Overflow)](#12-blank-pages-after-scrolling-mupdf-error-stack-overflow)

---

## 1. PDF Text Not Rendering on X11 (and Ragged Text Fix)

**Bug:** Some PDF documents displayed blank pages — text was completely invisible while background colours and vector graphics rendered fine. This affected all documents on X11 when using `poppler_page_render()` directly on the window's cairo context. Later, the fix (`poppler_page_render_for_printing`) introduced ragged/scraggly character edges on other documents.

**Root cause:**  
Poppler's `CairoOutputDev` has a text-rendering bug when targeting an X11 `cairo_surface_t` directly. The glyph-showing path (`cairo_show_glyphs`) produces no visible output on X11 surfaces in certain configurations.  

The subsequent ragged-text regression was caused by `poppler_page_render_for_printing()`, which renders all text as vector paths instead of display-optimised glyphs. Path rendering ignores font hinting and subpixel antialiasing, producing jaggies at screen resolutions.

**Solution:**  
Render each page to an **intermediate in-memory** `cairo_image_surface_create(CAIRO_FORMAT_ARGB32)` at the exact device-pixel dimensions, then blit the finished image onto the window. This bypasses the X11 surface bug entirely because the render target is a plain memory buffer, not an X11 drawable.

Use `poppler_page_render()` (not `render_for_printing`) on the intermediate surface so text is rendered as glyphs with full subpixel antialiasing and hinting. Set `CAIRO_ANTIALIAS_BEST` explicitly on the intermediate context for consistent quality across GTK themes.

**Key code** (`src/siters.c`):
```c
int iw = (int)(pw * scale * dsx + 0.5);
int ih = (int)(ph * scale * dsy + 0.5);
cairo_surface_t *pimg = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, iw, ih);
cairo_surface_set_device_scale(pimg, dsx, dsy);
cairo_t *picr = cairo_create(pimg);
cairo_set_font_options(picr, fo);
cairo_set_antialias(picr, CAIRO_ANTIALIAS_BEST);
cairo_scale(picr, scale, scale);
poppler_page_render(page, picr);
cairo_destroy(picr);
cairo_set_source_surface(cr, pimg, off_x, off_y);
cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
cairo_paint(cr);
cairo_surface_destroy(pimg);
```

---

## 2. Session Rename Destroys All Per-Document Settings

**Bug:** Renaming a session via the "Update" button in the Sessions sidebar silently discarded every per-document setting (scroll position, zoom, layout mode) for all documents in that session. After the rename, reopening the session showed documents at page 1, default zoom.

**Root cause:**  
`session_models` and `document_models` are `GHashTable`s keyed by session name (the latter embeds the session name in the key via `make_document_key(session_name, uri, is_helper)`). The rename code only updated the sessions model's name list. All hash table entries still referenced the old session name, becoming orphaned. On next `restore_open_tabs_for_session` with the new name, no entries matched — defaults were used.

**Solution:**  
In `on_sessions_update_clicked`, re-key both hash tables:
1. `g_hash_table_steal()` the old entry from `session_models`, `g_hash_table_insert()` with the new name.
2. Iterate `document_models`, `g_hash_table_steal()` every entry whose key starts with the old session name, rebuild the key with the new name, and `g_hash_table_insert()` it back.
3. Call `save_state()` to persist immediately.

Also update `current_selected_session` to the new name before calling `populate_sessions_treeview()` so the tree's auto-selection finds the correct row (preventing `switch_to_session()` from clearing tabs).

---

## 3. CPU at 25% Idle Due to Per-Scroll Document Model Update

**Bug:** The application consumed ~25% of one CPU core even when the user was not interacting — just hovering over the document caused continuous high CPU usage. On battery-powered devices this halved battery life.

**Root cause:**  
`update_document_model_from_tab(tab)` iterates every page in the PDF via `poppler_document_get_page()` to compute page offsets and determine the current page. This function was connected directly to the `value-changed` signal of the scroll adjustment, firing on **every single scroll tick** (including GTK's internal scroll-animation ticks). A single scroll gesture could trigger 50–100 Poppler page iterations.

**Solution:**  
Debounce the document model update with a 400 ms timer:
```c
static void schedule_doc_model_update(TabData *tab) {
    if (!tab) return;
    cancel_doc_model_debounce(tab);
    tab->scroll_doc_debounce_id = g_timeout_add(400, deferred_update_document_model, tab);
}
```
Added `guint scroll_doc_debounce_id` to `TabData`. The update only fires 400 ms after scrolling stops, eliminating redundant iterations. Also bounded stage-0 idle retries in `do_initial_scroll_stage` to 50 attempts and removed a redundant `initial_scroll_pending = TRUE` in `restore_open_tabs_for_session`.

---

## 4. View-Mode Radio Buttons Fail to Sync on Startup

**Bug:** After restarting the application, the column/double-column/row radio buttons in the toolbar always showed "column" selected, regardless of the saved layout mode for the current document. Clicking a different tab and switching back sometimes fixed it.

**Root cause:**  
`on_left_notebook_switch_page` calls `sync_left_layout_buttons(tab)` **before** `tab->doc` is set for the new tab. The sync function reads `tab->layout_mode`, but that field hasn't been restored from the document model yet because `restore_document_model_to_tab(tab)` runs later in the same handler. The radio buttons always saw the default `layout_mode = 0` (column mode).

**Solution:**  
Move `sync_left_layout_buttons(tab)` and `sync_page_widget_from_tab(tab)` to **after** `restore_document_model_to_tab(tab)` and `build_continuous_view(tab)`. Also added explicit sync calls after `restore_open_tabs_for_session()` in `load_state()` to catch the initial load path.

---

## 5. TOC Chapter Click Scrolls to Wrong Page (Off-by-One)

**Bug:** Clicking any chapter in the Table of Contents sidebar scrolled to **one page before** the intended chapter. For example, clicking "Chapter 3" would scroll to page 2.

**Root cause:**  
Poppler's `PopplerDest::page_num` is **1-indexed** (page 1 = first page). The codebase consistently uses **0-indexed** page numbering internally (`tab->cur_page`, `scroll_to_page()`, page entry widget). TOC handler was passing the 1-indexed page directly to `scroll_to_page()` without subtracting 1.

**Solution:**  
In `on_toc_treeview_cursor_changed`, subtract 1 before any page operations:
```c
int page = 0;
gtk_tree_model_get(model, &iter, TOC_COL_PAGE, &page, -1);
if (page > 0) {
    tab->cur_page = page - 1;
    scroll_to_page(tab, page - 1);
    update_document_model_from_tab(tab);
}
```

---

## 6. TOC Sidebar Stale on Tab Switch

**Bug:** With the TOC sidebar open, clicking a different document tab left the TOC tree showing the previous document's table of contents. The tree was not refreshed to reflect the newly selected document.

**Root cause:**  
The `switch-page` handler `on_left_notebook_switch_page` did call `populate_toc_treeview()` when `current_sidebar_mode == SIDEBAR_TOC`, but `populate_toc_treeview()` re-fetched the current tab via `get_current_left_tab()`, which calls `gtk_notebook_get_current_page()`. In certain signal emission paths, the notebook's internal current-page property could resolve to the wrong tab.

**Solution:**  
Introduced `populate_toc_treeview_for_tab(TabData *tab)` that accepts an explicit tab argument. The `switch-page` handler now passes its `tab` parameter (the new current page) directly:
```c
static void populate_toc_treeview_for_tab(TabData *tab) {
    gtk_tree_store_clear(toc_tree_store);
    if (!tab || !tab->doc) return;
    PopplerIndexIter *root_iter = poppler_index_iter_new(tab->doc);
    if (!root_iter) return;
    populate_toc_treeview_recursive(root_iter, NULL);
    poppler_index_iter_free(root_iter);
}
```
Called from `on_left_notebook_switch_page` as:
```c
if (current_sidebar_mode == SIDEBAR_TOC) populate_toc_treeview_for_tab(tab);
```

---

## 7. TOC Navigation Causes Page Jump When Switching Sidebar

**Bug:** After navigating to a chapter via the TOC sidebar, clicking the Settings or Sessions button caused the document to "jump back" to the previous page. The sidebar switch itself should not affect the document scroll position.

**Root cause:**  
Two interacting issues:
1. When `gtk_tree_store_clear(toc_tree_store)` was called during sidebar mode switch, the TOC tree view could emit `cursor-changed` with a stale cursor pointing to the old (now-deleted) chapter, causing `on_toc_treeview_cursor_changed` to re-scroll to the old page.
2. A pending 400 ms debounced `update_document_model_from_tab` from the scroll handler could fire after the sidebar switch, overwriting the correct state saved by the TOC handler.

**Solution:**  
Three defensive measures:
- **Visibility guard**: `if (!gtk_widget_get_visible(toc_container)) return;` at the top of `on_toc_treeview_cursor_changed` prevents stale signal handling when TOC is hidden.
- **Cancel debounce**: `cancel_doc_model_debounce(tab)` called in the TOC handler before applying the new scroll position, ensuring no stale deferred update overwrites the immediate save.
- **Immediate save**: `update_document_model_from_tab(tab)` called right after `scroll_to_page(tab, ...)` in the TOC handler, persisting the correct position before any sidebar switch can interfere.

---

## 8. Horizontal Scrollbar Always Visible in Row View

**Bug:** In horizontal/layout-mode-2 (row view), the custom `GtkScrollbar` was always shown, occupying ~15 px of vertical space even when the user was not scrolling. This was visually distracting on small screens.

**Root cause:**  
The scrollbar had no auto-hide mechanism. Unlike the vertical scrollbar (handled by `GtkScrolledWindow` with `GTK_POLICY_AUTOMATIC`), the horizontal scrollbar in row view is a standalone `GtkScrollbar` widget that was always `gtk_widget_show()`.

**Solution:**  
Implement an auto-hide timer:
- **Hidden by default** (`gtk_widget_hide(tab->h_scrollbar)`) after layout.
- **Appears on scroll** — the scroll-event handler shows it and (re)starts a 2-second hide timer.
- **Appears on hover near bottom** — `motion-notify-event` on the drawing area checks if the pointer is within 30 px of the viewport bottom via `scroll_y + page_size` and shows the bar.
- **Stays visible while hovering** — `enter-notify-event` on the scrollbar cancels the hide timer; `leave-notify-event` restarts it.
- Added `guint h_scrollbar_timer_id` to `TabData` for timer management, cleaned up in `destroy_tab_data`.

---

## 9. File Chooser Forgets Last Used Directory

**Bug:** Every time the user clicked "Open File", the file chooser dialog opened in the home directory (or the process's current working directory), regardless of where they had previously opened a PDF. Navigating to a deeply nested directory every session was tedious.

**Root cause:**  
No state was kept between invocations of `open_file_in_notebook()`. The `GtkFileChooser` dialog was created fresh each time with no initial directory set.

**Solution:**  
Added a `static char *last_open_dir` variable that persists to the JSON state file:
- **Save**: Written as `"last_open_dir"` in `save_state()` at the root JSON object.
- **Load**: Read in `load_state()` via `json_object_get_string_member_with_default`.
- **Use**: In `open_file_in_notebook()`, set the initial folder using `gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), last_open_dir)` after validating the directory exists (`g_file_test(last_open_dir, G_FILE_TEST_IS_DIR)`).
- **Update**: After a successful file selection, extract the directory from the first selected path via `g_path_get_dirname()` and store it in `last_open_dir`.

---

## 10. Dev Build and Installed .deb Share Config File

**Bug:** Running the development binary (built with `make`) and the installed `.deb` version used the same configuration file at `~/.config/siters/siters.json`. Each would overwrite the other's session data, causing lost open tabs, scroll positions, and settings. A developer testing a change would lose their production configuration.

**Root cause:**  
Both builds resolved the config path identically via `g_get_user_config_dir() + "/siters/siters.json"`. There was no mechanism to distinguish between development and production runs.

**Solution:**  
Added a `SITERS_CONFIG_DIR` environment variable override:
```c
const gchar *cfg_override = g_getenv("SITERS_CONFIG_DIR");
const gchar *config_dir = cfg_override ? cfg_override : g_get_user_config_dir();
gchar *app_config_dir = g_build_filename(config_dir, "siters", NULL);
```
Applied in both `save_state()` and `load_state()`. Developers set `export SITERS_CONFIG_DIR=/tmp/siters-config` before running the dev binary. The unit test suite (`test_siters_gui.py`) also uses this variable via `setUpClass` for test isolation.

---

## 11. TOC Clicking Does Nothing on Some PDFs (Named Destinations)

**Bug:** The Table of Contents sidebar worked correctly on some PDFs but was completely non-functional on others. Clicking any TOC entry had no effect — the document did not scroll and no page navigation occurred. This affected many professionally-produced PDFs.

**Root cause:**  
PDFs store TOC destinations in two ways:
1. **Direct page destinations** — `PopplerDest` with `type == POPPLER_DEST_XYZ` (or similar), where `page_num` holds the 1-indexed target page directly.
2. **Named destinations** — `PopplerDest` with `type == POPPLER_DEST_NAMED`, where `page_num` is `0` and the actual target is stored as a string in `named_dest` (e.g., `"chapter3"` or `"/D (section1)"`). The page must be resolved via `poppler_document_find_dest()`.

The original code only read `action->goto_dest.dest->page_num`, which is `0` for named destinations. The `if (page > 0)` guard in both the recursive populate function and the click handler caused these entries to be silently skipped — stored with page 0 and ignored on click.

**Solution:**  
Two-phase resolution:
1. **At populate time** (in `populate_toc_treeview_recursive`): detect named dests, call `poppler_document_find_dest(doc, named_dest)` to resolve the actual page number, and store it in `TOC_COL_PAGE` as usual. The named dest string is also stored in a new `TOC_COL_NAMED_DEST` column for fallback.
2. **At click time** (in `on_toc_row_activated`): if the stored page is still 0 (resolution failed at populate time), attempt `poppler_document_find_dest()` again using the named dest from `TOC_COL_NAMED_DEST`.

The tree store schema gained a third `G_TYPE_STRING` column for `TOC_COL_NAMED_DEST` and the `gtk_tree_store_new()` call was updated accordingly.

**Key code** (`src/siters.c`):
```c
// In populate_toc_treeview_recursive:
if (action->goto_dest.dest->page_num > 0) {
    page = action->goto_dest.dest->page_num;
} else if (action->goto_dest.dest->type == POPPLER_DEST_NAMED &&
           action->goto_dest.dest->named_dest) {
    named_dest = g_strdup(action->goto_dest.dest->named_dest);
    PopplerDest *resolved = poppler_document_find_dest(doc, named_dest);
    if (resolved) {
        page = resolved->page_num;
        poppler_dest_free(resolved);
    }
}
gtk_tree_store_set(toc_tree_store, &child,
                   TOC_COL_LABEL, title,
                   TOC_COL_PAGE, page,
                   TOC_COL_NAMED_DEST, named_dest,
                   -1);

// In on_toc_row_activated (fallback):
if (page == 0 && named_dest) {
    PopplerDest *resolved = poppler_document_find_dest(tab->doc, named_dest);
    if (resolved && resolved->page_num > 0) page = resolved->page_num;
    if (resolved) poppler_dest_free(resolved);
}
```

---

## 12. Blank Pages After Scrolling (MuPDF Error Stack Overflow)

**Bug:** After switching from Poppler to MuPDF, pages rendered correctly on initial load but went blank after scrolling. Once blank, no amount of scrolling could bring the content back. Affected ALL PDFs, not just damaged ones.

**Root cause:**  
MuPDF's `fz_try`/`fz_catch` error‑handling macros use `setjmp`/`longjmp` on an internal error stack. Each `fz_try` pushes an error state via `fz_push_try()` which must be popped by `fz_always_try()` (called at the end of the `fz_try` body). Using C `return` (or `goto`) inside the `fz_try` body bypasses the pop, **leaking one error‑stack entry per call**. Once the stack exceeded its fixed depth (~250), EVERY subsequent `fz_try` block immediately failed with "exception stack overflow", causing all page loads and renders to silently return NULL or produce empty pixmaps.

Three functions were affected:

1. **`pdfr_open`** — `return pd;` inside `fz_try`.
2. **`pdfr_count_pages`** — `return fz_count_pages(...);` inside `fz_try`.
3. **`pdfr_load_page`** — `return pd;` and `return NULL;` inside `fz_try`.

At ~250 calls (loading ~250 pages across cache warmup, scroll events, and resize redraws) the stack saturated and all rendering ceased.

`pdfr_search_page` also used `return 0;` inside `fz_catch` — safe (`fz_always_catch` pops before the body runs), but changed to empty body for consistency.

**Solution:**  
Move `return` statements outside `fz_try`/`fz_catch` by using result variables:

```c
// Before (BUGGY)
PdfrPage *pdfr_load_page(PdfrDoc *doc, int page_idx) {
    fz_try(doc->ctx) {
        fz_page *page = fz_load_page(doc->ctx, doc->doc, page_idx);
        if (page) {
            PdfrPage *pd = calloc(1, sizeof(PdfrPage));
            pd->page = page;
            return pd;           // ← LEAKS error stack
        }
    }
    fz_catch(doc->ctx) {
        return NULL;
    }
    return NULL;
}

// After (FIXED)
PdfrPage *pdfr_load_page(PdfrDoc *doc, int page_idx) {
    PdfrPage *pd = NULL;        // result variable OUTSIDE try
    fz_try(doc->ctx) {
        fz_page *page = fz_load_page(doc->ctx, doc->doc, page_idx);
        if (page) {
            pd = calloc(1, sizeof(PdfrPage));
            pd->page = page;
        }
    }
    fz_catch(doc->ctx) {
        free(pd);               // clean up partial alloc
        pd = NULL;
    }
    return pd;
}
```

Same pattern applied to `pdfr_open` (moved `PdfrDoc` allocation before `fz_try`) and `pdfr_count_pages` (assigned to local `int n` instead of returning directly).

**Key files:**
- `src/pdf_mupdf.c:47-78` (pdfr_open)
- `src/pdf_mupdf.c:80-87` (pdfr_count_pages)
- `src/pdf_mupdf.c:105-119` (pdfr_load_page)
- `src/pdf_mupdf.c:289-301` (pdfr_search_page — catch cleanup)
```
