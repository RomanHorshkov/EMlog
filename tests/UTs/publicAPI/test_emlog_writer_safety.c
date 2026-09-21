/**
 * @file test_emlog_writer_safety.c
 * @brief Memory-safety tests for the two P1 fixes:
 *        - the custom writer receives a NUL-terminated line (public contract);
 *        - a component name longer than the internal header buffer no longer
 *          reads out of bounds (run under AddressSanitizer to catch the read).
 */

#define _POSIX_C_SOURCE 200809L /* nanosleep, pthreads for the replacement-synchronization test */

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>

#include <cmocka.h>

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
    g_called    += 1;
    g_n          = n;
    /* Contract (emlog.h): `line` is a NUL-terminated string of `n` bytes. Reading
     * line[n] must be safe AND equal '\0'. If the implementation passed an
     * unterminated buffer this read is out of bounds (ASan flags it) or non-NUL. */
    g_nul_ok     = (line[n] == '\0');
    size_t copy  = (n < sizeof(g_line) - 1) ? n : sizeof(g_line) - 1;
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
    assert_true(g_nul_ok);                 /* still NUL-terminated */
    assert_int_equal(g_n, strlen(g_line)); /* consistent length */
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

    assert_int_equal(g_called, 2); /* truncated line + TRUNCATED warning */
    assert_true(g_n < 4096);       /* every delivered line under LOG_MAX_WRITE */
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

/* ---- writer replacement must synchronize old-context lifetime ---- */

static int g_slow_done; /* set by the slow writer as its LAST action */

static ssize_t slow_ctx_writer(eml_level_t lvl, const char* line, size_t n, void* user)
{
    (void)lvl;
    (void)line;
    struct timespec delay = {.tv_sec = 0, .tv_nsec = 100 * 1000 * 1000}; /* 100 ms */
    nanosleep(&delay, NULL);
    volatile char c = *(char*)user;                                      /* ASan flags this if the context was reclaimed early */
    (void)c;
    g_slow_done = 1;
    return (ssize_t)n;
}

static void* log_one_line(void* arg)
{
    (void)arg;
    EML_INFO("t", "line into the slow writer");
    return NULL;
}

void emlog_writer_replacement_synchronizes(void** state)
{
    (void)state;
    /* The 1.1.0 lifetime hole: emlog_set_writer returned immediately while a
     * logging thread was still inside (or queued behind) the OLD callback, so
     * `emlog_set_writer(NULL, NULL); free(ctx);` was a use-after-free. The
     * fix routes replacement through the emit lock. Two assertions prove the
     * lock is really taken:
     *   - ordering: set_writer must return only AFTER the in-flight callback
     *     finished (g_slow_done observed set);
     *   - lifetime: freeing the context right after set_writer is ASan-clean. */
    emlog_init(EML_LEVEL_DBG, false);

    char* ctx = malloc(64);
    assert_non_null(ctx);
    memset(ctx, 0x5a, 64);

    g_slow_done = 0;
    emlog_set_writer(slow_ctx_writer, ctx);

    pthread_t th;
    assert_int_equal(pthread_create(&th, NULL, log_one_line, NULL), 0);

    struct timespec settle = {.tv_sec = 0, .tv_nsec = 20 * 1000 * 1000}; /* 20 ms: callback is now sleeping inside emit */
    nanosleep(&settle, NULL);

    emlog_set_writer(NULL, NULL);                                        /* must block until the callback returns */
    assert_int_equal(g_slow_done, 1);                                    /* the emission lock really exists */
    free(ctx);                                                           /* safe by contract; ASan verifies */

    assert_int_equal(pthread_join(th, NULL), 0);
}

/* ---- level edge semantics ---- */

void emlog_level_above_crit_disables(void** state)
{
    (void)state;
    /* CRIT+1 must genuinely disable output — not fall back to EMLOG_LEVEL. */
    emlog_init(EML_LEVEL_DBG, false);
    g_called = 0;
    emlog_set_writer(capture_writer, NULL);

    emlog_set_level((eml_level_t)(EML_LEVEL_CRIT + 1));
    emlog_log(EML_LEVEL_CRIT, "comp", "must not appear");
    assert_int_equal(g_called, 0);

    /* same through emlog_init — the callback counts init's own line too */
    emlog_init(EML_LEVEL_CRIT + 1, false);
    emlog_log(EML_LEVEL_CRIT, "comp", "must not appear either");
    assert_int_equal(g_called, 0);

    emlog_set_writer(NULL, NULL);
}

void emlog_negative_level_clamped(void** state)
{
    (void)state;
    /* A negative set_level must clamp to DBG (everything emits), not disable
     * or enable phantom levels. */
    emlog_init(EML_LEVEL_CRIT + 1, false); /* start silenced */
    g_called = 0;
    emlog_set_writer(capture_writer, NULL);

    emlog_set_level((eml_level_t)-5);
    emlog_log(EML_LEVEL_DBG, "comp", "dbg visible after clamp");
    assert_int_equal(g_called, 1);

    emlog_set_writer(NULL, NULL);
}
