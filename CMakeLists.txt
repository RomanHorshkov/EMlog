cmake_minimum_required(VERSION 3.18)

project(emlog VERSION 1.0.0 LANGUAGES C)

option(EMLOG_BUILD_STATIC "Build the static libemlog.a archive" ON)
option(EMLOG_BUILD_SHARED "Build the shared libemlog.so library" ON)
option(EMLOG_BUILD_TESTS "Build emlog unit/integration tests" OFF)
option(EMLOG_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(EMLOG_ENABLE_COVERAGE "Enable gcov-style coverage instrumentation" OFF)

add_subdirectory(app)

if(EMLOG_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
