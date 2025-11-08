/* tests/unit/unit_test_runner.c
 * Single runner that aggregates all unit tests for emlog
 */

#include <cmocka.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include "unit_tests.h"

/* declare external test functions from other unit files */
extern void emlog_set_level_simple(void** state);
extern void emlog_init_default_env(void** state);
extern void emlog_init_explicit_dbg(void** state);
extern void emlog_init_env_parsing(void** state);
extern void emlog_timestamps_true(void** state);
extern void emlog_timestamps_false(void** state);
extern void emlog_timestamps_toggle(void** state);

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(emlog_set_level_simple),
        cmocka_unit_test(emlog_init_default_env),
        cmocka_unit_test(emlog_init_explicit_dbg),
        cmocka_unit_test(emlog_init_env_parsing),
        cmocka_unit_test(emlog_timestamps_true),
        cmocka_unit_test(emlog_timestamps_false),
        cmocka_unit_test(emlog_timestamps_toggle),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
