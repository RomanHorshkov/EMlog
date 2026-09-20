/**
 * @file unit_test_runner.c
 * @brief CMocka runner that aggregates the EMlog public API unit-test wrappers.
 */

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <cmocka.h>
#include "unit_tests.h"

/* declare external test functions from other unit files */
extern void emlog_set_level_simple(void** state);
extern void emlog_init_default_env(void** state);
extern void emlog_init_explicit_dbg(void** state);
extern void emlog_init_env_parsing(void** state);
extern void emlog_init_env_variants(void** state);
extern void emlog_timestamps_true(void** state);
extern void emlog_timestamps_false(void** state);
extern void emlog_timestamps_toggle(void** state);
extern void emlog_error_from_errno(void** state);
extern void emlog_error_name_strings(void** state);
extern void emlog_error_exit_codes(void** state);
extern void emlog_log_errno_captures_context(void** state);
extern void emlog_default_writer_stdout(void** state);
extern void emlog_default_writer_stderr(void** state);
extern void emlog_writer_receives_nul_terminated(void** state);
extern void emlog_long_component_no_overread(void** state);
extern void emlog_truncation_warning_no_overread(void** state);
extern void emlog_huge_message_bounded(void** state);
extern void emlog_writer_callback_may_reconfigure(void** state);
extern void emlog_reentrant_log_dropped(void** state);
extern void emlog_writer_replacement_synchronizes(void** state);
extern void emlog_level_above_crit_disables(void** state);
extern void emlog_negative_level_clamped(void** state);

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(emlog_set_level_simple),      cmocka_unit_test(emlog_init_default_env),
        cmocka_unit_test(emlog_init_explicit_dbg),     cmocka_unit_test(emlog_init_env_parsing),
        cmocka_unit_test(emlog_init_env_variants),     cmocka_unit_test(emlog_timestamps_true),
        cmocka_unit_test(emlog_timestamps_false),      cmocka_unit_test(emlog_timestamps_toggle),
        cmocka_unit_test(emlog_log_errno_captures_context),
        cmocka_unit_test(emlog_errno_preserved_custom_writer),
        cmocka_unit_test(emlog_errno_preserved_default_path_failed_write),
        cmocka_unit_test(emlog_control_bytes_sanitized),
        cmocka_unit_test(emlog_journal_mode_single_stream_with_priority),
        cmocka_unit_test(emlog_journal_mode_autodetected_from_env),
        cmocka_unit_test(emlog_truncation_notice_has_header),
        cmocka_unit_test(emlog_concurrent_default_path_lines_intact),
        cmocka_unit_test(emlog_default_writer_stdout), cmocka_unit_test(emlog_default_writer_stderr),
        cmocka_unit_test(emlog_writer_receives_nul_terminated),
        cmocka_unit_test(emlog_long_component_no_overread),
        cmocka_unit_test(emlog_truncation_warning_no_overread),
        cmocka_unit_test(emlog_huge_message_bounded),
        cmocka_unit_test(emlog_writer_callback_may_reconfigure),
        cmocka_unit_test(emlog_reentrant_log_dropped),
        cmocka_unit_test(emlog_writer_replacement_synchronizes),
        cmocka_unit_test(emlog_level_above_crit_disables),
        cmocka_unit_test(emlog_negative_level_clamped),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
