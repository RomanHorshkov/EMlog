#!/usr/bin/env bash
set -euo pipefail

# SET THE SCRIPT TO GO INTO DESIRED FOLDER AND COME BACK FROM WHERE LAUNCHED.
# Save where launched the script from
START_DIR="$(pwd -P)"
# Always come back, even if something fails
cleanup() { cd -- "$START_DIR"; }
trap cleanup EXIT

# Set the project root (expand ~ safely)
ROOT_DIR="$HOME/Projects/EMlog"
cd -- "$ROOT_DIR"

# Build the UTs private object file
BUILD_DIR="build/UTs"

mkdir -p $BUILD_DIR

# build without optimization and debug options
gcc -std=c11 -O0 -g -I. -Iapp -c tests/UTs/test_private.c -o $BUILD_DIR/test_private.o
gcc $BUILD_DIR/test_private.o -o $BUILD_DIR/ut_private -lcmocka -pthread
