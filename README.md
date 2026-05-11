# HDGL-SQL v1

Strand-native persistent store standard extracted from the ZCHG codebase.

## Scope

This repository intentionally contains only the HDGL-SQL standard source set:

- include/zchg_core.h
- include/zchg_lattice.h
- include/zchg_store.h
- src/zchg_lattice.c
- src/zchg_store.c

No frontierland content, daemon runtime, HTTP layer, or benchmark tooling is included.

## Architecture Summary

HDGL-SQL implements:

- 8 append-only binary strand files
- Phi-tau geometric strand addressing
- EMA authority signaling across strands
- HMAC-SHA256 signed frame records
- Append-only log semantics

## Notes

This is a standards-focused extraction intended to preserve the canonical HDGL-SQL store and lattice implementation without unrelated modules.

## License

See LICENSE.
