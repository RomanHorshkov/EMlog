/**************************************************************************
 * @file     test_private.c
 * @brief    Unit tests for private functions in emlog.c
 *************************************************************************
*/

/* Ensure GNU/POSIX prototypes are available before any system headers. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
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
    assert_string_equal(_level_to_string(EML_LEVEL_CRIT+1), "UNK");
    assert_string_equal(_level_to_string(-1), "UNK");
}

static void test_private_string_to_level(void** state)
{
    (void)state;
    assert_int_equal(_string_to_level("debug"), EML_LEVEL_DBG);
    assert_int_equal(_string_to_level("info"), EML_LEVEL_INFO);
    assert_int_equal(_string_to_level("warn"), EML_LEVEL_WARN);
    assert_int_equal(_string_to_level("warning"), EML_LEVEL_WARN);
    assert_int_equal(_string_to_level("error"), EML_LEVEL_ERROR);
    assert_int_equal(_string_to_level("crit"), EML_LEVEL_CRIT);
    assert_int_equal(_string_to_level("fatal"), EML_LEVEL_CRIT);
    assert_int_equal(_string_to_level("unknown"), EML_LEVEL_INFO);
    assert_int_equal(_string_to_level(NULL), EML_LEVEL_INFO);
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
static void set_cache(const char *prefix, const char *tz)
{
    /* Always NUL-terminate, even if prefix/tz are long */
    snprintf(_ts_cache_prefix_tls, sizeof _ts_cache_prefix_tls, "%s", prefix);
    snprintf(_ts_cache_tz_tls,     sizeof _ts_cache_tz_tls,     "%s", tz);
}

static void fill_bytes(void *p, size_t n, unsigned char v)
{
    unsigned char *b = p;
    for (size_t i = 0; i < n; ++i) b[i] = v;
}

static void test_copy_cached_ts_golden(void **state)
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

static void test_copy_cached_ts_n0_does_not_write(void **state)
{
    (void)state;
    char out[8];
    fill_bytes(out, sizeof out, 0xAA);

    set_cache("2024-06-01T12:34:56", "+03:00");

    _copy_cached_ts(out, 0, 123);

    /* unchanged */
    for (size_t i = 0; i < sizeof out; ++i)
        assert_int_equal((unsigned char)out[i], 0xAA);
}

static void test_copy_cached_ts_out_null_does_not_write(void **state)
{
    (void)state;
    char out[8];
    fill_bytes(out, sizeof out, 0xAA);

    set_cache("2024-06-01T12:34:56", "+03:00");

    _copy_cached_ts(NULL, 0, 123);

    /* unchanged */
    for (size_t i = 0; i < sizeof out; ++i)
        assert_int_equal((unsigned char)out[i], 0xAA);
}

static void test_copy_cached_ts_n1_only_nul(void **state)
{
    (void)state;
    char out[1];
    out[0] = 'X';

    set_cache("2024-06-01T12:34:56", "+03:00");
    _copy_cached_ts(out, sizeof out, 123);

    assert_int_equal(out[0], '\0');
}

static void test_copy_cached_ts_truncates_and_nul_terminates(void **state)
{
    (void)state;
    set_cache("2024-06-01T12:34:56", "+03:00");

    const char *full = "2024-06-01T12:34:56.789+03:00";

    char out[10];
    fill_bytes(out, sizeof out, 0xBB);

    _copy_cached_ts(out, sizeof out, 789);

    assert_int_equal(out[sizeof(out) - 1], '\0');
    assert_memory_equal(out, full, sizeof(out) - 1);
}

static void test_copy_cached_ts_no_overflow_canary(void **state)
{
    (void)state;
    set_cache("2024-06-01T12:34:56", "+03:00");

    struct {
        char out[12];
        unsigned char canary;
    } b;

    fill_bytes(&b, sizeof b, 0xCC);

    _copy_cached_ts(b.out, sizeof b.out, 789);

    assert_int_equal(b.canary, 0xCC);
    assert_int_equal(b.out[sizeof(b.out) - 1], '\0');
}

static void test_copy_cached_ts_ms_normalized(void **state)
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
