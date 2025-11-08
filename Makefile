# --- toolchain ---------------------------------------------------------------
CC       ?= gcc
AR       ?= ar
ARFLAGS  ?= rcs
CFLAGS   ?= -Wall -Wextra -Wpedantic -std=c11 -O2

# --- layout -----------------------------------------------------------------
SRCDIRS  := app/src src
INCDIRS  := app/include include
CPPFLAGS := $(addprefix -I, $(INCDIRS))
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
OBJDIR   := build
LIBNAME  := libemlog.a

# discovered sources and objects
SRCS := $(foreach d,$(SRCDIRS),$(wildcard $(d)/*.c))
OBJS := $(patsubst %.c,$(OBJDIR)/%.o,$(SRCS))

# test binary
TEST_SRC  := tests/test.c
TEST_BIN  := test_emlog

# unit tests (cmocka)
UNIT_TEST_SRC := tests/unit/test_emlog_set_level.c
UNIT_TEST_BIN := unit_test_emlog

# try to discover cmocka via pkg-config; fall back to -lcmocka
PKG_CMOCKA_CFLAGS := $(shell pkg-config --cflags cmocka 2>/dev/null || echo "")
PKG_CMOCKA_LIBS  := $(shell pkg-config --libs cmocka 2>/dev/null || echo "-lcmocka")

# stress harness (optional location)
STRESS_SRC := $(firstword $(wildcard tests/stress.c src/stress.c app/src/stress.c))
STRESS_BIN := stress_emlog

CLANG_FORMAT := $(shell command -v clang-format 2>/dev/null)

.PHONY: all libs test clean format format-check help
.PHONY: stress-build stress-run stress-massif stress-helgrind stress-memcheck stress-all

###############################################################################
# Stress targets  (console-only output)
###############################################################################

stress-build: $(LIBNAME) ## Build the stress harness (from tests/stress.c)
	@if [ -z "$(STRESS_SRC)" ]; then \
		echo "[stress] no stress source found (create tests/stress.c or src/stress.c)"; exit 1; \
	fi
	@echo "[stress] building $(STRESS_BIN) from $(STRESS_SRC)"
	$(CC) $(CFLAGS) $(CPPFLAGS) $(STRESS_SRC) -L. -lemlog -pthread -o $(STRESS_BIN)

stress-run: stress-build ## Build and run the stress harness (ARGS default: 10 1000 0)
	@echo "[stress] running $(STRESS_BIN) with args: $${ARGS:-10 1000 0}"
	./$(STRESS_BIN) $${ARGS:-10 1000 0}

stress-memcheck: stress-build ## Valgrind Memcheck (console)
	@echo "[stress-memcheck] memcheck on $(STRESS_BIN) with args: $${ARGS:-5 500}"
	@if [ -x "./my_vlgrnd_full.sh" ]; then \
		./my_vlgrnd_full.sh $(PWD)/$(STRESS_BIN) $${ARGS:-5 500}; \
	else \
		valgrind --tool=memcheck --leak-check=full --show-leak-kinds=all \
		         --errors-for-leak-kinds=all ./$(STRESS_BIN) $${ARGS:-5 500}; \
	fi

stress-helgrind: stress-build ## Helgrind race detector (console)
	@echo "[stress-helgrind] helgrind on $(STRESS_BIN) with args: $${ARGS:-20 5000}"
	@if [ -x "./my_hlgrnd.sh" ]; then \
		./my_hlgrnd.sh $(PWD)/$(STRESS_BIN) $${ARGS:-20 5000}; \
	else \
		valgrind --tool=helgrind ./$(STRESS_BIN) $${ARGS:-20 5000}; \
	fi

stress-massif: stress-build ## Massif heap profiler (prints ms_print to console)
	@echo "[stress-massif] massif on $(STRESS_BIN) with args: $${ARGS:-10 1000}"
	@if [ -x "./my_massif_full.sh" ]; then \
		./my_massif_full.sh $(PWD)/$(STRESS_BIN) $${ARGS:-10 1000}; \
	else \
		set -e; \
		before=$$(ls -1t massif.out.* 2>/dev/null | head -n1 || true); \
		( valgrind --tool=massif --time-unit=ms --stacks=yes ./$(STRESS_BIN) $${ARGS:-10 1000} ) 2>&1; \
		after=$$(ls -1t massif.out.* 2>/dev/null | head -n1 || true); \
		outfile=""; \
		if [ -n "$$after" ] && [ "$$after" != "$$before" ]; then outfile="$$after"; fi; \
		if [ -z "$$outfile" ]; then outfile=$$(ls -1t massif.out.* 2>/dev/null | head -n1 || true); fi; \
		if [ -n "$$outfile" ]; then \
			echo "\n[stress-massif] ===== ms_print $$outfile ====="; \
			ms_print "$$outfile" || true; \
			rm -f "$$outfile"; \
		else \
			echo "[stress-massif] no massif output found"; \
		fi; \
	fi

stress-all: stress-memcheck stress-massif stress-helgrind ## memcheck -> massif -> helgrind
	@echo "[stress-all] done"

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
# test binary

$(TEST_BIN): $(TEST_SRC) $(LIBNAME)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(HARDEN_CFLAGS) $< -L. -lemlog -pthread \
	    $(HARDEN_LDFLAGS_BIN) $(LDFLAGS) -o $@

# unit test target (cmocka)
$(UNIT_TEST_BIN): $(UNIT_TEST_SRC) $(LIBNAME)
	@echo "[unit-test] building $(UNIT_TEST_BIN)"
	$(CC) $(CFLAGS) $(CPPFLAGS) $(PKG_CMOCKA_CFLAGS) $(HARDEN_CFLAGS) $< -L. -lemlog -pthread \
	    $(PKG_CMOCKA_LIBS) $(HARDEN_LDFLAGS_BIN) $(LDFLAGS) -o $@

# stress binary
$(STRESS_BIN): $(STRESS_SRC) $(LIBNAME)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(HARDEN_CFLAGS) $< -L. -lemlog -pthread \
	    $(HARDEN_LDFLAGS_BIN) $(LDFLAGS) -o $@

test: $(TEST_BIN) $(UNIT_TEST_BIN) ## Run unit tests (legacy + cmocka)
	@echo "[test] running $(TEST_BIN)"
	./$(TEST_BIN)
	@echo "[test] running $(UNIT_TEST_BIN)"
	./$(UNIT_TEST_BIN)

# --- housekeeping ------------------------------------------------------------
clean: ## Remove build artifacts
	rm -rf $(OBJDIR) $(LIBNAME) $(TEST_BIN) $(STRESS_BIN)

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
	@printf "\nAvailable targets:\n\n"; \
	grep -E '^[a-zA-Z0-9_.-]+:.*?##' $(MAKEFILE_LIST) | \
		sed -E 's/^([a-zA-Z0-9_.-]+):.*?##[ \t]?(.*)/\1\t\2/' | \
		awk -F"\t" '{printf("  %-20s - %s\n", $$1, $$2)}'; \
	@printf "\nPass ARGS=\"<threads> <msgs> <enable_ts>\" to stress targets, e.g. ARGS=\"10 1000 0\" make stress-run\n\n"
