# --- toolchain ---------------------------------------------------------------
CC       ?= gcc
AR       ?= ar
ARFLAGS  ?= rcs
CFLAGS   ?= -Wall -Wextra -Wpedantic -std=c11 -O2

# --- layout -----------------------------------------------------------------
SRCDIRS  := app/src src
INCDIRS  := app/include include
CPPFLAGS := $(addprefix -I, $(INCDIRS))
CPPFLAGS += -D_GNU_SOURCE
OBJDIR   := build
LIBNAME  := libemlog.a

# all sources/objects (mirror subdirs under build/)
###############################################################################
# Makefile - EMlog
#
# Targets with brief descriptions (use `make help` to show this list)
###############################################################################

# toolchain
CC       ?= gcc
AR       ?= ar
ARFLAGS  ?= rcs
CFLAGS   ?= -Wall -Wextra -Wpedantic -std=c11 -O2

# layout
SRCDIRS  := app/src src
INCDIRS  := app/include include
CPPFLAGS := $(addprefix -I, $(INCDIRS))
CPPFLAGS += -D_GNU_SOURCE
OBJDIR   := build
LIBNAME  := libemlog.a

# discovered sources and objects
SRCS := $(foreach d,$(SRCDIRS),$(wildcard $(d)/*.c))
OBJS := $(patsubst %.c,$(OBJDIR)/%.o,$(SRCS))

# unit tests (cmocka)
UNIT_TEST_SRC := tests/unit/unit_test_runner.c \
				tests/unit/emlog_set_level.c \
			tests/unit/test_emlog_init.c \
			tests/unit/test_emlog_timestamps.c
UNIT_TEST_BIN := unit_test_emlog

# try to discover cmocka via pkg-config; fall back to -lcmocka
PKG_CMOCKA_CFLAGS := $(shell pkg-config --cflags cmocka 2>/dev/null || echo "")
PKG_CMOCKA_LIBS  := $(shell pkg-config --libs cmocka 2>/dev/null || echo "-lcmocka")

# stress harness (optional location)
STRESS_SRC := $(firstword $(wildcard tests/stress.c src/stress.c app/src/stress.c))
STRESS_BIN := stress_emlog

CLANG_FORMAT := $(shell command -v clang-format 2>/dev/null)

.PHONY: all libs clean format format-check help
.PHONY: UT-build UT-run IT-build IT-run
.PHONY: coverage

# Simplified integration test (IT) and unit test (UT) targets
# IT-build/IT-run: build and run integration/stress harness
# UT-build/UT-run: build and run unit tests

IT-build: $(STRESS_BIN) ## Build integration (stress) binary
	@if [ -z "$(STRESS_SRC)" ]; then \
		echo "[IT] no integration source found (create tests/stress.c or src/stress.c)"; exit 1; \
	fi
	@echo "[IT] building $(STRESS_BIN) from $(STRESS_SRC)"
	$(CC) $(CFLAGS) $(CPPFLAGS) $(STRESS_SRC) -L. -lemlog -pthread -o $(STRESS_BIN)

IT-run: IT-build ## Run the integration test (stress harness)
	@echo "[IT] running $(STRESS_BIN) with args: $${ARGS:-10 1000 0}"
	./$(STRESS_BIN) $${ARGS:-10 1000 0}

IT-run-coverage: ## Run integration with coverage and write results to tests/results
	@echo "[IT] running integration tests with coverage"
	@mkdir -p tests/results
	$(MAKE) CFLAGS="$(CFLAGS) -D_GNU_SOURCE --coverage -O0 -g -include stddef.h -include stdarg.h -include setjmp.h" LDFLAGS="$(LDFLAGS) --coverage" all
	$(CC) $(CFLAGS) --coverage -O0 -g -include stddef.h -include stdarg.h -include setjmp.h $(CPPFLAGS) $(HARDEN_CFLAGS) $(STRESS_SRC) -L. -lemlog -pthread $(HARDEN_LDFLAGS_BIN) $(LDFLAGS) --coverage -o $(STRESS_BIN)
	./$(STRESS_BIN) $${ARGS:-10 1000 0}
	if command -v gcovr >/dev/null 2>&1; then \
		gcovr -r . --exclude 'tests/' --html --html-details -o tests/results/IT_coverage.html && \
		gcovr -r . --exclude 'tests/' --xml -o tests/results/IT_coverage.xml && echo "[IT] coverage: tests/results/IT_coverage.html"; \
		gcovr -r . --exclude 'tests/' --html --html-details -o tests/results/combined_coverage.html || true; \
	else \
		if command -v lcov >/dev/null 2>&1 && command -v genhtml >/dev/null 2>&1; then \
			lcov --capture --directory . --output-file tests/results/IT_coverage.info || true; \
			lcov --remove tests/results/IT_coverage.info '/usr/*' 'tests/*' --output-file tests/results/IT_coverage.info || true; \
			genhtml tests/results/IT_coverage.info --output-directory tests/results/IT_coverage_html || true; \
			# combined
			lcov --capture --directory . --output-file tests/results/combined_coverage.info || true; \
			lcov --remove tests/results/combined_coverage.info '/usr/*' 'tests/*' --output-file tests/results/combined_coverage.info || true; \
			genhtml tests/results/combined_coverage.info --output-directory tests/results/combined_coverage_html || true; \
			echo "[IT] coverage: tests/results/IT_coverage_html/index.html"; \
		else \
			echo "[IT] coverage tools not found (gcovr or lcov/genhtml)"; \
		fi; \
	fi

UT-build: $(UNIT_TEST_BIN) ## Build unit test binary (cmocka)
	@echo "[UT] built $(UNIT_TEST_BIN)"

UT-run: ## Build & run unit tests with coverage and write results to tests/results
	@echo "[UT] running unit tests with coverage"
	@mkdir -p tests/results
	# remove stale gcov data files to avoid checksum overwrite warnings
	@find . -name "*.gcda" -print0 | xargs -0 -r rm -f || true
	# rebuild library with coverage instrumentation
	$(MAKE) CFLAGS="$(CFLAGS) --coverage -O0 -g -include stddef.h -include stdarg.h -include setjmp.h" LDFLAGS="$(LDFLAGS) --coverage" all
	# compile unit test with coverage flags (force include of stddef/stdarg to satisfy cmocka)
	$(CC) $(CFLAGS) -D_GNU_SOURCE --coverage -O0 -g -include stddef.h -include stdarg.h -include setjmp.h $(CPPFLAGS) $(PKG_CMOCKA_CFLAGS) $(HARDEN_CFLAGS) $(UNIT_TEST_SRC) -L. -lemlog -pthread $(PKG_CMOCKA_LIBS) $(HARDEN_LDFLAGS_BIN) $(LDFLAGS) --coverage -o $(UNIT_TEST_BIN)
	# run unit tests (generates .gcda)
	./$(UNIT_TEST_BIN)
	# collect coverage (suppress verbose tool output)
	@bash -c 'if command -v gcovr >/dev/null 2>&1; then gcovr -r . --exclude "tests/" --html --html-details -o tests/results/UT_coverage.html > /dev/null 2>&1 && gcovr -r . --exclude "tests/" --xml -o tests/results/UT_coverage.xml > /dev/null 2>&1 && echo "[UT] coverage: tests/results/UT_coverage.html"; elif command -v lcov >/dev/null 2>&1 && command -v genhtml >/dev/null 2>&1; then lcov --capture --directory . --output-file tests/results/UT_coverage.info > /dev/null 2>&1 || true; lcov --remove tests/results/UT_coverage.info "/usr/*" "tests/*" --output-file tests/results/UT_coverage.info > /dev/null 2>&1 || true; genhtml tests/results/UT_coverage.info --output-directory tests/results/UT_coverage_html > /dev/null 2>&1 || true; echo "[UT] coverage: tests/results/UT_coverage_html/index.html"; else echo "[UT] coverage tools not found (gcovr or lcov/genhtml)"; fi'
# ---------------- Warnings & hardening ----------------
CC_VERSION := $(shell $(CC) -dumpversion 2>/dev/null || echo 0)
CC_ID      := $(shell $(CC) -dM -E - < /dev/null 2>/dev/null | grep -q __clang__ && echo clang || echo gcc)

WARN_COMMON := \
  -Wall -Wextra -Wpedantic -Wformat=2 -Wformat-overflow=2 -Wformat-truncation=2 \
  -Wshadow -Wpointer-arith -Wcast-qual -Wwrite-strings -Wvla \
  -Wmissing-prototypes -Wmissing-declarations -Wstrict-prototypes \
  -Wswitch-enum -Wdouble-promotion -Winit-self -Wundef \
  -Werror=format-security

# compiler-specific sweeteners
ifeq ($(CC_ID),gcc)
  WARN_CC := -Wlogical-op -Wduplicated-cond -Wduplicated-branches -Walloca -Warray-bounds=2
  # GCC static analyzer (off by default; see target 'gcc-analyzer')
  ANALYZER_CC := -fanalyzer
else
  # clang defaults are saner; -Weverything is too noisy:
  WARN_CC := -Wcomma -Wextra-semi -Wnewline-eof -Wshorten-64-to-32
  # disable a few noisy ones if you later enable -Weverything:
  # WARN_CC += -Weverything -Wno-padded -Wno-cast-align -Wno-switch-enum -Wno-disabled-macro-expansion
endif

SECURITY_FLAGS := \
  -D_FORTIFY_SOURCE=3 \
  -fstack-protector-strong \
  -fno-strict-overflow -fno-delete-null-pointer-checks

# PIE helps even for executables in tests/stress
HARDEN_LDFLAGS := -Wl,-z,relro -Wl,-z,now
HARDEN_CFLAGS  := -fPIE
HARDEN_LDFLAGS_BIN := -pie

# fold into your CFLAGS/CPPFLAGS/LDFLAGS without breaking overrides
CFLAGS   := $(filter-out -Wall -Wextra -Wpedantic,$(CFLAGS)) \
            $(WARN_COMMON) $(WARN_CC) $(SECURITY_FLAGS)
LDFLAGS  := $(HARDEN_LDFLAGS) $(LDFLAGS)



# --- default ----------------------------------------------------------------
all: libs ## Default target: build the static library

# --- static lib --------------------------------------------------------------
libs: $(LIBNAME) ## Build the static library

$(LIBNAME): $(OBJS)
	$(AR) $(ARFLAGS) $@ $^

# compile to build/<subdirs> while preserving tree
$(OBJDIR)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

# --- tests ------------------------------------------------------------------
# legacy test binary removed; use UT-build/UT-run for unit tests

# unit test target (cmocka)
$(UNIT_TEST_BIN): $(UNIT_TEST_SRC) $(LIBNAME)
	@echo "[unit-test] building $(UNIT_TEST_BIN)"
	$(CC) -D_GNU_SOURCE -include stddef.h -include stdarg.h -include setjmp.h $(CFLAGS) $(CPPFLAGS) $(PKG_CMOCKA_CFLAGS) $(HARDEN_CFLAGS) $(UNIT_TEST_SRC) -L. -lemlog -pthread \
		$(PKG_CMOCKA_LIBS) $(HARDEN_LDFLAGS_BIN) $(LDFLAGS) -o $@

# stress binary
$(STRESS_BIN): $(STRESS_SRC) $(LIBNAME)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(HARDEN_CFLAGS) $< -L. -lemlog -pthread \
	    $(HARDEN_LDFLAGS_BIN) $(LDFLAGS) -o $@

## coverage is produced as part of UT-run and IT-run; standalone target removed

# --- housekeeping ------------------------------------------------------------
clean: ## Remove build artifacts
	rm -rf $(OBJDIR) $(LIBNAME) $(STRESS_BIN)

# --- formatting (requires .clang-format at repo root) -----------------------
format: ## Format sources using .clang-format
ifeq ($(CLANG_FORMAT),)
	$(error clang-format not found (install it, e.g. sudo apt install clang-format))
endif
	@test -f .clang-format || { echo ".clang-format missing at repo root"; exit 1; }
	@echo "[format] formatting sources"
	@dirs="app src tests"; \
	found=""; \
	for d in $$dirs; do [ -d "$$d" ] && found="$$found $$d"; done; \
	if [ -z "$$found" ]; then echo "[format] no source directories found"; exit 0; fi; \
	find $$found -type f \( -name "*.c" -o -name "*.h" \) -print0 \
	 | xargs -0 -r $(CLANG_FORMAT) -i -style=file

format-check: ## Check formatting (no changes)
ifeq ($(CLANG_FORMAT),)
	$(error clang-format not found (install it, e.g. sudo apt install clang-format))
endif
	@test -f .clang-format || { echo ".clang-format missing at repo root"; exit 1; }
	@echo "[format] checking formatting"
	@dirs="app src tests"; \
	found=""; \
	for d in $$dirs; do [ -d "$$d" ] && found="$$found $$d"; done; \
	if [ -z "$$found" ]; then echo "[format] no source directories found"; exit 0; fi; \
	find $$found -type f \( -name "*.c" -o -name "*.h" \) -print0 \
	 | xargs -0 -r $(CLANG_FORMAT) --dry-run -Werror -style=file

# ---------------- Build modes ----------------
# Usage: make MODE=asan test   |   make MODE=ubsan   |   make MODE=tsan   |   make MODE=msan (clang)
ASAN_FLAGS := -O1 -g -fsanitize=address -fno-omit-frame-pointer
UBSAN_FLAGS:= -O1 -g -fsanitize=undefined -fno-omit-frame-pointer
TSAN_FLAGS := -O1 -g -fsanitize=thread
MSAN_FLAGS := -O1 -g -fsanitize=memory -fsanitize-memory-track-origins

ifeq ($(MODE),asan)
  CFLAGS   += $(ASAN_FLAGS)
  LDFLAGS  += $(ASAN_FLAGS)
endif
ifeq ($(MODE),ubsan)
  CFLAGS   += $(UBSAN_FLAGS)
  LDFLAGS  += $(UBSAN_FLAGS)
endif
ifeq ($(MODE),tsan)
  CFLAGS   += $(TSAN_FLAGS)
  LDFLAGS  += $(TSAN_FLAGS)
endif
ifeq ($(MODE),msan)
  CFLAGS   += $(MSAN_FLAGS)
  LDFLAGS  += $(MSAN_FLAGS)
endif


help: ## Show this help message (targets with descriptions)
	printf "\nAvailable targets:\n\n"; \
	grep -E '^[a-zA-Z0-9_.-]+:.*?##' $(MAKEFILE_LIST) | \
		sed -E 's/^([a-zA-Z0-9_.-]+):.*?##[ \t]?(.*)/\1\t\2/' | \
		awk -F"\t" '{printf("  %-20s - %s\n", $$1, $$2)}'; \
	printf "\nPass ARGS=\"<threads> <msgs> <enable_ts>\" to IT targets, e.g. ARGS=\"10 1000 0\" make IT-run\n\n"
