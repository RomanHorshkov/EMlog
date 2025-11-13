# Tests rely on the libraries built in the app/ subdirectory. This file assumes
# add_subdirectory(app) has already been processed; it can also be configured
# standalone if the user sets EMLOG_LIB_ROOT to point at the build tree.

function(emlog_apply_coverage target_name)
    if(NOT EMLOG_ENABLE_COVERAGE)
        return()
    endif()
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target_name} PRIVATE --coverage -O0 -g)
        target_link_options(${target_name} PRIVATE --coverage)
    else()
        message(WARNING "Coverage requested but the active compiler does not support --coverage")
    endif()
endfunction()

# Prefer linking against the shared library, but fall back to the static archive
# if the user disabled shared builds.
if(TARGET emlog_shared)
    set(EMLOG_TEST_LIBRARY emlog_shared)
elseif(TARGET emlog_static)
    set(EMLOG_TEST_LIBRARY emlog_static)
else()
    message(FATAL_ERROR "No emlog library targets are available for unit tests. Enable EMLOG_BUILD_SHARED or EMLOG_BUILD_STATIC.")
endif()

# Discover cmocka either via pkg-config or by falling back to manual
# find_library/find_path logic.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(CMOCKA QUIET cmocka)
endif()

if(CMOCKA_FOUND)
    add_library(cmocka_dep INTERFACE)
    target_include_directories(cmocka_dep INTERFACE ${CMOCKA_INCLUDE_DIRS})
    target_link_libraries(cmocka_dep INTERFACE ${CMOCKA_LIBRARIES})
else()
    find_path(CMOCKA_INCLUDE_DIR cmocka.h)
    find_library(CMOCKA_LIBRARY NAMES cmocka)
    if(NOT (CMOCKA_INCLUDE_DIR AND CMOCKA_LIBRARY))
        message(FATAL_ERROR "cmocka could not be found. Install libcmocka-dev (or equivalent) or set CMOCKA_INCLUDE_DIR / CMOCKA_LIBRARY.")
    endif()
    add_library(cmocka_dep INTERFACE)
    target_include_directories(cmocka_dep INTERFACE ${CMOCKA_INCLUDE_DIR})
    target_link_libraries(cmocka_dep INTERFACE ${CMOCKA_LIBRARY})
endif()
add_library(cmocka::cmocka ALIAS cmocka_dep)

set(EMLOG_UNIT_TEST_SOURCES
    unit/unit_test_runner.c
    unit/emlog_set_level.c
    unit/test_emlog_init.c
    unit/test_emlog_timestamps.c
    unit/test_emlog_errors.c
    unit/test_emlog_default_writer.c
)

find_package(Threads REQUIRED)

add_executable(emlog_unit_tests ${EMLOG_UNIT_TEST_SOURCES})
target_include_directories(
    emlog_unit_tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/app/include
        ${CMAKE_CURRENT_SOURCE_DIR}/unit
)
target_compile_definitions(emlog_unit_tests PRIVATE _GNU_SOURCE)
target_link_libraries(emlog_unit_tests PRIVATE ${EMLOG_TEST_LIBRARY} cmocka::cmocka Threads::Threads)
emlog_apply_coverage(emlog_unit_tests)

add_test(NAME emlog_unit COMMAND emlog_unit_tests)

set(EMLOG_IT_SOURCES integration/integration_test.c)

add_executable(emlog_integration_test ${EMLOG_IT_SOURCES})
target_include_directories(emlog_integration_test PRIVATE ${CMAKE_SOURCE_DIR}/app/include)
target_compile_definitions(emlog_integration_test PRIVATE _GNU_SOURCE)
target_link_libraries(emlog_integration_test PRIVATE ${EMLOG_TEST_LIBRARY} Threads::Threads)
emlog_apply_coverage(emlog_integration_test)

add_test(NAME emlog_integration COMMAND emlog_integration_test)
