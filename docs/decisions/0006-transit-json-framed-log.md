# ADR 0006: Transit-JSON framed log

Status: accepted, 2026-10-29

## Context
The log needs deterministic tail recovery and native bytes/instant/uuid.
## Decision
Segment files seg-<first-seq>.klog: 16-byte header (KLOG, version, flags, first-seq) then frames [u32 len][u32 crc32][Transit-JSON event]. Roll at 64 MiB. Sidecar .idx is a cache. Bytes above 4 KiB go to a sha256 blob store.
## Consequences
A truncated tail is discarded on open; a bad frame elsewhere refuses to start. Switching to Transit-msgpack later is frame-level only.
