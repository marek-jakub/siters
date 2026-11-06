#include <string.h>
#include <glib.h>

#include "test_util.h"

/* Temp dir the whole suite logs into (or uses as a config dir), so tests
   that emit LOG_* warnings or write state never touch the developer's real
   dirs. Allocated and cleaned up by the runner's main(). */
char *tests_log_dir = NULL;

void write_text_pdf(const char *path, int repeat) {
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

void write_deep_outline_pdf(const char *path, int depth) {
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
