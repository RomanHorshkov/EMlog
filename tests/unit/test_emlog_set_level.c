/*
 * tests/unit/test_emlog_set_level.c
 * Minimal, single-file unit test for emlog_set_level using cmocka.
 */

#include <stddef.h>
#include <stdarg.h>
#include <setjmp.h>
#include <cmocka.h>
#include <string.h>
#include <stdlib.h>

#include "emlog.h"

struct capture
{
    char*  buf; /* buffer to hold captured output */
    size_t cap; /* capacity of the buffer */
    size_t len; /* current length of captured data */
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

static void test_emlog_set_level_simple(void** state)
{
    (void)state;

    struct capture c = {0};
    c.cap            = 2048;
    c.buf            = malloc(c.cap);
    if(!c.buf) fail_msg("malloc failed");
    c.len    = 0;
    c.buf[0] = '\0';

    emlog_set_writer(capture_writer, &c);
    emlog_init(-1, false);

    eml_level_t  levels[] = {EML_LEVEL_DBG, EML_LEVEL_INFO, EML_LEVEL_WARN, EML_LEVEL_ERROR,
                             EML_LEVEL_CRIT};
    const char*  marks[]  = {"DBG_MARKER", "INF_MARKER", "WRN_MARKER", "ERR_MARKER", "CRT_MARKER"};
    const size_t nlevels  = sizeof(levels) / sizeof(levels[0]);

    for(size_t min_i = 0; min_i < nlevels; ++min_i)
    {
        emlog_set_level(levels[min_i]);
        c.len    = 0;
        c.buf[0] = '\0';

        /* emit one message at each level */
        for(size_t i = 0; i < nlevels; ++i)
            emlog_log(levels[i], "UNIT", "%s", marks[i]);

        /* verify which messages were captured */
        for(size_t i = 0; i < nlevels; ++i)
        {
            /* messages below min_level should NOT be present */
            if(levels[i] < levels[min_i])
                assert_null(strstr(c.buf, marks[i]));
            else
                assert_non_null(strstr(c.buf, marks[i]));
        }
    }

    emlog_set_writer(NULL, NULL);
    free(c.buf);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_emlog_set_level_simple),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
