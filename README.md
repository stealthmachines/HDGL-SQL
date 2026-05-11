# HDGL-SQL v1

HDGL-SQL is an embeddable storage engine — a compiled library (`libhdglsql.a` / `libhdglsql.so`) you link directly into your application, similar in deployment model to SQLite but with a fundamentally different storage architecture.

It stores HMAC-signed binary frames in 8 geometric strand files, with deterministic key routing by phi-tau hash and EMA-based analog authority signaling. No SQL, no tables, no rows — records are addressed by geometric hash and queried by key, type, or lattice ancestry.

## SQLite Compatibility

HDGL-SQL is **not** compatible with SQLite. It does not implement the SQLite C API, does not use the SQLite file format, and has no concept of tables, rows, or SQL query syntax. The name "HDGL-SQL" uses "SQL" as an analogy for structured, queryable storage — not as a compatibility claim. If you need a drop-in SQLite replacement you are looking for the wrong library.

HDGL-SQL exists in a different category: no schema, no rows, no integer primary keys. Records are addressed by phi-tau geometric hash, stored in append-only strand logs, and queried by key, type, or lattice ancestry.

## What This Repository Is

This repository is the canonical HDGL-SQL storage engine — a self-contained library that builds to `libhdglsql.a` (static) or `libhdglsql.so` (shared) using the included Makefile.

Source files (6):

- include/zchg_core.h — frame protocol, frame header struct, HMAC declarations
- include/zchg_lattice.h — phi-spiral routing and EMA API declarations
- include/zchg_store.h — full store API, record types, strand signal types
- src/zchg_frame.c — frame serialization, HMAC-SHA256 sign/verify implementation
- src/zchg_lattice.c — phi-tau hash, strand routing, EMA, provisioner pipeline
- src/zchg_store.c — strand files, append-only write path, boot-scan, in-memory lattice index

This repository intentionally does not include the daemon, HTTP front door, benchmark tools, or application runtime.

## Building

Requires: GCC or Clang, OpenSSL development headers (`libssl-dev` / `openssl-devel`).

```bash
# Static library (default)
make

# Shared library
make shared

# Smoke test (builds static lib + compiles + runs a minimal open/put/get/close)
make test

# Clean
make clean
```

Outputs: `libhdglsql.a` (static) or `libhdglsql.so` (shared) in the repo root.

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

**Option A — link the prebuilt library (recommended):**

```bash
# In this repo:
make          # produces libhdglsql.a

# In your project:
cc -O3 -I/path/to/hdgl-sql/include your_app.c \
    -L/path/to/hdgl-sql -lhdglsql -lcrypto -lm -o your_app
```

**Option B — compile sources directly into your build:**

1. Add `include/` to your compiler include paths.
2. Compile `src/zchg_frame.c`, `src/zchg_lattice.c`, and `src/zchg_store.c` and link them into your host app.
3. Link against OpenSSL (`-lcrypto`) and math (`-lm`).

```bash
cc -O3 -Iinclude -c src/zchg_frame.c   -o zchg_frame.o
cc -O3 -Iinclude -c src/zchg_lattice.c -o zchg_lattice.o
cc -O3 -Iinclude -c src/zchg_store.c   -o zchg_store.o
cc -O3 -Iinclude your_app.c zchg_frame.o zchg_lattice.o zchg_store.o -lcrypto -lm -o your_app
```

Provide a persistent directory path for the store (e.g. `./hdgl_store`).

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

Benchmarked on an i7-6700T (4 cores / 8 threads, 2.80 GHz), WSL2. Run `make bench` to reproduce.

### Direct library throughput (`make bench`)

Zero network overhead. All numbers are single-threaded C API calls in a tight loop.

| Operation | Throughput | Notes |
|-----------|-----------|-------|
| PUT (signed append) | **170,517 req/s** | HMAC-SHA256 per frame + disk append |
| GET (phi-tau index) | **15,631,031 req/s** | O(1) in-memory Fibonacci hash index |
| SCAN (full index walk) | **32,276,406 req/s** | Iterates all live records in index |
| STRAND SIGNALS (EMA) | **27,639,382 req/s** | Reads per-strand EMA authority state |

### Through HTTP daemon (200 concurrent, 10s run)

Numbers when HDGL-SQL sits behind the full HTTP/epoll daemon (TCP + syscall overhead included). This is the deployment measured during the original MUD session-store workload.

| Operation | Throughput | Notes |
|-----------|-----------|-------|
| GET (key lookup) | ~82,000 req/s | Includes TCP round-trip, epoll dispatch |
| PUT (signed append) | ~4,000 req/s | Disk write + HMAC + HTTP overhead |
| Strand signal read | ~57,000 req/s | HTTP round-trip to EMA state |
| Error rate | 0 | 10-second run, 200 concurrent connections |

### HDGL-SQL vs SQLite C API (WAL) — apples-to-apples

Both benchmarks run on the same hardware, same phase duration, direct C API, no Python, no GIL, no HTTP. SQLite 3.37.2, WAL mode, `PRAGMA synchronous=NORMAL`, 64 MB page cache. HDGL-SQL with `WBUF_FLUSH_COUNT=1` (one `write()` per frame, same durability). Run with `make bench-sqlite` vs `make bench`.

| Operation | SQLite 3.37.2 (C API, WAL) | HDGL-SQL v1 (C API) | Delta |
|-----------|---------------------------|---------------------|-------|
| PUT / INSERT | 267,009 req/s | **171,620 req/s** | SQLite 1.6x faster |
| GET / SELECT PK | 201,851 req/s | **15,550,133 req/s** | **HDGL 77x faster** |
| SCAN / SELECT * | 186,750 req/s | **32,879,426 req/s** | **HDGL 176x faster** |
| Signals / GROUP BY | 114,019 req/s | **27,514,841 req/s** | **HDGL 241x faster** |

PUT is the one phase where SQLite has the edge — its WAL append is cheaper than HMAC-SHA256 per frame. That is the intentional tradeoff for cryptographic strand integrity. Raise `WBUF_FLUSH_COUNT` in `src/zchg_store.c` to coalesce N frames per flush and push PUT past 500K req/s at the cost of a small durability window.

GET is where the architectures diverge: SQLite must descend a B-tree to resolve a primary key. HDGL-SQL resolves a key by a single Fibonacci hash into a flat open-address array — O(1) with no tree traversal. The 77x gap is structural.

### Prior Python-gateway SQLite comparison (for historical context)

The original comparison was measured against the MUD session-store workload. SQLite numbers were Python `sqlite3` under GIL contention — included here because they explain why HDGL-SQL was built.

| Operation | SQLite WAL (Python, GIL) | HDGL-SQL (HTTP daemon) |
|-----------|--------------------------|------------------------|
| Baseline no-op read | 403,062 req/s | 83,286 req/s |
| Write (PUT / upsert) | 10,015 req/s | ~4,000 req/s |
| Read by key | **18 req/s** | **85,421 req/s** |
| Aggregate / scan | **114 req/s** | ~57,000 req/s |

SQLite's keyed read collapsed to 18 req/s under concurrent load because the Python GIL serializes every `SELECT` through a single connection. HDGL-SQL's phi-tau index has no locking on reads.

## Migration from SQLite

`hdgl_from_sqlite` imports any SQLite table into HDGL-SQL binary strand files in a single pass. Build it with:

```sh
make migrate
```

Usage:

```
./hdgl_from_sqlite <sqlite_path> <table> <key_col> <hdgl_store_dir> [secret]
```

| Argument | Description |
|----------|-------------|
| `sqlite_path` | Path to the `.db` / `.sqlite3` file |
| `table` | Table name to read |
| `key_col` | Column to use as the HDGL-SQL phi-address key |
| `hdgl_store_dir` | Output directory for the 8 strand binary files |
| `secret` | Optional HMAC signing secret (default: empty string) |

Every row is serialized to a JSON payload containing all columns, signed with HMAC-SHA256, and appended to the appropriate strand file. If the table has columns named `record_type` or `lattice_ref`, they are used verbatim; otherwise the values default to `"row"` and `""`.

**Example** — import the frontierland state table:

```sh
./hdgl_from_sqlite frontierland.db hdgl_lattice phi_addr ./hdgl_store mykey
```

After import, use the standard `zchg_store_get()` / `zchg_store_scan()` API to query. The store is byte-compatible with any other HDGL-SQL producer — migration is destructive only in the sense that the source SQLite file is never written to.

## Repository Purpose

This repo exists to publish and preserve the HDGL-SQL storage standard implementation in isolation from unrelated runtime modules.

## License

See LICENSE.
