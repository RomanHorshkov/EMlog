#!/usr/bin/env bash
# shellcheck shell=bash
# emlog_project.sh — EMlog's declarations for the pinned kyr-build (every other script in utils/ is its wrapper): the library,
# the suites, the debs and what the ABI gate treats as public. Declarations only.
KYR_LIB="emlog"
KYR_LIB_SOURCES=(app/emlog.c)
KYR_LIB_HEADER="app/emlog.h"
KYR_LIB_MAP="utils/emlog.map"
KYR_LIB_CPPFLAGS=(-I"${ROOT_DIR}/app")
# pthread mutexes; with -Wl,-z,defs every needed library is named
KYR_LIB_LDLIBS=(-lpthread)

# Unit suites: private is white-box (test_private.c includes app/emlog.c), public is black-box.
KYR_UT_SUITES=(private public)
KYR_SUITE_PRIVATE_SOURCES=(tests/UTs/privateAPI/test_private.c)
KYR_SUITE_PRIVATE_LINK=none
KYR_SUITE_PRIVATE_CPPFLAGS=(-D_GNU_SOURCE)
KYR_SUITE_PRIVATE_LDLIBS=(-lcmocka -pthread)
KYR_SUITE_PUBLIC_SOURCES=('tests/UTs/publicAPI/*.c')
KYR_SUITE_PUBLIC_CPPFLAGS=(-D_GNU_SOURCE -I"${ROOT_DIR}/tests/UTs/publicAPI")
KYR_SUITE_PUBLIC_LDLIBS=(-lcmocka -pthread)
# The multithreaded integration test: release, then coverage.
KYR_IT_SUITES=(it)
KYR_SUITE_IT_SOURCES=(tests/ITs/integration_test.c)
KYR_SUITE_IT_CPPFLAGS=(-D_GNU_SOURCE)
KYR_SUITE_IT_LDLIBS=(-pthread)
KYR_IT_COVERAGE=1
# Stress: the single-threaded and the 8-thread benchmark, every profile, static and shared.
KYR_STRESS_SUITES=(stress)
KYR_SUITE_STRESS_SOURCES=(tests/stress/stress.c tests/stress/stress_mt.c)
KYR_SUITE_STRESS_CPPFLAGS=(-pthread -I"${ROOT_DIR}/tests/stress")
KYR_SUITE_STRESS_LDLIBS=(-lm -pthread)
KYR_STRESS_PROFILES=("${GCC_BUILD_PROFILES[@]}")
KYR_STRESS_LINKAGES=(static shared)
KYR_STRESS_METRICS=("stress|cost per line|throughput" "stress_mt|all per-thread cost samples|aggregate throughput per run")

# libemlog and libemlog-dev, both replacing the former single package "emlog".
KYR_PKG="libemlog"
KYR_PKG_DESCRIPTION="Tiny thread-safe C logger with printf-style API and canonical errno mapping"
KYR_DEB_HEADERS=(app/emlog.h)
KYR_DEB_RUNTIME_FIELDS=("Conflicts: emlog" "Replaces: emlog")
KYR_DEB_DEV_FIELDS=("Conflicts: emlog" "Replaces: emlog")
KYR_DEB_DEV_DESCRIPTION="Development files for libemlog (header, static library, linker symlink)"
KYR_DEB_PC=0
KYR_SMOKE_LDLIBS=(-pthread)

# The public ABI: app/emlog.h; the linker map's node is EMLOG_<major>.
KYR_ABI_NAME="EMlog"
KYR_ABI_INCLUDE_DIR="app"
KYR_ABI_UMBRELLA="emlog.h"
KYR_ABI_TYPE_PREFIX="eml_"
KYR_ABI_FUNC_PREFIX="eml"
KYR_ABI_CONST_PREFIX="EML_"
KYR_ABI_NODE="EMLOG"

