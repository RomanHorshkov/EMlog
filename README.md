EMLog — Minimal thread-safe logging and canonical error utilities
===============================================================

Overview
--------

EMLog is a compact, thread-safe logging and error categorization library written in C. It provides:

- A small printf-style logging API with configurable minimum level and optional ISO8601 timestamps.
- A portable, tiny mapping layer from POSIX errno values to a small set of canonical error categories (eml_err_t).
- A pluggable writer callback to replace default stdout/stderr sinks (useful for testing or embedding in other systems).
- Convenience macros for short call-sites (EML_DBG, EML_INFO, EML_WARN, EML_ERROR, EML_CRIT) and an errno helper (EML_PERR).

The library is intended to be simple to embed in command-line tools and daemons that need lightweight structured logging without pulling a large dependency.

Files
-----

- include/emlog.h  — Public API and documentation (Doxygen comments).
- src/emlog.c      — Implementation, organized with documented static helpers and public API implementations.
- src/test.c       — Extended test suite exercising the public API.
- Makefile         — Builds a static library (`libemlog.a`) and the test executable (`test_emlog`).

Building
--------

A simple Makefile is included. From the project root run:

```bash
make
```

This produces `libemlog.a` in the project root and places intermediate objects in `build/`.

Running tests
-------------

To build and run the included test suite:

```bash
make test
```

The test binary (`test_emlog`) will run several assertions that exercise the logging macros, writer callback, errno formatting, error category mappings, long message handling, and level filtering.

Usage
-----

Include the header and link with the static library or build `emlog.c` into your project:

```c
#include "emlog.h"

int main(void) {
    /* initialize; pass negative to read EMLOG_LEVEL env var */
    emlog_init(-1, true); // enable timestamps

    /* set minimum log level */
    emlog_set_level(EML_LEVEL_INFO);

    /* basic logging */
    emlog_log(EML_LEVEL_INFO, "main", "listening on port %d", port);

    /* or use convenience macros */
    EML_WARN("net", "socket slow: %d ms", latency_ms);

    /* log errno with message */
    if (open(path, O_RDONLY) < 0) {
        EML_PERR("fs", "failed to open %s", path);
    }

    return 0;
}
```

Custom writer
-------------

To capture or redirect formatted lines, install a writer callback:

```c
ssize_t my_writer(eml_level_t lvl, const char* line, size_t n, void* user) {
    // write to a file, push to a queue, or store for testing
}

emlog_set_writer(my_writer, my_context);
```

Passing `NULL` as the writer restores default behavior (stdout for INFO and below, stderr for ERROR and above).

Error categories
----------------

The library maps platform errno values into the `eml_err_t` enum (see `include/emlog.h`). Use these helpers:

- `eml_from_errno(int err)` — convert errno to `eml_err_t`.
- `eml_err_name(eml_err_t e)` — get a textual name for the category.
- `eml_err_to_exit(eml_err_t e)` — suggested program exit code for the category.

Thread id helper
----------------

`eml_tid()` returns a numeric thread identifier used in log lines. On Linux it returns the kernel thread id, on other platforms it returns a converted `pthread_t` value for human-readable logs.

Portability and behavior
------------------------

- The API is POSIX-oriented and uses `pthread` for synchronization.
- `emlog_log_errno()` uses `strerror_r()`; behavior may vary between GNU and POSIX variants, but the function will include errno text in the logged message.
- The implementation avoids exposing internal symbols and places helper functions as static to keep the public ABI small.

Contributing
------------

Contributions, bug reports, and improvements are welcome. When contributing:

- Keep changes small and focused.
- Update or add tests in `src/test.c` for new behavior.
- Run `make test` to verify nothing regresses.

License
-------

The repository uses the MIT license (see header comments in the source files).

Contact
-------

For questions or feature requests, open an issue in the repository or provide a patch with a focused description of the change and rationale.
