/**************************************************************************
 * @file     test_private.c
 * @brief    Unit tests for private functions in emlog.c
 *************************************************************************
*/

#include <cmocka.h>
#include "emlog.c"

void test_private_level_to_string(void)
{
    assert_string_equal(_level_to_string(EML_LEVEL_DBG), "DBG");
    assert_string_equal(_level_to_string(EML_LEVEL_INFO), "INF");
    assert_string_equal(_level_to_string(EML_LEVEL_WARN), "WRN");
    assert_string_equal(_level_to_string(EML_LEVEL_ERROR), "ERR");
    assert_string_equal(_level_to_string(EML_LEVEL_CRIT), "CRT");
    assert_string_equal(_level_to_string(EML_LEVEL_CRIT+1), "UNK");
    assert_string_equal(_level_to_string(-1), "UNK");
}
