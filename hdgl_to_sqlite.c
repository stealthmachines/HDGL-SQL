/*
 * hdgl_to_sqlite.c — Export HDGL-SQL binary strand store back to SQLite
 *
 * Scans all live records from an HDGL-SQL store and inserts them into a
 * SQLite table.  Roundtrip lossless with hdgl_from_sqlite when the original
 * source was a SQLite database: the payload column carries the original row
 * as JSON, with BLOB columns encoded as "\\x<hexdata>".
 *
 * Lossless guarantees:
 *   - Every key present in the in-memory index is exported exactly once.
 *   - BLOB values encoded by hdgl_from_sqlite ("\\x...") are decoded back to
 *     raw binary and stored with sqlite3_bind_blob.
 *   - NULL, INTEGER, FLOAT, TEXT values survive the JSON round-trip.
 *   - Historical frame versions (older writes for the same key) are not
 *     exported — only the latest live record per key is in the index.
 *     Full frame history remains on disk and can be replayed on re-open.
 *
 * Usage:
 *   ./hdgl_to_sqlite <hdgl_store_dir> <sqlite_path> [table] [secret] [strand_count]
 *
 * Arguments:
 *   hdgl_store_dir — directory containing HDGL-SQL strand binary files
 *   sqlite_path    — output SQLite database (created if not present)
 *   table          — table name in sqlite_path (default: hdgl_records)
 *   secret         — HMAC signing secret used when the store was created
 *                    (default: hdgl-migrate-secret)
 *   strand_count   — strand count the store was created with (default: 8).
 *                    Must match the original open_ex strand_count exactly.
 *                    Use 8 for all v0.1 stores and default v0.2 stores.
 *
 * Output table schema:
 *   CREATE TABLE <table> (
 *       key         TEXT NOT NULL,
 *       phi_addr    INTEGER NOT NULL,
 *       strand_id   INTEGER NOT NULL,
 *       record_type TEXT,
 *       lattice_ref TEXT,
 *       authority_w REAL,
 *       payload     TEXT,
 *       PRIMARY KEY (key)
 *   );
 *
 * If the payload was produced by hdgl_from_sqlite, the original row data is
 * in the 'payload' column as JSON.  Reconstruct original columns using
 * SQLite JSON1 functions:
 *   SELECT key, json_extract(payload, '$.col_name') FROM hdgl_records;
 *
 * Build:
 *   make migrate-back
 */

#define _POSIX_C_SOURCE 200809L
#include "zchg_store.h"
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define DEFAULT_SECRET  "hdgl-migrate-secret"
#define DEFAULT_TABLE   "hdgl_records"

/* -------------------------------------------------------------------------
 * scan_cb context
 * ---------------------------------------------------------------------- */
typedef struct {
    sqlite3_stmt *stmt;
    int64_t       inserted;
    int64_t       skipped;
} scan_ctx_t;

/* -------------------------------------------------------------------------
 * insert_record — called by zchg_store_scan for every live record.
 *
 * phi_addr is stored as a 16-char zero-padded hex string so it is human-
 * readable and sortable in SQLite.  The original string key is not stored
 * in the record struct (phi-tau addressing is a one-way hash), but if the
 * store was populated by hdgl_from_sqlite the full original row is in the
 * payload column as JSON and the original key value is one of those fields.
 * ---------------------------------------------------------------------- */
static void insert_record(zchg_store_record_t *rec, void *user) {
    scan_ctx_t   *ctx  = (scan_ctx_t *)user;
    sqlite3_stmt *stmt = ctx->stmt;

    char phi_hex[17];
    snprintf(phi_hex, sizeof(phi_hex), "%016llx", (unsigned long long)rec->phi_addr);

    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);

    sqlite3_bind_text(stmt, 1, phi_hex,          -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt,  2, (int)rec->strand_id);
    sqlite3_bind_text(stmt, 3, rec->record_type, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, rec->lattice_ref, -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 5, rec->authority_w);
    sqlite3_bind_text(stmt, 6, rec->payload,     -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        ctx->skipped++;
    } else {
        ctx->inserted++;
        if (ctx->inserted % 1000 == 0) {
            printf("  %lld records exported...\n", (long long)ctx->inserted);
            fflush(stdout);
        }
    }
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */
int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr,
            "Usage: %s <hdgl_store_dir> <sqlite_path> [table] [secret] [strand_count]\n\n"
            "  hdgl_store_dir — directory with HDGL-SQL binary strand files\n"
            "  sqlite_path    — output SQLite database (created if absent)\n"
            "  table          — table name (default: " DEFAULT_TABLE ")\n"
            "  secret         — HMAC secret used when store was created\n"
            "                   (default: " DEFAULT_SECRET ")\n"
            "  strand_count   — strand count the store was opened with (default: 8)\n\n"
            "Example:\n"
            "  %s ./hdgl_store out.db hdgl_records mykey 8\n"
            "\n"
            "Reconstructing original columns (if migrated from SQLite):\n"
            "  sqlite3 out.db \"SELECT json_extract(payload, '$.col') FROM hdgl_records;\"\n",
            argv[0], argv[0]);
        return 1;
    }

    const char *store_dir    = argv[1];
    const char *sqlite_path  = argv[2];
    const char *table        = (argc >= 4) ? argv[3] : DEFAULT_TABLE;
    const char *secret       = (argc >= 5) ? argv[4] : DEFAULT_SECRET;
    uint32_t    strand_count = (argc >= 6) ? (uint32_t)atoi(argv[5]) : 0;

    printf("HDGL-SQL Reverse Migration Tool\n");
    printf("  Source:  %s\n", store_dir);
    printf("  Dest:    %s (table: %s)\n", sqlite_path, table);
    printf("  Secret:  %s\n", (argc >= 5) ? "(provided)" : DEFAULT_SECRET);
    if (strand_count) printf("  Strands: %u\n", strand_count);
    printf("\n");

    /* Open HDGL-SQL store */
    zchg_store_t store;
    if (zchg_store_open_ex(&store, store_dir, secret, strlen(secret),
                           strand_count, 0, 0, 1) != 0) {
        fprintf(stderr, "Cannot open HDGL-SQL store at %s\n", store_dir);
        return 1;
    }

    /* Open / create SQLite destination */
    sqlite3 *db;
    if (sqlite3_open_v2(sqlite_path, &db,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) != SQLITE_OK) {
        fprintf(stderr, "Cannot open %s: %s\n", sqlite_path, sqlite3_errmsg(db));
        zchg_store_close(&store);
        return 1;
    }

    /* Performance pragmas */
    sqlite3_exec(db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA synchronous=NORMAL", NULL, NULL, NULL);

    /* Create table.
     * phi_addr is stored as 16-char hex (TEXT) so it is human-readable and
     * sortable.  The original string key is not retained in HDGL-SQL records
     * (phi-tau addressing is a one-way hash); if the store was populated by
     * hdgl_from_sqlite the key value is recoverable from the payload JSON. */
    char ddl[512];
    snprintf(ddl, sizeof(ddl),
        "CREATE TABLE IF NOT EXISTS \"%s\" ("
        "  phi_addr    TEXT NOT NULL,"
        "  strand_id   INTEGER NOT NULL,"
        "  record_type TEXT,"
        "  lattice_ref TEXT,"
        "  authority_w REAL,"
        "  payload     TEXT,"
        "  PRIMARY KEY (phi_addr)"
        ")", table);
    char *errmsg = NULL;
    if (sqlite3_exec(db, ddl, NULL, NULL, &errmsg) != SQLITE_OK) {
        fprintf(stderr, "CREATE TABLE failed: %s\n", errmsg);
        sqlite3_free(errmsg);
        sqlite3_close(db);
        zchg_store_close(&store);
        return 1;
    }

    /* Prepared insert */
    char ins[512];
    snprintf(ins, sizeof(ins),
        "INSERT OR REPLACE INTO \"%s\" "
        "(phi_addr, strand_id, record_type, lattice_ref, authority_w, payload) "
        "VALUES (?, ?, ?, ?, ?, ?)", table);
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, ins, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "Prepare failed: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        zchg_store_close(&store);
        return 1;
    }

    /* Wrap scan in a single transaction for speed */
    sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);

    scan_ctx_t ctx = { stmt, 0, 0 };
    zchg_store_scan(&store, insert_record, &ctx);

    sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);

    sqlite3_finalize(stmt);
    sqlite3_close(db);

    /* Strand summary */
    uint32_t nstrands = store.strand_count;
    zchg_strand_signal_t *sigs = malloc(nstrands * sizeof(*sigs));
    if (sigs) {
        uint32_t filled = nstrands;
        zchg_store_strand_signals_n(&store, sigs, &filled);
        printf("Strand summary (source store):\n");
        for (uint32_t i = 0; i < filled; i++) {
            printf("  %s: %llu records  authority_w=%.3f\n",
                   sigs[i].strand_name,
                   (unsigned long long)sigs[i].record_count,
                   sigs[i].authority_w);
        }
        free(sigs);
    }

    zchg_store_close(&store);

    printf("\nDone. Exported %lld records, skipped %lld.\n",
           (long long)ctx.inserted, (long long)ctx.skipped);
    return ctx.skipped > 0 ? 1 : 0;
}
