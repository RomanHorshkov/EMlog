/**
 * @file unit_tests.h
 * @brief Declarations for EMlog unit-test wrapper functions used by the unified CMocka runner.
 */

#ifndef TESTS_UNIT_UNIT_TESTS_H
#define TESTS_UNIT_UNIT_TESTS_H

#ifdef __cplusplus
extern "C"
{
#endif

/* set_level test */
void emlog_set_level_simple(void** state);

/* emlog_init tests */
void emlog_init_default_env(void** state);
void emlog_init_explicit_dbg(void** state);
void emlog_init_env_parsing(void** state);
void emlog_init_env_variants(void** state);

/* timestamp tests */
void emlog_timestamps_true(void** state);
void emlog_timestamps_false(void** state);
void emlog_timestamps_toggle(void** state);

/* error helper tests */
void emlog_error_from_errno(void** state);
void emlog_error_name_strings(void** state);
void emlog_error_exit_codes(void** state);
void emlog_log_errno_captures_context(void** state);

/* default writer tests */
void emlog_default_writer_stdout(void** state);
void emlog_default_writer_stderr(void** state);

/* writer/header memory-safety tests (P1 fixes) */
void emlog_writer_receives_nul_terminated(void** state);
void emlog_long_component_no_overread(void** state);

/* truncation-warning over-read, bounded formatting, writer reentrancy */
void emlog_truncation_warning_no_overread(void** state);
void emlog_huge_message_bounded(void** state);
void emlog_writer_callback_may_reconfigure(void** state);
void emlog_reentrant_log_dropped(void** state);

/* writer replacement lifetime + level edge semantics */
void emlog_writer_replacement_synchronizes(void** state);
void emlog_level_above_crit_disables(void** state);
void emlog_negative_level_clamped(void** state);

#ifdef __cplusplus
}
#endif

#endif /* TESTS_UNIT_UNIT_TESTS_H */
