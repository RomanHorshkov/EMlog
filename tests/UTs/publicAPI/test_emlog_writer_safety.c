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
    g_called = 1;
    g_n      = n;
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
