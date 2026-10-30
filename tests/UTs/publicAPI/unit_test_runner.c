/**
 * @file unit_test_runner.c
 * @brief CMocka runner that aggregates the EMlog public API unit-test wrappers.
 */

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>

#include <cmocka.h>

#include "unit_tests.h"

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(emlog_set_level_simple),
        cmocka_unit_test(emlog_init_default_env),
        cmocka_unit_test(emlog_init_explicit_dbg),
        cmocka_unit_test(emlog_init_env_parsing),
        cmocka_unit_test(emlog_init_env_variants),
        cmocka_unit_test(emlog_timestamps_true),
        cmocka_unit_test(emlog_timestamps_false),
        cmocka_unit_test(emlog_timestamps_toggle),
        cmocka_unit_test(emlog_log_errno_captures_context),
        cmocka_unit_test(emlog_errno_preserved_custom_writer),
        cmocka_unit_test(emlog_errno_preserved_default_path_failed_write),
        cmocka_unit_test(emlog_control_bytes_sanitized),
        cmocka_unit_test(emlog_journal_mode_single_stream_with_priority),
        cmocka_unit_test(emlog_journal_mode_autodetected_from_env),
        cmocka_unit_test(emlog_truncation_notice_has_header),
        cmocka_unit_test(emlog_concurrent_default_path_lines_intact),
        cmocka_unit_test(emlog_default_writer_stdout),
        cmocka_unit_test(emlog_default_writer_stderr),
        cmocka_unit_test(emlog_writer_receives_nul_terminated),
        cmocka_unit_test(emlog_long_component_no_overread),
        cmocka_unit_test(emlog_truncation_warning_no_overread),
        cmocka_unit_test(emlog_notice_survives_absurd_component),
        cmocka_unit_test(emlog_huge_message_bounded),
        cmocka_unit_test(emlog_writer_callback_may_reconfigure),
        cmocka_unit_test(emlog_reentrant_log_dropped),
        cmocka_unit_test(emlog_writer_replacement_synchronizes),
        cmocka_unit_test(emlog_level_above_crit_disables),
        cmocka_unit_test(emlog_negative_level_clamped),
        cmocka_unit_test(emlog_refused_writer_lines_counted_and_announced),
        cmocka_unit_test(emlog_full_nonblocking_sink_counted_and_announced),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
