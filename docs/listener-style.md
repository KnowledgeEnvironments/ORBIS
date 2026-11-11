# Listener style

The visual rule for every listener surface, desktop and phone.
The product name is **ORBIS**, in capitals, wherever the user reads it (window title, menu head, boot
lines, bundle display name). In the top bar it is bold, set apart from the menus. It stands for
On-line Relative Basis Information System. A modeline runs along the bottom of the main window: one line of
status text, what is running and what just happened, never a control. A refused open, or an error held in a
pane out of view, shows there in the error ink for 30 seconds, because the click that caused it may
have come from anywhere.
Legibility first (principle 8).

## Colour meanings are the rule

What a colour means is the same everywhere. A theme only says what those colours look like. Panes
use roles and nothing else: `tests/no_literal_colours.py` fails the build on a literal colour outside
the theme code, and no pane branches on which theme is active.

| Role | Means |
| --- | --- |
| `Ink::primary` | content |
| `Ink::secondary` | content that can wait: timestamps, labels, hints |
| `Ink::strong` | the one thing that must be seen now |
| `Ink::nav` | navigation, hover, current scope, links |
| `Ink::inspected` | the inspected object, evidence, warnings, blockers, pending approvals, a failed op |
| `Ink::unresolved` | an abstraction, an unresolved binding, the off-the-record frame |
| `Ink::connected` | reviewed, connected, something that went well |
| `Ink::fatal` | fatal failure only: a gap in the log, a link that gave up, an attach that failed |

So `[STALLED]` and a refused op are amber in every theme, and red stays rare enough to mean stop.

## Themes

A theme is data: `assets/themes/*.json`, embedded in the binary, and any file in `<data dir>/themes/*.json`,
so another theme needs no code. A file names its ink, its surfaces, the seven chrome roles, the
terminal's sixteen `ansi` colours, the mark's three `brand` colours, its geometry (`controls` raised
or bracketed) and its type (a font list, first that exists, and `label-case`, caps or sentence).
What it leaves out comes from the default. A pref naming a missing theme falls back to prometheus.
VIEW, THEME appears only when a theme file gives a choice. The `unsafe` ink is red in every theme:
it is a role so no theme can leave it out.

- **prometheus**, the default: TRON luminance in a workstation toolkit. Near-black blue glass, hard white
  text, electric cyan, deep inset wells, square controls, strong bevels. SF Mono where present, then
  the bundled JetBrains Mono, antialiased.

## The map

GRAPH is what a person lands on. People, agents and Models are actors in three columns, agents as a
tree under whoever made them. Equipment is never a node: Adapters and panes are ports listed on their
actor, joined by the tree's L guides. Links are Routes and bound Models, run in channels between the
columns on lanes of their own; a Route that has carried a Message is brighter, and a Message crossing
pulses along it. A node's place depends only on what it holds, never on what it is doing, so the map
does not move while the world works. Links carry no labels, so no label can land on another; hover
lights a node's links and names them. Below the width of three columns the columns stack, nothing
shrinks, and links give way to a node's frame lighting when it talks.

The frame says the most urgent thing: red `unsafe`, amber waiting on a card, nav when hot or talking.
The second line says what it is doing: `idle`, the hazard tape, `wants: ...`, `stalled: ...`. A single
card is decided on the node. Click opens the tab, shift-click inspects, right-click or a long press
opens the menu, drag onto another actor offers CONNECT and HAND OVER, each through the normal action.
A world nobody has used yet starts with a card naming the first steps.

VIEW, TWILIGHT MAP puts the map over PROMETHEUS's twilight field, flat: Howard Look's SGI `twilight.c` colours (orange `255,72,0`, blue `0,110,189`, black) in the lower quarter at 32 %, black above, seeded stars that never move, white above the colour and tinted within it. It is off by default. The colours are the theme's `twilight-*` roles.

## Notices

`notify` lands in a chip beside APPROVALS and a window of its own, newest first, and as a count on the
sender's tab and node. `attention` and `urgent` are amber and also reach the desktop's notification
centre while the window is behind, unless WORLD, OS NOTIFICATIONS is off. A notice carries no authority:
clicking one only marks it read and opens what it names.

## Chrome

`src/ui/Chrome` draws seven roles, and panes never restyle locally: raised pane, inset well, raised
control, pressed control, title well, scrollbar track, scrollbar control. Selection is a pressed
control. Tabs are raised and pressed buttons where the theme says square. A control keeps the box its
bracketed word would take, so a theme never moves the layout.

## Tabs, buttons, links (2026-11-05)

Three kinds of control, and each looks like what it does.

- **A tab chooses what you look at.** Two levels. The tab bar (`ui::tabBar`) holds the views
  (Principals, Chat, Graph, Panes, History) and, inside Chat, one tab per agent. Under it, the
  sub-strip (`ui::subTabs`) is plain words with the active one underlined: Transcript,
  Conversations, Fields and the rest. A choice of what a view shows is in the same words, right
  of the sub-strip on its row when it fits: `Show all · messages · activity` (`ui::subChoice`).
  A tab is never a bracket.
- **A bracket does something.** `[ ACTIONS ]`, `[ MANAGE USAGE ]`, `[ APPROVE ]`, `[ STOP ]`. A
  bracket never switches a view.
- **A link goes somewhere, in cyan, in the text.** A name, a path, `open thread` at the end of a
  thread's header line. Same font, same baseline, same case as the line it is in; the hand and an
  underline on hover.
- **The menu bar is menu titles.** World, Approvals, Notices and Search open windows, drawn as
  words like File and Edit (`ui::menuBarItem`), amber when something waits. No boxes there.
- **A kind mark only where a strip mixes kinds.** Views carry none: their names say what they are.
  Chat's agent tabs carry `>` for an agent, `@` for you, `(i)` for an inspector; Panes carries
  `>_` for a terminal and `md` for a file. One meaning per mark.
- **One case.** Every label, tabs and sub-tabs included, follows VIEW, LABELS. Names (agents,
  files, worlds) are shown as typed. Drive targets keep the lower-case name, whatever is shown.
- **One size inside a transcript.** The transcript and a thread's window are set at the monospace size, and so is
  everything in them: cards, view blocks, their chips and links. The chrome around them (tabs, the Reasoning row,
  the input) stays at the interface size.
- **A code block has a title band.** Its first row is a slim tinted band, like a view block's bar: the language on
  the left, dim, and `copy` on the right in cyan, which reads `copied` for a moment after a click. Nothing is drawn
  over the code.
- **A column fits what is in it.** Your page's sender column is as wide as the longest name in its section, at most
  28 columns; a longer name ends in `…` and shows whole on hover.
- **A row never runs past the pane.** What does not fit gives way, least needed first, and says
  so in a tooltip: on the Reasoning row the plain "Using ChatGPT plan" goes first, then the gauge.

## Shape

- Rounding 0 everywhere. Window and pane titles in a title well, uppercase, letter-spaced.
- Monospace everywhere, including labels.
- Row height tight.
- Everything the user can read can be selected and copied: transcript lines (drag, Cmd-C,
  right-click menu), HISTORY rows (click, shift-click a range, Cmd-C as tab-separated
  seq/at/kind/stream/actor/body, right-click for body or ids), Field values (read-only input
  boxes), and `COPY ID` on any Principal row or graph node.
- The input line has a blinking block cursor, 530 ms period.
- Kind chips are bracketed uppercase tokens in their family color: `[FIELD-SET]`,
  `[ATTACHED]`, `[GRANTED]`. No pill shapes.
- Buttons are bracketed words: `[ DROP ]`, `[ RECONNECT ]`, inverse on press.
- Optional scanline overlay (every other line at 8 percent black), off by default, toggle
  in settings; never on the phone.
- Boot line at the top of the history on attach: `ORBIS LISTENER // ATTACHED AS <principal> // HEAD <seq>` in white, then normal rows.

## Angular

- Panels and chips have one or two chamfered corners, a 45-degree cut of 6 px, drawn with
  the ImGui draw list rather than rounding. Top-left and bottom-right by default.
- The phone's strip (`ui::tabStrip`) has parallelogram tabs slanted 15 degrees, inverse video
  when active: phosphor fill, black text, never a tint behind phosphor text. Hover inverts at the
  dim phosphor; idle tabs are an outline. The desktop's tabs are in **Tabs, buttons, links** below.
  ImGui's own tab bars are never shown.
  Bracket words `[ WORD ]` follow the same rule: inverse when on or held, dim-inverse on hover.
- Tab metrics, in points at scale 1 and multiplied by the display scale, all on whole pixels:
  height = text line height + 6 (3 above and below the glyphs); 8 of padding each side of the
  text inside the parallelogram; the slant is tan 15 degrees of the height (5 px at the 20 px
  default height); 6 between neighbouring tabs; 4 between stacked rows. Text is centred in
  the parallelogram and sits on the same baseline in every strip. A pane that opens with a
  switch (TERMINAL | GRAPH) has no separate title: row 1 is the switch and the pane's own tabs
  (`TERMINAL | GRAPH   ROOT  GRID  ATLAS`, or the switch and the graph toolbar), row 2 the
  sub-tabs (`TRANSCRIPT | PROMPT | FIELDS`) starting at the pane's left edge.
- Every strip scopes its widgets by its own id; ImGui never gets two live items with one id.
  `LISTENER_IDCHECK=sweep` walks a synthetic mouse over every view and fails the run on a
  conflict (ctest `listener_idsweep`). ImGui's error recovery stays on but is mute: no Debug
  Log window, no error tooltip, no assert; recoverable errors go to the listener's own log.
- Section rules end in a short diagonal tick. Dividers between columns may be `//`.
- Progress and lag are drawn as a row of slanted bars `////` filling left to right.
- The dockspace splits show a 1 px phosphor seam with a small chevron `>` at the grip.

## Motion

Small, fast, never in the way of reading, and none of it optional.

- Streaming: while an Evaluation streams, its row ends with a spinner cycling `| / - \` at 120 ms.
- Reconnect: the session pane shows `CARRIER LOST` while dropped and `CARRIER` on reattach, with
  the acked seq it resumed from.
- Idle: nothing moves but the cursor. No screensavers of our own; the system's is allowed.
- One hidden thing: typing `who am i` in the input prints the principal's own fields as the world
  sees them. It is also a real query, so it is not a trick.

## Rich text

The model writes markdown; the transcript renders it, with selection and copy intact across
styles (`ui/Markdown` on top of `ui/SelectableText`, no library). A block copy (triple-click)
yields the markdown source; a character selection yields what is on screen.

| construct | rendering |
| --- | --- |
| `` `code` `` | amber on a raised black box (1 px dim outline) |
| `**bold**`, `__bold__` | white |
| `*italic*`, `_italic_` | dim phosphor; only between word boundaries, so `a_b_c`, `x*y`, `(message/text)` and `seq 680` stay plain |
| `# heading` | white, the `#` glyphs kept in dim |
| `- item`, `1. item` | hanging indent, the `·` or number in dim; two spaces of nesting per level |
| fenced ``` block | a 1 px dim chamfered frame on near-black, lines in phosphor, language tag and `[ COPY ]` (copies the source) in the top-right corner |
| `[text](url)`, bare `http(s)://` | cyan, underlined; the url is a tooltip, right-click copies it, the listener never opens a link |
| `\| a \| b \|` tables | columns aligned in the monospace grid, `\|` dim, header row white, a dim rule under it |
| `---`, `***` | a dim rule across the width |
| `> quote` | a 2 px dim bar on the left, text dim |

The same renderer shows `instructions/*` in PROMPT (read-only; `[ EDIT ]` opens the raw text)
and string Fields over one line in FIELDS. Anything it does not recognise is plain text.

## Keyboard

One table, the same on the desktop and with a hardware keyboard on the phone.

| key | in the input line | elsewhere in the pane |
| --- | --- | --- |
| Return | send the line; while the Principal works, that queues it. It never interrupts | |
| Shift-Return | steer: interrupt the running call with this line. Idle, the same as Return | |
| Option-Return | newline inside the line | |
| Up / Down | recall earlier lines; Up on an empty line brings back the last line Escape cleared | |
| Escape | with text: clear the line into the kill slot, keep focus; empty: deselect, keep focus | deselect (transcript, HISTORY rows, graph node) |
| Escape, Escape (within 400 ms) | stop the running Evaluation (`act cancel`); the status bar says `ESC again to stop` after the first | same |
| Cmd-Z | on an empty line: bring back the kill slot | |
| Cmd-C | copy the selection | copy the selection (text, rows, ids) |
| Cmd-W | close the tab, asking first on a dirty editor | same |
| Cmd-X, Cmd-V, Cmd-A | cut, paste, select all | in a terminal pane Cmd-C and Cmd-V copy and paste; Ctrl goes to the program |
| Cmd-+, Cmd--, Cmd-0 | text size, everywhere, live | same |
| Cmd-O | open a file by path | same |
| Ctrl-arrows | | move between panes |
| click on a file | | open it in the right module |
| shift-click | | inspect it, on anything |
| right-click on a file | | OPEN, OPEN WITH, OPEN IN NEW PANE, INSPECT, COPY PATH, COPY ID |
| long press | | the same menu, on touch |
| Cmd-. | stop the running Evaluation | same |

Escape never leaves the input line unfocused: the first Escape clears, the second deselects or
stops, and typing resumes without a click.

## Chats

One chat per agent, opened from the PRINCIPALS pane; tabs across the top of the
CHAT pane, Cmd-W closes. Views are the sub-strip's words, the active one underlined:
Transcript, Conversations, Instructions, Fields, Memory, Devices, Rules.

- Transcript blocks: a head line `NAME  hh:mm:ss  [via <eval>]  <- <in-reply-to>` then the
  body wrapped. Lines typed by this listener are headed `> NAME` in white and set in white; a
  Principal's replies are headed by its name in cyan and set in phosphor. `[via ...]` is cyan
  (an object reference). A reply from our own principal that carries `via` is the loop's, not
  ours, and is drawn as a reply.
- Under the transcript: `NAME evaluating |  iteration N` in phosphor while `loop/iteration` is
  above 0 (spinner budget applies), `STALLED <reason>` in red while `loop/stalled` is set.
- The line sends on the Principal's first inbox route; `NO ROUTE` in amber disables it. Whom it
  addresses is the right-hand label, `TO <name> · ROUTE <route>`: another Principal's tab sends to
  that Principal; the attached principal's own tab is a conversation with the Route's other
  members (`route/members` minus self, else the Route's participants seen in Messages, else the
  Principal that created us, appliances driven by nothing dropped; nobody known means self, a
  poke to one's own loop). With several members the first is default and a click on the label
  cycles. A person's own tab therefore talks to root, never to itself.
- PROMPT and FIELDS: Field names in amber, authorship and time in phosphor dim, the world's
  answer to `set-field` inline: `OK seq N` in magenta (access family), `ERR code message` in red.
- The PRINCIPALS loop chip: `[CARRIER |]` while a cycle runs, `[STALLED]` in red with the reason
  on hover, `[IDLE]` dim; nothing for Principals not driven by process.

## Responsive desktop

The left column (PRINCIPALS, SESSION, BIND) takes 26 % of the width and never less than 28
columns of the mono font; the terminal and HISTORY take the rest, HISTORY's merged
`STREAM | ACTOR | BODY` column truncating first. Under 820 px (at scale 1) the left column
hides, the terminal takes the full width, and `[ PANES ]` appears in the menu bar: it brings the
column back at its minimum width, `[ PANES X ]` while shown. The dock layout is rebuilt only when
the width class changes, never per frame. Phone width is the phone layout's business.

## Threads and noise

Messages that share a `message/thread` are one conversation: a 1 px dim rail runs down the left
edge, a column and a half in, from the thread's header (`▾ thread · asked by person · 5 messages · open thread`, dim with `open thread` a
cyan link to the thread's own window; a click elsewhere on the line folds the thread to its root and last reply) to its last paragraph. A reply inside the thread by someone
who is neither the asker nor this terminal's Principal (grid answering root in person's thread)
hangs two columns further in with `└> grid` in its head (U+21B3 is not in JetBrains Mono; the box-drawing corner is); the asker's and the Principal's own lines stay
flush. System lines, a Trigger's firing (`message/kind trigger`) and an approval decision
(`approval-result`), are one dim line each: `heartbeat  hh:mm:ss  root  …`. The filter row above
the transcript, `Show all · messages · activity` (the active one underlined, persisted), hides
them under `messages` and shows only them under `activity`. A courtesy reply between loops at hop 3 or beyond draws dim: present, not loud.

## Cards and dialogs

An approval (a Task with `task/kind approval`, delivered as a Message with `message/kind approval`)
is a card in the transcript, not a text block: a 1 px amber chamfered frame, the summary in white,
the requester and approvers dim, the op spelled out (a `task.run` command in a code box with its
COPY bracket; a grant as grantee, scope, ops), then one controls row: `[ APPROVE ]` in magenta,
`[ DENY ]` in red, a reason field, the time left dim, the op's answer inline. Under 72 columns
the controls take two reserved rows (APPROVE / DENY, then the reason field and the time left) so
nothing wraps over the next block. Decided, the card
keeps its head and collapses to one line (`approved by person · ran · ok: 8 tests, 227 assertions`
in phosphor, `denied by person: not now` in red) in a dim frame. PENDING counts are magenta chips.

Dialogs are modal, black, titled like a pane (`N E W  P R I N C I P A L`), built from the
world's `action/schema`: labels in phosphor (dim when optional), widgets in the pane style, enums
as combos, lists of enums as checkboxes, an `:or` as radio variants, `[ CREATE ]` / `[ RUN ]` in
magenta, `[ CANCEL ]` top right, the op's answer inline next to the bracket. Nothing is drawn
before the world answers: the new node appears when its events arrive.

## Chat, you, inspectors

Three kinds of tab, and each says what it is. `>` is a chat: clicking an agent opens it, and the input line carries no words unless there is a real choice of Route or recipient. `@` is your own page: what waits on you, what is addressed to you, notices, your devices and your Fields, with no input line, because a person is an identity and an authority, not someone to talk to. `(i)` is an inspector, from INSPECT on any object's right-click menu, a shift-click, or a Field link; it is a separate tab from that agent's chat. A terminal is only ever a real terminal, under PANES. Closing the last chat goes back to the map; nothing opens by itself on attach.

## What it is not

Not a game HUD, not neon glow, not blur. The reference is a terminal someone trusted in
1995: WarGames, the Genera console, a VT with the brightness up. Every color carries a
meaning listed above; if a color has no row in the table it does not appear.

## Views and attachments

A view block (a Message of kind `view`) is a slanted header bar, dim fill, white title, the kind as an
inverse chip in the kind's colour (table amber, graph cyan, image magenta, tree phosphor, text white),
the `about:` refs as cyan chips, `[via …]`, and `[ EXPAND ]` at the slanted end; the body sits in a thin
dim frame open at the top. Tables: monospace, a hovered header is inverse (black on phosphor), numbers
in amber, refs in cyan, borders dim, no row banding. Trees: chamfered 3 px expanders with `+` / `-`,
leaves marked `·`, values in amber. Graphs reuse the patcher's node and edge drawing; an expanded view
on the GRAPH pane replaces the map until dismissed. Images fit the width, never
upscale, and open full size on click. Everything else in the block follows the transcript rules
(selection flows around the block, zero rounding, 1 px lines).

A dropped file reports its progress as `ATTACH <stage>` above the input line: amber while working, dim
when done, red when refused; a dropped folder is a one-line prompt `DROPPED FOLDER <name>/
[ MOUNT ] [ ATTACH LISTING ] [ x ]`.

## Models, search, filtered seqs (2026-10-30)

- **MODEL** in every terminal's CTX bar: `[ MODEL <label> ]` opens the fold's Models as a
  table (name, model id, status chip, prices per million tokens, `[ PROBE ]`); the active one is
  the Principal's first `principal/models`; choosing writes that Field with the pick first;
  `[ NEW MODEL… ]` is `model.create` through the schema dialog. Status chips: `ok` phosphor,
  `unreachable` and `unauthenticated` red, `unknown` dim. GRAPH model nodes show the model id
  under the name and the same chip.
- **SEARCH**: `[ SEARCH ]` in the menu bar and the HISTORY header, Cmd-F. A one-line box above
  the rows; Return asks the world (`search {text limit 50}`), hits list kind chip, name (cyan),
  field (dim), the snippet with the match in **inverse video**, `seq N`. Click: a Principal opens
  its FIELDS, anything else scrolls the rows to the seq and selects it. Escape closes. Without a
  search op the listener searches what it holds and says `local` in dim.
- **Filtered, not gaps**: a narrow Principal's subscription skips seqs it may not read; the panes
  say `filtered N` in dim, never red. Red `GAPS` appears only for a real fault the client can see:
  the world's as-of head falling below our cursor (a different or truncated log under the URL).

## Worlds, devices, pairing (2026-10-30, sprint 2)

- **One world at a time, many known.** `[ WORLD <name> ]` in the menu bar (the phone: WORLDS in
  Settings) lists every world this machine has attached to, most recent first, with the principal
  and the last attach; `[ ATTACH ]` switches, the folds reboot from that world's snapshot, the
  window title reads `ORBIS // <name>`. `[ ADD WORLD… ]` takes a url and a token (or a token
  file) and offers `[ HELLO ]` first: the world's name and head before any token is spent.
  Tokens live in `worlds.json` (0600) beside `session.json`; sessions stay per world url, and a
  world whose id changed under a url starts fresh and says so in SESSION (FRESH).
- **DEVICES** is the fifth terminal tab: the Principal's token Objects as rows (label, issued,
  expires, last used, state chip LIVE / EXPIRED / REVOKED), `[ REVOKE ]` with a reason per row,
  `[ NEW DEVICE… ]` for a labelled token with an expiry. The secret shows once, in an amber
  chamfered box with `[ COPY ]` and `[ SAVE TO FILE ]` (`<label>.token`, 0600, in the data dir);
  it is never logged and never a Field.
- **Pairing** is a code, not a token in transit: `[ PAIR PHONE ]` shows a 6-digit code at three
  times the mono size, the world url in cyan, the time left ticking once a second, and a QR of
  `orbis://pair?url=…&code=…` on a white quiet zone (the one place white fills a region: a QR is
  for a camera, not a person). The phone claims it under SETTINGS > PAIR WITH CODE, or by opening
  the link; the new device appears in DEVICES when the world records it.
- Tables from views honour `align right` (and right-align numbers by default); unpriced Models
  show `–` in the MODEL menu; the search box offers the last query again after a restart.

## Kernel record, status lines, thread window (sprint 2)

- **Kernel record.** A loop's reply that ran commands ends with the world's record of them
  (`Runs this wake (kernel record):`). The terminal parses it back out and draws a framed box
  under the prose: dim title `[-] KERNEL RECORD · N runs` (click folds it to the title with the
  exit codes), each run on its own row with the command in amber, `→ exit N` in phosphor for 0
  and red otherwise, the duration and the Task id dim, the stdout tail (colour codes stripped)
  dim under it. The numbers come from the Task Fields when the fold has the Task (same command,
  same owner, created inside the wake), else from the text the world wrote; the row tooltip says
  which. A `reply/contradiction` Field puts a red `CONTRADICTION` chip on the reply's head, the
  specifics in its tooltip and in a red line inside the box, and keeps the box open and red.
- **Status lines.** `message/kind status` (a loop's interim word while an op awaits approval) is a
  system line like heartbeats and approval results: one dim line under `all`, hidden under
  `chat`, alone under `system`. A decided card's collapsed line reads the run it executed:
  `approved by person · ran · exit 0 · 129 s` from `task/ran`, `task/exit` and the run's duration.
- **Thread window.** `[ OPEN THREAD ]` at the right end of a thread's rail header (or the
  transcript's right-click menu) opens the whole conversation in its own window, `THREAD //
  asked by <name> · N messages`: every participant's Messages in log order, a line from a third
  Principal indented with `└>`, `hop n` and the kind on the head, system kinds as one short dim
  line, the kernel record rows, and the cost line per reply. The window asks the world once for
  `history {thread}` and says `from the world` or `from the fold`; `[ COPY ALL ]` copies it as text.

## Symbols

The world's names in running text are live, in the transcript, HISTORY, FIELDS values, kernel records and view tables. One matcher (`chat/Symbols`) runs over the listener's own fold, so a Grant decides what links and nothing outside the reader's view can leak. The model is never asked to escape anything.

| In the text | Shows as | Rule |
| --- | --- | --- |
| `root.route`, `grid.fs/README.md`, `task.run` | cyan | a dotted, slashed or hyphened bound name always links |
| a full id | cyan, the name | looked up in the fold |
| `88969202` | cyan | an 8+ hex prefix links when unique |
| `0fe4c2fd` after `message`, `task`, `(` | dim | matches nothing where an id belongs: a mistyped id shows |
| `[[id\|name\|class]]` | cyan, the name | the serializer's tag, which models half-copy |
| `loop/stalled` | cyan | a Field of the object in context |
| `seq 680` | cyan | its row in HISTORY |
| `root`, `person` | plain in prose | single words link only in code spans and tags |

Hover: name, class, driven-by, loop state, hint, full id, with the hand cursor. Click: a Principal opens its chat, another Object its inspector tab, a Field scrolls to its row, a seq scrolls HISTORY. Right-click: terminal, fields, graph, the world's actions, copy id, copy name. A character selection copies what is on screen; a block copy yields the source text.

An inspector tab, marked `(i)` and titled `inspector`, is an object's Fields apart from any chat: FIELDS only, plus MEMORY when the object has `memory/*` Fields.

## Reasoning

Two brackets, two meanings. `[ SHOW REASONING ]` / `[ HIDE REASONING ]` is display only, for reasoning the model already produced. `[ REASONING OFF ]`, `[ REASONING ON ]` or `[ REASONING DEFAULT ]` reads and writes that Principal's `loop/reasoning` and decides whether the model reasons at all; it is hidden on person-driven Principals, its tooltip states the cost and the Model's value, and `/think` and `/fast` override it for one message. Each reply's cost line says `reasoning on` or `reasoning off`, from what the Evaluation actually did.

## New Fields and memory

One widget: name, type (string, number, boolean, keyword, instant, ref, list, map), reader (agent, human, both), value, `[ SET ]`. It sits on top of FIELDS and MEMORY and under PROMPT, works on any object, and validates before sending: a ref takes a name and offers completion, list and map must parse as JSON. It refuses an existing name with a pointer to that row, never overwriting. Dim text under the name says what the world does with it (`instructions/` joins the system prompt, `memory/` is always in the prompt, `<field>/hint` is a one-line comment).

MEMORY lists every `memory/*` Field with author, time and size, each editable in place. The bar counts them all against `memory/budget`, because the serializer always includes them all. `[ FORGET ]`, with a confirm, clears a value through `set-field`: the entry sinks to the end, the log keeps its history, and `previous` shows on hover.

## Steer and queue

While a Principal works, a dim line above the input says `RETURN queues · SHIFT-RETURN steers`, and `[ STEER ]` beside the input does the same where there is no Shift. Idle, none of this shows. Plain Return never interrupts.

A `QUEUED n` strip sits directly above the input, only when something waits: `[ ^ ] [ v ]` reorder, `[ X ]` dequeues, `[ STEER ]` promotes. A line sent while the Principal works appears at once as `on its way` until `loop/queue` lists it. A steer shows `STEER · steering…`, then `STEER · applied at iteration N`; the call it replaced ends with a dim `steered` line, which is not an error. A refused steer keeps its text in the input, says why, and offers `[ QUEUE INSTEAD ]`.

## PRINCIPALS tree

The top level is who acts: people, then agents, folded, each with a name, `you` or `agent`, and its loop chip. Unfolded, one plain line per purpose: `model  root.model`, `files  grid.fs`, `shell  grid.shell`, `reads  harness`, `works in  ...`, `talks to  person, root`. A folder is `files` unless the world marks it `adapter/read-only`, then `reads`. Routes collapse into the names at the other end; each name opens that Principal and its right-click has `OPEN ROUTE`. A shared thing simply appears wherever it is used, and hover says who else. A Principal without a Model is an appliance, not an agent. What no actor uses sits in a folded `UNATTACHED` at the bottom. It all comes from real relations, never from dots in names. `[ FLAT ]` keeps the table with CLASS and DRIVEN. `[ ? ]` explains each word in one line.

## The layout at rest

PRINCIPALS has the left column. The right is CHAT, GRAPH or PANES over a one-line HISTORY strip with the newest event and `[ EXPAND ]`. The menu bar is the one line of SESSION: state and head, and `GAPS n` in the fatal colour if the log has any. SESSION and BIND are under DEBUG. The terminal header is one quiet row: CTX now, MODEL, ACTIONS, and the transcript filter; running totals are on hover; SHOW REASONING, REASONING, COMPRESS and OFF THE RECORD are at the top of ACTIONS. Raw ids stay out of headers: COPY ID has them.

## What the model was told

`[ VIEW PROMPT ]` in the FIELDS header of an agent opens the exact text of its latest Serialization, read-only, cut at the serializer's own markers into identity, Acting, objects, instructions, memory, thread and messages, with bytes on each section. It reads the Evaluation's Serialization, so it shows what was sent, never a reconstruction.

## Delete

`DELETE` in the right-click menu of a PRINCIPALS row, a graph node or a symbol link, and `[ CLEAR ]` per Field. The dialog says what happens in plain words: gone from view and prompts, kept in the log, restorable. On a conflict it lists what still depends on the object and offers `[ FORCE ]`. World, root and the first person cannot be deleted, and say why. `[ SHOW DELETED ]` in PRINCIPALS lists retired objects with `[ RESTORE ]`.


## Approvals

`[ APPROVALS n ]` in the menu bar is amber while anything waits. The inbox groups pending Tasks by who asked. Tick rows and use `[ APPROVE SELECTED ]` or `[ DENY SELECTED ]`, or `[ APPROVE ALL FROM name ]`; keys are `a`, `d` and the arrows while no box has focus. Decided rows stay in place until `[ CLEAR DECIDED ]`, so a row never moves under the cursor. `[ ALWAYS… ]` opens the rule form on the selected command: it tests the draft live through `approval.rule.test` and says `would run` or `would still ask` with the world's reason, then `[ APPROVE AND REMEMBER ]` or `[ APPROVE ONCE ]`. The POLICY tab of a Principal lists its standing rules with what ends each and `[ REMOVE ]`, and `[ NEW RULE… ]` uses the same form. A run a rule admitted has no card: it is one dim `auto · rule <note> · <cmd> → exit N` line in the wake. A decided card keeps its height while the mouse is on it.

## Conversations

CONVERSATIONS sits beside TRANSCRIPT: every thread with this Principal, newest first, with a search over titles and people. `[ OPEN ]` shows one read-only; `[ RESUME ]` sends `/resume` and returns to the transcript; `[ NEW ]` sends `/new`. `[ COMPRESS ]` in the status row sends `/compress`, and the CTX figure turns amber within a tenth of `serialize/compress-at`. Typing `/` offers the directives in the `@` list.

## Off the record

A window of its own, never a tab, with a magenta frame and the title `OFF THE RECORD // name // nothing is kept`. It opens from the right-click menu or the status row, after one screen that says what is kept, what is not, and that the Model still receives the text. Text lives in that window only: no session.json, prefs, disk cache, log line, search or COPY LOG. The drive sees a count and answers `err ui-only` to typing, keys, drops and screenshots while one is open. Closing warns; quitting and changing world end it. Only pushed events render it, because `read` cannot see inside a scratch.

## Panes and modules

`docs/panes.md` is the contract. PANES sits beside TERMINAL and GRAPH: tabs and splits of panes the
world holds, the layout on `view/panes`, so it follows a person across clients. `src/modules/` holds
one `Module` interface and one registry row per module: terminal, files, markdown, TeX, image,
editor. A module asks the world for everything through its Host, as `pane.*`, so a model and a
remote client do what a person does. `kindForPath` is the one place that says what a file opens
in, and `clickAction` the one place that says what a click does: a file opens, shift-click always
inspects, anything else keeps its terminal or Fields. No module knows another, and no module calls
the OS: `src/platform/` is that boundary, and `tests/no_literal_colours.py` covers `src/modules`.

- **Tab bar**: `ui/TabBar`, for TERMINAL and every pane group. Tabs join the content they own; the
  active one is open into it and a baseline runs under the rest. A kind mark tells a chat `>` from an
  inspector `(i)`, a viewer `md` from an editor `ed`. Every tab closes; the last falls back to home.
  Overflow scrolls, with a list at the end. Sub-views are a quiet strip below, and it wraps.
- **Terminal**: libvterm, fed the world's escape sequences, in the theme's ANSI colours. The program's
  own truecolour passes through. Shift overrides mouse reporting so a person can still select.
- **Math**: `modules/MathLayout`, native and with no dependency: `$...$` needs no space after the
  opening `$` and a closing `$` not followed by a digit, so `$5 and $6` stays text. A ```tex fence or
  `$$` is display math. The source stays in the document, so selecting and copying yield source, and
  SOURCE shows it. A whole document goes to the world's `render`; with no model the view still reads,
  from `modules/TexDocument`.
- A model's input flashes the pane in amber with its text, and the dim line under a pane names who is
  attached and who drives it.

## Text

Copy and paste work everywhere. Each text field reports itself through ImGui's item hook
(`ui/ImguiHooks`), so CUT, COPY, PASTE and SELECT ALL reach every field with no call at the site.
VIEW has an interface font and a monospace font, each with a size, per client; the interface size
scales the whole layout. ImGui bakes glyphs on demand, so changing either never rebuilds an atlas.
Headings are tabs on their panel's rule in Roboto Condensed: nothing reads as a control unless it
is one. Every label goes through `ui::upper`, which follows the label case; `CTX` and `CARRIER` keep
their capitals in either.

## Names and references

`nameOf` is `object/label`, then the bound name, then a short id; hover always shows the bound name
and the id. RENAME LABEL and a double-click on a tree row set the label; RENAME BOUND NAME is the
action that changes what agents resolve. References are structured: `message/refs` and
`<field>/refs` are spans, and text with a refs Field is never read by the matcher (`chat/RichText`).

## Unsafe mode

Per Principal, for 30 min, 2 h or 8 h, from `UNSAFE MODE (disable approvals)` in FIELDS and by the
input. The red indicator arms from `world/unsafe` and nothing else, so a second client shows it. It
stays red, never blinks, and keeps its room in the bar at any width; a click lists who, with one-click
off. Their tab and tree row carry a red mark, and what ran under it is a dim list in APPROVALS.

## Activity marks

One mark per agent, drawn, so no font has to carry it (`ui/ActivityMark`): `!` in the inspected ink for needs you, a red cross for failed, a green tick for finished and unseen, the hazard tape for working, a dim dot for idle, a hollow dot for inactive. It replaces the bracketed loop word on rows, sits before the name on chat tabs and at the right of a map node's name, and rolls up at the left of the modeline as `2 need you · 1 done`; a click goes to the next one. On a map node, working and idle add nothing to what the node already says.

## Controls

- `[+]` adds, and nothing else does. It sits at the right end of its header or row, and its tooltip says what it adds. The location names the thing, so no word is needed.
- The same bracket holds anything that small: a count `[3]`, a close `[x]`. A word button is for a verb with no symbol.
- A fold is a triangle: `+` means add.
- A Field row reads the same everywhere: what it is on one line, EDIT, MORE, CLEAR at the right of that line, the value below.
- Scrolling text sits in an inset well with `insetRim` and inner padding, so what scrolls reads as behind glass.
- A dialog's buttons take their own row, with `dialogGap` above them.

## Layout

The desktop window is one tree of tabs and splits, `view/layout` on the person: every view and pane can be dragged by its tab to any edge, resized by its divider, floated from its tab menu, and closed. A closed view comes back from VIEW, and RESET LAYOUT starts over without closing any pane. Views stay pinned at the head of their tab group, so panes never scroll them out of reach. A float's close button docks it.
