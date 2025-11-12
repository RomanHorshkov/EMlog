/* tests/unit/test_emlog_journald.c
 * Unit tests covering the journald writer integration.
 *
 * These tests exercise the public journald-related helpers regardless
 * of whether the library was compiled with real journald support. When
 * libsystemd is present the tests verify that enabling the journald
 * writer detaches any custom writer. When journald is unavailable the
 * tests confirm that the helper simply reports failure and leaves the
 * existing writer intact.
 */

#include <cmocka.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
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

static void test_journald_enable_behavior(void** state)
{
    (void)state;

    struct capture c = {0};
    c.cap            = 2048;
    c.buf            = malloc(c.cap);
    assert_non_null(c.buf);
    c.len    = 0;
    c.buf[0] = '\0';

    emlog_init(-1, false);
    emlog_set_level(EML_LEVEL_INFO);
    emlog_set_writer(capture_writer, &c);

    const bool have_journal = emlog_has_journald();
    const bool enabled      = emlog_enable_journald("unit-test");

    if(have_journal)
    {
        assert_true(enabled);
        /* The custom capture writer should no longer receive data once
         * the journald writer is installed. */
        emlog_log(EML_LEVEL_INFO, "UT", "JOURNALD_ACTIVE");
        assert_int_equal(c.len, 0);

        /* Re-installing a custom writer should override journald and
         * resume delivery to the capture helper. */
        c.len    = 0;
        c.buf[0] = '\0';
        emlog_set_writer(capture_writer, &c);
        emlog_log(EML_LEVEL_INFO, "UT", "CUSTOM_AGAIN");
        assert_non_null(strstr(c.buf, "CUSTOM_AGAIN"));
    }
    else
    {
        assert_false(enabled);
        /* Without journald support the helper should be a no-op and the
         * capture writer must still receive log lines. */
        emlog_log(EML_LEVEL_INFO, "UT", "JOURNALD_FALLBACK");
        assert_non_null(strstr(c.buf, "JOURNALD_FALLBACK"));
    }

    emlog_set_writer(NULL, NULL);
    free(c.buf);
}

void emlog_journald_enable_behavior(void** state)
{
    test_journald_enable_behavior(state);
}
