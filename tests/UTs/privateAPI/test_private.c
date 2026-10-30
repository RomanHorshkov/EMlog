/*****************************************************************************************************************************************
 * @file     test_private.c
 * @brief    Unit tests for private functions in emlog.c
 *****************************************************************************************************************************************
 */

/* Ensure GNU/POSIX prototypes are available before any system headers. */
#ifndef _GNU_SOURCE
#    define _GNU_SOURCE
#endif

/* White-box testing: pull the implementation into this translation unit so
 * file-static helpers become callable from tests. */
#include "emlog.c"

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>

#include <cmocka.h>

static void test_private_level_to_string(void** state)
{
    (void)state;
    assert_string_equal(_level_to_string(EML_LEVEL_DBG), "DBG");
    assert_string_equal(_level_to_string(EML_LEVEL_INFO), "INF");
    assert_string_equal(_level_to_string(EML_LEVEL_WARN), "WRN");
    assert_string_equal(_level_to_string(EML_LEVEL_ERROR), "ERR");
    assert_string_equal(_level_to_string(EML_LEVEL_CRIT), "CRT");
    assert_string_equal(_level_to_string(EML_LEVEL_CRIT + 1), "UNK");
    assert_string_equal(_level_to_string(-1), "UNK");
}

static eml_level_t _level_of(const char* s, bool expect_known)
{
    bool              known = !expect_known;
    const eml_level_t l     = _string_to_level(s, &known);
    assert_int_equal(known, expect_known);
    return l;
}

static void test_private_string_to_level(void** state)
{
    (void)state;
    assert_int_equal(_level_of("debug", true), EML_LEVEL_DBG);
    assert_int_equal(_level_of("info", true), EML_LEVEL_INFO);
    assert_int_equal(_level_of("warn", true), EML_LEVEL_WARN);
    assert_int_equal(_level_of("warning", true), EML_LEVEL_WARN);
    assert_int_equal(_level_of("error", true), EML_LEVEL_ERROR);
    assert_int_equal(_level_of("crit", true), EML_LEVEL_CRIT);
    assert_int_equal(_level_of("fatal", true), EML_LEVEL_CRIT);
    assert_int_equal(_level_of("unknown", false), EML_LEVEL_INFO);
    assert_int_equal(_level_of("verbose", false), EML_LEVEL_INFO);
    assert_int_equal(_level_of(NULL, true), EML_LEVEL_INFO); /* unset: the default, nothing to say */
    assert_int_equal(_level_of("", true), EML_LEVEL_INFO);
}

/** @brief The ledger of levels.def: every row's tag, priority, name and alias reach the helpers; a level past the rows is "UNK" at
 *         LOG_INFO. */
static void test_private_level_rows(void** state)
{
    (void)state;
    size_t rows = 0u;
#define X(_level, _tag, _priority, _name, _alias)                           \
    assert_string_equal(_level_to_string(_level), _tag);                    \
    assert_int_equal(_level_to_journal_priority(_level), _priority);        \
    assert_int_equal(_level_of(_name, true), _level);                       \
    assert_int_equal(_level_of(_tag, true), _level);                        \
    if((_alias) != NULL) assert_int_equal(_level_of(_alias, true), _level); \
    ++rows;
#include "levels.def"
#undef X
    assert_int_equal(rows, EML_LEVEL_ROW_COUNT);
    assert_int_equal(_level_to_journal_priority((eml_level_t)EML_LEVEL_ROW_COUNT), 6);
    assert_int_equal(_level_to_journal_priority((eml_level_t)-1), 6);
    assert_int_equal(_level_to_journal_priority(EML_LEVEL_CRIT), 2);
    assert_int_equal(_level_to_journal_priority(EML_LEVEL_DBG), 7);
}

static void test_private_default_stream(void** state)
{
    (void)state;
    assert_ptr_equal(_default_stream(EML_LEVEL_DBG), stdout);
    assert_ptr_equal(_default_stream(EML_LEVEL_INFO), stdout);
    assert_ptr_equal(_default_stream(EML_LEVEL_WARN), stderr);
    assert_ptr_equal(_default_stream(EML_LEVEL_ERROR), stderr);
    assert_ptr_equal(_default_stream(EML_LEVEL_CRIT), stderr);
}

static void test_private_get_thread_id(void** state)
{
    (void)state;
    uint64_t tid1 = _get_thread_id();
    uint64_t tid2 = _get_thread_id();
    assert_int_equal(tid1, tid2);
}

/**
 * copy cached ts tests
 */
static void set_cache(const char* prefix, const char* tz)
{
    /* Always NUL-terminate, even if prefix/tz are long */
    snprintf(_ts_cache_prefix_tls, sizeof _ts_cache_prefix_tls, "%s", prefix);
    snprintf(_ts_cache_tz_tls, sizeof _ts_cache_tz_tls, "%s", tz);
}

static void fill_bytes(void* p, size_t n, unsigned char v)
{
    unsigned char* b = p;
    for(size_t i = 0; i < n; ++i)
        b[i] = v;
}

static void test_copy_cached_ts_golden(void** state)
{
    (void)state;
    char out[64];

    set_cache("2024-06-01T12:34:56", "+03:00");

    _copy_cached_ts(out, sizeof out, 789);
    assert_string_equal(out, "2024-06-01T12:34:56.789+03:00");

    _copy_cached_ts(out, sizeof out, 5);
    assert_string_equal(out, "2024-06-01T12:34:56.005+03:00");

    _copy_cached_ts(out, sizeof out, 0);
    assert_string_equal(out, "2024-06-01T12:34:56.000+03:00");
}

static void test_copy_cached_ts_n0_does_not_write(void** state)
{
    (void)state;
    char out[8];
    fill_bytes(out, sizeof out, 0xAA);

    set_cache("2024-06-01T12:34:56", "+03:00");

    _copy_cached_ts(out, 0, 123);

    /* unchanged */
    for(size_t i = 0; i < sizeof out; ++i)
        assert_int_equal((unsigned char)out[i], 0xAA);
}

static void test_copy_cached_ts_out_null_does_not_write(void** state)
{
    (void)state;
    char out[8];
    fill_bytes(out, sizeof out, 0xAA);

    set_cache("2024-06-01T12:34:56", "+03:00");

    _copy_cached_ts(NULL, 0, 123);

    /* unchanged */
    for(size_t i = 0; i < sizeof out; ++i)
        assert_int_equal((unsigned char)out[i], 0xAA);
}

static void test_copy_cached_ts_n1_only_nul(void** state)
{
    (void)state;
    char out[1];
    out[0] = 'X';

    set_cache("2024-06-01T12:34:56", "+03:00");
    _copy_cached_ts(out, sizeof out, 123);

    assert_int_equal(out[0], '\0');
}

static void test_copy_cached_ts_truncates_and_nul_terminates(void** state)
{
    (void)state;
    set_cache("2024-06-01T12:34:56", "+03:00");

    const char* full = "2024-06-01T12:34:56.789+03:00";

    char out[10];
    fill_bytes(out, sizeof out, 0xBB);

    _copy_cached_ts(out, sizeof out, 789);

    assert_int_equal(out[sizeof(out) - 1], '\0');
    assert_memory_equal(out, full, sizeof(out) - 1);
}

static void test_copy_cached_ts_no_overflow_canary(void** state)
{
    (void)state;
    set_cache("2024-06-01T12:34:56", "+03:00");

    struct
    {
        char          out[12];
        unsigned char canary;
    } b;

    fill_bytes(&b, sizeof b, 0xCC);

    _copy_cached_ts(b.out, sizeof b.out, 789);

    assert_int_equal(b.canary, 0xCC);
    assert_int_equal(b.out[sizeof(b.out) - 1], '\0');
}

static void test_copy_cached_ts_ms_normalized(void** state)
{
    (void)state;
    char out[64];

    set_cache("2024-06-01T12:34:56", "+03:00");

    _copy_cached_ts(out, sizeof out, 1005);
    assert_true(strstr(out, ".005") != NULL);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_private_level_to_string),
        cmocka_unit_test(test_private_string_to_level),
        cmocka_unit_test(test_private_level_rows),
        cmocka_unit_test(test_private_default_stream),
        cmocka_unit_test(test_private_get_thread_id),

        cmocka_unit_test(test_copy_cached_ts_golden),
        cmocka_unit_test(test_copy_cached_ts_n0_does_not_write),
        cmocka_unit_test(test_copy_cached_ts_out_null_does_not_write),
        cmocka_unit_test(test_copy_cached_ts_n1_only_nul),
        cmocka_unit_test(test_copy_cached_ts_truncates_and_nul_terminates),
        cmocka_unit_test(test_copy_cached_ts_no_overflow_canary),
        cmocka_unit_test(test_copy_cached_ts_ms_normalized),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
