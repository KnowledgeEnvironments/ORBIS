# Self-hosting

A world carries the source it runs, and its root can change that source and move the world onto the
change without losing the agent doing it.

## On disk

```
<data>/
  release              the commit running now
  release.good         the last commit that passed health and stayed up
  releases/<commit>/   an exact tree of that commit plus release.edn, read-only
  src/                 the working copy, where edits happen; a clone of the repository
  restart.request      world to supervisor: release, by, reason
  restart.outcome      supervisor to world: result, from, to, reason
  logs/supervise.log   every start, health result, restart and rollback; rotated by size
  anchor/              the snapshot a pruned log stands on (`docs/log-format.md` Retention)
  world.env            optional, 0600: the Model key, sourced by the supervisor
  bin/holder           the terminal holder, copied in by release.sh when one is built
  holder.sock          its socket, 0600; a data dir with a long path uses a short one under /tmp
  holder.pid           the running holder
```

`harness`, the read-only mount, points at the running release. `shell.local` runs in `src/`, and a
developer Principal's mount is a subtree of `src/`. `world/release` on the world says what is
running, and any session can read it.

## Starting

```
scripts/release.sh --data <dir>            # first release, the working copy
KE_PORT=7777 scripts/supervise.sh <dir>    # the parent of the world
scripts/supervise.sh --stop <dir>          # or scripts/world-stop.sh <dir>
```

The supervisor also keeps the terminal holder running, with a bare environment, so terminals go on through every restart of the world (`docs/panes.md`). It starts another if one dies. A full stop ends it. The holder is a native binary built outside the tree with `make holder`, so `release.sh` copies it into `bin/` rather than the read-only release. A release made without one keeps the holder already there, and a running holder is untouched until a full stop. An older holder that speaks another protocol version is not used: the world says so in its log and runs new terminals itself, so a change of `holder/src/main.c` version needs a full stop.

`scripts/supervise.sh` is bash with nothing to install, so it runs anywhere the world does. Closing the
Terminal that started it does not stop it. It does not survive a reboot: nothing here installs itself.

Deleting a world: `scripts/supervise.sh --stop <dir>`, then `chmod -R u+w <dir>/releases` first, because a release is read-only.

## Changing ORBIS

1. Edit in the mount `src`, then build and test there.
2. Commit, then `release.make {rev?}` makes a release and answers its commit. A confined shell cannot write
   `releases/`, so from a shell inside the world this is the only way; `scripts/release.sh` is for a person.
3. `world.restart {release, reason}` asks for it.

A release is good once it has passed the health check, an attach and a read of the log head, and stayed
up `KE_SETTLE_S` seconds. Anything else on a new release returns to `release.good` by itself. The
snapshots the failed release wrote are moved aside, and the world that comes up tells the person with a
status Message and an urgent notification. A crash on a good release restarts with backoff. A clean
stop stays stopped.

## What it takes

| Restart | From a loop | A person's own session |
|---|---|---|
| Same release | An ordinary gated op: a card, or a rule or unsafe mode | Runs |
| New release | Always a person's own card | Runs |

No rule, no unsafe mode and no batch approval admits a new release, because whoever swaps the kernel can
remove every gate in `docs/authority.md`. `world/release` and `world/restart` are the kernel's: no
`set-field` writes them, so a forged Field cannot make a new kernel look like a plain restart.

A world started without a supervisor refuses with `err unsupervised`: nothing would bring it back.

## An agent survives its own restart

The wake that asked is unfinished when the process goes, so `loop/resume.clj` brings it back and it
carries on. If it asks again, `world.restart` answers `already` instead of restarting, because the
world it wanted is the one answering.

## The log only moves forward

Each release's manifest carries the log format it reads. An older one is refused by `world.restart` and
again by the supervisor, which reads the format from the newest segment. A rollback keeps every event
the failed release appended: events are data, and the older world has to read them. A release that
wrote something the older world cannot fold is not recoverable by rollback.

## Retention and restarts

A sweep can drop log segments, and a world that has must boot from a snapshot. So a self-hosting world relies on three things, each tested: a release that is not yet `release.good` drops no segment, so nothing it wrote holds the world up when it fails; `anchor/` sits outside what a rollback moves aside; and a release that cannot load any snapshot on a pruned log exits non-zero instead of folding what is left, which is an ordinary failed start and returns to `release.good`. On the good release itself the same exit is a crash loop with its reason in `logs/world.log`, and a person decides.

Old releases are removed by the sweep. It keeps `release`, `release.good`, one a restart request names, the running one and the newest three. A release is read-only, and removing one goes through the same `chmod` as deleting a world.

## Limits

- A release is read-only by file mode, against mistakes. An unconfined shell runs as the same OS user
  and could change it; see `docs/authority.md`.
- The card names a commit. What is in that commit is for the person to read in `src/`.
- Dependencies are resolved when a release first starts, so a release with new ones needs the network.
