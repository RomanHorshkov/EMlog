/**
 * @file test_emlog_init.c
 * @brief Unit tests for EMlog initialization, explicit levels, and EMLOG_LEVEL environment parsing.
 */

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>

#include <cmocka.h>

#include <stdlib.h>
#include <string.h>

#include "emlog.h"
#include "unit_tests.h"

/* Reuse the capture helper similar to set_level tests */
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

/* Test 1: env absent and init(-1,false) should default to INFO and no timestamps */
static void test_init_default_env(void** state)
{
    (void)state;
    unsetenv("EMLOG_LEVEL");
    struct capture c = {0};
    c.cap            = 1024;
    c.buf            = malloc(c.cap);
    assert_non_null(c.buf);
    c.len    = 0;
    c.buf[0] = '\0';

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
static void test_init_explicit_dbg(void** state)
{
    (void)state;
    struct capture c = {0};
    c.cap            = 1024;
    c.buf            = malloc(c.cap);
    assert_non_null(c.buf);
    c.len    = 0;
    c.buf[0] = '\0';

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
static void test_init_env_parsing(void** state)
{
    (void)state;
    setenv("EMLOG_LEVEL", "warn", 1);
    struct capture c = {0};
    c.cap            = 1024;
    c.buf            = malloc(c.cap);
    assert_non_null(c.buf);
    c.len    = 0;
    c.buf[0] = '\0';

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
void emlog_init_default_env(void** state)
{
    test_init_default_env(state);
}
void emlog_init_explicit_dbg(void** state)
{
    test_init_explicit_dbg(state);
}
void emlog_init_env_parsing(void** state)
{
    test_init_env_parsing(state);
}

static void assert_env_level_behavior(const char* env_value, eml_level_t allowed, int blocked, const char* tag)
{
    if(env_value)
        setenv("EMLOG_LEVEL", env_value, 1);
    else
        unsetenv("EMLOG_LEVEL");

    struct capture c = {0};
    c.cap            = 1024;
    c.buf            = malloc(c.cap);
    assert_non_null(c.buf);
    c.len    = 0;
    c.buf[0] = '\0';

    emlog_set_writer(capture_writer, &c);
    emlog_init(-1, false);

    if(blocked >= 0)
    {
        emlog_log((eml_level_t)blocked, "ENV", "BLOCK_%s", tag);
        assert_null(strstr(c.buf, "BLOCK"));
    }

    emlog_log(allowed, "ENV", "ALLOW_%s", tag);
    assert_non_null(strstr(c.buf, "ALLOW"));

    emlog_set_writer(NULL, NULL);
    free(c.buf);
}

static void test_init_env_variants(void** state)
{
    (void)state;
    struct
    {
        const char* value;
        eml_level_t allowed;
        int         blocked;
        const char* tag;
    } cases[] = {
        {"debug", EML_LEVEL_DBG, -1, "debug"},
        {"INFO", EML_LEVEL_INFO, EML_LEVEL_DBG, "info"},
        {"warn", EML_LEVEL_WARN, EML_LEVEL_INFO, "warn"},
        {"warning", EML_LEVEL_WARN, EML_LEVEL_INFO, "warning"},
        {"error", EML_LEVEL_ERROR, EML_LEVEL_WARN, "error"},
        {"fatal", EML_LEVEL_CRIT, EML_LEVEL_ERROR, "fatal"},
        {"crit", EML_LEVEL_CRIT, EML_LEVEL_ERROR, "crit"},
        {"unexpected", EML_LEVEL_INFO, EML_LEVEL_DBG, "default"},
        {"DBG", EML_LEVEL_DBG, -1, "dbg_tag"}, /* the tag a line carries is a name too (1.5.1) */
        {"err", EML_LEVEL_ERROR, EML_LEVEL_WARN, "err_tag"},
        {"Crt", EML_LEVEL_CRIT, EML_LEVEL_ERROR, "crt_tag"},
        {"", EML_LEVEL_INFO, EML_LEVEL_DBG, "empty"},
    };

    for(size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i)
    {
        assert_env_level_behavior(cases[i].value, cases[i].allowed, cases[i].blocked, cases[i].tag);
    }
}

void emlog_init_env_variants(void** state)
{
    test_init_env_variants(state);
}

/** @brief An EMLOG_LEVEL no level names logs at INF as before, and emlog_init says so in one WRN line naming the value (cut at 32
 *         bytes); a known value, unset or empty says nothing. Before 1.5.1 `EMLOG_LEVEL=verbose` (or `dbg`) was INF without a word. */
static void test_init_env_unknown_is_announced(void** state)
{
    (void)state;
    struct capture c = {0};
    c.cap            = 2048;
    c.buf            = malloc(c.cap);
    assert_non_null(c.buf);

    const char* quiet[] = {"debug", "WARNING", "fatal", "", NULL};
    for(size_t i = 0; i < sizeof quiet / sizeof quiet[0]; ++i)
    {
        c.len    = 0;
        c.buf[0] = '\0';
        if(quiet[i])
            setenv("EMLOG_LEVEL", quiet[i], 1);
        else
            unsetenv("EMLOG_LEVEL");
        emlog_set_writer(capture_writer, &c);
        emlog_init(-1, false);
        assert_null(strstr(c.buf, "names no level"));
    }

    c.len    = 0;
    c.buf[0] = '\0';
    setenv("EMLOG_LEVEL", "verbose-and-then-some-more-bytes-past-thirty-two", 1);
    emlog_set_writer(capture_writer, &c);
    emlog_init(-1, false);
    emlog_log(EML_LEVEL_DBG, "UT", "BLOCKED");
    emlog_log(EML_LEVEL_INFO, "UT", "ALLOWED");
    const char* warn = strstr(c.buf, "EMLOG_LEVEL=\"verbose-and-then-some-more-bytes\" names no level");
    assert_non_null(warn);
    assert_non_null(strstr(c.buf, "WRN"));
    assert_null(strstr(c.buf, "BLOCKED"));
    assert_non_null(strstr(c.buf, "ALLOWED"));

    unsetenv("EMLOG_LEVEL");
    emlog_set_writer(NULL, NULL);
    free(c.buf);
}

void emlog_init_env_unknown_is_announced(void** state)
{
    test_init_env_unknown_is_announced(state);
}
