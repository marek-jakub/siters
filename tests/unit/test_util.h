#ifndef SITERS_TEST_UTIL_H
#define SITERS_TEST_UTIL_H

#ifdef __cplusplus
extern "C" {
#endif

/* Temp scratch dir shared by the whole suite. */
extern char *tests_log_dir;

/* Build a minimal single-page PDF that repeats "word" `repeat` times. */
void write_text_pdf(const char *path, int repeat);

/* Build a minimal PDF with an outline chain of the given depth. */
void write_deep_outline_pdf(const char *path, int depth);

#ifdef __cplusplus
}
#endif

#endif /* SITERS_TEST_UTIL_H */