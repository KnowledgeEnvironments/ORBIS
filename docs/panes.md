# Panes: persistent, multiplexed, drivable

A pane is a live surface bound to an Object: a terminal on a shell Adapter, a file manager on a
directory, an editor or viewer on a file. It lives in the world, not in a client. Closing a client
leaves it running; several clients attach to one pane; a person, a model and a remote client drive
it through the same actions. No new class: a pane is an Object with `pane/*` Fields.

## Fields
`pane/kind` terminal | files | editor | markdown | latex | image. `pane/target` ref to the Adapter.
`pane/path` the file, relative to it. `pane/title`. `pane/state` open | exited. `pane/owner` ref.
`pane/asker` the person a model delivered it to. `pane/size` {cols rows}. `pane/exit`. `pane/cwd`,
`pane/selection` of a file pane. `pane/image`, `pane/pdf`, `pane/mime` as blobs a client fetches
with `read`. Layout is per person, a Field `view/panes`: a tree of splits and tabs of pane ids.

Nothing here runs in a client. Clients ship on iOS, Android and consoles, which have no PTY, shell
or local files: every pane, file operation and render happens in the world.

## Actions, all named actions through `act`, resolved under the caller's Grants
- `pane.open {kind target path? title? cols? rows? cwd?}` → `{pane}`; needs `:evaluate` on a shell
  Adapter, `:read` on a file. `target` is an id or a name; a file target is `<mount>/<path>`, or an
  fs Adapter with `path`.
- `file.open {path|name, as?, title?}` → `{pane kind path}`: a pane of the kind the file is (text
  editor, markdown, latex, image, a directory's files), delivered to the person a model is
  answering, as a view Message of kind `pane` with data `{pane kind}`. Reads are ungated.
- `pane.list {principal?}`, `pane.close {pane}`, `pane.resize {pane cols rows}`.
- `pane.read {pane, what screen|scrollback|buffer|listing, range?}`: what is there now, as text.
- `pane.read {pane, what bytes, field?, offset?, limit?}`: a binary Field of the pane, `pane/image` by default, as `{mime, size, offset, base64, more}`, at most 1 MiB a call. This is how a client draws an image or a rendered page.
- `pane.send {pane, text?, keys?, wait-ms?}`: bytes to a terminal. From a model the result carries
  `screen`, the terminal once output has been quiet for 1.2 s (at most `wait-ms`, default 8 s), so
  a gated send's approval result holds what actually ran.
- `pane.frob {pane, verb, args}`: a verb from that kind's table:
  files: `cd list select open mkdir rename copy move delete`;
  editor: `goto find insert replace undo redo save revert`;
  markdown, latex: `scroll goto render`.
  files args: `cd {path}`, `list {path?}`, `select {names}`, `open {name as?}`, `mkdir {name}`,
  `rename|copy|move {from to}`, `delete {name}`. editor args: `goto {line}`, `find {text}`,
  `insert {line col?|at, text, version?}`, `replace {find with all? version?}` or
  `{at del with version?}`, `undo`, `redo`, `save {force?}`. viewer args: `scroll {line}`.
- Reads are ungated. A model's `pane.send`, and any frob that changes something outside the world
  (`save`, `mkdir`, `rename`, `copy`, `move`, `delete`), pass the approval gate like `shell` and
  `fs:write`. Standing rules apply where a command is a whole line that parses; raw keys never match.

## Editors
The buffer is a fold of `pane-edit` events over the file at open, so every edit has an author and a
restart loses nothing, undo included. Whoever edits holds the buffer until they save. Anyone else
names the `version` they read; a stale or absent one answers `err conflict`. `save` is gated, needs
`:write`, refuses a read-only mount, and answers `err conflict` when the disk changed since the
open unless `force`.

An editor can sit on a Field instead of a file: `pane.open {kind editor, object, field}`, needing
`:read` on the Object. `pane/target` is the Object and `pane/field` the name. Only a string Field
opens, and a Field with its own visibility stays out, since a buffer's text is on the pane's stream.
`save` is `set-field` by whoever saves: `:write` on the Object, every rule of `act/field-guard`
(`policy/*` person-only, kernel Fields refused), the Field's own type and reader. A write to the
Field from anywhere reloads a buffer nobody holds, with a full frame, and marks a held one
`{stale true}`. Saving a stale buffer answers `err conflict` unless `force`. `revert` drops the
buffer's edits for what the Field or the file holds now, in either kind of editor.

## Render
LaTeX compiles on the host with tectonic: `KE_TECTONIC` names the binary, else `tectonic` on PATH.
The answer is `{view {kind image, data {bytes receipt, mime}}, log}`, the first page as a PNG, and
`pane/image`, `pane/pdf` hold the blobs. With no renderer, `err no-renderer`, and a view of kind `tex`
with `data {source}` lets the client typeset instead.

## Streams, for a phone
`act {action pane.subscribe, args {pane, after?, rate-ms?}}` → `{pane kind seq}`, and one
`pane.unsubscribe {pane}`. These two are session-only and have no action Object. Frames arrive on
the socket as `PaneFrame`, told apart by the key `pane`:

    {pane, seq, full: true, cols, rows, data}   a whole screen: reset, then apply data
    {pane, seq, base, data}                     a delta: applies on the screen at seq base
    {pane, seq, exited: true, exit}             the program is gone

`seq` is the pane's own counter. `data` is escape sequences, and only these: CUP, SGR 0 and colors,
EL, `CSI n S`, DECSTBM, `?25`, `?1049`, OSC 2, and the modes that change what a client sends: `?1`,
`?1000`, `?1002`, `?1003`, `?1004`, `?1005`, `?1006`, `?1015`, `?2004`, keypad `ESC =` and `ESC >`,
and cursor shape `CSI n SP q`. A full frame states all of those. The world diffs screens, so a keystroke is a few
bytes and a scroll reuses the lines a client holds; raw program output never crosses the wire and a
client's emulator can be small. Coalesced at 16 ms; `rate-ms` slows one subscriber further.

A client whose last seq is not `base` has a gap. After a dropped socket, or a gap, it subscribes
again with `after` its last seq: a delta from there while the world's ring (64 screens) holds it,
else a full screen. The same `after` works on a new session. Other kinds use the same frames:

    {pane, seq, full: true, state, data?}       an editor's data is its text; state is `pane/*`
    {pane, seq, base, edit {at del text who version}}   one buffer change
    {pane, seq, base, state}                    a place, selection, scroll or render moved on

`after` replays the frames missed from the ring, else a full frame.

Output is not on the event log: it goes to blobs, with a `pane-output {pane n blob size chain}`
event per 64 KiB (at exit, and after 30 s idle), `chain` being sha256(previous chain || blob hash).
Retention may drop old segments and old output blobs: an open terminal keeps its newest 16 MiB and `pane/record`, an open editor gets a `pane-edit` of verb `checkpoint` (`docs/log-format.md` Retention).
`pane-opened`, `pane-input` (who, how much, from where, text for a model's input), `pane-edit` and
`pane-closed` are events, so an agent's use of a terminal is on the record. A person's keystrokes
are counted per burst and not kept, so a password stays off the log.

## The gate
A model's `pane.send` is the word `shell`; its outward frobs are `fs:write`. A standing rule matches
a `pane.send` only when its text is one whole line with its newline and the pane is not `pane/dirty`.
A send no rule would admit, a person's typing or raw keys set `pane/dirty` for good, because from
then on the world cannot vouch for what is at the prompt.

A terminal inherits an allow-list of the world's environment (`pty/env-kept`), so a model reading
the screen cannot read the model key with `env`.

## Persistence
Terminal processes belong to the world's holder, so they survive every client and every restart of the world.

The holder (`holder/`) is a small native process per world, started by the supervisor. It owns the PTYs, spawns each program with the argv the world built, box included, and keeps a libghostty-vt terminal for each with 5,000 lines of scrollback. The world stays the only thing clients talk to, and its frames do not change. The socket is in the data dir, mode 0600, and the box forbids it.

| What happens | What a terminal does |
|---|---|
| A client closes | Goes on |
| The world restarts or crashes | Goes on. The world reconnects, asks for the output after the offset on its record and gets the whole screen again |
| The program exits while the world is down | The next world reads `exited` with its code |
| The holder crashes | `exited`, reason `holder-lost`, no exit code, last screen kept. The supervisor starts a new holder for the next terminal |
| A full stop of the supervisor | The holder and its terminals end. The next world reads them as `exited`, reason `lost` |

**No output is lost.** Each `pane-output` event records the byte offset it ends at. The holder keeps up to 4 MiB of output the world has not yet put on the record. Past that it stops reading the PTY, so a fast program waits instead of losing bytes. A program also waits if the world stays down that long.

**Screens.** The holder sends the rows that changed, at most every 16 ms, and the world folds them into the screen value `pane.screen` reads, so its `paint` and `diff` are the same for both kinds of terminal.

**Without a holder.** A world started by hand with `run-world.sh` has none unless `KE_HOLDER_SOCK` or `<data>/holder.sock` names one. It then runs terminals itself through pty4j and the parser in `pane.screen`, and a restart reads them as `exited`, with scrollback kept through the `pane-output` blobs. That parser also replays exited terminals from their blobs. It is a smaller xterm subset than libghostty-vt, so a supervised world is the one to trust with a long tail of programs.

Build it with `make holder`, which needs the Zig named in `holder/PIN`. libghostty-vt's API is pre-1.0, so the commit is pinned and only `holder/src/vt.c` includes it.

A phone reads a person's panes from `view/panes`; a desktop holds them in `view/layout` with its views, one tree per screen class (`docs/listener-style.md`). The layout of a person's panes is in `view/panes` on their Principal, so it follows them to another client.

## A person's terminal

`pane.open` from a person's own session starts the shell outside the box, with `pane/person`, and no agent can see it. `pane.share {pane, principal, mode read|type, minutes}` gives one agent that terminal for at most 480 minutes, and `pane.unshare {pane, principal?}` ends it; both need a person's own session. `read` is `pane.read`, `pane.list` and the stream. `type` adds `pane.send` and `pane.frob`, each a card unless a person's standing rule admits it; unsafe mode never does. See `docs/authority.md`.

## Formulas

`tex.render {source}` typesets what the client's math layout cannot, by hash of the source: a formula, a text fragment (`\section*`, `\textbf`, itemize, through a small document), or a whole document compiled as it is, first page shown on paper. It answers `{sha256 width height pages kind}` in points, or err `renderer-preparing` while packages are fetched once. `tex.read {source offset?}` gives the PNG in chunks. A person's own session only. The client keeps coverage and tints it with the theme's ink.

A math fragment TeX will not take is tried as text. Text commands in `$...$`, and an empty `$ $`, are not math: the client shows them as text and never asks. A failure answers one line, the reason and its line number, never the log; `tex.log {source}` gives what TeX said. `tex.open {source title?}` puts a rendered first page in an image pane.
