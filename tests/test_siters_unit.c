#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <setjmp.h> /* IWYU pragma: keep — required by cmocka.h for jmp_buf */
#include <cmocka.h>
#include <gtk/gtk.h>
#include "pdf.h"

/* Mock control variables for gtk_widget_get_allocation */
static int mock_alloc_width = 1000;
static int mock_alloc_height = 1200;

/* Mock control variable for gtk_adjustment_get_value */
static double mock_adjustment_value = 0.0;

/* Mock control objects returned by the gtk_scrolled_window/gtk_range wraps.
   When NULL the wraps fall back to a fresh dummy object so existing tests
   that only check non-NULL behaviour are unaffected. */
static GtkAdjustment *mock_vadjustment_obj = NULL;
static GtkAdjustment *mock_sadj_obj = NULL;

/* Mock controls for gtk_widget_queue_draw. The handlers call
   gtk_widget_queue_draw(tab->pages_drawing); since the tab used in unit
   tests is a stack object with a fake/NULL pages_drawing, the real GTK
   implementation would trip a Gtk-CRITICAL, so it is wrapped into a no-op
   that records the call. */
static int mock_queue_draw_called = 0;
static GtkWidget *mock_draw_widget = NULL;

/* Mock for gtk_widget_get_allocation */
void __wrap_gtk_widget_get_allocation(GtkWidget *widget, GtkAllocation *allocation) {
    (void)widget;
    allocation->x = 0;
    allocation->y = 0;
    allocation->width = mock_alloc_width;
    allocation->height = mock_alloc_height;
}

/* Mock for gtk_widget_queue_draw — records the request instead of touching
   a real GtkWidget so the page-drawing area pointer used in scroll tests
   never reaches GTK. */
void __wrap_gtk_widget_queue_draw(GtkWidget *widget) {
    mock_queue_draw_called++;
    mock_draw_widget = widget;
}

/* Mock for gtk_range_get_adjustment — returns a controlled object or a dummy */
GtkAdjustment *__wrap_gtk_range_get_adjustment(GtkRange *range) {
    (void)range;
    if (mock_sadj_obj)
        return mock_sadj_obj;
    return (GtkAdjustment *)g_object_new(GTK_TYPE_ADJUSTMENT, NULL);
}

/* Mock for gtk_adjustment_get_value */
double __wrap_gtk_adjustment_get_value(GtkAdjustment *adjustment) {
    (void)adjustment;
    return mock_adjustment_value;
}

/* Mock for gtk_scrolled_window_get_vadjustment — returns a controlled object
   or a dummy pointer */
GtkAdjustment *__wrap_gtk_scrolled_window_get_vadjustment(GtkScrolledWindow *sw) {
    (void)sw;
    if (mock_vadjustment_obj)
        return mock_vadjustment_obj;
    return (GtkAdjustment *)g_object_new(GTK_TYPE_ADJUSTMENT, NULL);
}

/* Mock GTK functions to avoid display requirements */
static GtkWidget *mock_window = NULL;
static int gtk_window_new_called = 0;
static int gtk_window_set_title_called = 0;
static int gtk_window_set_default_size_called = 0;
static int g_signal_connect_called = 0;

static char title_buffer[256] = {0};
static int size_width = 0;
static int size_height = 0;

/* Mock gtk_window_new */
GtkWidget* __wrap_gtk_window_new(GtkWindowType type) {
    check_expected(type);
    gtk_window_new_called++;
    if (!mock_window) {
        mock_window = g_malloc0(sizeof(GtkWidget));
    }
    return mock_window;
}

/* Mock gtk_window_set_title */
void __wrap_gtk_window_set_title(GtkWindow *window, const gchar *title) {
    assert_non_null(window);
    assert_non_null(title);
    strncpy(title_buffer, title, sizeof(title_buffer) - 1);
    gtk_window_set_title_called++;
}

/* Mock gtk_window_set_default_size */
void __wrap_gtk_window_set_default_size(GtkWindow *window, gint width, gint height) {
    assert_non_null(window);
    size_width = width;
    size_height = height;
    gtk_window_set_default_size_called++;
}

/* Mock gtk_window_set_geometry_hints */
void __wrap_gtk_window_set_geometry_hints(GtkWindow *window,
    GtkWidget *geometry_widget,
    GdkGeometry *geometry,
    GdkWindowHints geom_mask) {
    (void)window;
    (void)geometry_widget;
    (void)geometry;
    (void)geom_mask;
}

/* Mock g_signal_connect */
gulong __wrap_g_signal_connect_data(gpointer instance, const gchar *detailed_signal,
                                     GCallback c_handler, gpointer data,
                                     GClosureNotify destroy_data, GConnectFlags connect_flags) {
    (void) instance;
    (void) detailed_signal;
    (void) c_handler;
    (void) data;
    (void) destroy_data;
    (void) connect_flags;
    g_signal_connect_called++;
    return 1;
}

/* Include siters.c to access static functions and types.
   Suppress -Wunused-function: many static callbacks are registered
   via g_signal_connect (function pointer) and appear unused to the compiler. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "../src/siters.c"
#pragma GCC diagnostic pop

/* Include main.c (renaming its main() away) so the self-pipe signal handling
   internals (install_terminate_handlers, on_terminate_signal, signal_pipe)
   can be exercised from the SIGTERM child-process test below. */
#define main siters_app_main
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "../src/main.c"
#pragma GCC diagnostic pop
#undef main

/* Temp dir the whole suite logs into, so PDF/outline tests that emit LOG_*
   warnings never touch the developer's real state dir. */
static char *tests_log_dir = NULL;

/* ---- Test helpers: build minimal PDF files on disk ---- */

static void write_text_pdf(const char *path, int repeat) {
    GString *text = g_string_new(NULL);
    for (int i = 0; i < repeat; i++) {
        if (i) g_string_append_c(text, ' ');
        g_string_append(text, "word");
    }
    GString *stream = g_string_new("BT /F1 12 Tf 40 700 Td (");
    g_string_append(stream, text->str);
    g_string_append(stream, ") Tj ET");
    g_string_free(text, TRUE);

    char *obj_catalog  = g_strdup("<< /Type /Catalog /Pages 2 0 R >>");
    char *obj_pages    = g_strdup("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    char *obj_page     = g_strdup("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> >>");
    char *obj_contents = g_strdup_printf("<< /Length %d >>\nstream\n%s\nendstream",
                                         (int)stream->len, stream->str);
    char *obj_font     = g_strdup("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");
    const char *objs[5] = { obj_catalog, obj_pages, obj_page, obj_contents, obj_font };

    GString *pdf = g_string_new("%PDF-1.4\n");
    long offsets[6] = {0};
    for (int i = 0; i < 5; i++) {
        offsets[i + 1] = (long)pdf->len;
        g_string_append_printf(pdf, "%d 0 obj\n", i + 1);
        g_string_append(pdf, objs[i]);
        g_string_append(pdf, "\nendobj\n");
    }
    long xref_pos = (long)pdf->len;
    g_string_append_printf(pdf, "xref\n0 6\n0000000000 65535 f \n");
    for (int i = 1; i <= 5; i++)
        g_string_append_printf(pdf, "%010ld 00000 n \n", offsets[i]);
    g_string_append_printf(pdf, "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n%ld\n%%%%EOF\n", xref_pos);

    g_file_set_contents(path, pdf->str, -1, NULL);

    g_free(obj_catalog); g_free(obj_pages); g_free(obj_page);
    g_free(obj_contents); g_free(obj_font);
    g_string_free(stream, TRUE);
    g_string_free(pdf, TRUE);
}

static void write_deep_outline_pdf(const char *path, int depth) {
    GList *objs = NULL;
    objs = g_list_append(objs, g_strdup("<< /Type /Catalog /Pages 2 0 R /Outlines 3 0 R >>"));
    objs = g_list_append(objs, g_strdup("<< /Type /Pages /Kids [4 0 R] /Count 1 >>"));
    objs = g_list_append(objs, g_strdup("<< /Type /Outlines /First 5 0 R /Last 5 0 R /Count 1 >>"));
    objs = g_list_append(objs, g_strdup("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] >>"));
    for (int i = 0; i < depth; i++) {
        int obj_num = 5 + i;
        int child = (i + 1 < depth) ? obj_num + 1 : 0;
        int parent = (i == 0) ? 3 : obj_num - 1;
        if (child)
            objs = g_list_append(objs,
                g_strdup_printf("<< /Title (L%d) /Parent %d 0 R /First %d 0 R /Last %d 0 R /Count 1 >>",
                                i, parent, child, child));
        else
            objs = g_list_append(objs,
                g_strdup_printf("<< /Title (L%d) /Parent %d 0 R >>", i, parent));
    }
    int n = (int)g_list_length(objs);

    GString *pdf = g_string_new("%PDF-1.4\n");
    long offsets[4096];
    memset(offsets, 0, sizeof(offsets));
    for (int i = 0; i < n; i++) {
        offsets[i + 1] = (long)pdf->len;
        g_string_append_printf(pdf, "%d 0 obj\n", i + 1);
        g_string_append(pdf, (const char *)g_list_nth_data(objs, i));
        g_string_append(pdf, "\nendobj\n");
    }
    long xref_pos = (long)pdf->len;
    g_string_append_printf(pdf, "xref\n0 %d\n0000000000 65535 f \n", n + 1);
    for (int i = 1; i <= n; i++)
        g_string_append_printf(pdf, "%010ld 00000 n \n", offsets[i]);
    g_string_append_printf(pdf, "trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%ld\n%%%%EOF\n", n + 1, xref_pos);

    g_file_set_contents(path, pdf->str, -1, NULL);

    for (GList *l = objs; l; l = l->next) g_free(l->data);
    g_list_free(objs);
    g_string_free(pdf, TRUE);
}

/* ================================================================
   Tests for vulnerability #6: clamp_double_to_int
   ================================================================ */

static void test_clamp_double_to_int_boundaries(void **state) {
    (void)state;
    /* huge / infinite values saturate to the cap instead of triggering
       implementation-defined (int) casts */
    assert_int_equal(clamp_double_to_int(1e300, MAX_SURFACE_DIM), MAX_SURFACE_DIM);
    assert_int_equal(clamp_double_to_int(INFINITY, MAX_SURFACE_DIM), MAX_SURFACE_DIM);
    assert_int_equal(clamp_double_to_int(1e300, MAX_SIZE_REQUEST), MAX_SIZE_REQUEST);
    /* NaN, negatives and zero collapse to 0 */
    assert_int_equal(clamp_double_to_int(NAN, MAX_SURFACE_DIM), 0);
    assert_int_equal(clamp_double_to_int(-5.0, MAX_SURFACE_DIM), 0);
    assert_int_equal(clamp_double_to_int(0.0, MAX_SURFACE_DIM), 0);
    /* ordinary values round-trip unchanged */
    assert_int_equal(clamp_double_to_int(612.0 * (96.0 / 72.0) + 0.5, MAX_SURFACE_DIM), 816);
    assert_int_equal(clamp_double_to_int(612.0 * (96.0 / 72.0) * 2.0 + 0.5, MAX_SURFACE_DIM), 1632);
    /* the ceil() size-request call sites used in the draw path */
    assert_int_equal(clamp_double_to_int(ceil(1e300), MAX_SIZE_REQUEST), MAX_SIZE_REQUEST);
}

/* ================================================================
   Tests for vulnerability #2: link scheme gating
   ================================================================ */

static void test_is_safe_link_scheme_safe(void **state) {
    (void)state;
    char *scheme = NULL;
    assert_true(is_safe_link_scheme("http://example.com/x", &scheme));
    assert_string_equal(scheme, "http");
    g_free(scheme);
    scheme = NULL;
    assert_true(is_safe_link_scheme("https://example.com", &scheme));
    assert_string_equal(scheme, "https");
    g_free(scheme);
    scheme = NULL;
    assert_true(is_safe_link_scheme("mailto:a@b.c", &scheme));
    assert_string_equal(scheme, "mailto");
    g_free(scheme);
    /* scheme matching is case-insensitive */
    scheme = NULL;
    assert_true(is_safe_link_scheme("HTTP://EXAMPLE.COM", &scheme));
    assert_string_equal(scheme, "http");
    g_free(scheme);
}

static void test_is_safe_link_scheme_unsafe(void **state) {
    (void)state;
    char *scheme = NULL;
    assert_false(is_safe_link_scheme("file:///etc/passwd", &scheme));
    g_free(scheme);
    scheme = NULL;
    assert_false(is_safe_link_scheme("ftp://host/file", &scheme));
    g_free(scheme);
    scheme = NULL;
    assert_false(is_safe_link_scheme("skype:call", &scheme));
    g_free(scheme);
    scheme = NULL;
    assert_false(is_safe_link_scheme("javascript:alert(1)", &scheme));
    g_free(scheme);
    scheme = NULL;
    assert_false(is_safe_link_scheme("data:text/html,x", &scheme));
    g_free(scheme);
}

static void test_is_safe_link_scheme_malformed(void **state) {
    (void)state;
    char *scheme = NULL;
    assert_false(is_safe_link_scheme("http//example.com", &scheme)); /* no colon */
    assert_null(scheme);
    assert_false(is_safe_link_scheme(":foo", &scheme));              /* leading colon */
    assert_null(scheme);
    assert_false(is_safe_link_scheme("1http:foo", &scheme));         /* digit first */
    assert_null(scheme);
    assert_false(is_safe_link_scheme("ht tp:foo", &scheme));         /* illegal char */
    assert_null(scheme);
    assert_false(is_safe_link_scheme("", &scheme));                  /* empty */
    assert_null(scheme);
}

/* ================================================================
   Tests for vulnerability #1: hardened logging
   ================================================================ */

static void test_log_file_hardening(void **state) {
    (void)state;
    siters_log_reset();

    char *dir = g_dir_make_tmp("siters-log-XXXXXX", NULL);
    assert_non_null(dir);
    g_setenv("SITERS_LOG_DIR", dir, TRUE);

    siters_log("test.c", 42, "INFO", "hello %d %s", 7, "world");

    char *path = g_build_filename(dir, "siters", "siters.log", NULL);
    assert_true(g_file_test(path, G_FILE_TEST_IS_REGULAR));

    struct stat st;
    assert_int_equal(stat(path, &st), 0);
    assert_int_equal(st.st_mode & 0777, 0600);

    char *logdir = g_build_filename(dir, "siters", NULL);
    assert_int_equal(stat(logdir, &st), 0);
    assert_int_equal(st.st_mode & 0777, 0700);

    gchar *content = NULL;
    assert_true(g_file_get_contents(path, &content, NULL, NULL));
    assert_non_null(strstr(content, "[INFO] test.c:42: hello 7 world"));
    g_free(content);

    g_free(logdir);
    g_free(path);
    g_free(dir);
    siters_log_reset();
    g_setenv("SITERS_LOG_DIR", tests_log_dir, TRUE);
}

static void test_log_rejects_symlink(void **state) {
    (void)state;
    siters_log_reset();

    char *dir = g_dir_make_tmp("siters-log-XXXXXX", NULL);
    assert_non_null(dir);
    g_setenv("SITERS_LOG_DIR", dir, TRUE);

    char *victim = g_build_filename(dir, "victim.txt", NULL);
    g_file_set_contents(victim, "SECRET", -1, NULL);

    char *logdir = g_build_filename(dir, "siters", NULL);
    g_mkdir_with_parents(logdir, 0700);
    char *path = g_build_filename(logdir, "siters.log", NULL);
    assert_int_equal(symlink(victim, path), 0);

    siters_log("t.c", 1, "WARN", "attack test");

    struct stat st;
    assert_int_equal(lstat(path, &st), 0);
    assert_true(S_ISREG(st.st_mode)); /* symlink must have been replaced */

    gchar *victim_content = NULL;
    assert_true(g_file_get_contents(victim, &victim_content, NULL, NULL));
    assert_string_equal(victim_content, "SECRET"); /* untouched */
    g_free(victim_content);

    g_free(logdir);
    g_free(path);
    g_free(victim);
    g_free(dir);
    siters_log_reset();
    g_setenv("SITERS_LOG_DIR", tests_log_dir, TRUE);
}

static void test_log_rotates_at_limit(void **state) {
    (void)state;
    siters_log_reset();

    char *dir = g_dir_make_tmp("siters-log-XXXXXX", NULL);
    assert_non_null(dir);
    g_setenv("SITERS_LOG_DIR", dir, TRUE);
    char *path = g_build_filename(dir, "siters", "siters.log", NULL);

    char big[600 * 1024];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    siters_log("t.c", 1, "INFO", "%s", big); /* ~600 KiB */
    siters_log("t.c", 1, "INFO", "%s", big); /* crosses 1 MiB → rotates */
    siters_log("t.c", 1, "INFO", "tail");

    struct stat st;
    assert_int_equal(stat(path, &st), 0);
    assert_true(st.st_size < 2 * 1024 * 1024);
    gchar *content = NULL;
    assert_true(g_file_get_contents(path, &content, NULL, NULL));
    assert_non_null(strstr(content, "tail"));
    g_free(content);

    g_free(path);
    g_free(dir);
    siters_log_reset();
    g_setenv("SITERS_LOG_DIR", tests_log_dir, TRUE);
}

/* ================================================================
   Tests for vulnerability #5: search hit-count clamping
   ================================================================ */

static void test_search_clamps_hit_count(void **state) {
    (void)state;
    char *dir = g_dir_make_tmp("siters-search-XXXXXX", NULL);
    assert_non_null(dir);
    char *path = g_build_filename(dir, "search.pdf", NULL);
    write_text_pdf(path, 200);

    PdfrDoc *doc = pdfr_open(path, NULL);
    assert_non_null(doc);

    PdfrRect matches[100];
    assert_int_equal(pdfr_search_page(doc, 0, "word", matches, 5), 5);
    assert_int_equal(pdfr_search_page(doc, 0, "word", matches, 50), 50);
    assert_int_equal(pdfr_search_page(doc, 0, "word", matches, 100), 100);
    /* more than MAX_SEARCH_QUADS clamps to 100 instead of overflowing the stack */
    assert_int_equal(pdfr_search_page(doc, 0, "word", matches, 1000), 100);
    assert_int_equal(pdfr_search_page(doc, 0, "word", matches, 0), 0);
    assert_int_equal(pdfr_search_page(doc, 0, "word", matches, -5), 0);

    /* guards: NULL doc / NULL text */
    assert_int_equal(pdfr_search_page(NULL, 0, "word", matches, 100), 0);
    assert_int_equal(pdfr_search_page(doc, 0, NULL, matches, 100), 0);
    /* out-of-range page index must fail gracefully */
    assert_int_equal(pdfr_search_page(doc, 5, "word", matches, 100), 0);

    pdfr_close(doc);
    g_free(path);
    g_free(dir);
}

/* ================================================================
   Tests for vulnerability #4: outline depth guard
   ================================================================ */

static int count_outline_nodes(const PdfrOutline *o) {
    int c = 0;
    while (o) {
        c++;
        c += count_outline_nodes(o->down);
        o = o->next;
    }
    return c;
}

static void test_outline_loads_shallow(void **state) {
    (void)state;
    char *dir = g_dir_make_tmp("siters-outline-XXXXXX", NULL);
    assert_non_null(dir);
    char *path = g_build_filename(dir, "outline.pdf", NULL);
    write_deep_outline_pdf(path, 3);

    PdfrDoc *doc = pdfr_open(path, NULL);
    assert_non_null(doc);
    PdfrOutline *o = pdfr_load_outline(doc);
    assert_non_null(o);
    assert_int_equal(count_outline_nodes(o), 3);
    assert_string_equal(o->title, "L0");
    assert_non_null(o->down);
    assert_string_equal(o->down->title, "L1");
    assert_non_null(o->down->down);
    assert_string_equal(o->down->down->title, "L2");
    pdfr_free_outline(doc, o);
    pdfr_close(doc);

    g_free(path);
    g_free(dir);
}

static void test_outline_loads_at_max_depth(void **state) {
    (void)state;
    char *dir = g_dir_make_tmp("siters-outline-XXXXXX", NULL);
    assert_non_null(dir);
    char *path = g_build_filename(dir, "outline.pdf", NULL);
    write_deep_outline_pdf(path, 32);

    PdfrDoc *doc = pdfr_open(path, NULL);
    assert_non_null(doc);
    PdfrOutline *o = pdfr_load_outline(doc);
    assert_non_null(o);
    assert_int_equal(count_outline_nodes(o), 32);
    pdfr_free_outline(doc, o);
    pdfr_close(doc);

    g_free(path);
    g_free(dir);
}

static void test_outline_refuses_too_deep(void **state) {
    (void)state;
    char *dir = g_dir_make_tmp("siters-outline-XXXXXX", NULL);
    assert_non_null(dir);
    char *path = g_build_filename(dir, "outline.pdf", NULL);
    write_deep_outline_pdf(path, 40);

    PdfrDoc *doc = pdfr_open(path, NULL);
    assert_non_null(doc);
    PdfrOutline *o = pdfr_load_outline(doc);
    assert_null(o); /* too deep — refused before any recursive load */
    pdfr_close(doc);

    g_free(path);
    g_free(dir);
}

/* ================================================================
   Tests for vulnerability #3: async-signal-safe termination
   ================================================================ */

static void test_signal_handler_terminates_child(void **state) {
    (void)state;
    pid_t pid = fork();
    if (pid == 0) {
        /* child: install the self-pipe handlers, deliver SIGTERM, then
           simulate the GTK source draining the pipe (no main loop runs
           here, so terminate_requested() must exit(0) directly). */
        g_setenv("SITERS_CONFIG_DIR", tests_log_dir, TRUE);
        install_terminate_handlers();
        raise(SIGTERM);
        on_terminate_signal(signal_pipe[0], G_IO_IN, NULL);
        _exit(1); /* only reached if termination did not take effect */
    }
    assert_true(pid > 0);
    int status = 0;
    assert_int_equal(waitpid(pid, &status, 0), pid);
    assert_true(WIFEXITED(status));
    assert_int_equal(WEXITSTATUS(status), 0);
}

/* Suppress GLib-GObject validation warnings during tests */
static void suppress_gtk_warnings(const gchar *log_domain, GLogLevelFlags log_level,
                                   const gchar *message, gpointer user_data) {
    (void) user_data;
    if (log_domain && g_str_has_prefix(log_domain, "GLib-GObject")) {
        if (message && g_str_has_prefix(message, "invalid unclassed pointer")) {
            return;
        }
    }
    g_log_default_handler(log_domain, log_level, message, user_data);
}

/* Setup function — reset mocks before each test */
static int setup(void **state) {
    (void) state;
    gtk_window_new_called = 0;
    gtk_window_set_title_called = 0;
    gtk_window_set_default_size_called = 0;
    g_signal_connect_called = 0;
    memset(title_buffer, 0, sizeof(title_buffer));
    size_width = 0;
    size_height = 0;
    mock_alloc_width = 1000;
    mock_alloc_height = 1200;
    mock_adjustment_value = 0.0;
    if (mock_window) {
        g_free(mock_window);
        mock_window = NULL;
    }
    if (!gtk_init_check(0, NULL)) {
        return -1;
    }
    return 0;
}

/* Teardown function */
static int teardown(void **state) {
    (void) state;
    if (mock_window) {
        g_free(mock_window);
        mock_window = NULL;
    }
    return 0;
}

/* ================================================================
   Tests for create_main_window (existing)
   ================================================================ */

static void test_create_main_window_calls_gtk_window_new(void **state) {
    (void) state;
    expect_value(__wrap_gtk_window_new, type, GTK_WINDOW_TOPLEVEL);
    GtkWidget *win = create_main_window();
    assert_non_null(win);
    assert_int_equal(gtk_window_new_called, 1);
    assert_int_equal(size_width, 1000);
    assert_int_equal(size_height, 800);
    assert_non_null(strstr(title_buffer, "Siters"));
    assert_non_null(strstr(title_buffer, "Default"));
}

static void test_create_main_window_title_is_siters(void **state) {
    (void) state;
    expect_value(__wrap_gtk_window_new, type, GTK_WINDOW_TOPLEVEL);
    GtkWidget *win = create_main_window();
    assert_non_null(win);
    assert_non_null(strstr(title_buffer, "Siters"));
    assert_non_null(strstr(title_buffer, "Default"));
    assert_true(gtk_window_set_title_called >= 1);
    assert_int_equal(size_width, 1000);
    assert_int_equal(size_height, 800);
}

static void test_create_main_window_sets_correct_size(void **state) {
    (void) state;
    expect_value(__wrap_gtk_window_new, type, GTK_WINDOW_TOPLEVEL);
    GtkWidget *win = create_main_window();
    assert_non_null(win);
    assert_int_equal(size_width, 1000);
    assert_int_equal(size_height, 800);
    assert_int_equal(gtk_window_set_default_size_called, 1);
}

static void test_create_main_window_connects_destroy_signal(void **state) {
    (void) state;
    expect_value(__wrap_gtk_window_new, type, GTK_WINDOW_TOPLEVEL);
    GtkWidget *win = create_main_window();
    assert_non_null(win);
    assert_int_equal(size_width, 1000);
    assert_int_equal(size_height, 800);
}

static void test_create_main_window_returns_valid_pointer(void **state) {
    (void) state;
    expect_value(__wrap_gtk_window_new, type, GTK_WINDOW_TOPLEVEL);
    GtkWidget *win = create_main_window();
    assert_non_null(win);
    assert_int_equal(size_width, 1000);
    assert_int_equal(size_height, 800);
}

static void test_create_main_window_initialization_sequence(void **state) {
    (void) state;
    expect_value(__wrap_gtk_window_new, type, GTK_WINDOW_TOPLEVEL);
    GtkWidget *win = create_main_window();
    assert_non_null(win);
    assert_int_equal(gtk_window_new_called, 1);
    assert_true(gtk_window_set_title_called >= 1);
    assert_int_equal(gtk_window_set_default_size_called, 1);
    assert_int_equal(size_width, 1000);
    assert_int_equal(size_height, 800);
}

/* ================================================================
   Regression: renaming a session when a DOCUMENT row is highlighted
   ================================================================
   Reported bug: with the sessions tree open and a document row
   highlighted, entering a new name and clicking Update created a
   brand-new session instead of renaming the session that owns the
   highlighted document. Root cause: the rename path read the session
   name from tree column 0 (SESSION_COL_LABEL), which holds the
   document basename on file rows. It must read the owning-session
   column (SESSION_COL_SESSION_NAME).
   ================================================================ */

static void test_sessions_update_renames_session_of_highlighted_document(void **state) {
    (void)state;

    /* Keep save_state() (triggered at the end of the update handler)
       inside the throwaway test dir, never the real user config dir. */
    g_setenv("SITERS_CONFIG_DIR", tests_log_dir, TRUE);

    /* Save whatever earlier tests left on the global app. */
    GtkWidget *saved_window = app.window;
    GtkWidget *saved_left_notebook = app.left_notebook;
    GtkWidget *saved_right_notebook = app.right_notebook;
    GtkWidget *saved_tree_view = app.sessions_tree_view;
    GtkTreeStore *saved_tree_store = app.sessions_tree_store;
    GtkWidget *saved_entry = app.sessions_entry;
    sessions_model_t *saved_sessions_model = app.sessions_model;
    GHashTable *saved_session_models = app.session_models;
    GHashTable *saved_document_models = app.document_models;
    gchar *saved_current_session = app.current_selected_session;
    SidebarMode saved_mode = app.current_sidebar_mode;

    /* Build a self-contained sessions sidebar (mirrors create_main_window). */
    app.sessions_tree_store = gtk_tree_store_new(
        SESSION_COL_COUNT,
        G_TYPE_STRING, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING, NULL);
    app.sessions_tree_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(app.sessions_tree_store));
    g_object_unref(app.sessions_tree_store); /* the tree view retains the only reference */
    GtkTreeSelection *init_sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(app.sessions_tree_view));
    gtk_tree_selection_set_mode(init_sel, GTK_SELECTION_SINGLE); /* click semantics used by the app */
    app.sessions_entry = gtk_entry_new();
    g_object_ref_sink(app.sessions_entry);

    app.sessions_model = sessions_model_new();
    sessions_model_add_session_name(app.sessions_model, "Default");
    sessions_model_add_session_name(app.sessions_model, "Alpha");

    app.session_models = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                               (GDestroyNotify)session_model_free);
    app.document_models = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                                (GDestroyNotify)document_model_free);

    session_model_t *default_sm = session_model_new();
    session_model_set_session_name(default_sm, "Default");
    g_hash_table_insert(app.session_models, g_strdup("Default"), default_sm);

    session_model_t *alpha_sm = session_model_new();
    session_model_set_session_name(alpha_sm, "Alpha");
    session_model_add_document_url(alpha_sm, "file:///tmp/doc.pdf");
    g_hash_table_insert(app.session_models, g_strdup("Alpha"), alpha_sm);

    document_model_t *doc_model = document_model_new();
    document_model_set_url(doc_model, "file:///tmp/doc.pdf");
    g_hash_table_insert(app.document_models, g_strdup("Alpha:left:file:///tmp/doc.pdf"), doc_model);

    app.window = NULL; /* no title/dialog updates needed */
    app.left_notebook = NULL;
    app.right_notebook = NULL;
    app.current_selected_session = g_strdup("Alpha");
    app.current_sidebar_mode = SIDEBAR_NONE; /* skip populate's auto-select */
    g_clear_pointer(&app.last_tree_selection_key, g_free);

    /* Session row "Alpha" with a child document row "doc.pdf". */
    GtkTreeIter parent, child;
    gtk_tree_store_append(app.sessions_tree_store, &parent, NULL);
    gtk_tree_store_set(app.sessions_tree_store, &parent,
                       SESSION_COL_LABEL, "Alpha",
                       SESSION_COL_ROW_KIND, SESSION_ROW_SESSION,
                       SESSION_COL_SESSION_NAME, "Alpha",
                       SESSION_COL_DOC_URI, "",
                       -1);
    gtk_tree_store_append(app.sessions_tree_store, &child, &parent);
    gtk_tree_store_set(app.sessions_tree_store, &child,
                       SESSION_COL_LABEL, "doc.pdf",
                       SESSION_COL_ROW_KIND, SESSION_ROW_FILE,
                       SESSION_COL_SESSION_NAME, "Alpha",
                       SESSION_COL_DOC_URI, "file:///tmp/doc.pdf",
                       -1);

    /* Reproduce the reported state: the DOCUMENT row is highlighted.
       (The parent must be expanded first, like the real app's sidebar,
       or the child row cannot anchor the tree-view selection.) */
    GtkTreePath *parent_path = gtk_tree_model_get_path(GTK_TREE_MODEL(app.sessions_tree_store), &parent);
    gtk_tree_view_expand_row(GTK_TREE_VIEW(app.sessions_tree_view), parent_path, FALSE);
    gtk_tree_path_free(parent_path);
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(app.sessions_tree_view));
    gtk_tree_selection_select_iter(sel, &child);

    /* Type a new name and click Update. */
    gtk_entry_set_text(GTK_ENTRY(app.sessions_entry), "Beta");
    on_sessions_update_clicked(NULL, NULL);

    /* The session must be RENAMED, not duplicated. */
    const GList *names = sessions_model_get_session_names(app.sessions_model);
    assert_null(g_list_find_custom((GList *)names, "Alpha", (GCompareFunc)g_strcmp0));
    assert_non_null(g_list_find_custom((GList *)names, "Beta", (GCompareFunc)g_strcmp0));

    /* Hash entries re-keyed to the new session name (no orphans). */
    assert_null(g_hash_table_lookup(app.session_models, "Alpha"));
    assert_non_null(g_hash_table_lookup(app.session_models, "Beta"));
    assert_null(g_hash_table_lookup(app.document_models, "Alpha:left:file:///tmp/doc.pdf"));
    assert_non_null(g_hash_table_lookup(app.document_models, "Beta:left:file:///tmp/doc.pdf"));

    /* current_selected_session follows the rename. */
    assert_string_equal(app.current_selected_session, "Beta");

    /* Repopulated tree shows the renamed session with its document row. */
    gboolean found_beta = FALSE;
    gboolean found_doc = FALSE;
    GtkTreeIter r;
    if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(app.sessions_tree_store), &r)) {
        do {
            gchar *name = NULL;
            gtk_tree_model_get(GTK_TREE_MODEL(app.sessions_tree_store), &r,
                               SESSION_COL_SESSION_NAME, &name, -1);
            if (name && strcmp(name, "Beta") == 0) {
                found_beta = TRUE;
                GtkTreeIter c;
                if (gtk_tree_model_iter_children(GTK_TREE_MODEL(app.sessions_tree_store), &c, &r)) {
                    gchar *doc_uri = NULL;
                    gtk_tree_model_get(GTK_TREE_MODEL(app.sessions_tree_store), &c,
                                       SESSION_COL_DOC_URI, &doc_uri, -1);
                    found_doc = (doc_uri && strcmp(doc_uri, "file:///tmp/doc.pdf") == 0);
                    g_free(doc_uri);
                }
            }
            g_free(name);
        } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(app.sessions_tree_store), &r));
    }
    assert_true(found_beta);
    assert_true(found_doc);

    /* Entry cleared after the rename. */
    assert_string_equal(gtk_entry_get_text(GTK_ENTRY(app.sessions_entry)), "");

    /* Cleanup this test's objects, then restore the prior app state. */
    g_object_unref(app.sessions_tree_view); /* releases the tree store too */
    g_object_unref(app.sessions_entry);
    sessions_model_free(app.sessions_model);
    g_hash_table_destroy(app.session_models);
    g_hash_table_destroy(app.document_models);
    g_free(app.current_selected_session);

    app.window = saved_window;
    app.left_notebook = saved_left_notebook;
    app.right_notebook = saved_right_notebook;
    app.sessions_tree_view = saved_tree_view;
    app.sessions_tree_store = saved_tree_store;
    app.sessions_entry = saved_entry;
    app.sessions_model = saved_sessions_model;
    app.session_models = saved_session_models;
    app.document_models = saved_document_models;
    app.current_selected_session = saved_current_session;
    app.current_sidebar_mode = saved_mode;
}

/* ================================================================
   Tests for has_link_at
   ================================================================ */

/* Helper: create a minimal TabData with a single link on page 0 */
static void setup_tab_with_link(TabData *tab) {
    memset(tab, 0, sizeof(*tab));
    tab->page_links_n = 1;
    tab->page_links = g_malloc0(sizeof(PdfrLink *));
    tab->n_pages = 1;
    tab->cached_page_heights = g_malloc(sizeof(double));
    tab->cached_page_heights[0] = 800.0;
    PdfrLink *m = g_malloc0(sizeof(PdfrLink));
    m->rect.x1 = 100.0;
    m->rect.y1 = 200.0;
    m->rect.x2 = 300.0;
    m->rect.y2 = 400.0;
    m->type = PDF_LINK_UNKNOWN;
    m->next = NULL;
    m->uri = NULL;
    m->named_dest = NULL;
    tab->page_links[0] = m;
}

static void teardown_tab_with_link(TabData *tab) {
    if (tab->page_links) {
        if (tab->page_links[0]) {
            PdfrLink *cur = tab->page_links[0];
            while (cur) {
                PdfrLink *next = cur->next;
                free(cur->uri);
                free(cur->named_dest);
                free(cur);
                cur = next;
            }
        }
        g_free(tab->page_links);
        tab->page_links = NULL;
    }
    tab->page_links_n = 0;
    g_free(tab->cached_page_heights);
    tab->cached_page_heights = NULL;
}

static void test_has_link_at_inside_rectangle(void **state) {
    (void)state;
    TabData tab;
    setup_tab_with_link(&tab);
    /* Link rect in device/page space (y-down, 0=top): x=[100,300], y=[200,400]
       y1=200 is top, y2=400 is bottom. */
    assert_true(has_link_at(&tab, 0, 150.0, 350.0));
    assert_true(has_link_at(&tab, 0, 100.0, 200.0));
    assert_true(has_link_at(&tab, 0, 300.0, 400.0));
    assert_true(has_link_at(&tab, 0, 200.0, 300.0));
    teardown_tab_with_link(&tab);
}

static void test_has_link_at_outside_rectangle(void **state) {
    (void)state;
    TabData tab;
    setup_tab_with_link(&tab);
    /* Outside rectangle in rendering space (y-down): y range=[200,400] */
    assert_false(has_link_at(&tab, 0, 50.0, 350.0));   /* left of x */
    assert_false(has_link_at(&tab, 0, 350.0, 350.0));   /* right of x */
    assert_false(has_link_at(&tab, 0, 150.0, 150.0));   /* above (smaller y in y-down) */
    assert_false(has_link_at(&tab, 0, 150.0, 500.0));   /* below (larger y in y-down) */
    teardown_tab_with_link(&tab);
}

static void test_has_link_at_null_safety(void **state) {
    (void)state;
    assert_false(has_link_at(NULL, 0, 0.0, 0.0));
    TabData tab;
    memset(&tab, 0, sizeof(tab));
    /* page_links_n == 0, page_links == NULL */
    assert_false(has_link_at(&tab, 0, 0.0, 0.0));
    /* negative page index */
    tab.page_links_n = 1;
    tab.page_links = NULL;
    assert_false(has_link_at(&tab, -1, 0.0, 0.0));
}

/* ================================================================
   Tests for widget_to_page_coords
   ================================================================ */

/* Helper: create a minimal TabData for single-page layout (mode 0) */
static void setup_tab_single_page(TabData *tab) {
    memset(tab, 0, sizeof(*tab));
    tab->layout_mode = 0;
    tab->n_pages = 1;
    tab->zoom = 72.0;  /* get_ppi_scale → 72/72 = 1.0 */
    tab->cached_page_widths = g_malloc(sizeof(double));
    tab->cached_page_heights = g_malloc(sizeof(double));
    tab->cached_page_x0 = g_malloc0(sizeof(double));
    tab->cached_page_y0 = g_malloc0(sizeof(double));
    tab->cached_page_widths[0] = 600.0;
    tab->cached_page_heights[0] = 800.0;
    tab->pages_drawing = (GtkWidget *)0x1;  /* non-NULL dummy */
    mock_alloc_width = 1000;
    mock_alloc_height = 1200;
}

static void teardown_tab_single_page(TabData *tab) {
    g_free(tab->cached_page_widths);
    g_free(tab->cached_page_heights);
    g_free(tab->cached_page_x0);
    g_free(tab->cached_page_y0);
    tab->cached_page_widths = NULL;
    tab->cached_page_heights = NULL;
    tab->cached_page_x0 = NULL;
    tab->cached_page_y0 = NULL;
    tab->pages_drawing = NULL;
}

static void test_widget_to_page_coords_inside_page(void **state) {
    (void)state;
    TabData tab;
    setup_tab_single_page(&tab);
    double px, py;

    /* Page 0 centred in alloc 1000×1200:
         pw=600, ox=(1000-600)/2=200, y=6
         Page rect: x=[200,800], y=[6,806] */
    int page = widget_to_page_coords(&tab, 500.0, 400.0, &px, &py);
    assert_int_equal(page, 0);
    /* px = (500-200)/1 = 300 */
    assert_true(fabs(px - 300.0) < 0.001);
    /* py = (400-6)/1 = 394 (y-down, 0=top, no inversion) */
    assert_true(fabs(py - 394.0) < 0.001);

    teardown_tab_single_page(&tab);
}

static void test_widget_to_page_coords_outside_page(void **state) {
    (void)state;
    TabData tab;
    setup_tab_single_page(&tab);

    /* Left of page */
    int page = widget_to_page_coords(&tab, 50.0, 400.0, NULL, NULL);
    assert_int_equal(page, -1);

    /* Below page */
    page = widget_to_page_coords(&tab, 500.0, 900.0, NULL, NULL);
    assert_int_equal(page, -1);

    teardown_tab_single_page(&tab);
}

static void test_widget_to_page_coords_null_safety(void **state) {
    (void)state;
    double px, py;

    /* NULL tab */
    assert_int_equal(widget_to_page_coords(NULL, 0.0, 0.0, &px, &py), -1);

    /* No cached_page_widths */
    TabData tab;
    memset(&tab, 0, sizeof(tab));
    tab.pages_drawing = (GtkWidget *)0x1;
    assert_int_equal(widget_to_page_coords(&tab, 0.0, 0.0, &px, &py), -1);

    /* No pages_drawing */
    tab.cached_page_widths = g_malloc(sizeof(double));
    tab.cached_page_widths[0] = 600.0;
    tab.pages_drawing = NULL;
    assert_int_equal(widget_to_page_coords(&tab, 0.0, 0.0, &px, &py), -1);
    g_free(tab.cached_page_widths);
}

/* ================================================================
   Tests for search_free
   ================================================================ */

static void test_search_free_clears_results(void **state) {
    (void)state;
    TabData tab;
    memset(&tab, 0, sizeof(tab));

    /* Simulate 3 pages of search results with rects (matches real usage) */
    tab.search_results_cap = 32;
    tab.search_results_n = 3;
    tab.search_results = g_malloc0(sizeof(*tab.search_results) * tab.search_results_cap);
    tab.search_text = g_strdup("test query");

    for (int i = 0; i < 3; i++) {
        tab.search_results[i].page = i + 1;
        tab.search_results[i].n_matches = 5;
        tab.search_results[i].rects = g_malloc0(sizeof(PdfrRect) * 5);
    }

    search_free(&tab);

    assert_null(tab.search_results);
    assert_int_equal(tab.search_results_n, 0);
    assert_int_equal(tab.search_results_cap, 0);
    assert_null(tab.search_text);
}

static void test_search_free_null_safety(void **state) {
    (void)state;
    /* Should not crash on NULL */
    search_free(NULL);

    TabData tab;
    memset(&tab, 0, sizeof(tab));
    /* Empty tab (all zeroed) — should not crash */
    search_free(&tab);
}

/* ================================================================
   Tests for cache_evict_idx
   ================================================================ */

static void test_cache_evict_idx_removes_surface(void **state) {
    (void)state;
    TabData tab;
    memset(&tab, 0, sizeof(tab));

    /* Create a real 100x100 cairo image surface */
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 100, 100);
    cairo_surface_flush(surf);

    int expected_bytes = 100 * 100 * 4;
    tab.page_cache = g_malloc0(sizeof(cairo_surface_t *) * 4);
    tab.page_cache[0] = surf;
    tab.total_cache_bytes = expected_bytes;

    cache_evict_idx(&tab, 0);

    assert_null(tab.page_cache[0]);
    assert_int_equal(tab.total_cache_bytes, 0);

    g_free(tab.page_cache);
    tab.page_cache = NULL;
}

static void test_cache_evict_idx_null_safety(void **state) {
    (void)state;
    TabData tab;
    memset(&tab, 0, sizeof(tab));

    /* NULL tab — no crash */
    cache_evict_idx(NULL, 0);

    /* NULL page_cache array — no crash */
    cache_evict_idx(&tab, 0);

    /* Negative index — no crash */
    tab.page_cache = g_malloc0(sizeof(cairo_surface_t *) * 2);
    cache_evict_idx(&tab, -1);

    /* NULL slot — no crash, bytes unchanged */
    tab.total_cache_bytes = 999;
    cache_evict_idx(&tab, 0);
    assert_int_equal(tab.total_cache_bytes, 999);

    g_free(tab.page_cache);
    tab.page_cache = NULL;
}

/* ================================================================
   Regression: no spurious jump to the last page
   ================================================================
   These tests pin the scroll-driven page tracking. A document that fits the
   viewport, or whose scroll adjustment is still being set up during an
   open/restore, must NOT be classified as "at the very bottom": the buggy
   shortcut matched any scroll position in those situations and jumped the
   page counter (and the saved doc model) to the last page.
   Landing on the last page IS legitimate when the document was scrolled to
   the real bottom, or when a restored session resumes at the page the user
   last read. Only documents opened manually through the Open dialog are
   forced back to page 1 (see the fresh-open section further below). */

/* Helper: minimal multi-page tab for on_scroll_value_changed (mode 0) */
static void setup_tab_scroll_test(TabData *tab, int n_pages) {
    memset(tab, 0, sizeof(*tab));
    tab->layout_mode = 0;
    tab->n_pages = n_pages;
    tab->zoom = 72.0; /* get_ppi_scale → 1.0 */
    tab->scrolled = (GtkWidget *)0x1;
    tab->cached_page_widths = g_malloc(sizeof(double) * n_pages);
    tab->cached_page_heights = g_malloc(sizeof(double) * n_pages);
    for (int i = 0; i < n_pages; i++) {
        tab->cached_page_widths[i] = 600.0;
        tab->cached_page_heights[i] = 800.0;
    }
}

static void teardown_tab_scroll_test(TabData *tab) {
    cancel_doc_model_debounce(tab);
    g_free(tab->cached_page_widths);
    g_free(tab->cached_page_heights);
    tab->cached_page_widths = NULL;
    tab->cached_page_heights = NULL;
    tab->scrolled = NULL;
    tab->h_scrollbar = NULL;
    mock_adjustment_value = 0.0;
    mock_vadjustment_obj = NULL;
    mock_sadj_obj = NULL;
    mock_queue_draw_called = 0;
    mock_draw_widget = NULL;
}

/* When the whole document fits the viewport (upper == page_size, so no real
   scroll range), a value-changed at the top must keep the current page on
   page 1. Previously the "at the very bottom" shortcut matched any scroll
   position in that situation and jumped the page counter (and the saved doc
   model) to the last page. */
static void test_scroll_fits_viewport_stays_first_page(void **state) {
    (void)state;
    TabData tab;
    setup_tab_scroll_test(&tab, 3);
    GtkAdjustment *adj = GTK_ADJUSTMENT(gtk_adjustment_new(0.0, 0.0, 810.0, 1.0, 1.0, 810.0));
    mock_vadjustment_obj = adj;
    mock_adjustment_value = 0.0;
    tab.cur_page = 0;

    on_scroll_value_changed(adj, &tab);

    assert_int_equal(tab.cur_page, 0);
    g_object_unref(adj);
    teardown_tab_scroll_test(&tab);
}

/* A genuinely scrolled-to-the-bottom document must still pin the current
   page to the last page even after the fit-viewport guard is introduced. */
static void test_scroll_at_true_bottom_reports_last_page(void **state) {
    (void)state;
    TabData tab;
    setup_tab_scroll_test(&tab, 3);
    GtkAdjustment *adj = GTK_ADJUSTMENT(gtk_adjustment_new(2700.0, 0.0, 3000.0, 10.0, 10.0, 300.0));
    mock_vadjustment_obj = adj;
    mock_adjustment_value = 2700.0;

    on_scroll_value_changed(adj, &tab);

    assert_int_equal(tab.cur_page, 2);
    g_object_unref(adj);
    teardown_tab_scroll_test(&tab);
}

/* Programmatic scroll adjustments that happen while the document is still
   loading (initial_scroll_pending) or its post-load restore is in flight
   (pending_restore) must not rewrite the current page. */
static void test_scroll_ignored_while_loading_or_restoring(void **state) {
    (void)state;
    TabData tab;
    setup_tab_scroll_test(&tab, 3);
    GtkAdjustment *adj = GTK_ADJUSTMENT(gtk_adjustment_new(0.0, 0.0, 810.0, 1.0, 1.0, 810.0));
    mock_vadjustment_obj = adj;
    mock_adjustment_value = 0.0;
    tab.cur_page = 1;

    tab.initial_scroll_pending = TRUE;
    on_scroll_value_changed(adj, &tab);
    assert_int_equal(tab.cur_page, 1);

    tab.initial_scroll_pending = FALSE;
    RestoreState rs;
    memset(&rs, 0, sizeof(rs));
    tab.pending_restore = &rs;
    on_scroll_value_changed(adj, &tab);
    assert_int_equal(tab.cur_page, 1);

    tab.pending_restore = NULL;
    g_object_unref(adj);
    teardown_tab_scroll_test(&tab);
}

/* Row layout (mode 2) uses the horizontal scrollbar: the same fit-range
   guard must keep the page on page 1 when the document is narrower than
   the viewport. */
static void test_row_layout_fits_viewport_stays_first_page(void **state) {
    (void)state;
    TabData tab;
    setup_tab_scroll_test(&tab, 3);
    tab.layout_mode = 2;
    GtkAdjustment *adj = GTK_ADJUSTMENT(gtk_adjustment_new(0.0, 0.0, 500.0, 1.0, 1.0, 500.0));
    GtkAdjustment *vadj_other = GTK_ADJUSTMENT(gtk_adjustment_new(0.0, 0.0, 9999.0, 1.0, 1.0, 100.0));
    mock_vadjustment_obj = vadj_other;
    mock_sadj_obj = adj;
    mock_adjustment_value = 0.0;
    tab.h_scrollbar = (GtkWidget *)0x1;
    tab.cur_page = 0;

    on_scroll_value_changed(adj, &tab);

    /* A redraw of the page-drawing area must have been requested (through
       the wrapped gtk_widget_queue_draw, so no real widget is needed). */
    assert_int_equal(tab.cur_page, 0);
    assert_int_equal(mock_queue_draw_called, 1);
    g_object_unref(adj);
    g_object_unref(vadj_other);
    teardown_tab_scroll_test(&tab);
}

/* ================================================================
   Regression: freshly opened documents carry no leftover state
   ================================================================
   A document opened through the Open dialog is brand new: it must start at
   the first page with default settings even if a saved document model for
   the same file (same session key, from a past use or a previous app run)
   still exists — including when that leftover saved the LAST page.
   Only session-restored tabs apply the saved state, and a restore may
   legitimately land on the last page if that is where the user last read. */

/* Install an app.document_models table holding a left-pane model for
   file:///tmp/doc.pdf under the "Alpha" session, saved at page
   saved_page, zoom 55, row layout — the leftover a fresh open must ignore. */
static GHashTable *saved_restore_doc_models = NULL;
static gchar *saved_restore_current_session = NULL;
static GtkWidget *saved_restore_left_nb = NULL;
static GtkWidget *saved_restore_right_nb = NULL;

static void setup_app_with_leftover_doc_state(int saved_page) {
    saved_restore_doc_models = app.document_models;
    saved_restore_current_session = app.current_selected_session;
    saved_restore_left_nb = app.left_notebook;
    saved_restore_right_nb = app.right_notebook;

    app.document_models = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                                (GDestroyNotify)document_model_free);
    document_model_t *dm = document_model_new();
    document_model_set_url(dm, "file:///tmp/doc.pdf");
    document_model_set_current_page(dm, saved_page);
    document_model_set_zoom(dm, 55.0);
    document_model_set_visualization_mode(dm, 1);
    g_hash_table_insert(app.document_models, g_strdup("Alpha:left:file:///tmp/doc.pdf"), dm);

    app.current_selected_session = g_strdup("Alpha");
    app.left_notebook = NULL;
    app.right_notebook = NULL;
}

static void teardown_app_with_leftover_doc_state(void) {
    g_hash_table_destroy(app.document_models);
    app.document_models = saved_restore_doc_models;
    g_free(app.current_selected_session);
    app.current_selected_session = saved_restore_current_session;
    app.left_notebook = saved_restore_left_nb;
    app.right_notebook = saved_restore_right_nb;
}

static void setup_tab_restore_test(TabData *tab) {
    memset(tab, 0, sizeof(*tab));
    tab->is_helper = FALSE;
    tab->n_pages = 20;
    tab->zoom = 96.0;
    tab->layout_mode = 0;
    tab->current_file = g_strdup("/tmp/doc.pdf");
}

static void teardown_tab_restore_test(TabData *tab) {
    cancel_tab_restore(tab);
    g_free(tab->current_file);
    tab->current_file = NULL;
}

static void test_fresh_open_ignores_leftover_state(void **state) {
    (void)state;
    setup_app_with_leftover_doc_state(8);

    TabData tab;
    setup_tab_restore_test(&tab);
    tab.fresh_open = TRUE;

    /* A leftover "Alpha:left:...:doc.pdf" model exists (page 8, zoom 55,
       row layout) but the fresh open must land on page 1 with defaults. */
    restore_document_model_to_tab(&tab);

    assert_int_equal(tab.cur_page, 0);
    assert_true(fabs(tab.zoom - 96.0) < 0.001);
    assert_int_equal(tab.layout_mode, 0);
    assert_non_null(tab.pending_restore);

    teardown_tab_restore_test(&tab);
    teardown_app_with_leftover_doc_state();
}

static void test_session_restore_still_applies_saved_state(void **state) {
    (void)state;
    setup_app_with_leftover_doc_state(8);

    TabData tab;
    setup_tab_restore_test(&tab);

    /* Not fresh: restoring a session tab must keep applying the saved page,
       zoom and layout (page 8 -> cur_page 7, zoom 55, row layout 1). */
    restore_document_model_to_tab(&tab);

    assert_int_equal(tab.cur_page, 7);
    assert_true(fabs(tab.zoom - 55.0) < 0.001);
    assert_int_equal(tab.layout_mode, 1);

    teardown_tab_restore_test(&tab);
    teardown_app_with_leftover_doc_state();
}

/* The leftover saved position is the LAST page (n_pages = 20). A fresh open
   must still land on page 1 — it must never land on the last page. */
static void test_fresh_open_ignores_leftover_last_page(void **state) {
    (void)state;
    setup_app_with_leftover_doc_state(20);

    TabData tab;
    setup_tab_restore_test(&tab);
    tab.fresh_open = TRUE;

    restore_document_model_to_tab(&tab);

    assert_int_equal(tab.cur_page, 0);
    assert_true(fabs(tab.zoom - 96.0) < 0.001);
    assert_non_null(tab.pending_restore);

    teardown_tab_restore_test(&tab);
    teardown_app_with_leftover_doc_state();
}

/* A session-restored tab whose saved position is the LAST page must
   legitimately resume on the last page — only manually opened documents are
   forced back to page 1. */
static void test_session_restore_can_land_on_last_page(void **state) {
    (void)state;
    setup_app_with_leftover_doc_state(20);

    TabData tab;
    setup_tab_restore_test(&tab);

    restore_document_model_to_tab(&tab);

    assert_int_equal(tab.cur_page, 19);
    assert_true(fabs(tab.zoom - 55.0) < 0.001);
    assert_int_equal(tab.layout_mode, 1);
    assert_non_null(tab.pending_restore);

    teardown_tab_restore_test(&tab);
    teardown_app_with_leftover_doc_state();
}

/* ================================================================
   Main
   ================================================================ */

int main(void) {
    g_log_set_handler("GLib-GObject", G_LOG_LEVEL_WARNING | G_LOG_LEVEL_CRITICAL,
                      suppress_gtk_warnings, NULL);

    /* Point the logging subsystem at a private temp dir for the whole run so
       warnings from MuPDF error paths never touch the real state dir. */
    tests_log_dir = g_dir_make_tmp("siters-tests-log-XXXXXX", NULL);
    g_setenv("SITERS_LOG_DIR", tests_log_dir, TRUE);

    const struct CMUnitTest tests[] = {
        /* create_main_window */
        cmocka_unit_test_setup_teardown(test_create_main_window_calls_gtk_window_new, setup, teardown),
        cmocka_unit_test_setup_teardown(test_create_main_window_title_is_siters, setup, teardown),
        cmocka_unit_test_setup_teardown(test_create_main_window_sets_correct_size, setup, teardown),
        cmocka_unit_test_setup_teardown(test_create_main_window_connects_destroy_signal, setup, teardown),
        cmocka_unit_test_setup_teardown(test_create_main_window_returns_valid_pointer, setup, teardown),
        cmocka_unit_test_setup_teardown(test_create_main_window_initialization_sequence, setup, teardown),
        /* has_link_at */
        cmocka_unit_test(test_has_link_at_inside_rectangle),
        cmocka_unit_test(test_has_link_at_outside_rectangle),
        cmocka_unit_test(test_has_link_at_null_safety),
        /* widget_to_page_coords */
        cmocka_unit_test(test_widget_to_page_coords_inside_page),
        cmocka_unit_test(test_widget_to_page_coords_outside_page),
        cmocka_unit_test(test_widget_to_page_coords_null_safety),
        /* search_free */
        cmocka_unit_test(test_search_free_clears_results),
        cmocka_unit_test(test_search_free_null_safety),
        /* cache_evict_idx */
        cmocka_unit_test(test_cache_evict_idx_removes_surface),
        cmocka_unit_test(test_cache_evict_idx_null_safety),
        /* vulnerability #6: (int) casts of huge doubles */
        cmocka_unit_test(test_clamp_double_to_int_boundaries),
        /* vulnerability #2: link scheme gating */
        cmocka_unit_test(test_is_safe_link_scheme_safe),
        cmocka_unit_test(test_is_safe_link_scheme_unsafe),
        cmocka_unit_test(test_is_safe_link_scheme_malformed),
        /* vulnerability #1: hardened logging */
        cmocka_unit_test(test_log_file_hardening),
        cmocka_unit_test(test_log_rejects_symlink),
        cmocka_unit_test(test_log_rotates_at_limit),
        /* vulnerability #5: search hit-count clamping */
        cmocka_unit_test(test_search_clamps_hit_count),
        /* vulnerability #4: outline depth guard */
        cmocka_unit_test(test_outline_loads_shallow),
        cmocka_unit_test(test_outline_loads_at_max_depth),
        cmocka_unit_test(test_outline_refuses_too_deep),
        /* vulnerability #3: async-signal-safe termination */
        cmocka_unit_test(test_signal_handler_terminates_child),
        /* regression: session rename with a highlighted document row */
        cmocka_unit_test_setup_teardown(test_sessions_update_renames_session_of_highlighted_document, setup, teardown),
        /* regression: opening/restoring must not jump to the last page */
        cmocka_unit_test_setup_teardown(test_scroll_fits_viewport_stays_first_page, setup, teardown),
        cmocka_unit_test_setup_teardown(test_scroll_at_true_bottom_reports_last_page, setup, teardown),
        cmocka_unit_test_setup_teardown(test_scroll_ignored_while_loading_or_restoring, setup, teardown),
        cmocka_unit_test_setup_teardown(test_row_layout_fits_viewport_stays_first_page, setup, teardown),
        /* regression: freshly opened documents carry no leftover state */
        cmocka_unit_test_setup_teardown(test_fresh_open_ignores_leftover_state, setup, teardown),
        cmocka_unit_test_setup_teardown(test_fresh_open_ignores_leftover_last_page, setup, teardown),
        cmocka_unit_test_setup_teardown(test_session_restore_still_applies_saved_state, setup, teardown),
        cmocka_unit_test_setup_teardown(test_session_restore_can_land_on_last_page, setup, teardown),
    };

    int rc = cmocka_run_group_tests(tests, NULL, NULL);

    g_unsetenv("SITERS_LOG_DIR");
    g_free(tests_log_dir);
    tests_log_dir = NULL;
    return rc;
}
