EMLog — Minimal thread-safe logging and canonical error utilities
===================================================================

[![Quality](https://github.com/RomanHorshkov/EMlog/actions/workflows/quality.yml/badge.svg?branch=master)](https://github.com/RomanHorshkov/EMlog/actions/workflows/quality.yml?query=branch%3Amaster)
[![Security](https://github.com/RomanHorshkov/EMlog/actions/workflows/security.yml/badge.svg?branch=master)](https://github.com/RomanHorshkov/EMlog/actions/workflows/security.yml?query=branch%3Amaster)
[![Release](https://github.com/RomanHorshkov/EMlog/actions/workflows/release.yml/badge.svg?branch=master)](https://github.com/RomanHorshkov/EMlog/actions/workflows/release.yml?query=branch%3Amaster)
[![License: MIT](https://img.shields.io/badge/license-MIT-informational)](./LICENSE)
![Coverage](https://img.shields.io/badge/coverage-96%25%20line-brightgreen)

Overview
--------

EMLog is a compact, thread-safe logging and error-categorization library written in C. It provides a tiny, easy-to-embed logging API with a small footprint, a fixed set of canonical error categories mapped from `errno`, and predictable behaviour under heavy concurrency.

Why EMLog?
----------

- Thread-safe by construction: a custom writer can be installed and swapped safely across concurrent logging calls.
- No hot-path heap allocations: log lines are emitted with a single `writev(2)` against a header/message iovec pair, not a malloc'd buffer.
- Cheap timestamps: ISO8601 prefixes are cached per-thread, per-second in TLS; only the millisecond suffix is recomputed on every call.
- Pipe-safe by default: messages are truncated to respect `PIPE_BUF` so a single `write` never tears across readers.
- Canonical error categories: every POSIX `errno` maps to one of a small, stable set of `eml_err_t` values, so callers can branch on category instead of raw errno.

Project layout
--------------

- `src/emlog.h` — public API, fully documented with Doxygen comments.
- `src/emlog.c` — implementation (writer dispatch, timestamp cache, `errno` categorization).
- `tests/UTs/privateAPI/`, `tests/UTs/publicAPI/` — cmocka-based unit tests (white-box + black-box).
- `tests/ITs/integration_test.c` — multithreaded integration/stress harness.
- `utils/` — build, packaging, test, coverage, and hardening scripts.
- `VERSION` — library version (`MAJOR.MINOR.PATCH`), also the release-tag source of truth.

Build
-----

Everything you need to build/run is in `utils/`. Every script can be launched
from any directory; paths are resolved relative to the repo root. All compile
flags come from the shared profile catalog `utils/gcc_build_profiles.sh` —
no script carries ad-hoc flag literals.

<details>
<summary><strong>Script reference</strong> (click to expand)</summary>

| Script | Purpose |
| ------ | ------- |
| `utils/build_libs.sh [profile ...]` | Build `libemlog.so.<VERSION>` + `libemlog.a` per profile into `build/<profile>/` (default: debug audit sanitize release); release artifacts are gated by `check_hardening.sh`. |
| `utils/build_UTs.sh [--build-only\|--run-only]` | Build + run BOTH unit-test suites (private white-box + public black-box, separately compiled binaries, one run) with per-suite and combined gcovr coverage. Debug profile + coverage layer. |
| `utils/build_UTs_release.sh [--build-only\|--run-only]` | The same two suites compiled and run under the release profile (-O2, NDEBUG, hardening); the public suite links the release static library the deb ships. |
| `utils/build_ITs.sh [--build-only\|--run-only]` | Build + run the multithreaded integration test with its own gcovr coverage (debug profile + coverage layer). |
| `utils/build_ITs_release.sh [--build-only\|--run-only]` | The same integration test built and run under the release profile (-O2, hardening) — the real correctness gate. |
| `utils/build_sanitizer_tests.sh` | Both UT suites + IT under ASan/UBSan/LSan (sanitize profile). |
| `utils/build_tsan_tests.sh` | The integration test under ThreadSanitizer (tsan profile). |
| `utils/build_deb.sh` | Debian packages `libemlog` (runtime) + `libemlog-dev` (header, static lib, linker symlink) + `SHA256SUMS` into `build/debs/` (VERSION-validated, hardening-checked). |
| `utils/smoke_test_package.sh` | Compiles against the *installed* `/usr/local` package, never the repo build tree. Run after installing the debs. |
| `utils/check_hardening.sh <elf>` | `readelf` assertions on built ELFs (full RELRO, NX stack, stack canary, …). |
| `utils/run_pipeline.sh` | The full board, end to end: libs → unit tests → integration → debs, with a coverage and package report at the end. |

</details>

The test scripts take an optional phase flag:

- *(no flag)* — build the test binaries, then run them (and, for
  `build_UTs.sh`, generate coverage).
- `--build-only` — compile the binaries and stop; nothing executes.
- `--run-only` — run previously built binaries without recompiling
  (errors out with a hint if the binaries are missing).

Library artifacts:

```sh
./utils/build_libs.sh              # all four default profiles
./utils/build_libs.sh release      # just the release profile
```

- `build/<profile>/libemlog.a`
- `build/<profile>/libemlog.so.<VERSION>` (+ `.so` / `.so.<MAJOR>` symlinks)
- flat `build/libemlog.*` symlinks always point into `build/release/`

Packaging
---------

```sh
./utils/build_deb.sh
```

produces in `build/debs/`:

- `libemlog_<version>_<arch>.deb` — runtime: `/usr/local/lib/libemlog.so.<version>` + soname symlink, `ldconfig` hooks.
- `libemlog-dev_<version>_<arch>.deb` — development: `/usr/local/include/emlog.h`, `/usr/local/lib/libemlog.a`, `libemlog.so` linker symlink. Depends on `libemlog (= <version>)`.
- `SHA256SUMS` — checksums over both debs.

Install both (apt resolves the dependency order):

```sh
sudo apt-get install ./build/debs/libemlog_<version>_<arch>.deb \
                     ./build/debs/libemlog-dev_<version>_<arch>.deb
./utils/smoke_test_package.sh   # proves the INSTALLED package links and runs
```

Release process
----------------

Releases are tag-driven. See [RELEASING.md](./RELEASING.md) for the exact
merge, tag, and publish flow. Each GitHub Release attaches both debs, a
header+libs tarball, and `SHA256SUMS`.

Testing
-------

Requirements (Ubuntu/Debian):

```sh
sudo apt install libcmocka-dev gcovr
```

Run everything:

```sh
./utils/run_pipeline.sh
```

Or individually:

```sh
./utils/build_UTs.sh              # both unit-test suites + coverage
./utils/build_UTs_release.sh      # same suites under the release profile
./utils/build_ITs.sh              # integration test + its own coverage
./utils/build_ITs_release.sh      # integration test under the release profile
./utils/build_sanitizer_tests.sh  # UTs + IT under ASan/UBSan/LSan
./utils/build_tsan_tests.sh       # IT under TSan
```

Coverage outputs — UTs and the integration test each get their own, independent report; nothing here is silently merged, and they stay local (no hosted HTML site):

- `tests/results/private_UTs/UTs_private_coverage.html` + `coverage-summary.json` — private suite alone
- `tests/results/public_UTs/UTs_public_coverage.html` + `coverage-summary.json` — public suite alone
- `tests/results/UTs_all/UTs_all_coverage.{html,xml}` + `coverage-summary.json` — both UT suites merged
- `tests/results/ITs/ITs_coverage.{html,xml}` + `coverage-summary.json` — integration test alone

CI uploads all four as workflow artifacts on every push (download from the run's Artifacts section); the README **Coverage** badge is the combined-UT line-coverage number, updated by hand.

CI (`quality.yml`) runs the same scripts, plus compiler-portability (gcc + clang, `-Werror`), ASan/UBSan/LSan, ThreadSanitizer, and a package build/install/smoke-test stage. `security.yml` runs GCC's `-fanalyzer` on every push, PR, and weekly on a schedule.

Usage
-----

```c
#include "emlog.h"

int main(void) {
    emlog_init(-1, true); /* safe to call multiple times; last call wins */
    emlog_set_level(EML_LEVEL_DEBUG);

    EML_INFO("main", "hello world %d", 1);

    return 0;
}
```

Installing a custom writer:

```c
ssize_t my_writer(eml_level_t lvl, const char* line, size_t n, void* user) {
    /* sink to custom output (file, socket, ring buffer) */
}

emlog_set_writer(my_writer, my_context);
```

Compile locally against the built library:

```sh
gcc -std=c11 -Isrc -c myprog.c -o myprog.o
gcc myprog.o -Lbuild/release -lemlog -o myprog
```

Design notes
------------

- **No hot-path allocations.** `emlog_log()` builds a header iovec and a message iovec and emits both with a single `writev(2)` against the default FD-based writer, instead of concatenating into a heap buffer first.
- **One-time `tzset()`.** Timezone initialization is done once in `emlog_init()` (only when timestamps are enabled), moving libc's lazy timezone-file parsing out of the hot path.
- **Per-thread, per-second timestamp cache.** The ISO8601 prefix is cached in TLS for the current second; only the millisecond suffix is recomputed per call, cutting `localtime_r()`/`strftime()` calls under high message rates.
- **Truncation for pipe atomicity.** Messages are capped at `LOG_MAX_WRITE` (`PIPE_BUF` where available, else 4096) so a single `write` never exceeds what a reader can atomically observe; truncated lines get a short `TRUNCATED` notice.
- **Optional `writev` flush.** If the rest of your program writes to the same `stdout`/`stderr` `FILE*` via buffered stdio, call `emlog_set_writev_flush(true)` to `fflush()` before each `writev` and avoid interleaved output (off by default — it's the faster path).

Build profiles & hardening
---------------------------

Builds go through `utils/build_libs.sh [profile ...]`, driven by the shared catalog `utils/gcc_build_profiles.sh` (synced verbatim from `Utils/compilation/`, never edited locally); artifacts land in `build/<profile>/`; `utils/check_hardening.sh` gates every release artifact.

<details>
<summary><strong>Profile comparison table</strong> (click to expand)</summary>

| Profile | Optimization | Warnings | Instrumentation | Hardened | Use it for |
|---|---|---|---|---|---|
| debug | `-Og -g3` | core | — | no | day-to-day development |
| audit | `-O1 -g3` | everything + `-fanalyzer` | — | yes | compiler-driven validation |
| sanitize | `-O1 -g3` | strict | ASan+UBSan+LSan | yes minus FORTIFY — conflicts with ASan | runtime bug hunting |
| release | `-O2 -DNDEBUG` | strict | — | yes — full set below | production / the deb payload |
| native | `-O3 -flto -march=native` | strict | — | yes | benchmarks on the deploy box |
| extreme | `-O3 -flto -march=native` | core | — | deliberately none | max-perf experiments only |

</details>

<details>
<summary><strong>Release hardening flags, by stage</strong> (click to expand)</summary>

| Flag | Stage | Purpose |
|---|---|---|
| `-fstack-protector-strong` | compile | stack canary on frames with arrays / address-taken locals |
| `-fstack-clash-protection` | compile | page-by-page stack growth — the guard page can't be jumped |
| `-fcf-protection=full` | compile | x86-64 CET: indirect-branch tracking + shadow stack, NOP on older CPUs |
| `-D_FORTIFY_SOURCE=3` | preprocess | checked libc calls with dynamic object sizes |
| `-fPIC` | compile | position-independent code for the .so |
| `-Wl,-z,relro -Wl,-z,now` | link | GOT/PLT read-only after load — full RELRO |
| `-Wl,-z,noexecstack` | link | non-executable stack asserted |
| `-Wl,-z,defs` | link .so | undefined symbols fail the build not the load |

</details>

License
-------

MIT — see [LICENSE](./LICENSE).
