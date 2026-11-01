# Log format, version 1

The log is the only truth (principle 2). Everything else is derived and disposable. ADR 0003 (global seq), ADR 0006 (framing).

## Layout under `$KE_DATA`

```
world.edn                              {:world/id #uuid :format-version 1 :created-at #inst}
root.token                             bearer credential of the root agent; mode 0600; written once by kernel-zero
log/seg-00000000000000000001.klog      segment; name = first seq it holds, 20 digits
log/seg-00000000000000000001.klog.idx  sidecar index; a cache, rebuilt by scan when missing or stale
blobs/sha256/ab/abcd...<64 hex>        content-addressed bytes; bytes Fields above 4 KiB live here
blobs/tmp/                             in-flight blob writes (tmp, fsync, rename)
snapshots/snap-00000000000000012000.snap   Datascript snapshot tagged with the seq it covers; deletable
```

## Segment

16-byte header: `"KLOG"` (4 bytes) · u16 version = 1 · u16 flags = 0 · u64 first-seq. Then frames until EOF:

```
[u32 payload-len][u32 crc32(payload)][payload]
```

payload = Transit-JSON of one Event map (spec 4.4). Roll to a new segment at 64 MiB.

## Seq

Assigned by the single writer at append: `head + 1`. One total order per world. `event/stream` is a partition key; an event that belongs on several streams is written once with `event/streams` listing them. Event id is a random uuid when the appender gives none.

## Writer

One thread. `append!` enqueues `[pending-event promise]`. The writer drains what is queued (at most 1000), validates each against `:ke/PendingEvent`, assigns seq and id, encodes, writes one buffer, calls `force(false)` once per batch (`KE_FSYNC=batch`; `always` forces per event; `never` for tests), delivers seqs to the promises, then notifies subscribers with the batch. Group commit.

## Recovery

On open, scan the last segment. A short read or a CRC mismatch at the tail truncates to the last good frame (a crash mid-batch). A CRC mismatch anywhere else refuses to start; the file is damaged and a person decides.

## Sidecar index

Transit-JSON: `{:first-seq :last-seq :count :streams {stream-id [[seq offset] ...]} :sparse [[seq offset] every 256th]}`. Written on roll and on clean close. Never authoritative.

## Blobs

`put!` writes to `blobs/tmp/<uuid>`, fsyncs, renames to the sha256 path. A Field whose bytes exceed 4 KiB carries `{:blob/sha256 .. :blob/size ..}` in the log. Blobs are immutable; retention retires the Field, never the blob, until a sweep that nothing references.

## Snapshot

`{:seq N :schema-version 1 :datascript <d/serializable db>}` as Transit-JSON, tmp then rename, on graceful stop and every 10k events. Boot loads the newest parseable snapshot, then replays `read-from (inc N)`. Deleting every snapshot costs only startup time.

## Forks (sprint item W9)

A world directory may be a fork of another. `world.edn` then reads

```
{:world/id #uuid "<the parent's id, shared>" :format-version 1 :created-at #inst
 :fork/id #uuid "<this fork>"
 :parent {:data-dir "/abs/or/relative/parent" :world #uuid "<same id>" :seq N}}
```

The child holds no copy of the parent's events. Its log reader walks the parent's segments read-only, capped at seq `N` (the parent may keep running and growing; the child never sees past `N` and never writes, indexes or truncates under the parent), then the child's own segments, whose first frame is `N+1`. Blobs resolve child-then-parent; writes go to the child. The child's `snapshots/` starts with a copy of the parent's newest snapshot at or below `N`, a cache like any snapshot. On its first boot the child mints its own tokens (hashes on its log) and sets `world/parent`, `world/forked-at` and `world/fork-id` on the world Principal; kernel-zero does not run again (the log is not empty), the later steps do.

`scripts/world-fork.sh <parent> <child> [--at N]` makes one. `history {since-fork true}` (and the action `world.diff`) lists the child's own events with a summary. `scripts/world-promote.sh <child> <parent>` appends them to a stopped parent whose head is still `N`, each with `event/from-fork` (the fork id) and `event/fork-seq` (its seq in the child); a parent that moved refuses (no three-way merge in v1).

## Export and import

`scripts/world-export.sh <data-dir> <archive.tgz>` writes `world.edn`, `log/`, `blobs/`, `snapshots/` and a `manifest.edn` (`:world/id :head :created-at :code` and `:files {path sha256}`) to a gzipped tar. Token files and temp files are never included. Stop the world first; `--live` copies the active segment as it stands, and the first boot of the import cuts a torn tail as recovery always does. `scripts/world-import.sh <archive.tgz> <data-dir>` extracts into an empty directory and verifies every hash; the first boot re-mints tokens for every Principal carrying `principal/token-hash` and writes them under the data dir.
