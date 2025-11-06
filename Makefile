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

# stress harness (optional location)
STRESS_SRC := $(firstword $(wildcard tests/stress.c src/stress.c app/src/stress.c))
STRESS_BIN := stress_emlog

CLANG_FORMAT := $(shell command -v clang-format 2>/dev/null)

.PHONY: all libs test clean format format-check help
.PHONY: stress-build stress-run stress-massif stress-helgrind stress-memcheck stress-all

###############################################################################
# Stress targets
###############################################################################

stress-build: $(LIBNAME) ## Build the stress harness (from tests/stress.c)
	@if [ -z "$(STRESS_SRC)" ]; then \
		echo "[stress] no stress source found (create tests/stress.c or src/stress.c)"; exit 1; \
	fi
	@echo "[stress] building $(STRESS_BIN) from $(STRESS_SRC)"
	$(CC) $(CFLAGS) $(CPPFLAGS) $(STRESS_SRC) -L. -lemlog -pthread -o $(STRESS_BIN)

stress-run: stress-build ## Build and run the stress harness (ARGS default: 10 1000 0)
	@echo "[stress] running $(STRESS_BIN) with args: $${ARGS:-10 1000 0} (timing -> /tmp/emlog_stress_result.txt)"
	./$(STRESS_BIN) $${ARGS:-10 1000 0}

stress-massif: stress-build ## Run Massif heap profiler (ARGS default: 10 1000)
	@echo "[stress-massif] running massif for $(STRESS_BIN) with args: $${ARGS:-10 1000} (ms_print saved when available)"
	@OUT=$$(mktemp -u /tmp/emlog_massif_XXXXXX.out); \
	if [ -x "./my_massif_full.sh" ]; then \
		./my_massif_full.sh $(PWD)/$(STRESS_BIN) $${ARGS:-10 1000}; \
		echo "[stress-massif] wrapper result: check wrapper output (it may store massif files elsewhere)"; \
	else \
		valgrind --tool=massif --time-unit=ms --stacks=yes --massif-out-file=$$OUT ./$(STRESS_BIN) $${ARGS:-10 1000}; \
		if [ -f $$OUT ]; then \
			ms_print $$OUT > $${OUT}.txt 2>/dev/null || true; \
			echo "[stress-massif] massif raw: $$OUT"; \
			echo "[stress-massif] ms_print saved to: $${OUT}.txt"; \
		else \
			echo "[stress-massif] massif did not produce $$OUT"; \
		fi; \
	fi

stress-helgrind: stress-build ## Run Helgrind race detector (ARGS default: 20 5000)
	@echo "[stress-helgrind] running Helgrind (race detector) on $(STRESS_BIN) with args: $${ARGS:-20 5000}"
	@if [ -x "./my_hlgrnd.sh" ]; then \
		./my_hlgrnd.sh $(PWD)/$(STRESS_BIN) $${ARGS:-20 5000}; \
	else \
		OUT=$$(mktemp -u /tmp/emlog_helgrind_XXXXXX.out); \
		valgrind --tool=helgrind --log-file=$$OUT ./$(STRESS_BIN) $${ARGS:-20 5000}; \
		if [ -f $$OUT ]; then \
			echo "[stress-helgrind] helgrind log: $$OUT"; \
		else \
			echo "[stress-helgrind] helgrind produced no log"; \
		fi; \
	fi

stress-memcheck: stress-build ## Quick Valgrind Memcheck run (ARGS default: 5 500)
	@echo "[stress-memcheck] running Valgrind Memcheck on $(STRESS_BIN) with args: $${ARGS:-5 500}"
	@if [ -x "./my_vlgrnd_full.sh" ]; then \
		./my_vlgrnd_full.sh $(PWD)/$(STRESS_BIN) $${ARGS:-5 500}; \
	else \
		OUT=$$(mktemp -u /tmp/emlog_memcheck_XXXXXX.out); \
		valgrind --tool=memcheck --leak-check=full --log-file=$$OUT ./$(STRESS_BIN) $${ARGS:-5 500}; \
		if [ -f $$OUT ]; then \
			cat $$OUT | sed -n '1,200p'; \
			echo "[stress-memcheck] full log: $$OUT"; \
		fi; \
	fi

stress-all: stress-memcheck stress-massif stress-helgrind ## memcheck -> massif -> helgrind sequence
	@echo "[stress-all] completed memcheck, massif and helgrind sequence"

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
$(TEST_BIN): $(TEST_SRC) $(LIBNAME) ## Build test binary
	$(CC) $(CFLAGS) $(CPPFLAGS) $< -L. -lemlog -pthread -o $@

test: $(TEST_BIN) ## Run unit tests
	@echo "[test] running $(TEST_BIN)"
	./$(TEST_BIN)

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
	@find app src tests -type f \( -name "*.c" -o -name "*.h" \) -print0 \
	 | xargs -0 -r $(CLANG_FORMAT) -i -style=file

format-check: ## Check formatting (no changes)
ifeq ($(CLANG_FORMAT),)
	$(error clang-format not found (install it, e.g. sudo apt install clang-format))
endif
	@test -f .clang-format || { echo ".clang-format missing at repo root"; exit 1; }
	@echo "[format] checking formatting"
	@find app src tests -type f \( -name "*.c" -o -name "*.h" \) -print0 \
	 | xargs -0 -r $(CLANG_FORMAT) --dry-run -Werror -style=file

help: ## Show this help message (targets with descriptions)
	@sh -c '\
	  printf "\nAvailable targets:\n\n"; \
	  grep -E "^[a-zA-Z0-9_.-]+:.*?##" "$(MAKEFILE_LIST)" | \
	  sed -E "s/^([a-zA-Z0-9_.-]+):.*?##[ \t]?(.*)/\\1\\t\\2/" | \
	  awk -F"\\t" '{printf("  %-20s - %s\\n", $$1, $$2)}'; \
	  printf "\nPass ARGS=\"<threads> <msgs> <enable_ts>\" to stress targets, e.g. ARGS=\"10 1000 0\" make stress-run\n\n"; \
	'
