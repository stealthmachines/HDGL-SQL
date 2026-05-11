# HDGL-SQL v1 — Strand-Native Persistent Store
#
# Targets:
#   make             - build static library (libhdglsql.a)
#   make shared      - build shared library (libhdglsql.so)
#   make test        - build and run smoke test
#   make clean       - remove build artifacts
#   make help        - show this help

CC        = gcc
CFLAGS    = -Wall -Wextra -O3 -march=native -std=c11 -fPIC
LDFLAGS   = -lm -lcrypto

SRCDIR    = src
INCDIR    = include
OBJDIR    = obj

SOURCES   = $(SRCDIR)/zchg_frame.c \
            $(SRCDIR)/zchg_lattice.c \
            $(SRCDIR)/zchg_store.c

HEADERS   = $(INCDIR)/zchg_core.h \
            $(INCDIR)/zchg_lattice.h \
            $(INCDIR)/zchg_store.h

OBJECTS   = $(SOURCES:$(SRCDIR)/%.c=$(OBJDIR)/%.o)

LIB_STATIC  = libhdglsql.a
LIB_SHARED  = libhdglsql.so

# ============================================================================
# Default: static library
# ============================================================================

.PHONY: all shared test clean help

all: $(OBJDIR) $(LIB_STATIC)
	@echo "Built: $(LIB_STATIC)"

$(LIB_STATIC): $(OBJECTS)
	ar rcs $@ $^
	@echo "Archived: $@"

$(OBJDIR):
	@mkdir -p $(OBJDIR)

$(OBJDIR)/%.o: $(SRCDIR)/%.c $(HEADERS)
	$(CC) $(CFLAGS) -I$(INCDIR) -c -o $@ $<
	@echo "Compiled: $<"

# ============================================================================
# Shared library
# ============================================================================

shared: $(OBJDIR) $(LIB_SHARED)

$(LIB_SHARED): $(OBJECTS)
	$(CC) -shared -o $@ $^ $(LDFLAGS)
	@echo "Built: $@"

# ============================================================================
# Smoke test — open a store, put, get, close
# ============================================================================

TEST_SRC  = test_smoke.c
TEST_BIN  = test_smoke

test: all $(TEST_SRC)
	$(CC) $(CFLAGS) -I$(INCDIR) -o $(TEST_BIN) $(TEST_SRC) $(LIB_STATIC) $(LDFLAGS)
	@echo "Running smoke test..."
	@./$(TEST_BIN) && echo "PASS" || echo "FAIL"
	@rm -f $(TEST_BIN)

$(TEST_SRC):
	@printf '#include "zchg_store.h"\n#include <stdio.h>\n#include <string.h>\n#include <stdlib.h>\nint main(void) {\n    zchg_store_t s;\n    system("rm -rf /tmp/hdglsql_smoke");\n    if (zchg_store_open(&s, "/tmp/hdglsql_smoke", "test-secret", 11) != 0) return 1;\n    const char *json = "{\\"x\\":1}";\n    if (zchg_store_put(&s, "k:1", "t", NULL, json, strlen(json)) != 0) return 1;\n    zchg_store_record_t *r = zchg_store_get(&s, "k:1");\n    if (!r || strcmp(r->payload, json) != 0) return 1;\n    zchg_store_flush(&s);\n    zchg_store_close(&s);\n    system("rm -rf /tmp/hdglsql_smoke");\n    return 0;\n}\n' > $@

# ============================================================================
# Clean
# ============================================================================

clean:
	@rm -rf $(OBJDIR) $(LIB_STATIC) $(LIB_SHARED) $(TEST_BIN) $(TEST_SRC)
	@echo "Cleaned"

# ============================================================================
# Help
# ============================================================================

help:
	@echo "HDGL-SQL v1 — Strand-Native Persistent Store"
	@echo ""
	@echo "Targets:"
	@echo "  make           Build static library (libhdglsql.a)"
	@echo "  make shared    Build shared library (libhdglsql.so)"
	@echo "  make test      Build + run smoke test (open/put/get/close)"
	@echo "  make clean     Remove build artifacts"
	@echo ""
	@echo "Integration:"
	@echo "  CFLAGS: -Iinclude"
	@echo "  LDFLAGS: libhdglsql.a -lcrypto -lm"
	@echo ""
	@echo "Requirements:"
	@echo "  gcc, OpenSSL dev headers (libssl-dev / openssl-devel)"
