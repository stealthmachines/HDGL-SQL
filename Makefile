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

BENCH_SRC        = bench_store.c
BENCH_BIN        = bench_store
BENCH_SQLITE_SRC = bench_sqlite.c
BENCH_SQLITE_BIN = bench_sqlite
MIGRATE_SRC      = hdgl_from_sqlite.c
MIGRATE_BIN      = hdgl_from_sqlite
MIGRATE_BACK_SRC = hdgl_to_sqlite.c
MIGRATE_BACK_BIN = hdgl_to_sqlite

.PHONY: all shared test bench bench-sqlite migrate migrate-back clean help

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
	@printf '#define _POSIX_C_SOURCE 200809L\n#include "zchg_store.h"\n#include <stdio.h>\n#include <string.h>\n#include <stdlib.h>\nint main(void) {\n    zchg_store_t s;\n    system("rm -rf /tmp/hdglsql_smoke");\n    if (zchg_store_open(&s, "/tmp/hdglsql_smoke", "test-secret", 11) != 0) return 1;\n    const char *json = "{\\"x\\":1}";\n    if (zchg_store_put(&s, "k:1", "t", NULL, json, strlen(json)) != 0) return 1;\n    zchg_store_record_t *r = zchg_store_get(&s, "k:1");\n    if (!r || strcmp(r->payload, json) != 0) return 1;\n    zchg_store_flush(&s); zchg_store_close(&s);\n    system("rm -rf /tmp/hdglsql_smoke");\n    system("rm -rf /tmp/hdglsql_smoke64");\n    if (zchg_store_open_ex(&s, "/tmp/hdglsql_smoke64", "test-secret", 11, 64, 8192, 0, 1) != 0) return 1;\n    for (int i = 0; i < 200; i++) { char k[32]; snprintf(k, sizeof(k), "key:%%d", i); if (zchg_store_put(&s, k, "t", NULL, "{\\"n\\":1}", 7) != 0) return 1; }\n    if (s.strand_count != 64) return 1;\n    if (s.index_used != 200) return 1;\n    zchg_store_flush(&s); zchg_store_close(&s);\n    system("rm -rf /tmp/hdglsql_smoke64");\n    system("rm -rf /tmp/hdglsql_shard1");\n    uint32_t sh = zchg_store_shard_of("k:1", 2);\n    uint32_t other = 1 - sh;\n    if (zchg_store_open_ex(&s, "/tmp/hdglsql_shard1", "sec", 3, 8, 0, other, 2) != 0) return 1;\n    if (zchg_store_put(&s, "k:1", "t", NULL, "{\\"a\\":1}", 7) != ZCHG_ERR_WRONG_SHARD) return 1;\n    zchg_store_close(&s);\n    system("rm -rf /tmp/hdglsql_shard1");\n    return 0;\n}\n' > $@

# ============================================================================
# Clean
# ============================================================================

clean:
	@rm -rf $(OBJDIR) $(LIB_STATIC) $(LIB_SHARED) $(TEST_BIN) $(TEST_SRC) \
	        $(BENCH_BIN) $(BENCH_SQLITE_BIN) $(MIGRATE_BIN) $(MIGRATE_BACK_BIN)
	@echo "Cleaned"

# ============================================================================
# Benchmark — direct library throughput, no HTTP
# ============================================================================

bench: all $(BENCH_SRC)
	$(CC) $(CFLAGS) -I$(INCDIR) -o $(BENCH_BIN) $(BENCH_SRC) $(LIB_STATIC) $(LDFLAGS)
	@echo "Running HDGL-SQL benchmark (5s per phase)..."
	@./$(BENCH_BIN)

# ============================================================================
# Benchmark — SQLite C API (WAL, no Python/GIL, apples-to-apples with bench)
# ============================================================================

bench-sqlite: $(BENCH_SQLITE_SRC)
	$(CC) $(CFLAGS) -o $(BENCH_SQLITE_BIN) $(BENCH_SQLITE_SRC) -lsqlite3
	@echo "Running SQLite C API benchmark (5s per phase)..."
	@./$(BENCH_SQLITE_BIN)

# ============================================================================
# Migration — import any SQLite table into HDGL-SQL binary strand files
# ============================================================================

migrate: all $(MIGRATE_SRC)
	$(CC) $(CFLAGS) -I$(INCDIR) -o $(MIGRATE_BIN) $(MIGRATE_SRC) \
	    $(LIB_STATIC) $(LDFLAGS) -lsqlite3
	@echo "Built: $(MIGRATE_BIN)"
	@echo "Usage: ./$(MIGRATE_BIN) <sqlite_path> <table> <key_col> <hdgl_store_dir> [secret] [strand_count]"

# ============================================================================
# Reverse migration — export HDGL-SQL store back to SQLite
# ============================================================================

migrate-back: all $(MIGRATE_BACK_SRC)
	$(CC) $(CFLAGS) -I$(INCDIR) -o $(MIGRATE_BACK_BIN) $(MIGRATE_BACK_SRC) \
	    $(LIB_STATIC) $(LDFLAGS) -lsqlite3
	@echo "Built: $(MIGRATE_BACK_BIN)"
	@echo "Usage: ./$(MIGRATE_BACK_BIN) <hdgl_store_dir> <sqlite_path> [table] [secret] [strand_count]"

# ============================================================================
# Help
# ============================================================================

help:
	@echo "HDGL-SQL v1 — Strand-Native Persistent Store"
	@echo ""
	@echo "Targets:"
	@echo "  make           Build static library (libhdglsql.a)"
	@echo "  make shared    Build shared library (libhdglsql.so)"
	@echo "  make test          Build + run smoke test (open/put/get/close)"
	@echo "  make bench         Build + run HDGL-SQL throughput benchmark (direct C API)"
	@echo "  make bench-sqlite  Build + run SQLite C API benchmark (WAL, apples-to-apples)"
	@echo "  make migrate       Build hdgl_from_sqlite  (SQLite -> HDGL-SQL)"
	@echo "  make migrate-back  Build hdgl_to_sqlite    (HDGL-SQL -> SQLite)"
	@echo "  make clean         Remove build artifacts"
	@echo ""
	@echo "Integration:"
	@echo "  CFLAGS: -Iinclude"
	@echo "  LDFLAGS: libhdglsql.a -lcrypto -lm"
	@echo ""
	@echo "Requirements:"
	@echo "  gcc, OpenSSL dev headers (libssl-dev / openssl-devel)"
