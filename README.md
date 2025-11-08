EMLog — Minimal thread-safe logging and canonical error utilities
===============================================================

[![CI](https://github.com/RomanHorshkov/EMlog/actions/workflows/c-cpp.yml/badge.svg)](https://github.com/RomanHorshkov/EMlog/actions/workflows/c-cpp.yml)

Overview
--------

EMLog is a compact, thread-safe logging and error categorization library written in C. It provides a tiny, easy-to-embed logging API with a small footprint and predictable behaviour under heavy concurrency.

This repository contains the original library plus a set of targeted performance and robustness improvements implemented on the `adjustments` branch. The README below documents the library API, the performance improvements that were made, how to run the test and stress harness, and the profiling results collected while optimizing the hot path.

Files
-----

- `include/emlog.h`  — Public API and documentation (Doxygen comments).
- `src/emlog.c`      — Implementation and the performance-focused changes (timestamp caching, writev emission, truncation, etc.).
- `src/test.c`       — Unit tests exercising the public API.

- `src/stress.c`     — Multithreaded stress harness (builds to `stress_emlog`).
- `Makefile`         — Builds `libemlog.a`, `test_emlog` and `stress_emlog`.

Building
--------

Use the included Makefile:

```bash
make
```

This builds the static library `libemlog.a` and the test and stress binaries.

Running tests
-------------

```bash
make test
```

`test_emlog` runs the unit tests. Tests were updated to be tolerant of the logger's truncation behaviour for extremely long messages introduced to keep single-write sizes bounded for pipe atomicity.

New / Changed behaviour (adjustments branch)
--------------------------------------------

This project contains a set of defensive, low-risk performance changes focused on reducing per-log allocations and system calls in high-throughput scenarios. Key changes:

- One-time timezone initialization: `tzset()` is now called from `emlog_init()` (only when timestamps are enabled). This moves the lazy libc timezone file parsing out of the hot path and into startup.

- Per-thread, per-second timestamp cache: the time formatter used for ISO8601 timestamps now caches the formatted prefix for the current second in thread-local storage (TLS). If multiple log lines are emitted within the same second, the cached prefix is reused and only the millisecond portion is appended. This reduces calls to `localtime_r()` and `strftime()` on the hot path.

- writev-based emission: `vlog()` no longer concatenates header + message into a single malloc'd buffer on the heap. Instead it builds an iovec array (header iov + message iov) and emits them with a single `writev(2)` syscall when using the default FD-based writer. This avoids an extra heap allocation and reduces syscalls.

- Truncation to respect pipe atomicity: to preserve atomic writes to pipes and to limit writer work, the logger will truncate extremely long messages so that a single write does not exceed `LOG_MAX_WRITE` (based on `PIPE_BUF` when available, fallback 4096). When truncation occurs a short `TRUNCATED` notice line is emitted.

- writev flush control: mixing stdio buffered streams and direct FD writes can be unsafe unless the stdio buffer is flushed. A new API `emlog_set_writev_flush(bool on)` lets callers opt-in to calling `fflush()` before `writev` (slower but safe if other stdio writers are used). Default behaviour is the fastest (no fflush).

Public API additions (adjustments)
---------------------------------

- `void emlog_set_writev_flush(bool on);` — enable/disable fflush-before-writev behaviour.

Why these changes?
------------------

Profiling with Valgrind Massif showed repeated heap work coming from libc timezone parsing (tzset / tzfile) when timestamps were enabled in the logger. That work happened lazily inside libc and showed up during logging at scale. Moving `tzset()` to init, caching formatted timestamps per-thread per-second, and avoiding per-message heap allocations significantly reduced both heap churn and syscall load.

Stress harness and profiling
---------------------------

This repo includes `src/stress.c` which builds to `stress_emlog` and can be used to exercise the logger under heavy concurrent load.

Example: run 10 threads × 1000 messages (10k messages), redirect output to `/dev/null` and record elapsed time (the included wrapper scripts may be used):

```bash
# Build
make

# Run stress (10 threads, 1000 messages each)
./stress_emlog 10 1000

# The stress harness writes a short summary to /tmp/emlog_stress_result.txt
cat /tmp/emlog_stress_result.txt
```

Example measured result from a run used during development:

```
threads=10 msgs=1000 elapsed=1.222275
```

This means 10,000 messages were emitted in ~1.222s (≈8.2k messages/sec total).

Valgrind / Massif summary collected
---------------------------------

Massif findings (representative run):

- Peak total heap usage ~15 KiB; useful-heap attributed ~3 KiB at peak.
- The largest heap contributions were thread/TLS and stack allocation at thread startup (pthread_create -> allocate_stack -> _dl_allocate_tls). After the `tzset()` + caching changes there were no repeated libc timezone allocations on the hot path.

Valgrind Memcheck results (representative run):

- All heap allocations were freed at process exit; no leaks reported.
- No invalid memory read/write errors were detected.

Usage notes and caveats
----------------------

- If your program mixes other stdio (fwrite/fprintf) calls to the same `stdout`/`stderr` FILE* concurrently with the logger's default `writev` path, you must either:
  - Call `emlog_set_writev_flush(true)` to flush stdio before writev (safer, slightly slower), or
  - Ensure the rest of the program writes only via the same logger writer callback to avoid interleaved output.

- The truncation behaviour was introduced to preserve pipe atomicity (single writes <= PIPE_BUF). Tests were relaxed to accept truncated lines for extremely long messages; for most applications the truncation will not trigger.

- The per-second timestamp cache favors speed at high message rates. It retains correctness to the millisecond level for log lines (it formats yyyy-mm-ddThh:MM:ss.mmm±ZZZZ). If you need sub-millisecond timestamps or different formatting, consider modifying `fmt_time_iso8601` accordingly.

Examples
--------

Basic usage (same as before):

```c
#include "emlog.h"

int main(void) {
    emlog_init(-1, true); // enable timestamps and initialize timezone once
    emlog_set_level(EML_LEVEL_DEBUG);

    EML_INFO("main", "hello world %d", 1);

    return 0;
}
```

Installing a writer:

```c
ssize_t my_writer(eml_level_t lvl, const char* line, size_t n, void* user) {
    // sink to custom output (file, socket, ring buffer)
}

emlog_set_writer(my_writer, my_context);
```

How to reproduce profiling runs used during development
-----------------------------------------------------

Massif (heap profile):

```bash
# example wrapper used during development; adjust path as needed
my_massif_full.sh $(pwd)/stress_emlog 10 1000
```

Valgrind Memcheck:

```bash
my_vlgrnd_full.sh $(pwd)/stress_emlog 10 1000
```

Optional next steps (if you want even more speed)
------------------------------------------------

- Replace `strftime()` in the slow path with a small integer-based formatter for the common case (avoid strftime's generality). This can remove a small amount of CPU and possibly the last allocations in some libc variants.
- Pre-allocate larger per-thread buffers to avoid the rare heap fallback when formatting extremely large messages.
- Add an API to accept an explicit FD to log to (instead of FILE*/stdout/stderr) so the logger can avoid mixing stdio altogether.

Contributing
------------

If you add features or optimise further:

- Add or update tests in `src/test.c`.
- Keep changes small and provide a short benchmark or profiling notes for non-obvious performance changes.

License
-------

This project continues to use the MIT license (see source headers).

Contact
-------

Open an issue or a PR in the repository with the change and a short explanation of why it helps.
