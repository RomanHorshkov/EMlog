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

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_private_level_to_string),
        cmocka_unit_test(test_private_string_to_level),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
