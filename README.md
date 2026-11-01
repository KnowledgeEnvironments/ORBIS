# ORBIS

A shared knowledge environment for doing work with people, computers, and artificial intelligence.

ORBIS is the On-line Relative Basis Information System. It is shared here with invited friends, to
play with. It is not open source: see [License](#license).

## What it is

One graph where people, agents, and their office appliances share names, a record, and a room.

- The **world** is a persistent Clojure process and also the agent harness. Its truth is an
  append-only log. Everything in it is a named Object: Principals, Grants, Routes, Models, Adapters.
- The **listener** is the native client: C++20, SDL3, Dear ImGui. It runs on macOS, and builds for
  iOS and Android.
- The **holder** keeps terminals alive across client restarts, on libghostty-vt.
- A Principal's commands run in a confinement box: `sandbox-exec` on macOS, `bwrap` on Linux.

The ideas are in `docs/ethos.md`, the wire in `docs/protocol.md`, who
may do what in `docs/authority.md`. The same documents are built into a running world as its manual.

## Requirements

Developed and tested on Apple Silicon macOS only.

- World: OpenJDK 25 and the Clojure CLI.
- Listener: Xcode's clang, CMake 3.28 or later, Ninja, `sdl3`, `freetype` (Homebrew).
- Holder: Zig 0.17.0, needed for terminals and for `make test`. `make holder` clones and builds
  libghostty-vt.
- A model: any OpenAI-compatible chat endpoint.
- Optional: `tectonic` for TeX rendering.

```
brew install openjdk@25 clojure/tools/clojure cmake ninja sdl3 freetype zig
```

## Build

```
make deps        # Clojure dependencies
make listener    # the client; fonts and libraries are fetched at configure
make holder      # the terminal holder
make test        # schema, unit, exit and kernel-zero tests; run after make holder
```

## Run a world

```
cp .env.example .envrc      # then set the model URL, id and key
scripts/run-world.sh
```

`scripts/run-world.sh --fresh` wipes that world's data first. `scripts/world-stop.sh` stops a world.
The first start creates `data/`, with `root.token` and `person.token`. Treat those as passwords. A
world listens on port 7777 with no TLS, so keep it on a network you trust.

On macOS the JVM needs Local Network permission to reach a model on another machine, and that
permission belongs to the app that started it. If the world gets "No route to host" while `curl`
works, start it from Terminal.app: `open -a Terminal scripts/run-world.sh`.

## Connect a client

```
./listener/build/macos-debug/listener
```

On the world's own machine it opens as `person`, from `data/person.token`. `scripts/go.sh` starts the
world if it is down and opens the listener. A phone pairs by QR code. Set `KE_PUBLIC_URL` to the
address a client should dial. `-DORBIS_DEFAULT_URL=...` sets a phone build's starting address.

## Your own model endpoint

Set these in `.envrc`:

```
export KE_MODEL_URL=http://localhost:8000/v1
export KE_MODEL_ID=your-model-name
export KE_MODEL_KEY=your-key
```

Anything that speaks the OpenAI chat API works. More Models can be added inside the world.

## Known limits

- Real software development inside ORBIS does not work yet. Agents can read, write and run in a
  box, and a world carries its own source, but do not expect to do a real project's work in it.
- Expect to lose worlds. Upgrading one in place is not finished, and the log format can change.
- macOS on Apple Silicon is the only tested host. Linux and the phone builds get far less use.
- There is no TLS, and tokens are the only authentication. The confinement box limits an agent's
  mistakes. It has not been reviewed as a security boundary, so do not run untrusted models or
  prompts against a machine you care about.
- The holder's libghostty-vt is pre-1.0 and pinned to one commit.
- The design documents describe intent, and some are ahead of the code.

## License

License pending. All rights reserved: see `LICENSE`. You may look at it and run it because you were
invited, and anything you submit is assigned as `LICENSE` says. Third-party components keep their
own licences: see `THIRD_PARTY_NOTICES`.
