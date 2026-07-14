/**
 * @file test_emlog_writer_safety.c
 * @brief Memory-safety tests for the two P1 fixes:
 *        - the custom writer receives a NUL-terminated line (public contract);
 *        - a component name longer than the internal header buffer no longer
 *          reads out of bounds (run under AddressSanitizer to catch the read).
 */

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <cmocka.h>
#include <stdlib.h>
#include <string.h>

#include "emlog.h"
#include "unit_tests.h"

/* What the custom writer last received. */
static char   g_line[8192];
static size_t g_n;
static int    g_nul_ok; /* was line[n] == '\0'? */
static int    g_called;

static ssize_t capture_writer(eml_level_t lvl, const char* line, size_t n, void* user)
{
    (void)lvl;
    (void)user;
    g_called += 1;
    g_n = n;
    /* Contract (emlog.h): `line` is a NUL-terminated string of `n` bytes. Reading
     * line[n] must be safe AND equal '\0'. If the implementation passed an
     * unterminated buffer this read is out of bounds (ASan flags it) or non-NUL. */
    g_nul_ok      = (line[n] == '\0');
    size_t copy   = (n < sizeof(g_line) - 1) ? n : sizeof(g_line) - 1;
    memcpy(g_line, line, copy);
    g_line[copy] = '\0';
    return (ssize_t)n;
}

void emlog_writer_receives_nul_terminated(void** state)
{
    (void)state;
    g_called = 0;
    g_nul_ok = 0;
    g_n      = 0;

    emlog_set_writer(capture_writer, NULL);
    emlog_init(EML_LEVEL_DBG, false);
    emlog_log(EML_LEVEL_INFO, "comp", "hello %d", 42);
    emlog_set_writer(NULL, NULL);

    assert_true(g_called);
    assert_true(g_nul_ok);                 /* line[n] == '\0' — the contract */
    assert_int_equal(g_n, strlen(g_line)); /* n excludes the terminator */
    assert_non_null(strstr(g_line, "hello 42"));
}

void emlog_long_component_no_overread(void** state)
{
    (void)state;
    /* A component far longer than the internal 128-byte header buffer. The header
     * length used to come straight from snprintf's would-be length (unclamped)
     * and became the header iovec length → an out-of-bounds read of head[128].
     * This must now be safe: the header is clamped/truncated. */
    char big[512];
    memset(big, 'C', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';

    g_called = 0;
    g_nul_ok = 0;

    emlog_set_writer(capture_writer, NULL);
    emlog_init(EML_LEVEL_DBG, true); /* timestamps on → the longest header path */
    emlog_log(EML_LEVEL_INFO, big, "payload");
    emlog_set_writer(NULL, NULL);

    assert_true(g_called);
    assert_true(g_nul_ok);                  /* still NUL-terminated */
    assert_int_equal(g_n, strlen(g_line));  /* consistent length */
    assert_non_null(strstr(g_line, "payload"));
}

void emlog_truncation_warning_no_overread(void** state)
{
    (void)state;
    /* A long component AND a message long enough to trip the LOG_MAX_WRITE
     * truncation path. The follow-up "TRUNCATED: ..." warning used to take its
     * iovec length straight from snprintf's would-be length: with a component
     * longer than the 128-byte warnbuf that read hundreds of bytes past the
     * buffer (ASan catches the over-read). Both emitted lines must be clean. */
    char big_comp[512];
    memset(big_comp, 'C', sizeof(big_comp) - 1);
    big_comp[sizeof(big_comp) - 1] = '\0';

    char big_msg[8192];
    memset(big_msg, 'M', sizeof(big_msg) - 1);
    big_msg[sizeof(big_msg) - 1] = '\0';

    g_called = 0;
    g_nul_ok = 0;

    emlog_init(EML_LEVEL_DBG, true); /* init logs a line itself — writer goes in after */
    emlog_set_writer(capture_writer, NULL);
    emlog_log(EML_LEVEL_INFO, big_comp, "%s", big_msg);
    emlog_set_writer(NULL, NULL);

    assert_int_equal(g_called, 2);         /* truncated line + TRUNCATED warning */
    assert_true(g_nul_ok);                 /* last (warning) line NUL-terminated */
    assert_int_equal(g_n, strlen(g_line)); /* warning length consistent */
    assert_non_null(strstr(g_line, "TRUNCATED:"));
}

void emlog_huge_message_bounded(void** state)
{
    (void)state;
    /* A message far beyond the atomic-write cap must arrive truncated with the
     * "..." marker — the logger formats into bounded static storage and never
     * sizes an allocation from its input. */
    size_t huge_len = (size_t)1 << 20; /* 1 MiB */
    char*  huge     = malloc(huge_len + 1);
    assert_non_null(huge);
    memset(huge, 'A', huge_len);
    huge[huge_len] = '\0';

    g_called = 0;

    emlog_init(EML_LEVEL_DBG, false); /* init logs a line itself — writer goes in after */
    emlog_set_writer(capture_writer, NULL);
    emlog_log(EML_LEVEL_INFO, "comp", "%s", huge);
    emlog_set_writer(NULL, NULL);
    free(huge);

    assert_int_equal(g_called, 2);  /* truncated line + TRUNCATED warning */
    assert_true(g_n < 4096);        /* every delivered line under LOG_MAX_WRITE */
}

/* Writer that reconfigures the logger from inside the callback — this used to
 * deadlock on the (then single, non-recursive) global mutex. */
static ssize_t reconfiguring_writer(eml_level_t lvl, const char* line, size_t n, void* user)
{
    (void)lvl;
    (void)line;
    (void)n;
    (void)user;
    g_called += 1;
    emlog_set_level(EML_LEVEL_DBG); /* must not deadlock */
    return (ssize_t)n;
}

void emlog_writer_callback_may_reconfigure(void** state)
{
    (void)state;
    g_called = 0;

    emlog_init(EML_LEVEL_DBG, false); /* init logs a line itself — writer goes in after */
    emlog_set_writer(reconfiguring_writer, NULL);
    emlog_log(EML_LEVEL_INFO, "comp", "reconfigure from callback");
    emlog_set_writer(NULL, NULL);

    assert_int_equal(g_called, 1); /* returned — no deadlock */
}

/* Writer that logs from inside the callback: the reentrant line must be
 * dropped (not deadlock, not recurse). */
static ssize_t reentrant_writer(eml_level_t lvl, const char* line, size_t n, void* user)
{
    (void)lvl;
    (void)line;
    (void)user;
    g_called += 1;
    EML_INFO("inner", "logging from inside the writer");
    return (ssize_t)n;
}

void emlog_reentrant_log_dropped(void** state)
{
    (void)state;
    g_called = 0;

    emlog_init(EML_LEVEL_DBG, false); /* init logs a line itself — writer goes in after */
    emlog_set_writer(reentrant_writer, NULL);
    emlog_log(EML_LEVEL_INFO, "outer", "outer line");
    emlog_set_writer(NULL, NULL);

    assert_int_equal(g_called, 1); /* inner line dropped, no second callback */
}
