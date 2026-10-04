EMLog — Minimal thread-safe logging
===================================================================

[![Quality](https://github.com/RomanHorshkov/EMlog/actions/workflows/quality.yml/badge.svg?branch=master)](https://github.com/RomanHorshkov/EMlog/actions/workflows/quality.yml?query=branch%3Amaster)
[![Security](https://github.com/RomanHorshkov/EMlog/actions/workflows/security.yml/badge.svg?branch=master)](https://github.com/RomanHorshkov/EMlog/actions/workflows/security.yml?query=branch%3Amaster)
[![Release](https://github.com/RomanHorshkov/EMlog/actions/workflows/release.yml/badge.svg?branch=master)](https://github.com/RomanHorshkov/EMlog/actions/workflows/release.yml?query=branch%3Amaster)
[![License: MIT](https://img.shields.io/badge/license-MIT-informational)](./LICENSE)
![Coverage](.github/badges/coverage.svg)

**📦 [Download the latest release](https://github.com/RomanHorshkov/EMlog/releases/latest)** — `.deb` packages, tarball, `SHA256SUMS`.

Overview
--------

EMLog is a compact, thread-safe logging library written in C. It provides a tiny, easy-to-embed logging API with a small footprint, a fixed set of canonical error categories mapped from `errno`, and predictable behaviour under heavy concurrency.

Why EMLog?
----------

- Thread-safe by construction: a custom writer can be installed and swapped safely across concurrent logging calls.
- No hot-path heap allocations: log lines are emitted with a single `writev(2)` against a header/message iovec pair, not a malloc'd buffer.
- Cheap timestamps: ISO8601 prefixes are cached per-thread, per-second in TLS; only the millisecond suffix is recomputed on every call.
- Pipe-safe by default: messages are truncated to respect `PIPE_BUF` so a single `write` never tears across readers.
- errno-transparent: a log call never changes the errno the caller is about to report.
- Injection-safe: control bytes in a message become `?`, so an argument can never forge a second line.
- journald-aware: with `JOURNAL_STREAM` set (systemd), every level goes to one stream with a `<N>` priority prefix, so `journalctl -p` filters work and emission order is preserved. No libsystemd.
- Lock-free default path: each thread formats into its own buffer and emits with one syscall (EINTR-retried, partial-write-completed); only custom-writer callbacks are serialized.

Project layout
--------------

- `app/emlog.h` — public API, fully documented with Doxygen comments.
- `app/emlog.c` — implementation (writer dispatch, timestamp cache, `errno` categorization).
- `tests/UTs/privateAPI/`, `tests/UTs/publicAPI/` — cmocka-based unit tests (white-box + black-box).
- `tests/ITs/integration_test.c` — multithreaded integration/stress harness; `tests/stress/` the benchmarks; `tests/package/smoke.c` the installed-package test.
- `utils/` — kyr-build's wrappers and `emlog_project.sh` (the declarations).
- `VERSION` — library version (`MAJOR.MINOR.PATCH`), also the release-tag source of truth.

Build and test
--------------

`utils/` holds the pinned kyr-build's wrappers and `utils/emlog_project.sh`, which declares what is built, tested and packaged; the steps themselves are kyr-build's (its README, "Wrapper contract"). No sudo; every pin of `deps.txt` lands in `build/sysroot/`.

```sh
./utils/deps.sh sysroot && eval "$(./utils/deps.sh env)" && ./utils/run_pipeline.sh   # every gate
./utils/build_libs.sh release          # build/<profile>/libemlog.{a,so.<VERSION>} (no argument: every profile)
./utils/build_UTs.sh                   # private (white-box) + public suites, coverage: tests/results/UTs
./utils/build_UTs_release.sh           # the suites against the release archive the deb ships
./utils/build_ITs.sh                   # the multithreaded integration test (release), then coverage: tests/results/ITs/coverage
./utils/build_sanitizer_tests.sh       # suites + IT under ASan, UBSan, LSan
GCC_BUILD_ENABLE_TSAN=1 ./utils/build_tsan_tests.sh
./utils/build_stress.sh && ./utils/run_stress.sh   # every profile x static/shared: tests/results/stress/stress_summary.tsv
./utils/build_deb.sh                   # build/debs: libemlog, libemlog-dev, SHA256SUMS, one .buildinfo per deb
./utils/smoke_test_package.sh          # tests/package/smoke.c against the installed debs only
```

`libemlog`: `/usr/local/lib/libemlog.so.<version>` + soname link, `ldconfig` hooks. `libemlog-dev`: `/usr/local/include/emlog.h`, `libemlog.a` (`-fPIC`, links into shared objects and PIE executables alike), the linker link; Depends `libemlog (= <version>)`. Both Conflicts/Replaces the former `emlog`. The pipeline's `package-smoke` stage runs the smoke test against the debs extracted into `build/smoke`; CI runs it after installing them. The coverage badge is the unit coverage number, written by kyr-build's `coverage_badge.py`.

Release process
----------------

Releases are tag-driven. See [RELEASING.md](./RELEASING.md) for the exact
merge, tag, and publish flow. Each GitHub Release attaches both debs, a
header+libs tarball, and `SHA256SUMS`.

CI (`quality.yml`, cicd `c-quality`) runs the same wrappers, plus compiler portability (gcc + clang, `-Werror`), ThreadSanitizer and the package install + smoke test; `security.yml` runs GCC's `-fanalyzer`.

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
gcc -std=c11 -Iapp -c myprog.c -o myprog.o
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

Builds go through `utils/build_libs.sh [profile ...]`, driven by the profile catalog of the pinned `kyr-build` package (`deps.txt`; `utils/gcc_build_profiles.sh` and `utils/check_hardening.sh` are thin wrappers over `build/sysroot/share/kyr-build/`, never copies); artifacts land in `build/<profile>/`; `utils/check_hardening.sh` gates every release artifact.

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
| `-fPIC` | compile | position-independent code, used for **both** the `.so` and the `.a` so the archive links into shared libraries, PIE executables and static executables alike |
| `-Wl,-z,relro -Wl,-z,now` | link | GOT/PLT read-only after load — full RELRO |
| `-Wl,-z,noexecstack` | link | non-executable stack asserted |
| `-Wl,-z,defs` | link .so | undefined symbols fail the build not the load |

</details>

License
-------

MIT — see [LICENSE](./LICENSE).

Build from a clean clone: `./utils/deps.sh sysroot && eval "$(./utils/deps.sh env)" && ./utils/run_pipeline.sh` (no sudo; every pin of `deps.txt` lands in `build/sysroot/`). Diagram: `docs/diagrams/tooling_sequence.puml`.

ABI: `abi/layout.txt` is the committed public ABI of `app/emlog.h` (layouts, enumerator values, callbacks, function signatures, numeric constants), checked by kyr-build's gate through `utils/check_abi.sh` (pipeline stages `abi` and `abi-exports`): a changed line needs a raised major, a new public name a raised minor, and `libemlog.so` exports exactly the declared functions as `<name>@@EMLOG_<major>` (`utils/emlog.map`); `utils/emlog_project.sh` says what is public. Debs through kyr-build's `lib/deb.sh` (`X-Kyr-Tier`, byte-reproducible). Diagram: `docs/diagrams/abi_exports.puml`.
