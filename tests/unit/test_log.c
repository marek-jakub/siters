#include "test_common.h"

/* ================================================================
   Tests for vulnerability #1: hardened logging
   ================================================================ */

void test_log_file_hardening(void **state) {
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

void test_log_rejects_symlink(void **state) {
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

void test_log_rotates_at_limit(void **state) {
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
