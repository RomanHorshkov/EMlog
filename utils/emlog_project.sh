#!/usr/bin/env bash
# shellcheck shell=bash
# emlog_project.sh — what the ABI gate (utils/check_abi.sh, kyr-build share/abi) treats as EMlog's public ABI: the
# one public header app/emlog.h, and the prefixes of its types, functions and numeric constants. The build scripts do not read
# this file; the linker map's node is EMLOG_<major>.
KYR_LIB="emlog"
KYR_ABI_NAME="EMlog"
KYR_ABI_INCLUDE_DIR="app"
KYR_ABI_UMBRELLA="emlog.h"
KYR_ABI_TYPE_PREFIX="eml_"
KYR_ABI_FUNC_PREFIX="eml"
KYR_ABI_CONST_PREFIX="EML_"
KYR_ABI_NODE="EMLOG"
