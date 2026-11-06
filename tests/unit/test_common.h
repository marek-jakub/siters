#ifndef SITERS_TEST_COMMON_H
#define SITERS_TEST_COMMON_H

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

/* Module headers the test suites exercise directly. */
#include "ui/window.h"
#include "sessions_model.h"
#include "session_model.h"
#include "document_model.h"
#include "app.h"
#include "tab.h"
#include "search.h"
#include "pdf/links.h"
#include "ui/tab_lifecycle.h"
#include "sessions_sidebar.h"
#include "session/sessions_actions.h"
#include "view.h"
#include "render.h"
#include "view/scroll.h"

/* Shared test infrastructure. */
#include "test_mocks.h"
#include "test_util.h"

#endif /* SITERS_TEST_COMMON_H */