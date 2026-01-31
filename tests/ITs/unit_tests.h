/* tests/unit/unit_tests.h
 * Declarations for exported test wrapper functions so unit test
 * compilation does not emit "no previous prototype" warnings.
 *
 * This header is intentionally minimal: it only declares the public
 * wrappers defined by individual unit files. Include this header from
 * test sources that register or reference these functions.
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

#ifdef __cplusplus
}
#endif

#endif /* TESTS_UNIT_UNIT_TESTS_H */
