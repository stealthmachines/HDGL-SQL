# HDGL-SQL v1

HDGL-SQL is a strand-native persistent store — a fundamentally different storage model from relational databases.

It stores HMAC-signed binary frames in 8 geometric strand files, with deterministic key routing by phi-tau hash and EMA-based analog authority signaling.

## SQLite Compatibility

HDGL-SQL is **not** compatible with SQLite. It does not implement the SQLite C API, does not use the SQLite file format, and has no concept of tables, rows, or SQL query syntax. The name "HDGL-SQL" uses "SQL" as an analogy for structured, queryable storage — not as a compatibility claim. If you need a drop-in SQLite replacement you are looking for the wrong library.

HDGL-SQL exists in a different category: no schema, no rows, no integer primary keys. Records are addressed by phi-tau geometric hash, stored in append-only strand logs, and queried by key, type, or lattice ancestry.

## What This Repository Is

This repository is the canonical HDGL-SQL source set (6 files):

- include/zchg_core.h — frame protocol, frame header struct, HMAC declarations
- include/zchg_lattice.h — phi-spiral routing and EMA API declarations
- include/zchg_store.h — full store API, record types, strand signal types
- src/zchg_frame.c — frame serialization, HMAC-SHA256 sign/verify implementation
- src/zchg_lattice.c — phi-tau hash, strand routing, EMA, provisioner pipeline
- src/zchg_store.c — strand files, append-only write path, boot-scan, in-memory lattice index

This repository intentionally does not include the daemon, HTTP front door, benchmark tools, or application runtime.

## Mental Model

HDGL-SQL persists records as append-only frame history.

- Key -> phi_addr:
	A logical key (for example "session:abc123") is hashed with `zchg_compute_phi_tau` to a 64-bit phi address.
- phi_addr -> strand:
	`zchg_phi_tau_to_strand` maps that address to one of 8 strands.
- Write path:
	PUT appends a new signed frame to that strand file (history is preserved, no in-place overwrite).
- Read path:
	GET resolves the key to phi_addr and returns the latest in-memory reduced record.
- Boot recovery:
	On startup, each strand file is scanned sequentially and reduced into an in-memory index.

## On-Disk Layout

Each strand is one append-only file:

- strand_0_point.hdgl
- strand_1_line.hdgl
- strand_2_triangle.hdgl
- strand_3_tetrahedron.hdgl
- strand_4_pentachoron.hdgl
- strand_5_hexacross.hdgl
- strand_6_heptacube.hdgl
- strand_7_octacube.hdgl

Every frame is serialized as:

- [zchg_frame_header_t]
- [payload bytes]

Frame payload encoding used by the store:

- [uint8 type_len]
- [type bytes]
- [uint8 ref_len]
- [ref bytes]
- [json payload bytes]

`ref` is normally a 16-char hex parent phi address for lattice ancestry.

## Authority Signal

Each record and strand carries analog authority `authority_w` in range [0, 1].

- On write: EMA rises toward 1.0.
- On replay/merge: latest frame contributes to reduced authority state.

The value is encoded into frame header `reserved` as 24.8 fixed-point.

## Security Model

Frames are HMAC-SHA256 signed with the cluster secret.

- On write: frame is signed.
- On boot scan: invalid signatures are skipped.

This allows log replay to self-heal to the last valid history.

## Public API

Primary API from `include/zchg_store.h`:

- `zchg_store_open` - open/create store dir, open strand files, boot-scan index
- `zchg_store_close` - flush pending writes, close descriptors, free index
- `zchg_store_flush` - force buffered frames to disk
- `zchg_store_put` - append new record version for key
- `zchg_store_get` - latest record by key
- `zchg_store_get_by_addr` - latest record by phi address
- `zchg_store_scan` - iterate all live records
- `zchg_store_scan_type` - iterate by record_type
- `zchg_store_scan_ref` - iterate children by lattice_ref
- `zchg_store_strand_signals` - read per-strand authority and counts
- `zchg_store_phi_addr` - utility key -> phi address

## How To Integrate

This repository is a source module set, not a complete standalone executable.

1. Add `include/` to your compiler include paths.
2. Compile `src/zchg_frame.c`, `src/zchg_lattice.c`, and `src/zchg_store.c` and link them into your host app.
3. Link against OpenSSL (`-lcrypto`) and math (`-lm`). HMAC-SHA256 frame signing requires OpenSSL.
4. Provide a persistent directory path for the store (for example `./hdgl_store`).

Example compile line (Linux/macOS):

```bash
cc -O3 -Iinclude -c src/zchg_frame.c   -o zchg_frame.o
cc -O3 -Iinclude -c src/zchg_lattice.c -o zchg_lattice.o
cc -O3 -Iinclude -c src/zchg_store.c   -o zchg_store.o
cc -O3 -Iinclude your_app.c zchg_frame.o zchg_lattice.o zchg_store.o -lcrypto -lm -o your_app
```

## Minimal Usage Example

```c
#include "zchg_store.h"
#include <stdio.h>
#include <string.h>

int main(void) {
		zchg_store_t store;

		const char *secret = "hdgl-demo-secret";
		if (zchg_store_open(&store, "./hdgl_store", secret, strlen(secret)) != 0) {
				fprintf(stderr, "store open failed\n");
				return 1;
		}

		const char *key = "session:abc123";
		const char *json = "{\"user\":\"alice\",\"score\":42}";

		if (zchg_store_put(&store, key, "session", NULL, json, strlen(json)) != 0) {
				fprintf(stderr, "store put failed\n");
				zchg_store_close(&store);
				return 1;
		}

		zchg_store_record_t *rec = zchg_store_get(&store, key);
		if (rec) {
				printf("phi=%016llx strand=%u type=%s authority=%.3f payload=%s\n",
							 (unsigned long long)rec->phi_addr,
							 rec->strand_id,
							 rec->record_type,
							 rec->authority_w,
							 rec->payload ? rec->payload : "");
		}

		zchg_store_flush(&store);
		zchg_store_close(&store);
		return 0;
}
```

## Operational Notes

- Append-only semantics preserve history naturally. Records are never overwritten in-place.
- In-memory index capacity is fixed at 4096 live phi addresses (`ZCHG_STORE_INDEX_CAP` in `zchg_store.h`). Exceeding this causes inserts to fail silently. Raise the constant and recompile for larger working sets.
- `WBUF_FLUSH_COUNT` in `src/zchg_store.c` controls write batching. Default is 1 (every frame flushed via `writev` immediately). Raise to N to coalesce N frames per flush for higher write throughput at the cost of up to N frames of durability window.
- Single-threaded access assumptions apply unless your host app adds external synchronization.
- Boot-scan time is proportional to total frames on disk across all 8 strand files.

## Performance

Numbers measured on an i7-6700T (4 cores / 8 threads, 2.80 GHz) running the full ZCHG daemon with HTTP overhead included. Store-layer throughput in isolation will be higher.

| Operation | Throughput | Notes |
|-----------|-----------|-------|
| GET (key lookup) | ~82,000 req/s | O(1) in-memory Fibonacci hash index |
| PUT (signed append) | ~3,700 req/s | Disk write + HMAC-SHA256 per frame |
| Strand signal read | ~57,000 req/s | Reads per-strand EMA state |
| Error rate | 0 | Measured over 10-second run, 200 concurrent |

PUT throughput is bounded by disk I/O and HMAC signing. GET throughput is bounded by the in-memory hash table and is effectively CPU-only. On NVMe storage or with `WBUF_FLUSH_COUNT` > 1 the PUT rate scales significantly higher.

## Repository Purpose

This repo exists to publish and preserve the HDGL-SQL storage standard implementation in isolation from unrelated runtime modules.

## License

See LICENSE.
