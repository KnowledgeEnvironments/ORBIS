<p align="center">
  <img src="listener/assets/server.png" alt="The ORBIS server" width="160">
</p>

<h1 align="center">ORBIS</h1>

<p align="center"><b>A shared knowledge environment for doing work with people, computers, and artificial intelligence.</b></p>

<p align="center">
One append-only log is the truth. Everything in it is an Object with Fields. Agents are Principals with Grants, Routes and Models, and they live in the same room as you.<br>
The world is a Clojure process that is also the agent harness. The listener is a native client that draws what the world sends.
</p>

ORBIS is the On-line Relative Basis Information System. It is shared here with invited friends, to
play with. It is not open source: see [License](#license).

![root fixes its own kernel: the card, the approval, the restart, and the report](docs/figures/readme/chat.png)

*Above: root, running on a hosted model, has just changed ORBIS's own source, tested it, cut a release and asked to restart the world onto it. The person approved the card. The world came back on root's commit, root's wake carried on, and it reported with the commit, the files and the test it ran. Every line of that is on the log, in the HISTORY view under the chat.*

## What it is

- **A world, not a chat log.** People, agents, Models, files, shells, terminals, projects, approvals and notices are all Objects in one graph, named, with a record. An agent's prompt is a *serialization* of that graph around its task, not a transcript stuffed to the limit.
- **Authority is explicit.** A Principal does what its Grant resolves. Commands run through cards a person decides, or through rules a person wrote. Root is the operator; agents escalate to it; a new release of the kernel always waits for a person's own card.
- **It carries and changes its own source.** A supervised world holds the release it runs and a working copy root may edit. Root builds, tests, releases and restarts the world; a release that fails its health check comes back to the last good one by itself.
- **Any model.** An OpenAI-compatible endpoint on your LAN, or Sign in with ChatGPT for plan usage, per Principal, switchable mid-conversation, with a fallback.
- **Native, everywhere.** The listener is C++20, SDL3 and Dear ImGui: macOS today, iOS and Android builds in the tree. Terminals, files, TeX and search run in the world, so a thin client sees everything. The holder keeps terminals alive across a world restart, on libghostty-vt. A Principal's commands run in a confinement box: `sandbox-exec` on macOS, `bwrap` on Linux.

The ideas are in `docs/ethos.md`, the wire in `docs/protocol.md`, who may do what in `docs/authority.md`. The same documents are built into a running world as its manual.

## Pictures

| | |
|---|---|
| ![the graph: people, agents, models](docs/figures/readme/graph.png) | ![about](docs/figures/readme/about.png) |
| The GRAPH view: who is here, on what, with which mounts and shells. | A world says what it runs: release, commit, log format. |

![the log](docs/figures/readme/history.png)

The HISTORY view is the log itself: every seq, who did it, what changed. Filter to one Principal, to reads, to routine bookkeeping, or follow the head.

## Five minutes

```
scripts/install.sh
```

The installer checks the toolchain, offers Homebrew for what is missing, asks for a Model, builds the listener, makes a world in `data/`, starts it under its supervisor and opens the listener on your token. `docs/install.md` has every step by hand, a client-only install for a second machine, and what to do when something does not start.

Then, in the listener:

1. Click **root** in PRINCIPALS. Type in the box at the bottom. Root reads the harness it runs from, so ask it what it is.
2. **Add a project.** Tell root `Call add_project with name "myapp", path "/path/to/myapp"`. One op makes a Principal, a mount, a shell and a Grant narrower than root's; the new agent reads only there.
3. **Decide a card.** Ask the project's agent to run its tests. The command is a card in APPROVALS: what it will run, through which shell, in which directory. Approve it, or add a rule so test commands run on their own from now on.
4. **Change the Model.** The bar under the transcript: MODEL picks any Model in the world for this Principal or this conversation; REASONING turns thinking on; CTX shows how much of the window the next wake will use.

## Requirements

Developed and tested on Apple Silicon macOS only.

- World: OpenJDK 25 and the Clojure CLI.
- Listener: Xcode's clang, CMake 3.28 or later, Ninja. `sdl3` and `freetype` from Homebrew make the build quick; without them both are built from source.
- Holder: Zig 0.17.0, for terminals that survive a world restart and for `make test`. `make holder` clones and builds libghostty-vt.
- A model: any OpenAI-compatible chat endpoint, or a ChatGPT plan to sign in with from the listener.
- Optional: `tectonic` for TeX rendering.

```
brew install openjdk@25 clojure/tools/clojure cmake ninja sdl3 freetype zig
```

## Build and run by hand

```
make deps        # Clojure dependencies
make listener    # the client; fonts and libraries are fetched at configure
make holder      # the terminal holder
make test        # schema, unit, exit and kernel-zero tests; run after make holder
```

A supervised world, the kind root can change and restart: write `<dir>/world.env` (`docs/install.md`
shows it), then `scripts/world.sh <dir>`; `scripts/supervise.sh --stop <dir>` stops it. A plain world:
`cp .env.example .envrc`, set the model URL, id and key, `scripts/run-world.sh`. The first start
creates the data directory with `root.token` and `person.token`: treat those as passwords. A world
listens on its port with no TLS, so keep it on a network you trust.

On macOS the JVM needs Local Network permission to reach a model on another machine, and that
permission belongs to the app that started it. If the world gets "No route to host" while `curl`
works, start it from Terminal.app: `open -a Terminal <dir>/start.sh`.

The listener: `./listener/build/macos-debug/listener --url ws://127.0.0.1:7777/ws --token-file data/person.token`.
On a second machine, copy the token once; a phone pairs by QR code. `KE_PUBLIC_URL` is the address
clients are told to dial.

## Demo: a world that improves itself

`scripts/test-bootstrap.py` is the demonstration in the picture at the top, run end to end by a script that plays the person. It makes a fresh world from the checkout's HEAD, puts root on a real model, gives it an open item from the work list, decides root's cards by a policy it prints, approves the kernel swap, and checks from outside: the world runs root's commit and it is good, the commit touches what the task named, tests ran before the release, the reply names the commit, the behaviour the task asked for holds on the new world.

```
scripts/test-bootstrap.py <scratch> --port 7811 --record <dir with a ChatGPT sign-in record> --task view-refs-test
```

What root wrote back on one run, verbatim:

> Implemented and released the focused view-reference regression test.
> - **Commit:** `2e7c72048c4d672461ed7e7c9b9aeb6e8750cefd` — `Test references recorded on views`
> - **Changed:** `world/test/knowledge/world/view/refs_test.clj`
> - **Test run:** `clojure -M:test --focus knowledge.world.view.refs-test` — 1 test, 8 assertions, 0 failures.
> - **Restart:** moved the running kernel from `14ecbbb0feb4…` to the release above.
> - **Post-restart confirmation:** `world/release` reports commit `2e7c72048c4d…`.

200 seconds wall, 150 of them the model's, one card. The test it wrote is on this branch. The report with both runs, including the defect the first run found in the kernel, is `the project's own notes`. The design for what comes next, a wake's own failures becoming a change, is `docs/lessons.md`.

## Talk to a world from anything

The protocol is JSON (or Transit) over a WebSocket: four frame kinds, a few dozen ops, every response carrying the log head. `docs/protocol.md` is the contract; `docs/protocol-examples/` has one file per frame. A person's session in ten lines, with the standard library only:

```python
import sys; sys.path.insert(0, "scripts/bench")
from ws import World                                 # scripts/bench/ws.py
w = World(token_file="data/person.token", port=7777) # attach as you
root = w.resolve("root")
route = w.resolve("root.route")
w.call("send", {"to": [root], "route": route, "text": "what is this world running?"})
for e in w.events(w.head() - 50):                    # the log, from 50 events back
    print(e["event/seq"], e["event/kind"], e.get("event/stream"))
```

The same ops the listener uses: `attach`, `read`, `set-field`, `send`, `history`, `act {action, args}`. `scripts/world-health.py`, the supervisor's health check, is the same client in forty lines.

## How it is built

```
schema/    Malli schemas shared by the world and generated for the listener
world/     the world: log, fold, index, resolution, serializer, loop, actions, protocol, adapters
listener/  the client: C++20, SDL3, Dear ImGui (docking), FreeType; desktop, iOS, Android
holder/    the terminal holder: terminals outlive a world restart (Zig, libghostty-vt)
scripts/   install, run, stop, release, supervise, back up, fork, export; the bootstrap test
docs/      the contract and the working documents
```

- The log is the truth: `docs/log-format.md`. The fold builds the index; a snapshot stands in for pruned history.
- Resolution decides who may do what: `docs/authority.md`.
- A world that carries and changes its own source: `docs/self-hosting.md`.
- Why things are as they are: `docs/decisions.md`, append-only. The ethos: `docs/ethos.md`. How an agent works on this repository: `AGENTS.md`.

## Known limits

- Real work inside ORBIS holds on a strong hosted model (the demo above, on gpt-5.6-sol) and not yet on a small local one: agents can read, write and run in a box, but a local model writes against APIs it has not read.
- Expect to lose worlds. Upgrading one in place works through root's releases, and the log only moves forward, but the log format can still change.
- macOS on Apple Silicon is the only tested host. Linux and the phone builds get far less use.
- There is no TLS, and tokens are the only authentication. The confinement box limits an agent's mistakes. It has not been reviewed as a security boundary, so do not run untrusted models or prompts against a machine you care about.
- The holder's libghostty-vt is pre-1.0 and pinned to one commit.
- The design documents describe intent, and some are ahead of the code.

## License

License pending. All rights reserved: see `LICENSE`. You may look at it and run it because you were
invited, and anything you submit is assigned as `LICENSE` says. Third-party components keep their
own licences: see `THIRD_PARTY_NOTICES`.
