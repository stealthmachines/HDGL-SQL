/*
 * zchg_store.h  —  HDGL-sql: Strand-Native Persistent Store
 *
 * This is not a wrapper around SQLite.
 * SQLite was the inspiration; HDGL-sql is the evolution.
 *
 * Architecture
 * ────────────
 * • 8 append-only binary strand files on disk, one per geometric strand:
 *       strand_0_point.hdgl  …  strand_7_octacube.hdgl
 *
 * • Every record on disk is a zchg_frame_t — the same binary frame type
 *   used for transport, gossip, and fileswap.  Storage and network speak
 *   the same language.
 *
 * • Addressing is phi-tau geometric, not integer.  The phi-tau hash of the
 *   logical key (zchg_compute_phi_tau) determines both which strand file the
 *   record lives in AND its unique address within the lattice.
 *   phi_addr is stored in the frame header's authority_ep + source_ip fields
 *   (low 32 / high 32 bits respectively).
 *
 * • authority_w is an analog EMA signal, not a timestamp.  It rises toward
 *   1.0 as a record is written and decays on cold records.  It is stored in
 *   the frame's reserved field as a 24.8 fixed-point integer.
 *
 * • All frames are HMAC-SHA256 signed (same key as the cluster secret).
 *   A frame whose HMAC fails verification during boot-scan is skipped —
 *   the log self-heals from prior good frames.
 *
 * • Records are append-only.  Overwrites append a new frame with updated
 *   payload and EMA weight; history is preserved in the strand file.
 *   On startup the store scans each strand file sequentially, EMA-reducing
 *   all frames per phi_addr into an in-memory lattice index.
 *
 * Strand file layout (per file)
 * ─────────────────────────────
 *   [sizeof(zchg_frame_header_t) bytes header][payload_len bytes payload]
 *   [sizeof(zchg_frame_header_t) bytes header][payload_len bytes payload]
 *   …  (never overwritten, append-only)
 *
 * Payload encoding inside each frame
 * ────────────────────────────────────
 *   [uint8_t type_len][type_len bytes record_type]
 *   [uint8_t ref_len] [ref_len bytes lattice_ref hex addr, or 0 if root]
 *   [remaining bytes: raw JSON payload]
 */

#ifndef ZCHG_STORE_H
#define ZCHG_STORE_H

#include "zchg_core.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Constants
 * ============================================================================ */

/* New frame type for store records */
#define ZCHG_FRAME_STORE            0x08

/* Strand file names (lowercase to match STRAND_NAMES) */
#define ZCHG_STORE_FILE_0           "strand_0_point.hdgl"
#define ZCHG_STORE_FILE_1           "strand_1_line.hdgl"
#define ZCHG_STORE_FILE_2           "strand_2_triangle.hdgl"
#define ZCHG_STORE_FILE_3           "strand_3_tetrahedron.hdgl"
#define ZCHG_STORE_FILE_4           "strand_4_pentachoron.hdgl"
#define ZCHG_STORE_FILE_5           "strand_5_hexacross.hdgl"
#define ZCHG_STORE_FILE_6           "strand_6_heptacube.hdgl"
#define ZCHG_STORE_FILE_7           "strand_7_octacube.hdgl"

/* In-memory index capacity (must be power of 2) */
#define ZCHG_STORE_INDEX_CAP        4096

/* EMA alpha — mirrors zchg_lattice.c ZCHG_EMA_ALPHA */
#define ZCHG_STORE_EMA_ALPHA        0.3

/* Max record_type length (excluding null) */
#define ZCHG_STORE_TYPE_MAX         31

/* lattice_ref is a 16-char hex phi_addr + null */
#define ZCHG_STORE_REF_LEN          17

/* ============================================================================
 * Record (in-memory representation of the latest frame for a phi_addr)
 * ============================================================================ */

typedef struct zchg_store_record {
    uint64_t    phi_addr;                       /* Geometric address (phi-tau hash) */
    uint8_t     strand_id;                      /* Strand (0-7) */
    char        record_type[ZCHG_STORE_TYPE_MAX + 1];
    char        lattice_ref[ZCHG_STORE_REF_LEN];/* Parent phi_addr hex, or "" */
    double      authority_w;                    /* EMA authority signal [0.0, 1.0] */
    char       *payload;                        /* Heap-allocated JSON payload */
    size_t      payload_len;
    uint64_t    last_ts;                        /* Milliseconds since epoch */
    struct zchg_store_record *_next;            /* Open-addressing chain */
} zchg_store_record_t;

/* ============================================================================
 * Per-Strand Analog Signal
 * ============================================================================ */

typedef struct {
    uint8_t     strand_id;
    char        strand_name[32];
    double      authority_w;        /* EMA of write activity on this strand */
    uint64_t    record_count;       /* Unique phi_addrs in this strand */
    uint64_t    frame_count;        /* Total frames appended (inc. history) */
} zchg_strand_signal_t;

/* ============================================================================
 * Store
 * ============================================================================ */

typedef struct {
    char                    store_dir[512];
    int                     strand_fd[8];               /* open(O_RDWR|O_CREAT|O_APPEND) */
    zchg_store_record_t    *index[ZCHG_STORE_INDEX_CAP];/* phi_addr hash table */
    zchg_strand_signal_t    strands[8];
    const char             *cluster_secret;
    size_t                  secret_len;
    uint64_t                total_puts;
    uint64_t                total_gets;
} zchg_store_t;

/* ============================================================================
 * API
 * ============================================================================ */

/*
 * zchg_store_open — initialise store, create strand dirs, boot-scan existing
 *                   strand files to rebuild in-memory lattice index.
 * Returns 0 on success, -1 on error.
 */
int  zchg_store_open(zchg_store_t *store,
                     const char   *store_dir,
                     const char   *cluster_secret,
                     size_t        secret_len);

/*
 * zchg_store_close — flush and close all strand file descriptors, free index.
 */
void zchg_store_close(zchg_store_t *store);

/*
 * zchg_store_flush — flush the coalescing write buffer for all 8 strands to
 * disk immediately.  Called automatically by zchg_store_close().  The HTTP
 * handler calls this after each PUT response so single-request durability is
 * preserved.  Raise WBUF_FLUSH_COUNT in zchg_store.c to batch N frames per
 * flush for higher write throughput at the cost of a small durability window.
 */
int  zchg_store_flush(zchg_store_t *store);

/*
 * zchg_store_put — write a record into the lattice.
 *   key         : logical identifier (e.g. "session:abc123")
 *   record_type : category tag (e.g. "session", "player", "home")
 *   lattice_ref : parent phi_addr as 16-char hex string, or NULL
 *   payload     : raw JSON bytes
 *   payload_len : length of payload
 * The phi-tau address and strand are computed from key.
 * Returns 0 on success.
 */
int  zchg_store_put(zchg_store_t *store,
                    const char   *key,
                    const char   *record_type,
                    const char   *lattice_ref,
                    const char   *payload,
                    size_t        payload_len);

/*
 * zchg_store_get — O(1) lookup of latest record by logical key.
 * Returns pointer to in-memory record (do NOT free), or NULL if not found.
 */
zchg_store_record_t* zchg_store_get(zchg_store_t *store, const char *key);

/*
 * zchg_store_get_by_addr — lookup by phi_addr directly (hex string or uint64).
 */
zchg_store_record_t* zchg_store_get_by_addr(zchg_store_t *store, uint64_t phi_addr);

/*
 * zchg_store_scan — iterate every live record in the lattice index.
 * Callback receives each record and user data pointer.
 */
int  zchg_store_scan(zchg_store_t *store,
                     void (*cb)(zchg_store_record_t *rec, void *user),
                     void *user);

/*
 * zchg_store_scan_type — iterate records matching a record_type.
 */
int  zchg_store_scan_type(zchg_store_t *store,
                           const char   *record_type,
                           void (*cb)(zchg_store_record_t *rec, void *user),
                           void *user);

/*
 * zchg_store_scan_ref — iterate records whose lattice_ref matches a phi_addr.
 * Used to load children of a parent record.
 */
int  zchg_store_scan_ref(zchg_store_t *store,
                          uint64_t      parent_phi_addr,
                          void (*cb)(zchg_store_record_t *rec, void *user),
                          void *user);

/*
 * zchg_store_strand_signals — fill out[8] with per-strand EMA signals.
 */
void zchg_store_strand_signals(zchg_store_t *store, zchg_strand_signal_t out[8]);

/*
 * zchg_store_phi_addr — compute phi-tau address for a logical key.
 * Useful for building lattice_ref values without calling put.
 */
uint64_t zchg_store_phi_addr(const char *key);

#ifdef __cplusplus
}
#endif

#endif /* ZCHG_STORE_H */
