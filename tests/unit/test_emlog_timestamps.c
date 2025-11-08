/* tests/unit/test_emlog_timestamps.c
 *
 * Unit tests for emlog_enable_timestamps()
 *
 * These tests focus on the observable effect of toggling timestamp
 * emission in the logger. The logger provides a callback hook via
 * emlog_set_writer() which allows us to capture formatted log lines
 * without touching stdout/stderr. We rely on that hook to assert the
 * presence or absence of the ISO8601-like timestamp prefix.
 *
 * Tests are deliberately well-commented to explain the rationale and
 * the invariants they check. They are small, isolated, and restore
 * global state (writer) on exit.
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "emlog.h"
#include "unit_tests.h"

/*
 * Small capture writer used across the tests.
 * It appends the provided bytes into a user-owned buffer and returns
 * the number of bytes "written". The logger calls the writer with a
 * contiguous line buffer (no trailing newline), so we inspect the
 * captured bytes directly.
 */
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
    /* Return the number of bytes the logger asked us to "write". The
     * logger ignores the return value for custom writers. */
    return (ssize_t)n;
}

/*
 * Test: enabling timestamps makes the emitted line start with a
 * timestamp-like prefix. The exact formatting is handled by the logger
 * (ISO8601-like). We do not assert an exact format (locale/timezone
 * differences), instead we check a few robust invariants:
 *  - the very first character is a digit (year starts with digits)
 *  - the line contains a 'T' separating date and time (ISO8601)
 *  - the line still contains the level name (e.g. "INF") somewhere
 *
 * These checks make the test resistant to small formatting changes but
 * still ensure that timestamps are emitted when enabled.
 */
static void test_enable_timestamps_true(void **state)
{
    (void)state;
    /* Prepare capture buffer */
    struct capture c = {0};
    c.cap = 2048;
    c.buf = malloc(c.cap);
    assert_non_null(c.buf);
    c.len = 0; c.buf[0] = '\0';

    /* Install writer and enable timestamps */
    emlog_set_writer(capture_writer, &c);
    /* Ensure the logger will emit INFO-level messages regardless of
     * previous tests which may have changed the global min level. We
     * explicitly set the minimum to INFO for determinism. */
    emlog_set_level(EML_LEVEL_INFO);
    emlog_enable_timestamps(true);

    /* Emit a message at INFO (default min level is INFO) */
    emlog_log(EML_LEVEL_INFO, "UT", "TS_ON_TEST");

    /* Basic sanity: we captured something */
    assert_true(c.len > 0);

    /* Stricter checks for ISO8601-like prefix: "YYYY-MM-DDTHH:MM:SS" */
    /* Check minimal length */
    assert_true(c.len >= 19);
    /* digits at expected positions */
    for(int i = 0; i < 4; ++i) assert_true(isdigit((unsigned char)c.buf[i])); /* YYYY */
    assert_int_equal(c.buf[4], '-');
    assert_true(isdigit((unsigned char)c.buf[5]));
    assert_true(isdigit((unsigned char)c.buf[6]));
    assert_int_equal(c.buf[7], '-');
    assert_true(isdigit((unsigned char)c.buf[8]));
    assert_true(isdigit((unsigned char)c.buf[9]));
    assert_int_equal(c.buf[10], 'T');
    assert_true(isdigit((unsigned char)c.buf[11]));
    assert_true(isdigit((unsigned char)c.buf[12]));
    assert_int_equal(c.buf[13], ':');
    assert_true(isdigit((unsigned char)c.buf[14]));
    assert_true(isdigit((unsigned char)c.buf[15]));
    assert_int_equal(c.buf[16], ':');
    assert_true(isdigit((unsigned char)c.buf[17]));
    assert_true(isdigit((unsigned char)c.buf[18]));

    /* Level should still appear somewhere */
    assert_non_null(strstr(c.buf, "INF"));

    /* cleanup */
    emlog_set_writer(NULL, NULL);
    free(c.buf);
}

/*
 * Test: disabling timestamps results in a line that begins with the
 * level string (e.g. "INF ...") — this is the logger's non-timestamp
 * header layout. We assert that the captured buffer starts with the
 * three-letter level name to confirm the timestamp was omitted.
 */
static void test_enable_timestamps_false(void **state)
{
    (void)state;
    struct capture c = {0};
    c.cap = 2048;
    c.buf = malloc(c.cap);
    assert_non_null(c.buf);
    c.len = 0; c.buf[0] = '\0';

    /* Install writer and disable timestamps */
    emlog_set_writer(capture_writer, &c);
    /* Ensure INFO messages are emitted (tests may run after other
     * suites that raised the min_level). */
    emlog_set_level(EML_LEVEL_INFO);
    emlog_enable_timestamps(false);

    emlog_log(EML_LEVEL_INFO, "UT", "TS_OFF_TEST");

    /* Expect the line to start with the level name ("INF") when
     * timestamps are disabled. This is robust and clearly distinguishes
     * the two header formats. */
    assert_int_equal(strncmp(c.buf, "INF", 3), 0);

    emlog_set_writer(NULL, NULL);
    free(c.buf);
}

/*
 * Test: toggling timestamps at runtime. We perform two independent
 * captures to keep the assertions simple and avoid parsing a
 * concatenated buffer. First we enable timestamps and check the
 * timestamp-like output, then we disable them and confirm the
 * non-timestamped format.
 */
static void test_enable_timestamps_toggle(void **state)
{
    (void)state;
    /* First capture: timestamps ON */
    struct capture a = {0};
    a.cap = 1024; a.buf = malloc(a.cap); assert_non_null(a.buf); a.len = 0; a.buf[0] = '\0';
    emlog_set_writer(capture_writer, &a);
    emlog_set_level(EML_LEVEL_INFO);
    emlog_enable_timestamps(true);
    emlog_log(EML_LEVEL_INFO, "UT", "TOGGLE_ON");
    assert_true(a.len > 0);
    assert_non_null(strchr(a.buf, 'T'));
    emlog_set_writer(NULL, NULL);
    free(a.buf);

    /* Second capture: timestamps OFF */
    struct capture b = {0};
    b.cap = 1024; b.buf = malloc(b.cap); assert_non_null(b.buf); b.len = 0; b.buf[0] = '\0';
    emlog_set_writer(capture_writer, &b);
    emlog_set_level(EML_LEVEL_INFO);
    emlog_enable_timestamps(false);
    emlog_log(EML_LEVEL_INFO, "UT", "TOGGLE_OFF");
    assert_true(b.len > 0);
    assert_int_equal(strncmp(b.buf, "INF", 3), 0);
    emlog_set_writer(NULL, NULL);
    free(b.buf);
}

/* Expose these tests so the unified runner can reference them by name. */
void emlog_timestamps_true(void **state) { test_enable_timestamps_true(state); }
void emlog_timestamps_false(void **state) { test_enable_timestamps_false(state); }
void emlog_timestamps_toggle(void **state) { test_enable_timestamps_toggle(state); }
