#!/usr/bin/env bash
set -euo pipefail

# Package smoke test: proves the .deb genuinely works for an external consumer — compiles and
# runs a tiny program against ONLY the installed system paths (/usr/local/include,
# /usr/local/lib), never the repo's own build/ tree. This is deliberately NOT a rebuild of the
# library: if this script had to compile src/emlog.c itself, it would only prove the SOURCE
# works, not that the shipped, installed PACKAGE does.

START_DIR="$(pwd -P)"
cleanup() { cd -- "${START_DIR}"; }
trap cleanup EXIT

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd -- "${ROOT_DIR}"

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "${WORK_DIR}"; cleanup' EXIT

if [[ ! -f /usr/local/include/emlog.h ]]; then
    printf 'smoke_test_package: /usr/local/include/emlog.h not found — install the .deb first\n' >&2
    exit 1
fi

cat > "${WORK_DIR}/smoke.c" <<'EOF'
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <emlog.h>

static char     g_last[256];
static ssize_t sink(eml_level_t lvl, const char* line, size_t n, void* user)
{
    (void)lvl;
    (void)user;
    size_t cap = n < sizeof g_last - 1 ? n : sizeof g_last - 1;
    memcpy(g_last, line, cap);
    g_last[cap] = '\0';
    return (ssize_t)n;
}

int main(void)
{
    emlog_set_writer(sink, NULL);
    emlog_init(EML_LEVEL_INFO, false);
    EML_INFO("SMOKE", "installed package round-trips correctly");
    assert(strstr(g_last, "installed package round-trips correctly") != NULL);
    printf("smoke test: %s", g_last);
    return 0;
}
EOF

gcc -std=c11 -Wall -Wextra -Werror \
    -I/usr/local/include \
    "${WORK_DIR}/smoke.c" \
    -L/usr/local/lib -Wl,-rpath,/usr/local/lib -lemlog -pthread \
    -o "${WORK_DIR}/smoke"

"${WORK_DIR}/smoke"
