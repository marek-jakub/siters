#include "test_common.h"

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

void test_has_link_at_inside_rectangle(void **state) {
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

void test_has_link_at_outside_rectangle(void **state) {
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

void test_has_link_at_null_safety(void **state) {
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
   Tests for vulnerability #2: link scheme gating
   ================================================================ */

void test_is_safe_link_scheme_safe(void **state) {
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

void test_is_safe_link_scheme_unsafe(void **state) {
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

void test_is_safe_link_scheme_malformed(void **state) {
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
