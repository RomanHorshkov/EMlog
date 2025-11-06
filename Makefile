# Makefile for building libemlog.a and test harness
CC = gcc
CFLAGS = -Wall -Wextra -Wpedantic -std=c11 -O2
AR = ar
ARFLAGS = rcs
SRCDIR = src
INCDIR = include
OBJDIR = build
LIBNAME = libemlog.a

SRC = $(SRCDIR)/emlog.c
OBJ = $(OBJDIR)/emlog.o

TEST_SRC = $(SRCDIR)/test.c
TEST_BIN = test_emlog

.PHONY: all test clean
all: $(LIBNAME)

$(OBJDIR):
	mkdir -p $(OBJDIR)

$(OBJ): | $(OBJDIR)
	$(CC) $(CFLAGS) -I$(INCDIR) -c $(SRCDIR)/emlog.c -o $(OBJ)

$(LIBNAME): $(OBJ)
	$(AR) $(ARFLAGS) $@ $^

test: $(LIBNAME)
	$(CC) $(CFLAGS) -I$(INCDIR) $(TEST_SRC) -L. -lemlog -pthread -o $(TEST_BIN)
	./$(TEST_BIN)

clean:
	rm -rf $(OBJDIR) $(LIBNAME) $(TEST_BIN)
