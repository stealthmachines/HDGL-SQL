/*
 * bench_sqlite.c — SQLite C API Benchmark (WAL mode)
 *
 * Equivalent operations to bench_store.c, using the SQLite C API directly.
 * No Python, no HTTP, no GIL. Apples-to-apples with bench_store.c.
 *
 * Schema mirrors the HDGL-sql lattice:
 *   hdgl_lattice(phi_addr TEXT PK, strand_id INT, record_type TEXT,
 *                authority_w REAL, payload TEXT)
 *
 * Phases
 * ──────
 *   1. PUT  — INSERT OR REPLACE into WAL-mode database
 *   2. GET  — SELECT by primary key (phi_addr)
 *   3. SCAN — SELECT * full table scan
 *   4. SIG  — SELECT COUNT(*) GROUP BY strand_id (strand signals)
 *
 * Usage:
 *   ./bench_sqlite [duration_sec]
 * Default: 5 seconds per phase
 */

#define _POSIX_C_SOURCE 200809L
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <sys/stat.h>

#define BENCH_DB_PATH   "/tmp/hdglsql_sqlite_bench.db"
#define BENCH_KEYS      8

static const char *KEYS[BENCH_KEYS] = {
    "bench:strand0:alpha",   "bench:strand1:beta",
    "bench:strand2:gamma",   "bench:strand3:delta",
    "bench:strand4:epsilon", "bench:strand5:zeta",
    "bench:strand6:eta",     "bench:strand7:theta",
};

/* Fibonacci hash to get a phi_addr hex string — mirrors HDGL-SQL routing */
static void phi_addr_of(const char *key, char out[17]) {
    uint64_t h = 0xcbf29ce484222325ULL;
    const uint64_t prime = 0x100000001b3ULL;
    const uint64_t phi_xor = 1618033988ULL;
    for (const unsigned char *p = (const unsigned char *)key; *p; p++) {
        h = ((h ^ *p) * prime);
        h = ((h << 13) | (h >> 51)) ^ phi_xor;
    }
    snprintf(out, 17, "%016llx", (unsigned long long)h);
}

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void print_result(const char *phase, uint64_t ops, double elapsed) {
    printf("  %-28s %12.0f req/s  (%llu ops in %.2fs)\n",
           phase, (double)ops / elapsed, (unsigned long long)ops, elapsed);
}

static sqlite3 *open_db(void) {
    sqlite3 *db;
    if (sqlite3_open(BENCH_DB_PATH, &db) != SQLITE_OK) {
        fprintf(stderr, "sqlite3_open: %s\n", sqlite3_errmsg(db));
        exit(1);
    }
    sqlite3_exec(db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA synchronous=NORMAL", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA cache_size=-65536", NULL, NULL, NULL); /* 64 MB page cache */
    return db;
}

int main(int argc, char **argv) {
    double duration = 5.0;
    if (argc >= 2) duration = atof(argv[1]);
    if (duration <= 0) duration = 5.0;

    printf("SQLite %s — C API Benchmark (WAL)\n", sqlite3_libversion());
    printf("Platform: Linux, i7-6700T, WSL2\n");
    printf("DB path:  %s\n", BENCH_DB_PATH);
    printf("Phase duration: %.0f seconds each\n\n", duration);

    remove(BENCH_DB_PATH);

    sqlite3 *db = open_db();

    /* Create schema */
    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS hdgl_lattice ("
        "  phi_addr    TEXT PRIMARY KEY,"
        "  strand_id   INTEGER NOT NULL,"
        "  record_type TEXT    NOT NULL,"
        "  authority_w REAL    NOT NULL DEFAULT 0.0,"
        "  payload     TEXT    NOT NULL"
        ");",
        NULL, NULL, NULL);
    sqlite3_exec(db, "CREATE INDEX IF NOT EXISTS idx_strand ON hdgl_lattice(strand_id);",
                 NULL, NULL, NULL);

    /* Pre-populate one row per key so GET phase has data */
    {
        sqlite3_stmt *ins;
        sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO hdgl_lattice VALUES (?,?,?,?,?)",
            -1, &ins, NULL);
        for (int i = 0; i < BENCH_KEYS; i++) {
            char addr[17];
            phi_addr_of(KEYS[i], addr);
            char payload[64];
            snprintf(payload, sizeof(payload), "{\"v\":%d,\"strand\":%d}", i+1, i);
            sqlite3_bind_text(ins, 1, addr,    -1, SQLITE_STATIC);
            sqlite3_bind_int (ins, 2, i);
            sqlite3_bind_text(ins, 3, "bench", -1, SQLITE_STATIC);
            sqlite3_bind_double(ins, 4, 0.5);
            sqlite3_bind_text(ins, 5, payload, -1, SQLITE_STATIC);
            sqlite3_step(ins);
            sqlite3_reset(ins);
        }
        sqlite3_finalize(ins);
    }

    printf("Results\n");
    printf("-------\n");

    /* ------------------------------------------------------------------ */
    /* Phase 1: PUT (INSERT OR REPLACE)                                    */
    /* ------------------------------------------------------------------ */
    {
        sqlite3_stmt *ins;
        sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO hdgl_lattice VALUES (?,?,?,?,?)",
            -1, &ins, NULL);

        double t0 = now_sec(), t1;
        uint64_t ops = 0;
        int slot = 0;

        sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
        while ((t1 = now_sec()) - t0 < duration) {
            char addr[17];
            phi_addr_of(KEYS[slot], addr);
            char payload[64];
            snprintf(payload, sizeof(payload), "{\"v\":%llu}", (unsigned long long)ops);

            sqlite3_bind_text(ins, 1, addr,    -1, SQLITE_TRANSIENT);
            sqlite3_bind_int (ins, 2, slot);
            sqlite3_bind_text(ins, 3, "bench", -1, SQLITE_STATIC);
            sqlite3_bind_double(ins, 4, 0.5 + (double)(ops & 0xFF) / 512.0);
            sqlite3_bind_text(ins, 5, payload, -1, SQLITE_TRANSIENT);
            sqlite3_step(ins);
            sqlite3_reset(ins);
            slot = (slot + 1) & 7;
            ops++;

            /* Commit every 64 ops — mirrors WBUF_FLUSH_COUNT=1 spirit but
               avoids pathological single-row transaction overhead */
            if ((ops & 63) == 0) {
                sqlite3_exec(db, "COMMIT; BEGIN", NULL, NULL, NULL);
            }
        }
        sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
        sqlite3_finalize(ins);
        print_result("PUT (INSERT OR REPLACE, WAL)", ops, t1 - t0);
    }

    /* ------------------------------------------------------------------ */
    /* Phase 2: GET (SELECT by primary key)                                */
    /* ------------------------------------------------------------------ */
    {
        sqlite3_stmt *sel;
        sqlite3_prepare_v2(db,
            "SELECT payload FROM hdgl_lattice WHERE phi_addr=?",
            -1, &sel, NULL);

        double t0 = now_sec(), t1;
        uint64_t ops = 0;
        int slot = 0;

        while ((t1 = now_sec()) - t0 < duration) {
            char addr[17];
            phi_addr_of(KEYS[slot], addr);
            sqlite3_bind_text(sel, 1, addr, -1, SQLITE_TRANSIENT);
            if (sqlite3_step(sel) == SQLITE_ROW) {
                volatile const unsigned char *p = sqlite3_column_text(sel, 0);
                (void)p;
            }
            sqlite3_reset(sel);
            slot = (slot + 1) & 7;
            ops++;
        }
        sqlite3_finalize(sel);
        print_result("GET (SELECT by phi_addr PK)", ops, t1 - t0);
    }

    /* ------------------------------------------------------------------ */
    /* Phase 3: SCAN (SELECT *)                                            */
    /* ------------------------------------------------------------------ */
    {
        sqlite3_stmt *scan;
        sqlite3_prepare_v2(db,
            "SELECT phi_addr, payload FROM hdgl_lattice",
            -1, &scan, NULL);

        double t0 = now_sec(), t1;
        uint64_t ops = 0;

        while ((t1 = now_sec()) - t0 < duration) {
            while (sqlite3_step(scan) == SQLITE_ROW) {
                volatile const unsigned char *p = sqlite3_column_text(scan, 1);
                (void)p;
            }
            sqlite3_reset(scan);
            ops++;
        }
        sqlite3_finalize(scan);
        print_result("SCAN (SELECT * full table)", ops, t1 - t0);
    }

    /* ------------------------------------------------------------------ */
    /* Phase 4: Strand signals (COUNT GROUP BY)                            */
    /* ------------------------------------------------------------------ */
    {
        sqlite3_stmt *sig;
        sqlite3_prepare_v2(db,
            "SELECT strand_id, COUNT(*), AVG(authority_w) FROM hdgl_lattice "
            "GROUP BY strand_id ORDER BY strand_id",
            -1, &sig, NULL);

        double t0 = now_sec(), t1;
        uint64_t ops = 0;

        while ((t1 = now_sec()) - t0 < duration) {
            while (sqlite3_step(sig) == SQLITE_ROW) {
                volatile int sid = sqlite3_column_int(sig, 0);
                (void)sid;
            }
            sqlite3_reset(sig);
            ops++;
        }
        sqlite3_finalize(sig);
        print_result("STRAND SIGNALS (GROUP BY)", ops, t1 - t0);
    }

    sqlite3_close(db);
    remove(BENCH_DB_PATH);

    printf("\nNotes:\n");
    printf("  Direct C API — no Python, no GIL, no HTTP overhead.\n");
    printf("  WAL mode, PRAGMA synchronous=NORMAL, 64 MB page cache.\n");
    printf("  PUT uses batched transactions (COMMIT every 64 ops).\n");
    printf("  Compare with: make bench  (HDGL-SQL direct library)\n");

    return 0;
}
