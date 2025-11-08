/* tests/unit/test_emlog_init.c
 * Unit tests for emlog_init
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <string.h>
#include <stdlib.h>

#include "emlog.h"

/* Reuse the capture helper similar to set_level tests */
struct capture { char *buf; size_t cap; size_t len; };

static ssize_t capture_writer(eml_level_t lvl, const char *line, size_t n, void *user)
{
    (void)lvl;
    struct capture *c = (struct capture*)user;
    size_t avail = (c->cap > 0) ? c->cap - 1 - c->len : 0;
    size_t to_copy = (n < avail) ? n : avail;
    if (to_copy > 0) {
        memcpy(c->buf + c->len, line, to_copy);
        c->len += to_copy;
        c->buf[c->len] = '\0';
    }
    return (ssize_t)n;
}

/* Test 1: env absent and init(-1,false) should default to INFO and no timestamps */
static void test_init_default_env(void **state)
{
    (void)state;
    unsetenv("EMLOG_LEVEL");
    struct capture c = {0};
    c.cap = 1024; c.buf = malloc(c.cap); assert_non_null(c.buf);
    c.len = 0; c.buf[0] = '\0';

    emlog_set_writer(capture_writer, &c);
    emlog_init(-1, false);
    emlog_log(EML_LEVEL_DBG, "UT", "DBG");
    emlog_log(EML_LEVEL_INFO, "UT", "INF");

    assert_null(strstr(c.buf, "DBG"));
    assert_non_null(strstr(c.buf, "INF"));

    emlog_set_writer(NULL, NULL);
    free(c.buf);
}

/* Test 2: explicit init with min_level DBG should let DBG through */
static void test_init_explicit_dbg(void **state)
{
    (void)state;
    struct capture c = {0};
    c.cap = 1024; c.buf = malloc(c.cap); assert_non_null(c.buf);
    c.len = 0; c.buf[0] = '\0';

    emlog_set_writer(capture_writer, &c);
    emlog_init(EML_LEVEL_DBG, true);
    emlog_log(EML_LEVEL_DBG, "UT", "DBG");
    emlog_log(EML_LEVEL_INFO, "UT", "INF");
    assert_non_null(strstr(c.buf, "DBG"));
    assert_non_null(strstr(c.buf, "INF"));

    emlog_set_writer(NULL, NULL);
    free(c.buf);
}

/* Test 3: EMLOG_LEVEL env parsing - set to 'warn' should filter out info */
static void test_init_env_parsing(void **state)
{
    (void)state;
    setenv("EMLOG_LEVEL", "warn", 1);
    struct capture c = {0};
    c.cap = 1024; c.buf = malloc(c.cap); assert_non_null(c.buf);
    c.len = 0; c.buf[0] = '\0';

    emlog_set_writer(capture_writer, &c);
    emlog_init(-1, true);
    emlog_log(EML_LEVEL_INFO, "UT", "INF");
    emlog_log(EML_LEVEL_WARN, "UT", "WRN");

    assert_null(strstr(c.buf, "INF"));
    assert_non_null(strstr(c.buf, "WRN"));

    emlog_set_writer(NULL, NULL);
    free(c.buf);
}

/* expose tests to runner */
void emlog_init_default_env(void **state) { test_init_default_env(state); }
void emlog_init_explicit_dbg(void **state) { test_init_explicit_dbg(state); }
void emlog_init_env_parsing(void **state) { test_init_env_parsing(state); }
