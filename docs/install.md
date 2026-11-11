# Install

ORBIS is two programs: a world, a Clojure process that is the record and the agent harness, and a
listener, a native client that draws what the world sends. One machine runs a world; any machine on
the network runs a listener against it. `scripts/install.sh` does everything on this page for the
common case, on macOS; the steps are here so you can do any of them by hand.

## The short way

```
git clone <the repository> orbis && cd orbis
scripts/install.sh
```

It checks the toolchain and offers to install what is missing with Homebrew, asks for the Model root
thinks with and writes `.envrc`, resolves the Clojure dependencies, builds the listener, makes a world
in `data/`, starts it under its supervisor through Terminal.app, and opens the listener on your token.
`scripts/install.sh --check` only reports the toolchain. `--data <dir>`, `--port`, `--name` place and
name the world; `--no-listener` leaves the window closed.

On a Mac the world should be started through Terminal.app, as the script does: the JVM needs macOS
Local Network permission to reach a Model on another machine, and that permission belongs to the app
that launched it. A world started from an editor's terminal may see "No route to host" where `curl`
works.

## Toolchain

| What | Why | Homebrew |
|---|---|---|
| Xcode command line tools | clang for the listener | `xcode-select --install` |
| OpenJDK 25 | the world | `brew install openjdk@25` |
| Clojure CLI | the world | `brew install clojure/tools/clojure` |
| CMake 3.28 or later, Ninja | the listener | `brew install cmake ninja` |
| SDL3, FreeType | the listener; optional: without them the build fetches and builds both from source | `brew install sdl3 freetype` |
| Zig | optional, the terminal holder (`make holder`, exact version in `holder/PIN`): terminals survive a world restart | |
| tectonic | optional, TeX rendering in chat | `brew install tectonic` |

Without Homebrew, everything user-local under `~/opt` works too: `~/opt/env.sh` exporting `JAVA_HOME`
and `PATH` is sourced by every script when `JAVA_HOME` is unset (the README's vantage layout).

## By hand

1. **The Model.** Copy `.env.example` to `.envrc` (git-ignored, keep it `0600`) and set `KE_MODEL_URL`,
   `KE_MODEL_ID` and `KE_MODEL_KEY` for any OpenAI-compatible chat endpoint. With no URL the world boots
   with a stub Model, and you give root a real one afterwards: Sign in with ChatGPT from the listener's
   model picker (plan usage, no key), or `model.create` with an endpoint and the name of the environment
   variable that holds its key.
2. **Dependencies and build.**
   ```
   make deps
   cd listener && cmake --preset macos-debug && cmake --build --preset macos-debug     # Homebrew SDL3 and FreeType
   cd listener && cmake --preset macos-source && cmake --build --preset macos-source   # or from source, no Homebrew
   ```
   The binary is `listener/build/<preset>/listener`.
3. **A world.** A data directory holds the log, the blobs, the tokens, the releases and the working copy
   root edits. `scripts/world.sh <dir>` makes the first release of this checkout if there is none and
   runs the world under its supervisor; it reads `<dir>/world.env`, a file of exports:
   ```
   export KE_WORLD_NAME=home
   export KE_PORT=7777
   export KE_BIND=0.0.0.0                       # only for listeners on other machines; the default is 127.0.0.1
   export KE_PUBLIC_URL=ws://<this host>:7777/ws
   export KE_MODEL_URL=...   KE_MODEL_ID=...   KE_MODEL_KEY=...
   ```
   On a Mac: `open -a Terminal <dir>/start.sh`, a two-line file that execs `scripts/world.sh <dir>`
   (`install.sh` writes it; Terminal.app passes a script no arguments). Closing that window does not
   stop the world. `scripts/supervise.sh --stop <dir>` does.
4. **The listener.**
   ```
   listener/build/macos-debug/listener --url ws://127.0.0.1:7777/ws --token-file data/person.token
   ```
   `data/person.token` is written at the world's first boot: you, the first person, holding every op in
   the world. `data/root.token` is root's, the operator's key; leave it where it is.

A plain world without a supervisor also runs: `scripts/run-world.sh` (or `make world`), with
`scripts/go.sh` to start it and open the listener in one go. It cannot change its own kernel; the
supervised one can (`docs/self-hosting.md`).

## A listener on another machine

Build the listener there (steps 2 above, or copy a built binary of the same architecture), copy the
token once, and dial the world:

```
scp host:orbis/data/person.token ~/orbis/home.token && chmod 600 ~/orbis/home.token
listener --url ws://host:7777/ws --token-file ~/orbis/home.token --data-dir ~/orbis/state
```

`--data-dir` is where the listener keeps its own session and preferences (default: Application
Support). The world must be reachable on its port: on the host, `KE_PUBLIC_URL` in `world.env` is what
clients are told to dial. The connection is a plain WebSocket on the LAN; TLS for the open internet is
on the punch list.

A phone pairs instead of carrying a token file: `ios/` and `listener/platform/android/` build the same
client.

## Where things are

```
<data>/
  release, release.good      the running commit and the last one that stayed up
  releases/<commit>/         read-only trees of each release; `harness` in the world points here
  src/                       root's working copy, branch work; `src` in the world
  log/ blobs/ snapshots/     the record
  person.token root.token    yours and root's, 0600
  credentials/               Sign in with ChatGPT tokens, never Fields
  logs/world.log, supervise.log
  world.env, start.sh        how this world starts
```

## Upgrading

A supervised world upgrades itself: root edits `src/`, builds and tests, commits, `release.make`, and
asks `world.restart`, which is always your own card to approve; a release that fails its health check
comes back to the last good one by itself. By hand, from the host:

```
git -C <data>/src pull          # or merge what you want
scripts/release.sh --data <data> --repo <data>/src --rev HEAD
```

then `world.restart {release}` from a session, or stop and start the supervisor. The log only moves
forward: an older release that cannot read the log is refused.

## When something does not start

`./start.sh` at the top of the checkout starts the world if it is down (after a reboot, a crash of the supervisor), `./start.sh --restart` stops and starts it (after a change to `data/world.env`), and either way it prints the state: the release and the last good one, the supervisor's last line, the address clients dial, your token, and the devices paired. `./start.sh --status` only prints. `./start.sh --fresh` makes a new, empty world and moves the old `data/` to `data.trashed-<time>` (nothing is deleted; move it back to recover), keeping the Model and port from its `world.env`; it asks before moving anything.

- **The world does not answer on its port.** `tail <data>/logs/world.log`. A first start resolves
  dependencies and needs the network; a port in use is named there.
- **"No route to host" to the Model, while `curl` works.** Local Network permission: start the world
  through Terminal.app (`open -a Terminal <data>/start.sh`), and allow it when macOS asks.
- **`clojure` finds no Java.** `JAVA_HOME` is unset and Java is not at `/opt/homebrew/opt/openjdk@25`:
  export it, or put it in `~/opt/env.sh`.
- **CMake mixes MacPorts and Homebrew.** The presets ignore `/opt/local`; use them rather than a bare
  `cmake`. `/opt/local/bin/curl` may not reach the LAN: the scripts use `/usr/bin/curl`.
- **The listener opens with no world.** Check `--url` and that the token file is the world's
  `person.token`; the title bar says which world it is attached to.
- **The kernel swap card never comes.** `world.restart` onto a new release is always a person's own
  card; it is in the APPROVALS menu of your listener, or `task.approve` from any person session.
