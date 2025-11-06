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

help: ## Show this help message (targets with descriptions)
	@printf "\nAvailable targets:\n\n"; \
	grep -E '^[a-zA-Z0-9_.-]+:.*?##' $(MAKEFILE_LIST) | \
		sed -E 's/^([a-zA-Z0-9_.-]+):.*?##[ \t]?(.*)/\1\t\2/' | \
		awk -F"\t" '{printf("  %-20s - %s\n", $$1, $$2)}'; \
	@printf "\nPass ARGS=\"<threads> <msgs> <enable_ts>\" to stress targets, e.g. ARGS=\"10 1000 0\" make stress-run\n\n"
