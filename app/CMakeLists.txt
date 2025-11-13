cmake_minimum_required(VERSION 3.18)

# Allow this directory to be configured either as the top-level project
# (cmake -S app -B build/app) or from a parent CMake project.
if(NOT DEFINED PROJECT_NAME)
    set(EMLOG_STANDALONE_PROJECT TRUE)
    project(emlog VERSION 1.0.0 LANGUAGES C)
endif()

# Options can be overridden by a parent project or via -D on the command line.
option(EMLOG_BUILD_STATIC "Build the static libemlog.a archive" ON)
option(EMLOG_BUILD_SHARED "Build the shared libemlog.so library" ON)
option(EMLOG_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(EMLOG_ENABLE_COVERAGE "Enable gcov-style coverage instrumentation" OFF)

set(EMLOG_HEADERS ${CMAKE_CURRENT_SOURCE_DIR}/include/emlog.h)
set(EMLOG_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/emlog.c)

# Compile the sources once via an object library so both static and shared
# variants reuse the same object files and compile flags.
add_library(emlog_obj OBJECT ${EMLOG_SOURCES})
target_include_directories(emlog_obj PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(emlog_obj PUBLIC c_std_11)
set(EMLOG_GNU_CLANG_WARNINGS
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wformat=2
    -Wconversion
    -Wnull-dereference
    -Wdouble-promotion
    -Wduplicated-cond
    -Wduplicated-branches
    -Wlogical-op
    -Wfloat-equal
    -Wunsafe-loop-optimizations
)
target_compile_options(
    emlog_obj
    PRIVATE
        $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:${EMLOG_GNU_CLANG_WARNINGS}>
        $<$<AND:$<NOT:$<BOOL:${EMLOG_ENABLE_COVERAGE}>>,$<C_COMPILER_ID:GNU,Clang,AppleClang>>:-O2>
        $<$<AND:$<BOOL:${EMLOG_WARNINGS_AS_ERRORS}>,$<C_COMPILER_ID:GNU,Clang,AppleClang>>:-Werror>
)
if(EMLOG_ENABLE_COVERAGE AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(emlog_obj PRIVATE --coverage -O0 -g)
endif()
set_target_properties(emlog_obj PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(EMLOG_BUILD_STATIC)
    add_library(emlog_static STATIC $<TARGET_OBJECTS:emlog_obj>)
    target_include_directories(emlog_static PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    set_target_properties(emlog_static PROPERTIES OUTPUT_NAME emlog)
endif()

if(EMLOG_BUILD_SHARED)
    add_library(emlog_shared SHARED $<TARGET_OBJECTS:emlog_obj>)
    target_include_directories(emlog_shared PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    set_target_properties(
        emlog_shared
        PROPERTIES
            OUTPUT_NAME emlog
            SOVERSION 1
            VERSION ${PROJECT_VERSION}
            WINDOWS_EXPORT_ALL_SYMBOLS ON
    )
endif()

if(EMLOG_ENABLE_COVERAGE AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    foreach(lib emlog_static emlog_shared)
        if(TARGET ${lib})
            target_link_options(${lib} PRIVATE --coverage)
        endif()
    endforeach()
endif()

# When configuring this directory directly, place build artifacts next to the
# configured build folder to make them easy to discover.
if(EMLOG_STANDALONE_PROJECT)
    foreach(dir ARCHIVE LIBRARY RUNTIME)
        set(CMAKE_${dir}_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    endforeach()
endif()
