/*
 * bench_store.c — HDGL-SQL v1 Direct Library Benchmark
 *
 * Measures store-layer throughput with zero network overhead.
 * All phases call the C API directly — no HTTP, no sockets.
 *
 * Phases
 * ──────
 *   1. PUT  — signed append to all 8 strands, rotating keys
 *   2. GET  — O(1) phi-tau index read, rotating keys (pre-populated)
 *   3. SCAN — full index walk via zchg_store_scan
 *   4. SIG  — per-strand EMA signal read via zchg_store_strand_signals
 *
 * Usage:
 *   ./bench_store [duration_sec] [ops_per_iteration]
 * Defaults: 5 seconds per phase, 1 op per iteration
 */

#define _POSIX_C_SOURCE 200809L
#include "zchg_store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>

#define BENCH_STORE_DIR   "/tmp/hdglsql_bench"
#define BENCH_SECRET      "hdgl-bench-secret"
#define BENCH_SECRET_LEN  17
#define BENCH_KEYS        8

static const char *KEYS[BENCH_KEYS] = {
    "bench:strand0:alpha",
    "bench:strand1:beta",
    "bench:strand2:gamma",
    "bench:strand3:delta",
    "bench:strand4:epsilon",
    "bench:strand5:zeta",
    "bench:strand6:eta",
    "bench:strand7:theta",
};

static const char *PAYLOADS[BENCH_KEYS] = {
    "{\"v\":1,\"strand\":0}",
    "{\"v\":2,\"strand\":1}",
    "{\"v\":3,\"strand\":2}",
    "{\"v\":4,\"strand\":3}",
    "{\"v\":5,\"strand\":4}",
    "{\"v\":6,\"strand\":5}",
    "{\"v\":7,\"strand\":6}",
    "{\"v\":8,\"strand\":7}",
};

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void print_result(const char *phase, uint64_t ops, double elapsed) {
    double rate = (double)ops / elapsed;
    printf("  %-24s %12.0f req/s  (%llu ops in %.2fs)\n",
           phase, rate, (unsigned long long)ops, elapsed);
}

static void rmdir_store(void) {
    /* Best-effort cleanup — system() is fine for a bench tool */
    if (system("rm -rf " BENCH_STORE_DIR) != 0) { /* ignored intentionally */ }
}

int main(int argc, char **argv) {
    double duration = 5.0;
    if (argc >= 2) duration = atof(argv[1]);
    if (duration <= 0) duration = 5.0;

    printf("HDGL-SQL v1 — Direct Library Benchmark\n");
    printf("Platform: Linux, i7-6700T, WSL2\n");
    printf("Store dir: %s\n", BENCH_STORE_DIR);
    printf("Phase duration: %.0f seconds each\n\n", duration);

    rmdir_store();

    zchg_store_t store;
    if (zchg_store_open(&store, BENCH_STORE_DIR, BENCH_SECRET, BENCH_SECRET_LEN) != 0) {
        fprintf(stderr, "store open failed\n");
        return 1;
    }

    /* Pre-populate one record per strand so GET phase has live data */
    for (int i = 0; i < BENCH_KEYS; i++) {
        zchg_store_put(&store, KEYS[i], "bench", NULL,
                       PAYLOADS[i], strlen(PAYLOADS[i]));
    }
    zchg_store_flush(&store);

    printf("Results\n");
    printf("-------\n");

    /* ------------------------------------------------------------------ */
    /* Phase 1: PUT                                                         */
    /* ------------------------------------------------------------------ */
    {
        double t0 = now_sec(), t1;
        uint64_t ops = 0;
        int slot = 0;
        while ((t1 = now_sec()) - t0 < duration) {
            zchg_store_put(&store, KEYS[slot], "bench", NULL,
                           PAYLOADS[slot], strlen(PAYLOADS[slot]));
            slot = (slot + 1) & 7;
            ops++;
        }
        zchg_store_flush(&store);
        print_result("PUT (signed append)", ops, t1 - t0);
    }

    /* ------------------------------------------------------------------ */
    /* Phase 2: GET                                                         */
    /* ------------------------------------------------------------------ */
    {
        double t0 = now_sec(), t1;
        uint64_t ops = 0;
        int slot = 0;
        while ((t1 = now_sec()) - t0 < duration) {
            volatile zchg_store_record_t *r = zchg_store_get(&store, KEYS[slot]);
            (void)r;
            slot = (slot + 1) & 7;
            ops++;
        }
        print_result("GET (phi-tau index)", ops, t1 - t0);
    }

    /* ------------------------------------------------------------------ */
    /* Phase 3: SCAN                                                        */
    /* ------------------------------------------------------------------ */
    {
        double t0 = now_sec(), t1;
        uint64_t ops = 0;
        while ((t1 = now_sec()) - t0 < duration) {
            zchg_store_scan(&store, NULL, NULL);
            ops++;
        }
        print_result("SCAN (full index walk)", ops, t1 - t0);
    }

    /* ------------------------------------------------------------------ */
    /* Phase 4: Strand signals                                              */
    /* ------------------------------------------------------------------ */
    {
        zchg_strand_signal_t sigs[8];
        double t0 = now_sec(), t1;
        uint64_t ops = 0;
        while ((t1 = now_sec()) - t0 < duration) {
            zchg_store_strand_signals(&store, sigs);
            ops++;
        }
        print_result("STRAND SIGNALS (EMA)", ops, t1 - t0);
    }

    zchg_store_flush(&store);
    zchg_store_close(&store);
    rmdir_store();

    printf("\nNotes:\n");
    printf("  All numbers are direct C API calls — zero network/HTTP overhead.\n");
    printf("  PUT bounded by HMAC-SHA256 per frame + disk append (O_WRONLY|O_APPEND).\n");
    printf("  GET bounded by in-memory open-address Fibonacci hash lookup only.\n");
    printf("  SCAN bounded by index capacity (%d slots).\n", ZCHG_STORE_INDEX_CAP);
    printf("  Single-threaded (library has no internal locking).\n");

    return 0;
}
