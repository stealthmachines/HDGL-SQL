/*
 * zchg_store.c  —  HDGL-sql: Strand-Native Persistent Store
 *
 * Every record is a zchg_frame_t on disk.
 * phi-tau hash = geometric address.  EMA = analog authority signal.
 * No tables, no integer keys.  The lattice IS the database.
 */

#define _POSIX_C_SOURCE 200809L

#include "zchg_store.h"
#include "zchg_lattice.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

/* Per-strand coalescing write buffer.
 * Frames are packed here until WBUF_FLUSH_COUNT is reached or
 * zchg_store_flush() is called explicitly.  The HTTP handler calls flush
 * after every PUT so single-request latency is unchanged; the gain comes
 * from the kernel seeing one writev() instead of two write() calls.
 * Raising WBUF_FLUSH_COUNT to e.g. 16 amortises the VFS overhead further
 * at the cost of up to 16 frames of durability window. */
#define WBUF_FLUSH_COUNT  1      /* 1 = writev only (no extra buffering) */
#define WBUF_MAX_BYTES    (65536 * 4)  /* 256 KiB safety cap per strand */

typedef struct {
    uint8_t  *data;
    size_t    len;
    size_t    cap;
    int       count;   /* frames staged */
} _strand_wbuf_t;

static _strand_wbuf_t _wbuf[8];  /* one per strand, zero-initialised */

static int _wbuf_flush(int strand_fd, _strand_wbuf_t *wb) {
    if (wb->len == 0) return 0;
    if (lseek(strand_fd, 0, SEEK_END) < 0) return -1;
    ssize_t nw = write(strand_fd, wb->data, wb->len);
    int ok = (nw == (ssize_t)wb->len) ? 0 : -1;
    wb->len   = 0;
    wb->count = 0;
    return ok;
}

static int _wbuf_append(int strand, int strand_fd,
                         const void *hdr, size_t hdr_len,
                         const void *pay, size_t pay_len) {
    _strand_wbuf_t *wb = &_wbuf[strand];
    size_t need = hdr_len + pay_len;

    /* Grow buffer if needed */
    if (wb->len + need > wb->cap) {
        size_t new_cap = wb->cap ? wb->cap * 2 : 4096;
        while (new_cap < wb->len + need) new_cap *= 2;
        if (new_cap > WBUF_MAX_BYTES) {
            /* Safety: flush first */
            if (_wbuf_flush(strand_fd, wb) != 0) return -1;
            new_cap = need * 2;
        }
        uint8_t *p = (uint8_t *)realloc(wb->data, new_cap);
        if (!p) return -1;
        wb->data = p;
        wb->cap  = new_cap;
    }

    memcpy(wb->data + wb->len, hdr, hdr_len);
    wb->len += hdr_len;
    memcpy(wb->data + wb->len, pay, pay_len);
    wb->len += pay_len;
    wb->count++;

    if (wb->count >= WBUF_FLUSH_COUNT)
        return _wbuf_flush(strand_fd, wb);
    return 0;
}

/* ============================================================================
 * Internal constants
 * ============================================================================ */

static const char *_STRAND_FILE_NAMES[8] = {
    ZCHG_STORE_FILE_0, ZCHG_STORE_FILE_1, ZCHG_STORE_FILE_2,
    ZCHG_STORE_FILE_3, ZCHG_STORE_FILE_4, ZCHG_STORE_FILE_5,
    ZCHG_STORE_FILE_6, ZCHG_STORE_FILE_7,
};

static const char *_STRAND_NAMES[8] = {
    "Point", "Line", "Triangle", "Tetrahedron",
    "Pentachoron", "Hexacross", "Heptacube", "Octacube",
};

/* EMA authority_w is stored in frame.header.reserved as 24.8 fixed-point
 * (integer part in upper 24 bits, fractional in lower 8 bits → range 0–1
 * encoded as 0–256). */
#define _W_TO_FP(w)   ((uint32_t)((w) * 256.0))
#define _FP_TO_W(fp)  ((double)(fp) / 256.0)

/* phi_addr split: low 32 bits → authority_ep, high 32 bits → source_ip */
#define _ADDR_LO(a)   ((uint32_t)((a) & 0xFFFFFFFFULL))
#define _ADDR_HI(a)   ((uint32_t)(((a) >> 32) & 0xFFFFFFFFULL))
#define _ADDR_JOIN(lo, hi) (((uint64_t)(hi) << 32) | (uint64_t)(lo))

/* ============================================================================
 * Hash table helpers
 * ============================================================================ */

static uint32_t _slot(uint64_t phi_addr) {
    /* Fibonacci hashing — mix bits, reduce to table index */
    uint64_t h = phi_addr * 0x9e3779b97f4a7c15ULL;
    return (uint32_t)((h >> 32) & (ZCHG_STORE_INDEX_CAP - 1));
}

static zchg_store_record_t **_index_slot(zchg_store_t *store, uint64_t phi_addr) {
    uint32_t s = _slot(phi_addr);
    /* Linear probe */
    for (uint32_t i = 0; i < ZCHG_STORE_INDEX_CAP; i++) {
        uint32_t idx = (s + i) & (ZCHG_STORE_INDEX_CAP - 1);
        if (store->index[idx] == NULL)
            return &store->index[idx];               /* empty slot */
        if (store->index[idx]->phi_addr == phi_addr)
            return &store->index[idx];               /* existing record */
    }
    return NULL;  /* table full — should not happen at normal load */
}

static zchg_store_record_t *_index_find(zchg_store_t *store, uint64_t phi_addr) {
    uint32_t s = _slot(phi_addr);
    for (uint32_t i = 0; i < ZCHG_STORE_INDEX_CAP; i++) {
        uint32_t idx = (s + i) & (ZCHG_STORE_INDEX_CAP - 1);
        if (store->index[idx] == NULL) return NULL;
        if (store->index[idx]->phi_addr == phi_addr) return store->index[idx];
    }
    return NULL;
}

/* ============================================================================
 * Payload encoding / decoding
 * ============================================================================ *
 *  Layout: [type_len:1][type:N][ref_len:1][ref:M][json payload:rest]
 */

static uint8_t *_encode_payload(const char *record_type,
                                 const char *lattice_ref,
                                 const char *json, size_t json_len,
                                 size_t *out_len)
{
    size_t type_len = record_type ? strlen(record_type) : 0;
    if (type_len > ZCHG_STORE_TYPE_MAX) type_len = ZCHG_STORE_TYPE_MAX;
    size_t ref_len  = lattice_ref ? strlen(lattice_ref) : 0;
    if (ref_len > 16) ref_len = 16;
    size_t total    = 1 + type_len + 1 + ref_len + json_len;

    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) return NULL;

    uint8_t *p = buf;
    *p++ = (uint8_t)type_len;
    if (type_len) { memcpy(p, record_type, type_len); p += type_len; }
    *p++ = (uint8_t)ref_len;
    if (ref_len)  { memcpy(p, lattice_ref, ref_len);  p += ref_len; }
    if (json_len) { memcpy(p, json, json_len); }

    *out_len = total;
    return buf;
}

static int _decode_payload(const uint8_t *buf, size_t buf_len,
                            char *out_type,  /* ZCHG_STORE_TYPE_MAX+1 */
                            char *out_ref,   /* ZCHG_STORE_REF_LEN    */
                            char **out_json, size_t *out_json_len)
{
    if (!buf || buf_len < 2) return -1;
    const uint8_t *p = buf;
    const uint8_t *end = buf + buf_len;

    uint8_t type_len = *p++;
    if (p + type_len >= end) return -1;
    memset(out_type, 0, ZCHG_STORE_TYPE_MAX + 1);
    if (type_len) {
        size_t copy = type_len <= ZCHG_STORE_TYPE_MAX ? type_len : ZCHG_STORE_TYPE_MAX;
        memcpy(out_type, p, copy);
    }
    p += type_len;

    uint8_t ref_len = *p++;
    if (p + ref_len > end) return -1;
    memset(out_ref, 0, ZCHG_STORE_REF_LEN);
    if (ref_len) {
        size_t copy = ref_len <= 16 ? ref_len : 16;
        memcpy(out_ref, p, copy);
    }
    p += ref_len;

    size_t json_len = (size_t)(end - p);
    char *json = NULL;
    if (json_len > 0) {
        json = (char *)malloc(json_len + 1);
        if (!json) return -1;
        memcpy(json, p, json_len);
        json[json_len] = '\0';
    } else {
        json = (char *)calloc(1, 1);
    }
    *out_json     = json;
    *out_json_len = json_len;
    return 0;
}

/* ============================================================================
 * Millisecond timestamp
 * ============================================================================ */

static uint64_t _now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

/* ============================================================================
 * Boot scan — read strand file, reconstruct lattice index
 * ============================================================================ */

static void _boot_scan_strand(zchg_store_t *store, uint8_t strand_id, int fd) {
    /* Seek to beginning for read pass */
    if (lseek(fd, 0, SEEK_SET) < 0) return;

    size_t hdr_size = sizeof(zchg_frame_header_t);
    uint8_t hdr_buf[sizeof(zchg_frame_header_t)];

    uint64_t frame_count = 0;

    for (;;) {
        ssize_t nr = read(fd, hdr_buf, hdr_size);
        if (nr == 0) break;                 /* EOF */
        if (nr != (ssize_t)hdr_size) break; /* truncated — stop */

        zchg_frame_header_t hdr;
        memcpy(&hdr, hdr_buf, hdr_size);

        /* Sanity: only accept STORE frames */
        if (hdr.version != zchg_FRAME_VERSION || hdr.type != ZCHG_FRAME_STORE) {
            /* Skip past claimed payload and continue */
            if (hdr.payload_len > 0 && hdr.payload_len < zchg_FRAME_MAX_PAYLOAD)
                lseek(fd, (off_t)hdr.payload_len, SEEK_CUR);
            continue;
        }

        if (hdr.payload_len == 0 || hdr.payload_len > zchg_FRAME_MAX_PAYLOAD) {
            /* Skip malformed frame */
            continue;
        }

        uint8_t *payload_buf = (uint8_t *)malloc(hdr.payload_len);
        if (!payload_buf) break;

        nr = read(fd, payload_buf, hdr.payload_len);
        if (nr != (ssize_t)hdr.payload_len) { free(payload_buf); break; }

        /* Verify HMAC */
        zchg_frame_t frame;
        memset(&frame, 0, sizeof(frame));
        frame.header     = hdr;
        frame.payload     = payload_buf;
        frame.payload_len = hdr.payload_len;

        if (store->cluster_secret && store->secret_len > 0) {
            if (zchg_hmac_verify_frame(&frame, store->cluster_secret, store->secret_len) != 0) {
                /* Bad signature — skip silently; log self-heals */
                free(payload_buf);
                continue;
            }
        }

        frame_count++;

        /* Decode payload */
        char rec_type[ZCHG_STORE_TYPE_MAX + 1];
        char rec_ref[ZCHG_STORE_REF_LEN];
        char *rec_json = NULL;
        size_t rec_json_len = 0;

        if (_decode_payload(payload_buf, hdr.payload_len,
                            rec_type, rec_ref, &rec_json, &rec_json_len) != 0) {
            free(payload_buf);
            continue;
        }
        free(payload_buf);

        /* Reconstruct phi_addr from the two split uint32 fields */
        uint64_t phi_addr  = ((uint64_t)hdr.source_ip << 32) | (uint64_t)hdr.authority_ep;
        double   authority = _FP_TO_W(hdr.reserved);

        /* Upsert in-memory index */
        zchg_store_record_t **slot = _index_slot(store, phi_addr);
        if (!slot) { free(rec_json); continue; }

        if (*slot == NULL) {
            /* New record */
            *slot = (zchg_store_record_t *)calloc(1, sizeof(zchg_store_record_t));
            if (!*slot) { free(rec_json); continue; }
            (*slot)->phi_addr  = phi_addr;
            (*slot)->strand_id = strand_id;
            store->strands[strand_id].record_count++;
        } else {
            /* Existing — free old payload, EMA-update weight */
            if ((*slot)->payload) free((*slot)->payload);
            /* EMA: newer frame's authority_w is authoritative signal */
            authority = ZCHG_STORE_EMA_ALPHA * 1.0
                        + (1.0 - ZCHG_STORE_EMA_ALPHA) * (*slot)->authority_w;
        }

        memcpy((*slot)->record_type, rec_type, ZCHG_STORE_TYPE_MAX + 1);
        memcpy((*slot)->lattice_ref, rec_ref,  ZCHG_STORE_REF_LEN);
        (*slot)->authority_w  = authority;
        (*slot)->payload      = rec_json;
        (*slot)->payload_len  = rec_json_len;
        (*slot)->last_ts      = hdr.timestamp;
    }

    store->strands[strand_id].frame_count += frame_count;
}

/* ============================================================================
 * zchg_store_open
 * ============================================================================ */

int zchg_store_open(zchg_store_t *store,
                    const char   *store_dir,
                    const char   *cluster_secret,
                    size_t        secret_len)
{
    if (!store || !store_dir) return -1;
    memset(store, 0, sizeof(*store));

    strncpy(store->store_dir, store_dir, sizeof(store->store_dir) - 1);
    store->cluster_secret = cluster_secret;
    store->secret_len     = secret_len;

    /* Initialise strand signal metadata */
    for (int i = 0; i < 8; i++) {
        store->strands[i].strand_id = (uint8_t)i;
        strncpy(store->strands[i].strand_name, _STRAND_NAMES[i],
                sizeof(store->strands[i].strand_name) - 1);
    }

    /* Create store directory if necessary */
    struct stat st;
    if (stat(store_dir, &st) != 0) {
        if (mkdir(store_dir, 0755) != 0 && errno != EEXIST) {
            fprintf(stderr, "[HDGL-sql] Cannot create store dir %s: %s\n",
                    store_dir, strerror(errno));
            return -1;
        }
    }

    /* Open all 8 strand files */
    for (int s = 0; s < 8; s++) {
        char path[600];
        snprintf(path, sizeof(path), "%s/%s", store_dir, _STRAND_FILE_NAMES[s]);

        int fd = open(path, O_RDWR | O_CREAT, 0644);
        if (fd < 0) {
            fprintf(stderr, "[HDGL-sql] Cannot open strand file %s: %s\n",
                    path, strerror(errno));
            /* Close already-opened fds */
            for (int j = 0; j < s; j++) close(store->strand_fd[j]);
            return -1;
        }
        store->strand_fd[s] = fd;
    }

    /* Boot scan — rebuild in-memory lattice from on-disk frames */
    for (int s = 0; s < 8; s++) {
        _boot_scan_strand(store, (uint8_t)s, store->strand_fd[s]);
    }

    /* Print strand lattice on boot */
    fprintf(stderr, "[HDGL-sql] Lattice store: %s\n", store_dir);
    fprintf(stderr, "[HDGL-sql] Strand lattice: ");
    for (int s = 0; s < 8; s++) {
        fprintf(stderr, "%s(%llu|w=%.3f)%s",
                _STRAND_NAMES[s],
                (unsigned long long)store->strands[s].record_count,
                store->strands[s].authority_w,
                s < 7 ? "  " : "\n");
    }

    return 0;
}

/* ============================================================================
 * zchg_store_close
 * ============================================================================ */

void zchg_store_close(zchg_store_t *store) {
    if (!store) return;

    /* Flush any buffered frames before closing file descriptors */
    for (int s = 0; s < 8; s++) {
        if (store->strand_fd[s] > 0)
            _wbuf_flush(store->strand_fd[s], &_wbuf[s]);
    }

    for (int s = 0; s < 8; s++) {
        if (_wbuf[s].data) { free(_wbuf[s].data); _wbuf[s].data = NULL; }
        if (store->strand_fd[s] > 0) {
            close(store->strand_fd[s]);
            store->strand_fd[s] = 0;
        }
    }

    /* Free index records */
    for (int i = 0; i < ZCHG_STORE_INDEX_CAP; i++) {
        if (store->index[i]) {
            if (store->index[i]->payload) free(store->index[i]->payload);
            free(store->index[i]);
            store->index[i] = NULL;
        }
    }
}

/* ============================================================================
 * zchg_store_flush
 * ============================================================================ */

int zchg_store_flush(zchg_store_t *store) {
    if (!store) return -1;
    int rc = 0;
    for (int s = 0; s < 8; s++) {
        if (store->strand_fd[s] > 0)
            if (_wbuf_flush(store->strand_fd[s], &_wbuf[s]) != 0) rc = -1;
    }
    return rc;
}

/* ============================================================================
 * zchg_store_put
 * ============================================================================ */

int zchg_store_put(zchg_store_t *store,
                   const char   *key,
                   const char   *record_type,
                   const char   *lattice_ref,
                   const char   *payload,
                   size_t        payload_len)
{
    if (!store || !key || !payload) return -1;

    uint64_t phi_addr = zchg_compute_phi_tau(key, strlen(key));
    uint8_t  strand   = zchg_phi_tau_to_strand(phi_addr);
    int      fd       = store->strand_fd[strand];

    /* Look up existing authority_w for EMA continuation */
    zchg_store_record_t *existing = _index_find(store, phi_addr);
    double prev_w = existing ? existing->authority_w : 0.0;
    double new_w  = prev_w < 0.001
                    ? 1.0
                    : ZCHG_STORE_EMA_ALPHA * 1.0 + (1.0 - ZCHG_STORE_EMA_ALPHA) * prev_w;

    /* Encode payload: [type_len][type][ref_len][ref][json] */
    size_t  enc_len = 0;
    uint8_t *enc    = _encode_payload(record_type, lattice_ref,
                                       payload, payload_len, &enc_len);
    if (!enc) return -1;

    /* Build frame */
    zchg_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.header.version     = zchg_FRAME_VERSION;
    frame.header.type        = ZCHG_FRAME_STORE;
    frame.header.strand_id   = strand;
    frame.header.reserved    = _W_TO_FP(new_w);
    frame.header.authority_ep = _ADDR_LO(phi_addr);
    frame.header.source_ip   = _ADDR_HI(phi_addr);
    frame.header.payload_len  = (uint32_t)enc_len;
    frame.header.timestamp   = _now_ms();
    frame.payload            = enc;
    frame.payload_len        = enc_len;

    /* HMAC sign */
    if (store->cluster_secret && store->secret_len > 0) {
        zchg_hmac_sign_frame(&frame, store->cluster_secret, store->secret_len);
    }

    /* Append to strand file via coalescing buffer (single writev path) */
    int rc = _wbuf_append(strand, fd,
                           &frame.header, sizeof(frame.header),
                           enc, enc_len);
    free(enc);
    if (rc != 0) return -1;

    /* Update in-memory index */
    zchg_store_record_t **slot = _index_slot(store, phi_addr);
    if (!slot) return -1;

    if (*slot == NULL) {
        *slot = (zchg_store_record_t *)calloc(1, sizeof(zchg_store_record_t));
        if (!*slot) return -1;
        (*slot)->phi_addr  = phi_addr;
        (*slot)->strand_id = strand;
        store->strands[strand].record_count++;
    } else {
        if ((*slot)->payload) free((*slot)->payload);
    }

    memset((*slot)->record_type, 0, sizeof((*slot)->record_type));
    memset((*slot)->lattice_ref, 0, sizeof((*slot)->lattice_ref));
    if (record_type)
        strncpy((*slot)->record_type, record_type, ZCHG_STORE_TYPE_MAX);
    if (lattice_ref)
        strncpy((*slot)->lattice_ref, lattice_ref, 16);

    (*slot)->authority_w = new_w;
    (*slot)->payload     = (char *)malloc(payload_len + 1);
    if ((*slot)->payload) {
        memcpy((*slot)->payload, payload, payload_len);
        (*slot)->payload[payload_len] = '\0';
    }
    (*slot)->payload_len = payload_len;
    (*slot)->last_ts     = frame.header.timestamp;

    /* Update strand EMA signal */
    zchg_strand_signal_t *sig = &store->strands[strand];
    sig->authority_w = ZCHG_STORE_EMA_ALPHA * 1.0
                       + (1.0 - ZCHG_STORE_EMA_ALPHA) * sig->authority_w;
    sig->frame_count++;

    store->total_puts++;
    return 0;
}

/* ============================================================================
 * zchg_store_get
 * ============================================================================ */

zchg_store_record_t* zchg_store_get(zchg_store_t *store, const char *key) {
    if (!store || !key) return NULL;
    uint64_t phi_addr = zchg_compute_phi_tau(key, strlen(key));
    store->total_gets++;
    return _index_find(store, phi_addr);
}

zchg_store_record_t* zchg_store_get_by_addr(zchg_store_t *store, uint64_t phi_addr) {
    if (!store) return NULL;
    store->total_gets++;
    return _index_find(store, phi_addr);
}

/* ============================================================================
 * zchg_store_phi_addr
 * ============================================================================ */

uint64_t zchg_store_phi_addr(const char *key) {
    if (!key) return 0;
    return zchg_compute_phi_tau(key, strlen(key));
}

/* ============================================================================
 * Scan helpers
 * ============================================================================ */

int zchg_store_scan(zchg_store_t *store,
                    void (*cb)(zchg_store_record_t *, void *),
                    void *user)
{
    if (!store || !cb) return -1;
    for (int i = 0; i < ZCHG_STORE_INDEX_CAP; i++) {
        if (store->index[i]) cb(store->index[i], user);
    }
    return 0;
}

int zchg_store_scan_type(zchg_store_t *store,
                          const char *record_type,
                          void (*cb)(zchg_store_record_t *, void *),
                          void *user)
{
    if (!store || !cb || !record_type) return -1;
    for (int i = 0; i < ZCHG_STORE_INDEX_CAP; i++) {
        if (store->index[i] &&
            strncmp(store->index[i]->record_type, record_type, ZCHG_STORE_TYPE_MAX) == 0) {
            cb(store->index[i], user);
        }
    }
    return 0;
}

int zchg_store_scan_ref(zchg_store_t *store,
                         uint64_t      parent_phi_addr,
                         void (*cb)(zchg_store_record_t *, void *),
                         void *user)
{
    if (!store || !cb) return -1;
    char ref_hex[17];
    snprintf(ref_hex, sizeof(ref_hex), "%016llx",
             (unsigned long long)parent_phi_addr);
    for (int i = 0; i < ZCHG_STORE_INDEX_CAP; i++) {
        if (store->index[i] &&
            strncmp(store->index[i]->lattice_ref, ref_hex, 16) == 0) {
            cb(store->index[i], user);
        }
    }
    return 0;
}

void zchg_store_strand_signals(zchg_store_t *store, zchg_strand_signal_t out[8]) {
    if (!store) return;
    memcpy(out, store->strands, 8 * sizeof(zchg_strand_signal_t));
}
