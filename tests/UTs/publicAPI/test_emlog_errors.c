/**
 * @file test_emlog_errors.c
 * @brief Unit tests for EMlog errno mapping, error names, exit-code mapping, and errno log formatting.
 */

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>

#include <cmocka.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "emlog.h"
#include "unit_tests.h"

struct capture
{
    char*  buf;
    size_t cap;
    size_t len;
};

static ssize_t capture_writer(eml_level_t lvl, const char* line, size_t n, void* user)
{
    (void)lvl;
    struct capture* c       = (struct capture*)user;
    size_t          avail   = (c->cap > 0) ? c->cap - 1 - c->len : 0;
    size_t          to_copy = (n < avail) ? n : avail;
    if(to_copy > 0)
    {
        memcpy(c->buf + c->len, line, to_copy);
        c->len         += to_copy;
        c->buf[c->len]  = '\0';
    }
    return (ssize_t)n;
}

static void test_emlog_log_errno_includes_context(void** state)
{
    (void)state;
    struct capture c = {.cap = 2048};
    c.buf            = calloc(1, c.cap);
    assert_non_null(c.buf);
    emlog_set_writer(capture_writer, &c);
    emlog_set_level(EML_LEVEL_DBG);
    emlog_enable_timestamps(false);

    const char* arg = "config.yaml";
    emlog_log_errno(EML_LEVEL_ERROR, "ERR", ENOENT, "failed to open %s", arg);

    const char* errstr = strerror(ENOENT);
    assert_non_null(errstr);
    assert_non_null(strstr(c.buf, "failed to open"));
    assert_non_null(strstr(c.buf, arg));
    assert_non_null(strstr(c.buf, errstr));
    assert_non_null(strstr(c.buf, "(2)"));

    emlog_set_writer(NULL, NULL);
    free(c.buf);
}

void emlog_log_errno_captures_context(void** state)
{
    test_emlog_log_errno_includes_context(state);
}
