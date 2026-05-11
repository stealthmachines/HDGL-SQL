/*
 * hdgl_from_sqlite.c — Migrate any SQLite table into HDGL-SQL binary strands
 *
 * Reads a SQLite database table, converts each row to an HDGL-SQL record, and
 * appends it to the binary strand files using the HDGL-SQL C API.
 *
 * The key insight: rows don't need to be redesigned. You specify which column
 * is the logical key; the rest of the row is serialized as JSON payload. The
 * phi-tau hash of the key column determines the strand automatically.
 *
 * Usage:
 *   ./hdgl_from_sqlite <sqlite_path> <table> <key_col> <hdgl_store_dir> [secret]
 *
 * Example:
 *   ./hdgl_from_sqlite frontierland_mega.hdgl hdgl_lattice phi_addr ./hdgl_store mykey
 *
 *   Migrates all rows from hdgl_lattice table, using phi_addr column as the
 *   logical key, into the HDGL-SQL binary strand store at ./hdgl_store.
 *
 * Notes:
 *   - All column values are serialized as JSON strings in the payload field.
 *   - record_type is set to the table name unless a 'record_type' column exists.
 *   - lattice_ref is set from a 'lattice_ref' column if present, else empty.
 *   - Existing HDGL-SQL store data is preserved (appends only).
 *   - Progress is printed every 1000 rows.
 */

#define _POSIX_C_SOURCE 200809L
#include "zchg_store.h"
#include <sqlite3.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PAYLOAD  65536
#define DEFAULT_SECRET  "hdgl-migrate-secret"

/* Build a JSON object string from sqlite3 column names and values */
static int build_json(sqlite3_stmt *stmt, int ncols, char *buf, size_t bufsz) {
    size_t pos = 0;
    buf[pos++] = '{';

    for (int i = 0; i < ncols; i++) {
        const char *name = sqlite3_column_name(stmt, i);
        int type = sqlite3_column_type(stmt, i);

        if (i > 0) {
            if (pos + 1 >= bufsz) return -1;
            buf[pos++] = ',';
        }

        /* key: "name" */
        if (pos + strlen(name) + 4 >= bufsz) return -1;
        buf[pos++] = '"';
        for (const char *c = name; *c; c++) {
            if (*c == '"' || *c == '\\') buf[pos++] = '\\';
            buf[pos++] = *c;
        }
        buf[pos++] = '"';
        buf[pos++] = ':';

        /* value */
        if (type == SQLITE_NULL) {
            if (pos + 4 >= bufsz) return -1;
            memcpy(buf + pos, "null", 4); pos += 4;
        } else if (type == SQLITE_INTEGER) {
            sqlite3_int64 v = sqlite3_column_int64(stmt, i);
            pos += (size_t)snprintf(buf + pos, bufsz - pos, "%lld", (long long)v);
        } else if (type == SQLITE_FLOAT) {
            double v = sqlite3_column_double(stmt, i);
            pos += (size_t)snprintf(buf + pos, bufsz - pos, "%.17g", v);
        } else {
            /* TEXT or BLOB — emit as JSON string with escaping */
            const char *s = (const char *)sqlite3_column_text(stmt, i);
            if (!s) s = "";
            if (pos + 2 >= bufsz) return -1;
            buf[pos++] = '"';
            for (const char *c = s; *c && pos + 6 < bufsz; c++) {
                switch (*c) {
                    case '"':  buf[pos++] = '\\'; buf[pos++] = '"';  break;
                    case '\\': buf[pos++] = '\\'; buf[pos++] = '\\'; break;
                    case '\n': buf[pos++] = '\\'; buf[pos++] = 'n';  break;
                    case '\r': buf[pos++] = '\\'; buf[pos++] = 'r';  break;
                    case '\t': buf[pos++] = '\\'; buf[pos++] = 't';  break;
                    default:   buf[pos++] = *c;                      break;
                }
            }
            if (pos + 1 >= bufsz) return -1;
            buf[pos++] = '"';
        }
    }

    if (pos + 1 >= bufsz) return -1;
    buf[pos++] = '}';
    buf[pos]   = '\0';
    return (int)pos;
}

/* Find column index by name, return -1 if not found */
static int find_col(sqlite3_stmt *stmt, int ncols, const char *name) {
    for (int i = 0; i < ncols; i++) {
        if (strcasecmp(sqlite3_column_name(stmt, i), name) == 0)
            return i;
    }
    return -1;
}

int main(int argc, char **argv) {
    if (argc < 5) {
        fprintf(stderr,
            "Usage: %s <sqlite_path> <table> <key_col> <hdgl_store_dir> [secret]\n\n"
            "  sqlite_path    — path to SQLite database file\n"
            "  table          — table name to migrate\n"
            "  key_col        — column whose value is used as the HDGL-SQL record key\n"
            "  hdgl_store_dir — output directory for HDGL-SQL binary strand files\n"
            "  secret         — HMAC signing secret (default: hdgl-migrate-secret)\n\n"
            "Example:\n"
            "  %s frontierland_mega.hdgl hdgl_lattice phi_addr ./hdgl_store mykey\n",
            argv[0], argv[0]);
        return 1;
    }

    const char *sqlite_path = argv[1];
    const char *table       = argv[2];
    const char *key_col     = argv[3];
    const char *store_dir   = argv[4];
    const char *secret      = (argc >= 6) ? argv[5] : DEFAULT_SECRET;

    printf("HDGL-SQL Migration Tool\n");
    printf("  Source:  %s (table: %s, key: %s)\n", sqlite_path, table, key_col);
    printf("  Dest:    %s\n", store_dir);
    printf("  Secret:  %s\n\n", (argc >= 6) ? "(provided)" : DEFAULT_SECRET);

    /* Open SQLite source */
    sqlite3 *db;
    if (sqlite3_open_v2(sqlite_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        fprintf(stderr, "Cannot open %s: %s\n", sqlite_path, sqlite3_errmsg(db));
        return 1;
    }
    sqlite3_exec(db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);

    /* Count rows first */
    char count_sql[256];
    snprintf(count_sql, sizeof(count_sql), "SELECT COUNT(*) FROM \"%s\"", table);
    sqlite3_stmt *cnt_stmt;
    int64_t total_rows = 0;
    if (sqlite3_prepare_v2(db, count_sql, -1, &cnt_stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(cnt_stmt) == SQLITE_ROW)
            total_rows = sqlite3_column_int64(cnt_stmt, 0);
        sqlite3_finalize(cnt_stmt);
    }
    printf("Rows to migrate: %lld\n\n", (long long)total_rows);

    /* Open HDGL-SQL store */
    zchg_store_t store;
    if (zchg_store_open(&store, store_dir, secret, strlen(secret)) != 0) {
        fprintf(stderr, "Cannot open HDGL-SQL store at %s\n", store_dir);
        sqlite3_close(db);
        return 1;
    }

    /* Iterate rows */
    char sel_sql[512];
    snprintf(sel_sql, sizeof(sel_sql), "SELECT * FROM \"%s\"", table);
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sel_sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "Cannot query table %s: %s\n", table, sqlite3_errmsg(db));
        zchg_store_close(&store);
        sqlite3_close(db);
        return 1;
    }

    int ncols = sqlite3_column_count(stmt);
    char *payload_buf = malloc(MAX_PAYLOAD);
    if (!payload_buf) { perror("malloc"); return 1; }

    int64_t migrated = 0, skipped = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        /* Find key column */
        int key_idx = find_col(stmt, ncols, key_col);
        if (key_idx < 0) {
            fprintf(stderr, "Key column '%s' not found in row — skipping\n", key_col);
            skipped++;
            continue;
        }

        const char *key_val = (const char *)sqlite3_column_text(stmt, key_idx);
        if (!key_val || !*key_val) {
            skipped++;
            continue;
        }

        /* Optional: record_type and lattice_ref from columns if present */
        int rt_idx  = find_col(stmt, ncols, "record_type");
        int ref_idx = find_col(stmt, ncols, "lattice_ref");

        const char *record_type = table; /* default: table name */
        if (rt_idx >= 0) {
            const char *v = (const char *)sqlite3_column_text(stmt, rt_idx);
            if (v && *v) record_type = v;
        }

        const char *lattice_ref = NULL;
        if (ref_idx >= 0) {
            const char *v = (const char *)sqlite3_column_text(stmt, ref_idx);
            if (v && *v) lattice_ref = v;
        }

        /* Build JSON payload from all columns */
        int plen = build_json(stmt, ncols, payload_buf, MAX_PAYLOAD);
        if (plen <= 0) {
            fprintf(stderr, "Row %lld: payload too large, skipping\n",
                    (long long)(migrated + skipped));
            skipped++;
            continue;
        }

        if (zchg_store_put(&store, key_val, record_type, lattice_ref,
                           payload_buf, (size_t)plen) != 0) {
            fprintf(stderr, "Row %lld: store_put failed, skipping\n",
                    (long long)(migrated + skipped));
            skipped++;
            continue;
        }

        migrated++;
        if (migrated % 1000 == 0) {
            printf("  %lld / %lld rows migrated...\n",
                   (long long)migrated, (long long)total_rows);
            fflush(stdout);
        }
    }

    zchg_store_flush(&store);

    /* Print strand distribution */
    zchg_strand_signal_t sigs[8];
    zchg_store_strand_signals(&store, sigs);
    printf("\nStrand distribution after migration:\n");
    for (int i = 0; i < 8; i++) {
        printf("  %s: %llu records  authority_w=%.3f\n",
               sigs[i].strand_name,
               (unsigned long long)sigs[i].record_count,
               sigs[i].authority_w);
    }

    zchg_store_close(&store);
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    free(payload_buf);

    printf("\nDone. Migrated %lld rows, skipped %lld.\n",
           (long long)migrated, (long long)skipped);
    return 0;
}
