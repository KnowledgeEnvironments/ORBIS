# Log format, version 1

The log is the only truth (principle 2). Everything else is derived and disposable. ADR 0003 (global seq), ADR 0006 (framing). A world over its retention limit keeps the truth of its present, not all of its past: see [Retention](#retention), which is an exception to this paragraph and says exactly how far it goes.

## Layout under `$KE_DATA`

```
world.edn                              {:world/id #uuid :format-version 1 :created-at #inst}
root.token                             bearer credential of the root agent; mode 0600; written once by kernel-zero
log/seg-00000000000000000001.klog      segment; name = first seq it holds, 20 digits
log/seg-00000000000000000001.klog.idx  sidecar index; a cache, rebuilt by scan when missing or stale
blobs/sha256/ab/abcd...<64 hex>        content-addressed bytes; bytes Fields above 4 KiB live here
blobs/tmp/                             in-flight blob writes (tmp, fsync, rename)
snapshots/snap-00000000000000012000.snap   Datascript snapshot tagged with the seq it covers; deletable on a log that starts at seq 1
anchor/snap-00000000000000012000.snap      the snapshot retention verified before dropping segments; never deletable
forks/<fork id>.edn                    a note that a fork reads this log in place up to a seq
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

`put!` writes a temp file beside the sha256 path, fsyncs, renames. A Field whose bytes exceed 4 KiB carries `{:blob/sha256 .. :blob/size ..}` in the log. Blobs are immutable. Their mtime is their last use, for retention: `get` moves it at most once an hour, so a read does not become a write, and `put!` of bytes already there always moves it.

## Snapshot

`{:seq N :schema-version 1 :datascript <d/serializable db>}` as Transit-JSON, tmp then rename, on graceful stop and every 10k events. Boot loads the newest parseable snapshot of this release's format, then replays `read-from (inc N)`. While the log starts at seq 1, deleting every snapshot costs only startup time. Once retention has dropped a segment it does not: see below.

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

`scripts/world-export.sh <data-dir> <archive.tgz>` writes `world.edn`, `log/`, `blobs/`, `snapshots/`, `anchor/` and a `manifest.edn` (`:world/id :head :created-at :code` and `:files {path sha256}`) to a gzipped tar. Token files and temp files are never included. Stop the world first; `--live` copies the active segment as it stands, and the first boot of the import cuts a torn tail as recovery always does. `scripts/world-import.sh <archive.tgz> <data-dir>` extracts into an empty directory and verifies every hash; the first boot re-mints tokens for every Principal carrying `principal/token-hash` and writes them under the data dir.

## Retention

`scripts/backup-world.sh <data>` copies a world's log, snapshots, anchor and blobs to `/path/to/backups/worlds/<name>`, at most once a day, keeping three. The supervisor runs it at each start. A pruned world can depend on `anchor/` alone, so this is what stands behind it.

A world has one limit on its blobs and its log segments with their `.idx` together: 16 GiB, or `KE_RETENTION_GB`, until a person sets another with `retention.set {gb}`, kept in `retention/limit-gb` on the world. `gb` may be `"infinite"`, which turns this section off. No agent can change it: `retention/*` Fields refuse every write but the action's, and the action wants a person's own session. Snapshots, `anchor/`, `releases/` and `logs/` are outside the limit.

A sweep runs at start and every 15 minutes, on a thread of its own: it never runs on the writer, holds no lock over its work, and appends only as any Principal does.

**Blobs go first, least recently used first**, down to 90 percent of the limit. A blob used in the last hour stays, since it may be on its way to the log. These stay whatever their age, because something live needs them:

- the current value of a Field on a live Object (superseded values, a retired Object's and a closed pane's do not count)
- images on Messages of a conversation with a Message in the last 14 days
- an open terminal's newest 16 MiB of output
- what an open editor's fold starts from

The whole point is what does go: `serialization/bytes` and `serialization/ids`, every prompt ever sent, and everything superseded. The receipt stays on the log and the Field stays on the Object, so `serialization/hash` and `serialization/size` are still there.

**Log segments go only when that is not enough**, oldest first, down to the limit. A segment goes only if all of this holds:

1. It is closed. The active segment never goes.
2. Its last seq is at or below the anchor. The anchor is the newest snapshot that has been read whole by this release, every datom walked, with its seq and the world's id checked, then copied to `anchor/`, forced to disk and renamed before anything is deleted. It lives outside `snapshots/` because `prune!` keeps only the newest three there and the supervisor moves aside what a failed release wrote.
3. The running release is `release.good`. A release that could still be rolled back may evict blobs but drops no segment: the release it would return to may not read what this one wrote.
4. No living fork reads it in place (`forks/`). A note whose child has gone is removed.
5. This world is not itself a fork: a fork keeps its own log whole, because `world.diff` and `world-promote` read it.

Before segments go, the sweep moves up what live panes hold only in events: a `pane-edit` of verb `checkpoint` for an open editor, whose blob is the whole buffer with its undo, and `pane/record` `{n chain offset}` for an open terminal. Deleted oldest first, the frames before the index, the floor moved in the state first, so a crash leaves at worst an index with no segment. A hole in the middle of the log is damage, not retention, and refuses to start.

`retention/last-sweep` on the world says what the last sweep that did anything took: `{bytes count blobs segments releases logs-rotated first-seq used limit-gb at}`, with `over`, `pinned` and `blocked` when it could not get under the limit and why. A sweep with nothing to do writes nothing.

Whatever the limit, `logs/*.log` over 64 MiB is copied to `.1`, older copies shifted, two kept, and truncated: copied because the supervisor holds these open for append, so a line written between the copy and the truncate is lost. Under `releases/` the sweep keeps what `release`, `release.good`, a pending `restart.request` and the running process name, and the newest three. `src/` is never touched.

### What a pruned world no longer has

The log was the whole history of the world. On a pruned world it is the history since the first segment, and the snapshot is the only record of what came before. Principle 2 changes as follows, and this is a real change, not a detail:

- **The present is intact.** Every Object, Field, name and Grant the fold held at the snapshot is there. Nothing the world knows now is lost.
- **There is no fold from seq 1.** Ground rule 3 said anything derived can be deleted and rebuilt. On a pruned world the snapshot at or past the first segment is not derived from anything that still exists. Lose it and the world does not start: `anchor/` must be backed up with the log, and an export carries it.
- **The past below the first seq is gone.** Who set a Field to what and when, for events below `first-seq`: `history`, `history {stream}`, `read {seq}` on an Object older than that, `field.diff`, and the audit trail of `field-read`, `attached` and the rest. What a Field used to hold, an old terminal's output, and old edits of a buffer go with it. Only a Field's current `field/author`, `field/set-at` and `field/set-seq` remain.
- **A prompt's bytes go earlier, and without touching the log.** What a Model was sent can no longer be read back, only its hash and size: a record that a prompt existed and could be recognised, not what it said.
- **A new fold cannot be applied to the past.** A release that folds an event differently changes only what comes after. Its schema is applied to the snapshot's datoms on load, but a fact that was never folded cannot be derived.
- **A snapshot format change needs a reader for the old one.** `schema-version` is read strictly. A release that moves it refuses to boot on a pruned world, and on a new release the supervisor returns to the last good one, so nothing is lost, but the upgrade does not happen until the release can read the old format.

Set `"infinite"` before any of this if the history matters more than the disk. A pruned segment does not come back from anywhere but a backup.

### Reading what has gone

A blob that has gone reads as nil from `blob/get`, as it always could. `read` keeps the receipt and marks it `pruned true` under `object/blobs`; a `range` or `byte-range` of it, `image.read` and `pane.read` of bytes answer `err pruned`. A prompt referring to an image gets a line saying it is no longer stored. A replayed terminal shows `[older output pruned]` where output is missing. A rendered formula compiles again.

Readers of the log never hang and never fail on a gap, and never mistake one for filtered events. `log/gap` is how a reader that must not skip asks. The index asks, and refuses; so does boot:

| Reader | Below the first seq |
|---|---|
| Boot | Needs a snapshot this release loads at or past `first-seq - 1`, from `snapshots/` or `anchor/`. Without one it stops with `:log/unrecoverable`, naming what it tried. It never folds what is left |
| Index catch-up | Throws `:log/gap` rather than skip |
| `tools/fold-at`, `world.diff`, promote | Throw: they need what is gone |
| `history`, `history {stream}` | What is held, plus `gap` |
| `history {last before}` | Stops at the first seq; no `prev` past it, and `gap` |
| `subscribe {after}` | `gap` in the answer, and on the sub's first push, empty if need be; the cursor moves up once |
| `attach {subs}` | `gaps {sub first-seq}` in the answer, then as `subscribe` |
| `read {seq}` | `err pruned` on an Object older than the first seq |
| `field.diff` | `gap` beside what it found |
| `object.restore` | Restores, with `names-pruned true` |
| `hello` | Says `first-seq` |
| `world-fork.sh` | Refuses a seq below the first, or one no snapshot reaches |

Forks read a parent's blobs through the parent's store, so an old blob a fork relies on can go with the parent's sweep; it reads as gone. Forks made before `forks/` existed left no note.

