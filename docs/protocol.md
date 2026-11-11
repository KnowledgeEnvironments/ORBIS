# Wire protocol, version 0

The contract between the world and every client. Frozen at tag `protocol-v0`; changes after
that go through `docs/decisions.md` first. The source of truth for shapes is
`schema/src/knowledge/schema/protocol.clj`; the JSON Schema mirror for non-Clojure clients is
`listener/generated/schema/` (regenerate with `make schema-json`). Examples that must always
decode and validate live in `docs/protocol-examples/`.

Spec sections: 14 (wire protocol), 13 (architecture), 7 (streams), ADRs 0001 to 0004, 0008.

## Transport

- `ws://<host>:<port>/ws?format=transit|json`. Default port 7777. Text frames in both formats.
- `transit` is Transit-JSON, the native form for Clojure clients and the log.
- `json` is the JSON form (ADR 0002) for clients without Transit, including the listener.
- One connection per session. Max message 4 MiB. Push frames are capped at 256 events or 1 MiB.

## Frames

Four kinds, told apart by key presence.

| Kind | Shape | Direction |
| --- | --- | --- |
| request | `{id op args}` | client to world |
| response | `{id ok=true result seq}` or `{id ok=false err seq}` | world to client |
| push | `{sub events as-of lagging? from? gap?}` | world to client |
| ack | `{ack=<sub> seq}` | client to world, no response |

- `id` is a client-chosen string, unique per connection. Responses may arrive out of order.
- `seq` in every response is the world log head at the time of the reply.
- `err` is `{code message data?}`; codes: `unresolved gated budget residency conflict lagging invalid not-implemented unauthenticated pruned`, and more in the schema. `pruned` means retention took what was asked for (`docs/log-format.md` Retention). A failed resolution and a missing name are both `unresolved` with no detail.

## JSON form (ADR 0002)

| Clojure | JSON |
| --- | --- |
| keyword key `:event/kind` | string key `"event/kind"` |
| keyword value `:field-set` | string `"field-set"` |
| uuid | string |
| instant | ISO-8601 UTC `"2026-10-29T06:00:00.000Z"` |
| bytes | base64 string |
| ref | the id string |
| seq | number |
| set | array |

Decoding is by schema position, so enumerations and discriminators (`op`, `event/kind`, `kind`, `class`, `reader`, `state`) come back as keywords. `knowledge.schema.wire/decode` is the reference decoder.

## Events

An Event (spec 4.4, ADR 0003): `event/id event/stream event/streams? event/seq event/kind event/actor event/via? event/delegation? event/at event/body`. `event/seq` is global over the world log; `event/stream` is a partition key. Bodies per kind are in `knowledge.schema.events/bodies`. Kinds:

`created bound unbound field-set field-read resolved granted revoked sent delivered task-moved evaluated fired attached detached retired scratch-began scratch-ended`
`created bound unbound field-set field-read resolved granted revoked sent delivered task-moved evaluated fired attached detached retired restored`

## Session

`attach {token principal? session? subs? client-kind?}` returns `{session principal resumed seq}` and appends `attached` to the principal's stream. The token is one of the principal's credentials: since a credential is a **token Object** (`token/principal`, `token/hash` = sha256 of the secret, `token/label`, `token/issued-by`, `token/issued-at`, `token/expires-at?`, `token/last-used`, `token/revoked-at?`), minted at `act create` for a person or process Principal (the secret to `data/<name>.token`, and to the session's result), by `token.issue`, or by pairing; `attach` accepts any unrevoked, unexpired token Object of the Principal and the `attached` event carries `token` (which device attached); an expired or revoked one answers `err unauthenticated "token expired|revoked"`. A Principal's legacy single `principal/token-hash` still authenticates (its `attached` has no `token`) and becomes a token Object labelled `legacy` at the kernel-zero later step `token-objects`. A fork or an import re-mints: inherited token Objects are revoked (`re-minted: fork or import`) and fresh ones recorded.

Before attach a socket may send two ops: `hello {}` → `{world name url format-versions head first-seq root storage {world-bytes disk-total-bytes disk-free-bytes}}` (which world this is, how much storage its data directory occupies and the total/free space on that disk, and the oldest seq its log still holds: `world/name` from `KE_WORLD_NAME` or the host name, `world/url` from `KE_PUBLIC_URL` or `ws://<host>:<port>/ws`, both Fields on the world Principal), and `pair.claim {pairing-code label?}` → `{token secret principal world}` (`pairing-code`, not `code`: `code` is a JSON-form discriminator): a live, unclaimed pairing code (from `pair.begin`) is consumed for a fresh credential of its Principal; a wrong code answers `err unauthenticated`, the fifth wrong code within a minute on one socket `err rate-limited` and the socket is closed.

Reattach: `attach {token session subs {<sub-id> <acked-seq>}}`.
- Known session (within the 300 s TTL after the socket closed): subs are restored with each cursor set to the client's acked seq, reply `resumed true`.
- Unknown session: reply `resumed false` with a new session id; the client re-subscribes with `after = acked`.
Both paths deliver exactly `acked+1 .. head`. The client's acked seq is authoritative. The one exception is a log pruned past `acked`: the world cannot deliver what it no longer has, so the `attach` answer carries `gaps {<sub-id> <first-seq>}` and the sub's next push `gap`.

A client may choose not to hold the whole log. The listener's `history` sub starts at most 2,000 events behind the head on a process's first subscribe, moving a stale cursor forward by subscribing the same sub again, and pages older rows with `history {before last}` as History is scrolled. Folds start from the snapshot either way. Within a process a reattach stays contiguous unless a push says `gap`: then the listener folds again from a snapshot.

`detach {}` closes the session and appends `detached`.

## Subscriptions

`history {stream after? limit?}` pages one stream after `seq`, oldest first; `limit` defaults to 256 and is clamped to 1000 (a larger limit is never refused). Without `stream` it pages the whole log, every event filtered by resolution of its streams for `read`. With `snapshot true` the answer opens with `snapshot {seq objects total next?}`: the resolvable state of every Object at `seq` (each in the shape `read` returns: class, names, visible Fields), the index pinned so `events` is the tail after `seq` (256 of it; subscribe `after snapshot.seq` for the rest). In snapshot mode `limit` is objects per page (default 400, at most 1000 or 2 MiB) and the answer's top-level `next` is the object cursor (a string `<seq>:<last id>`) to pass back as `next` (or `cursor`); every page of one boot is the fold at the first page's seq (the world keeps that pinned a minute); no `next` means the last page. One `resolved` event with name `*`, the count and the seq records the read. A client boots from the snapshot and subscribes `after snapshot.seq` instead of replaying from 0. `history {last n, before?}` is the newest page: the `last` visible events (default `limit`, clamped) with seq below `before` (default past the head), ascending, with `prev` (the first seq of the page) to pass as `before` for the page before it while older visible events remain; a model that wants "the latest" reads this rather than `after 0`.

`subscribe {sub stream?|streams? kinds? after? window? query?}` returns `{sub as-of}`. A subscription is a cursor over the log, never a queue. The world pushes events with `seq > cursor` that match the filter and that the session principal resolves, at most `window` (default 256) unacknowledged at a time, coalesced into pushes at most every 16 ms. Each push carries `from`, the cursor the page was read from: seqs between `from` and the first event were filtered by resolution, not lost, so a client paints a hole dim and a gap red only when a push's `from` is above its last acked seq. The client acknowledges with `{ack <sub> seq}`; the world advances `acked` and continues. When the window is full the next push carries `lagging true` and the sub resumes on ack. A cursor below the first seq the log holds moves up to it once, and that push carries `gap <first-seq>`, with no events if there are none: seqs below it are gone, not filtered, so what a client folded is stale until it takes a snapshot. A `subscribe` with such an `after` also answers `gap`, and so do `history` and `history {stream}` with such an `after`; `history {last before}` answers `gap` when its page reaches the first seq, with no `prev`. A first `subscribe` without `after` starts at the current head; use `history` for the past. Acks are transport state and are not logged.

## Ops

| Op | Args | Result | Sprint 0 |
| --- | --- | --- | --- |
| attach | token, principal?, session?, subs?, client-kind? | session, principal, resumed, seq, gaps? | yes |
| detach | | | yes |
| resolve | name, op, watch? | object, class, or nothing | later |
| read | object? or name?, field?, seq?, depth?, range?, bytes? (false leaves bytes payloads out) | state map with reference tags; a mounted name materializes its file Object; range [first last] pages lines of a text or bytes Field: the given field (k, with k-range, k-lines, k-range-truncated beside it), else file/text (file/range, file/lines) | yes; `object/blobs` summarises every bytes Field (size, sha256, mime when a sibling mime Field says, inline when the bytes were inline) so a client can skip a large payload |
| history | stream?, after?, limit?, snapshot?, cursor?, before?, last?, since-fork?, thread? | events, next?, prev?, gap?, snapshot?, fork?, summary?, thread?, asked-by?, count?, messages? | yes |
| subscribe | sub, stream?/streams?, kinds?, after?, window?, query? | sub, as-of, gap? | yes |
| unsubscribe | sub | | yes |
| search | text, limit?, kinds? (field/message/object), since? (a seq) | hits [{object field? name? seq snippet score kind}], total | yes: substring and word match over string Fields, Message texts and bound names the caller may read (visible Fields only), newest first then by score; `limit` clamped to 100; one `resolved` event (name `search:<text>`, op read, count) records the read; the named action `search.find` gives loops the same |
| present | to, route, kind (text/table/tree/graph/image), title, data, about?, in-reply-to? | serialization, message, seq, truncated | yes: a view Serialization sent as a Message of kind view; the named actions view.present, view.table-of, view.graph-of build the data |
| bind, unbind | namespace?, name, object | seq | yes |
| set-field | object, field, type?, value? or from {object or name, field?, range?}, reader?, visibility? | seq | yes |
| send | to, route, text, refs? (see References), tags?, in-reply-to?, kind? (`view` only: a Message naming a view Object a person authored), view?, reasoning? (`on`/`off`: the sender asks the recipient's loop to reason, or not, on this Message; a line starting with `/think` or `/fast` in the text says the same) | message, seq | later |
| move | task, to, evidence? | seq | yes |
| evaluate | working-set, model, route?, policy?, depth? | evaluation | later |
| cancel, release, fail | evaluation | seq | later |
| grant | grantee, scope, ops, delegable?, until? | grant, seq | yes |
| revoke | grant, reason? | seq | yes |
| arm, disarm | arm: trigger (an existing Trigger) or predicate (schedule every-ms → interval, state {object field} → on-change) with as?, once?; disarm: trigger | trigger, seq / seq | yes: :write on the Trigger; a Trigger is created with `act create` on class trigger {kind every-s at watch route to text armed max-fires unless-empty name} |
| serialize | working-set, model, depth? | serialization, size, hash | later |
| act | class?/object?/action, args | result, object?, seq? | yes: `create` for object, principal, group, model, adapter, route (`members?`/`to?` set `route/members`), task (a Principal's `token` is returned only to a session; `token-file` always); `run` {cmd cwd? timeout-s?} on a host Adapter; `index` on an fs Adapter; `cancel` on an Evaluation, or on a Principal for its current wake (resolution for `evaluate` or `write`; a Message's sender may cancel the Evaluation answering it); result {evaluation state elapsed-ms}, `err conflict` when not running; and with neither class nor object a **named action** `{action "<name>" args}` resolved as `action/<name>` for `evaluate`, validated against its `action/schema` (`err invalid-args`, `err unknown-action`): `principal.create` (`template "developer"` + `subtree` (+ `check`) fills the shipped developer charter and mounts `harness/<subtree>` for the new Principal as `<name>` in the same action; `charter` is then optional), `workspace.mount`, `route.create`, `route.join`, `harness.index`, `task.run` {adapter cmd cwd? timeout-s? background?}, `task.list` {state?}, `task.cancel` {task}, `task.approve` {task note?}, `task.deny` {task reason} (decisions 2026-10-30); `model.create` {name url model-id key-env? reasoning? residency? price-in? price-out? context-tokens? label?} (the key is never an arg or a Field: `key-env` names the environment variable; `err secret-in-args` for a literal credential; the Model is probed at once), `model.probe` {model}, `cost.report` {by? since? to? route?} (a table view of what Evaluations cost by principal, model or day), `search.find` {text limit? kinds? since?}; `token.issue` {principal label ttl-s?} (delegate or write on the Principal; the secret only to a session, a loop gets `secret-written-to` under `data/tokens/`), `token.revoke` {token reason?}, `token.list` {principal? to? route?} (rows in the result, a table view when there is an audience), `pair.begin` {principal label ttl-s?} (session only, `err session-only`; a six-digit code for `pair.claim`), `world.link` {name url key-env?/token-file?} (an Adapter of kind `world` bound `world.<name>` and a bridge Principal `bridge.<name>` on a Route with the caller), `world.send` {world to? route? text timeout-s?} (the line goes to `to` on that world as the Adapter's credential; the answer arrives later as a Message from the bridge, in-reply-to what was being answered). Inside a loop an op the Principal's `policy/approval` names answers `err approval-pending {task summary}` and waits as a pending approval Task for a person (approval gates) |

A background `task.run` returns as soon as its Task is active. The command has the same timeout and durable output, exit, duration and final-state Fields as a foreground run. On completion it sends the owner one `task-result` Message (`Task <title> finished: exit N in <duration>`), which wakes the owner's loop; `task.list {state "active"}` lists running owned Tasks and `task.cancel` forcibly stops an active owned Task.

Every op not marked yes is registered and answers `err not-implemented`, so the surface is enumerable from day one. A Route's membership is explicit: `route/members` (a set of Principal refs) is set from `members`/`to` at creation and maintained by the world whenever a Principal's `principal/routes` gains or loses the Route, so a client draws Route edges from it rather than inferring them. `act {class action create args}` is the only way to create an Object (ADR 0008); it appends `created`. Named actions are Objects bound `action/<name>` in the world namespace (`action/name`, `action/schema` as EDN text, `action/handler`, `action/gated`, `instructions/own`); a Principal calls the ones it resolves for `evaluate`, and the Acting section of its Serializations lists them. `grant` with ops outside `read reference transmit write delegate evaluate create` answers `err invalid-ops`.

## Models and cost

A Model's `model/residency` says where inference runs: `local` is hardware controlled by the owner, including its LAN; `external` is hosted inference; `tenant` remains for tenant-isolated inference whose deployment is neither asserted local nor generally external. Model creation defaults to `external`; `local` is always explicit. `KE_MODEL_RESIDENCY=local` makes the installed root Model local when its configured endpoint is owner-controlled. A Model carries its locator (`model/base-url`), model (`model/id`), the name of the environment variable holding its key (`model/key-env`; the key itself is never a Field), its prices per million tokens (`model/price-in`, `model/price-out`, absent until set: unpriced, and `evaluation/cost.priced` is false), `model/context-tokens`, `model/label`, and `model/status` (`ok` | `unreachable` | `unauthenticated` | `unknown`) with `model/available` and `model/probed-at`, kept by a probe (`GET <base-url>/models`) the scheduler runs every five minutes and the action `model.probe` runs on demand. A world born before the catalogue gets label, prices and status from kernel-zero's later step `model-catalogue`.

When an Evaluation is done its `evaluation/cost` Field holds `{tokens-in tokens-out price-in price-out amount}`: the Model's usage and the money at the Model's prices at that moment. The running books `cost/evaluations`, `cost/tokens-in`, `cost/tokens-out`, `cost/total` and `cost/since` are kept on the initiator, on the Model and on the world Principal, written by the world as actor. `cost.report` folds the Evaluations the caller may read into a table view by principal, model or day.

## Threads

`history {thread <root Message id>}` answers `{thread asked-by count messages [...]}`: the thread's Messages oldest first in the shape `read` returns, only those the caller may read; `events` is empty in that answer. An id that is no Message answers `err unresolved`.

A Message of kind `request` is a Principal's reply to its creator after ops of the wake failed `unresolved` under the Principal's own name; `message/lacking` lists them as `{op target args err}` so the creator can grant, mount or bind what is missing.

A Message sent from a session carries `message/session` and, when the session attached with a token
Object, `message/device`, that token's label. The world sets both; they cannot be sent as args. A
loop's Message carries neither, so a client tells a person at a device from an agent's loop.

Every Message carries `message/thread`, the root Message of its conversation (a Message that
answers none and continues no wake is its own thread), and `message/asked-by`, the thread root's
sender. `send` with `in-reply-to` inherits the parent's thread; a `send` from inside a loop with no
`in-reply-to` inherits the thread of the Message the loop is answering. Inside a loop, a `send`
whose recipients, Route and text repeat a send already made in the same wake answers
`{message <the earlier id>, duplicate true}` and sends nothing. Models take
`model/reasoning-max-chars` (default 20000): a call whose streamed reasoning passes it fails with
`reasoning exceeded N chars (model/reasoning-max-chars)`.

## Other worlds

An Adapter of kind `world` (`adapter/binds :world`, `adapter/locator` the other world's ws URL, `adapter/key-env` or `adapter/token-file` naming where its credential lives, never a Field value) stands for another world, reached over this same protocol as the Principal that credential authenticates there. `world.link` makes it with a bridge Principal `bridge.<name>`: a process Principal without a Model whose loop forwards a Message on its Route to root on the other world (`send-and-await!`: attach, resolve the recipient by name, a Route both share, `send`, wait for the reply in-reply-to) and relays the answer back on the same Route in-reply-to the Message; `world.send` does one such round trip for any Principal that evaluates on the Adapter, the answer arriving as a Message from the bridge. One round trip, no retries; a failure is the bridge's text `[<name>] could not deliver: …`.

## Guarantees

- Every write returns the `seq` of the event it appended. A client showing a pending write replaces it with the pushed event at that seq.
- Reattach never loses or duplicates an event the client acknowledged.
- A push never contains an event the session principal does not resolve.
- A dropped connection costs nothing on the world beyond the session record until its TTL.

## Forks

`history {since-fork true, after?, limit?}` in a fork answers `{events, fork {forked-at, parent}, summary {events, by-kind, objects-created [{object class}], fields-set {object count}, messages, evaluations, first-seq, last-seq}, next?}`: the fork's own events (seq > forked-at) through resolution, paged. In a world that is not a fork it answers `err conflict`. The named action `world.diff {after?, limit?}` returns the same for loops. Every event may carry `event/from-fork` (a fork id string) and `event/fork-seq` when it was promoted from a fork.
`act create object` accepts typed create-time Fields beside the plain map: `fields {"image/bytes": {"type": "bytes", "value": "<base64>"}, "image/mime": "image/png"}`; a typed Field is set after `created` with its declared type (bytes decoded, blobbed above 4 KiB), so an attachment is one op.

## Cards and their expiry

An approval card (`message/kind approval`) carries `message/expires-at`, copied from its Task's `task/expires-at` at creation, so a client shows the time left before it has read the Task. Messages between loops at hop 3 or more that only acknowledge (at most 200 characters, no question, opening with thanks/noted/acknowledged, or a quiet reply) are not sent: the text stays on the Evaluation, like a quiet heartbeat. Every Serialization opens with `world <name> at <url>` (and the fork, when the world is one), so any Principal can say which world it is in.

## The queue

While a Principal's loop runs a wake, later wakes wait in line. `loop/queue` on the Principal lists them, next first: `{message, from, at, kind, wake?}`, ids and the instant as strings. A wake that starts at once never appears there.

- `act {action "loop.dequeue", args {principal, message}}` removes a waiting wake; the Message gets `message/dequeued true`. The Message's sender may, or anyone with `write` on the Principal. `err conflict` when it is not waiting.
- `act {action "loop.reorder", args {principal, message, position}}` moves it; 0 is next. Result `{principal, queue [message ids]}`. Same rights.

Waiting wakes survive a restart.

## Steering

`send {to, route, text, steer true}` redirects the recipient's running wake instead of waiting behind it. `to` names one recipient.

- A wake is running and the sender may steer it: result `{message, seq, steered true}`. The Message has `message/kind "steer"` and joins that wake; it starts no wake of its own. A Model call in flight ends with `evaluated {state cancelled, by, partial-chars, reason steer}`, ops already run stand, and the same iteration starts again, with the text at the top of the Serialization. `message/steer-applied` names the Evaluation that first carried it.
- Nothing is running: an ordinary Message, `steered false`.
- The wake ended as the steer arrived: `steered false, next-wake true`; it is the Principal's next wake.
- The sender is neither the wake's asker nor holder of `evaluate` or `write` on the Evaluation or the Principal: `err not-allowed`. Send without `steer` to queue. A Principal does not steer its own wake.

A cancel beats a steer. A call cut either way leaves `evaluation/partial` and `evaluation/thinking`.
## Conversations

A conversation is a `message/thread`. A person's plain line with no `in-reply-to` joins the newest thread they asked on that Route while its last Message is younger than `serialize/conversation-idle-s` (on a recipient, default 1800), `in-reply-to` the last thing someone else said.

`history {conversations true, principal?, limit?, before?}` answers `{events [], conversations [{thread route participants title first-at last-at last-seq messages tokens-in tokens-out compressed}], next?}`, newest first, only threads whose root the caller reads. `before` takes a `last-seq`. The title is `conversation/title` on the thread root, else the first line of its first Message. `act conversation.rename {thread, title}`, a person's own session only, writes the explicit title on a writable root Message.

A whole-line `/word` from a person is a Message of `message/kind directive` with `message/directive`, answered by a `status` Message `in-reply-to` it, and it never wakes a loop:

| Directive | Effect |
| --- | --- |
| `/new` | Closes the open conversation; the next line starts fresh |
| `/compress [note]` | Compresses the open conversation now |
| `/resume <thread>` | Reopens a thread of yours on this Route, by id or its first characters (`message/resumes`) |
| anything else | The status lists the directives |

`/think` and `/fast` stay on a line and go to the model. `/Users/x` is a path, not a directive.

Compression is an Evaluation by the recipient's Model with reasoning off, using `serialize/compressor` on the Principal or the shipped default, over everything but the newest `serialize/messages-verbatim`. It writes on the thread root, with `via` the Evaluation: `conversation/summary`, `conversation/from`, `conversation/through` (seqs). The Serialization renders the summary in place of what it covers. A later summary covers the earlier one. Nothing is deleted. A wake compresses first when the uncovered conversation passes `serialize/compress-at` tokens (default 70 percent of `serialize/budget-tokens`, 0 off), recorded as `compressed` in `evaluation/context`.

## Where a Principal is

The world line opens every Serialization: `world <name> on host <host>, you run here at <url>; local paths are reachable through <shell adapters>`. `world/host` is a Field on the world Principal. `read` of `/path`, `~/path` or `<host>/path` answers `err unresolved` with the exact `act run`. `world.link` and `world.send` to this world answer `err conflict`.

## Kinds

`class/description` on a schema Object says in plain English what that kind of thing is; `class/kinds` on `schema.Adapter` does so per kind (`fs`, `shell`, `world`, `listener`). Clients read these rather than keeping their own words.
## References

A reference to an Object, a Field or an event is structure beside text, never markup in it. A text
Field holds the display text; `<field>/refs` beside it holds spans. `message/text` keeps its spans in
`message/refs`; `conversation/summary` in `conversation/summary/refs`; a view's title in
`view/title/refs`, its markdown in `view/data/refs`.

A `Ref` is `{start end kind object? field? seq? name?}`: `start` and `end` are offsets in Unicode
code points of the text, `kind` is `object`, `field` or `seq`, `name` is the name it was shown under.
`message/unresolved-refs` lists what a model wrote that resolved to nothing; that text stays literal
and the model is told on its next wake.

`read`, `history`, the snapshot and every push hand a reader only the spans whose target it can
resolve. The text is the same for everyone who can read it; a reference it cannot follow is just
words, so a span never tells a reader that an Object exists.

`send {text, refs}` carries a person's `@` mentions: each span is checked against the text and
against the sender's own resolution (`invalid` when it does not fit), and the Objects it names join
`message/tags`, so tags and refs agree. A Message with no refs is plain text, as every Message
before them.

Inside a prompt ids are handles: the shortest hex prefix unique within that Serialization, two
characters at least, kept in `serialization/ids` (`{handle id}`, blobbed when large) and reused for
the whole wake. A model writes `[[a1]]`, `[[a1.loop/stalled]]` and `[[seq:680]]` in text, and `a1`
where an op takes an id. A full id still works. A handle the wake's table does not hold answers
`err unknown-id`, with what it could have meant, and is never guessed at.

## Off-the-record sessions

A person can run under a process-driven Principal and read what it can read, keeping nothing. Three calls, through `act` with no object and no class, from a session of a person-driven Principal only; anything else answers `err session-only`. They are not named actions: they are never bound and never listed, so no model is told they exist.

| Call | Args | Result |
| --- | --- | --- |
| `scratch.begin` | `principal` (id or name); the caller needs `evaluate` on it | `scratch, principal, forked-at, route` |
| `scratch.send` | `scratch, text, reasoning?` (`on` or `off`, kept by the session until changed, default off) | `scratch, message`; `err conflict` while it is still answering |
| `scratch.stop` | `scratch` | `scratch, stopped`: ends the model call, the session stays open |
| `scratch.end` | `scratch` | `scratch, principal, messages, tokens-in, tokens-out, duration-ms, reason` |

A scratch is an in-memory fork of the world at `forked-at`: its index starts from the world's db, its log and blobs live in memory, and it is forgotten at the end. It also ends when its session is gone past the session TTL, after `config :scratch-max-s` (default 7200), and at world stop. Only the session that opened it may use it.

Its events reach that session alone, as push frames with sub `scratch-<id>` and an extra `scratch` key, without `field-read` and `resolved` rows, and they are not acknowledged. The listener must accept them without treating the sub as stale.

Inside, reads go through normal resolution as the Principal, mounts included, and Field and memory writes land in the fork and vanish. The allow-list is `read resolve history search`, `set-field` on an Object not backed by a file, `send` and `present` to the person only, and the actions `search.find view.present view.table-of view.graph-of cost.report`. Everything else answers `err scratch-readonly`, including `act run`, `task.run`, every shell, `world.send`, `grant`, `bind` and `token.*`.

Two things are not off the record. The Model still receives the prompt: the world keeps nothing, but modelhost or any other model server sees the text, and what a server logs is outside the world. And `scratch-began {scratch principal forked-at}` and `scratch-ended {scratch principal messages tokens-in tokens-out duration-ms reason}` go on the real log on the person's stream, with no text, Field names or paths, so that nobody, root included, can read through a Principal invisibly. World Field `policy/scratch-audit` false turns them off only when a person set it.

## Approval rules

`policy/*` Fields are written only from a person's own session; anything else answers `err person-only`, root included. Only a Task's `task/approvers` decide it.

A Principal's `policy/approval-rules` is a list of rules: `{id, action, match, until?, uses-left?, used, by, at, note?}`. `action` is the gate word: `task.run` and `shell` are one family, also `fs:write`, `grant` or a named action. For a shell run, `match` is `{programs?, preset?, args-prefix?, paths?}`, with presets `read-only` and `test`. A command is tokenized without a shell: any of `; & | > < $` a backtick, a newline, a backslash, parentheses or braces matches no rule, and every path-like argument must stay inside the Adapter's root after normalisation and through symlinks. A rule ends at `until` or when `uses-left` reaches 0.

A rule-admitted op creates the same Task a manual approval leaves: `task/kind "approval"`, state approved then done or failed, `task/decided-by` the rule's author, `task/rule` its id, `task/ran`, `task/exit`, `task/result`. No card is sent and the result returns in the same wake.

Person-only actions, from the person's own session:

| action | args | result |
| --- | --- | --- |
| approval.rule.add | principal, action, match?, until?, uses?, note? | principal, rule |
| approval.rule.remove | principal, rule | principal, removed |
| approval.rule.list | principal?, to?, route? | columns, rows, count, view?, message? |
| task.decide-many | tasks? or filter {requested-by?, action?, thread?}, decision approve or deny, reason? | count, decided, results [{task, ok, state or err}] |

| approval.unsafe | principal, minutes (1 to 480), except? | on, until, except, by, at, principal |
| approval.unsafe.off | principal | principal, on false |

`task.approve` also takes `remember {match?, until?, uses?}`: approve this one and add a rule, returned as `rule`; without `match` the rule names the command's program. `approval.rule.test {principal, cmd, rule? or match?, adapter?, cwd?}` is a dry run answering `{matches, reason, rule?}`.

Unsafe mode lets one process Principal's gated ops run without a card for a limited time. `approval.unsafe` writes `policy/unsafe {on, until, except, by, at}` on that Principal; there is no form for several Principals and no form without `minutes`. A person needs write on the target, and a loop or a process's session answers `err person-only`, on itself or anyone else. `set-field` of `policy/unsafe` or `world/unsafe` answers `err conflict`, and so does `field.clear`.

While it is on, a gated op that Principal asks for runs like a rule-admitted op: `task/rule "unsafe"`, `task/decided-by` whoever armed it, no card, the result in the same wake. A card is still raised when:

- the op is on `except`. The default is `grant`, `object.retire`, `principal.create`, `token.*`, `policy/*`; a trailing `*` is a prefix, and `except` is matched against the gate word, the op, the action, the handler it reaches and the Field a `set-field` names. Given, `except` replaces the default.
- the action has `policy/never-unsafe` true, which a person sets and which no `except` drops.
- the sender of the wake Message, or whoever started its thread, is not the Principal itself, a person or another armed Principal. That covers a worker's reply too. An op with no wake Message also gets a card.

The mode covers only the Principal it is on. A standing rule that fits is used first, so its Task names the rule. Off-the-record sessions are unchanged: nothing is gated or admitted there.

The mode is checked at use, and the scheduler sweeps once a minute: it clears the Field, the world as actor, and tells the person who armed it with a Message of kind `status`. Arming and ending are `field-set` events on the Principal; ending leaves the Field out of the fold with `previous` on the event.

`world/unsafe` on the world is a list of `{principal, until, by}`, derived on arm, off and expiry. Any session may `read {object <world>, field "world/unsafe"}` and subscribe to `{stream <world>, kinds [field-set]}`, which delivers this Field's changes without a Grant on the world. The rest of the world Object still needs one.

## Grant scopes

A Grant's `scope` is a set of ids, `everything`, or a filter: `{class?, prefix?, field-prefix?}`. Every constraint named must hold, and a filter naming none is refused. Filters are evaluated at resolution, so an Object that comes to match is covered. Delegation stays strictly narrower: a delegable filter covers a filter at least as tight, and a loop's `grant` still passes the approval gate.

| scope | covers |
| --- | --- |
| `{"class": "task"}` | every Task |
| `{"prefix": "grid"}` | every Object with a name starting `grid` |
| `{"class": "adapter", "prefix": "grid"}` | grid's Adapters |
| `{"field-prefix": "notes/"}` | every Object holding a `notes/*` Field |

## Delete: retire, restore, clear (decisions 2026-10-31)

The log is append-only, so delete means retire. Named actions:

- `object.retire {object, reason?, force?}` needs `:write`; result `{object retired names dependents?}`. Names are unbound (`unbound`, then `retired` with `{reason? names}`), so they can be reused. The Object then resolves only for `read` by id, whose state carries `object/retired true`. `err conflict` for root, the world, the first person, a Grant, a schema Object, and for a Model, Adapter or Route still used by a live Principal unless `force`. `action/gated` is true: a loop's retire waits for a person; a person's session is direct.
- `object.restore {object}` (id) reverses it (`restored` with `{names}`); result `{object restored names names-taken?}`. A name taken meanwhile is reported, not stolen.
- `field.clear {object, field}` needs `:write`; a `field-set` with `cleared true` takes the Field out of `read` and the Serialization, and the log keeps its history. `err conflict` for structural Fields, and `policy/*` from anyone but a person.

A retired Route refuses `send`, a retired recipient refuses delivery, a retired Adapter refuses `act`, a retired Principal cannot attach, its wakes are dropped (`loop/dropped` counts them) and its running Evaluation is cancelled, and a retired Trigger stops. `history {snapshot true}` leaves retired Objects out unless `retired true`; event history still shows their events, and `search` skips them.

## Working in mounted files

Actions, open to every Principal; reach is the mount's Grant.

- `code.search {pattern, regex?, path?, glob?, limit?}` answers `{matched, files-read, files-matching, results [{path, hits, lines [{line, text}]}], next, more?, truncated?}`. A plain phrase matches however it is spelled. When no line has it whole, lines with any word stand in and `matched` says so. `next` is the read that follows from the first hit.
- `code.outline {name?}` answers `{name, lines, outline [{line, text}]}` for a file, or `{name, dirs [{name, files}], files}` for a directory.
- `file.edit {name, new, old?, all?, at?}` answers `{name, created, bytes, diff}`. `old` must match exactly once, or `all` true; `err no-match` and `err ambiguous` change nothing. With no `old`, `at` top or end inserts whole lines. With only `new`, a path that does not exist is created. Gated as `fs:write`.

## Projects

`act projects {}` answers `{projects, next}`, one row per mount the caller can read, other than the harness: `{name, path, owner, state, contents, about?, git?, tests?, last-run?, last-said?}`. `git` is `{branch, last-commit, uncommitted}`. No card, and any Principal may call it.

`act project.add {name, path, charter?}` makes a Principal named `name` with an fs mount, a shell, a Route from the caller and a Grant on the prefix `name`, and answers `{principal, name, route, route-name, mount, path, shell-name, next, readme?, tests?}`. It always takes a card, unsafe mode included, and needs a Grant of scope everything. A `charter` under 40 chars gives way to the README. Refused before the card: `invalid-args` for a path that is no directory, `conflict` for a name in use.

The tools `projects`, `add_project` and `test` reach these from a loop. `test` is `task.run` with the shell's `adapter/test`.

A shell command into a directory the waking Message or its thread names, and that is under no mount, answers `err not-mounted` when the Principal may add a project. An op behind a card stays unrun; `task/held` lists it and the approval result repeats it.

## Activity marks

A client shows one mark per agent for its person, derived from the log: needs you (a pending card it may decide, or a loud unread notification from that agent), failed (`loop/stalled` set on an agent this person spoke to, after what they have seen, except a person's own cancel), finished (a Message to this person, other than cards and other plumbing, after what they have seen), working, idle, inactive (no Evaluation yet).

`view/seen` on a person's Principal is a map of agent id to the log seq they have seen, written when that agent's chat is in front of them. Only a person's own session writes it (`err person-only`), so no agent can mark a person's turn read, and every client of that person agrees. A subagent answering its parent sends no Message to the person, so it raises no mark: one job gives one mark, from the agent that answers them.

## Notifications

`act notify {text, level?, object?}` tells the people who own the sender, with no Route. `level` is `info` (default), `attention` or `urgent`. `object` must be something the sender can read; naming it gives the recipient no access to it. Any Principal may call it (`action/public`). Six a minute per Principal: past that the answer is `{dropped true}`, and a summary with `notification/dropped` follows when the window passes. Nothing wakes and no authority passes.

A client finds the collection from `read {object <world>, field "world/notifications"}`, which any session may do, and subscribes with `{stream <collection>, kinds [created field-set]}`. The honest filter passes a person only what is addressed to them.

| Field | Value |
|---|---|
| `notification/to` | the people it reached |
| `notification/from` | the sender |
| `notification/level` | `info`, `attention`, `urgent` |
| `notification/text` | at most 500 characters |
| `notification/at` | ISO instant |
| `notification/object` | optional id |
| `notification/dropped` | on a summary, how many |
| `notification/read-by` | people who marked it read |

`act notify.list {unread?, limit?}` answers `{collection, unread, rows}`, yours only. `act notify.read {notifications?}` marks the ones named, or all of yours.

## The manual

`read {object <world>, field "world/manual"}`, which any session may do, names the collection. Every Principal reads topics through public actions: `doc.show {topic}` answers `{topic title type book format body previous? next?}`, or `{found false candidates}`. `doc.candidates {text, type?, limit?}` answers `{candidates [{topic title type book snippet}] total}`. `doc.overview {topic?}` answers a paragraph, or `{books}`. `doc.contents {book?}` answers `{book topics}`. `type` is concept, section, action, field or class; `format` is markdown or latex.

## Path completion

`act path.complete {mount, path?}` lists the directory `path` ends in, keeping the names its last part begins: `{mount dir stem entries}`, a directory ending in `/`. It creates no Object, so a client may ask on every key. Dot files show only when the part begins with a dot. It needs read on the mount, and stops at the mount's root.

## Changes to a Principal

A wake's first turn names Fields someone else set since `loop/seen-seq`, which the wake then moves. `act field.diff {object, field, since?}` answers `{object field changed changes by seq diff clipped?}`: `diff` is `- ` and `+ ` lines, for the last change, or everything after seq `since`.

## Choosing a Model

`act model.choose {principal, model?, id?, thread?, fallback?, clear?}`, a person's session only, says what a Principal runs on. `id` is one of the Model's `model/available`. With `thread` it holds for that conversation, in `conversation/model` on the thread root; otherwise `principal/model-choice`. `fallback true` writes `principal/model-fallback`. Each value is `{model, id?}`. An agent cannot choose for itself: those Fields answer `person-only` to anyone else. A person holds no `delegate`, so the kernel grants the Principal `evaluate` on the Model.

`policy/model-residencies` on a Principal is an enforceable list of admitted `model/residency` values. It applies to direct evaluations, conversation and Principal choices, first-choice selection, and fallbacks. An absent or empty Field admits every residency; residency is restricted only when a person sets a non-empty list. For example, `[:local]` makes that Principal local-only, while an isolated worker may explicitly admit `:external`.

`act model.chosen {principal, thread?}` answers what a wake would use now: `{model, name, id, status, plan, source, fell-back-from?, fallback?, signed-in?, usage-url?}`, where `source` is `conversation`, `principal`, `first` or `fallback`.

A Model is passed over while `model/status` is `unreachable`, `unauthenticated` or `limit`, and a call that fails that way marks it so. A fallback that is up then answers, in the same wake when the call failed, and `loop/model-note` on the Principal says which Model is standing in and why. It goes when the chosen Model is back. A fault in the request itself, such as an unsupported field, changes nothing. If the fallback is down too, the choice stands and fails plainly.

## Sign in with ChatGPT

A ChatGPT plan Model has residency `external`. Worlds that recorded an older plan as `local` relabel it once at boot with a logged `field-set`, so a `[:local]` policy cannot admit hosted inference by an obsolete label.

A person's client starts it, because a browser's `127.0.0.1` callback reaches the browser's own computer, not the world. The client opens a loopback listener, then `act model.signin.begin {port, model?}` answers `{attempt, url}`. The client opens `url`, takes the callback, and sends `act model.signin.finish {attempt, state, code?, client-id?, error?}`. The world holds the state, nonce and PKCE verifier, so the client only carries a one-time code. Only the world exchanges it. It checks state first, then the ID token's signature, issuer, audience, expiry and nonce, then that `chatgpt.tokens.use.direct` was granted, and answers `{model, name, status, models, account, first, usage-url}`. Errors: `consent-declined`, `plan-not-granted`, `sign-in-failed`. `first` true asks the client for the one-time confirmation OpenAI's guidelines require. `act model.signout {model}` revokes the session and clears the tokens, answering `{revoked}`.

The Model has `model/api` `chatgpt-plan`, and `model/catalogue` `[{slug, name}]`. Its tokens are never Fields: they live in `<data>/credentials/chatgpt/<model>.json`, owner-only, inside the directory the box denies, and they are left out of exports. Calls go to `POST /responses` with `store: false` and `stream: true`. Plan usage is not reported as numbers: the client shows `Using ChatGPT plan` and a link to ChatGPT's usage settings, and a limit reads as `model/status` `limit`.

## Subagents

`act delegate {task, charter?, deadline-s?, ttl-s?, grant?}` makes a Task with `task/due` and a temporary Principal bound to it, and sends it the Task. Answers `{delegated, principal, name, deadline-s, until, message}`. It is gated. `principal.create` takes `for` (a Task) or `ttl-s` for the same temporary Principal without the Task or Message.

`principal/ephemeral` is `{for, until, parent, route, supervisor}`, written only by the kernel. The Principal ends when its Task ends, at `until`, or when its parent goes. `until` is at most 86400 s away and never after the parent's. Its Grants are revoked and its Route and stall Trigger are retired with it. Its record stays on the log and reads by id.

`act task.progress {text, task?}` writes `task/progress` and wakes nobody. `act task.ask {text}` sends the parent a Message of kind `request`, keeps the Task open and ends the wake with no reply. The final reply of any other wake is the result, in `task/result {text, by}`, and the Task moves to `done`. A failure moves it to `failed` with `task/result {reason, by}`. A passed `task/due` sends the parent one `status` Message, sets `task/overdue-told`, and ends nothing. A parent's reply to a result goes to whoever asked for the work, in `task/requested-by`.

## Restart

`act world.restart {reason, release?}` asks the supervisor to bring the world back, on `release` (a commit, or a unique prefix, under `releases/`) or the running one. Answers `{restarting, from, to}`, then the process exits. Refused with `err unsupervised` when no supervisor started it, `err log-version` for a release older than the log, `err person-only` for a new release asked for by anything but a person's session or a person's own card. See `docs/self-hosting.md`.

`world/release` on the world is `{commit, version, log-version}`, read by any session. `world/restart` is the last request.

## The box

Shell commands and terminals run in the host's sandbox (docs/authority.md). `err unconfined` means no shell may
start: the host has no sandbox, or the Adapter's root holds the home directory. `act adapter.unconfine {adapter,
minutes}` lifts it for at most 480 minutes, a person's own session or a person's card only; it appears in
`world/unsafe` as `{principal, until, by, kind "unconfined"}`, where `principal` is the Adapter, and

`pane.share {pane, principal, mode read|type, minutes}` and `pane.unshare {pane, principal?}`, a person's own session only, open a person's terminal to one agent (`docs/panes.md`). A live share lists in `world/unsafe` as `{principal, agent, mode, until, by, kind "shared-terminal"}`, where `principal` is the pane, and `approval.unsafe.off {principal}` on it ends every share of that pane.
`approval.unsafe.off {principal}` ends it. `act adapter.confine {adapter, network?, read?}` sets `adapter/network`,
one of `model`, `none`, `all`, and `adapter/read`. `set-field` refuses `adapter/confined`, `adapter/network`,
`adapter/read` and `adapter/locator`. `act release.make {rev?}` answers `{commit, log-version}`.

## Gates on a session

A session that is not a person's, or `data/root.token`, meets the loop's gate: a gated op answers `err
approval-pending` with `data {task, summary}` and does not run. A person decides it as usual. A standing rule
admits as it does for a loop. Unsafe mode needs a wake, so it never admits a session's op.

## Pane frames

A fourth push form alongside `push`, `ack` and responses: a frame with the key `pane` is a
`PaneFrame`, one pane's stream to one session, started by `act {action pane.subscribe}`. Full
screens, deltas with `base`, resuming with `after`: docs/panes.md. New err code: `no-renderer`.

## Gateways

A message service is an Adapter with `adapter/binds gateway`; `transport.clj` is the interface, BlueBubbles the first kind. Every action but `gateway.list` and `gateway.send` is a person's own session (`err person-only`).

| Action | Args | Answers |
|---|---|---|
| `gateway.create` | `name kind url key-env? poll-s?` | `{gateway status}`, reading from now |
| `gateway.bind` | `gateway handle agent` | `{gateway handle agent route}` |
| `gateway.unbind` | `gateway handle` | `{gateway handle bound}` |
| `gateway.enable` | `gateway on` | `{gateway enabled}` |
| `gateway.list` | none | `{gateways [{gateway name kind status enabled dropped bindings}]}` |
| `gateway.send` | `text gateway? handle?` | `{gateway handle sent}`, gated, and in unsafe mode's default exceptions |

A bound text is a Message of kind `gateway` from the Adapter, with `message/gateway {handle person id chat}`. An unbound sender is counted in `gateway/dropped`, unread. `gateway/*` Fields refuse `set-field` and `field.clear`. The password is an environment variable named by `gateway/key-env`, never a Field. The model's tool is `text_back`.

## Speech

A Model's `model/stt` and `model/tts` name what its endpoint serves, `model/voice` the voice; `model.create` takes `stt tts voice`. New err codes: `no-speech-model`, `too-large`, `unreachable`. Both actions are a person's own session and append nothing.

| Action | Args | Answers |
|---|---|---|
| `speech.transcribe` | `audio` (base64, 3 MiB) `mime? model?` | `{text}`, by `POST <url>/audio/transcriptions` |
| `speech.say` | `text model?` | `{audio mime}`, base64 WAV, by `POST <url>/audio/speech` |

## Images

A Message carries images in `message/images`, each `{sha256, size, mime, name}`. The bytes are in the blob store and the type is read from them; PNG, JPEG, GIF and WebP, at most 8 MiB.

`act image.put` stores one: `{path}` or `{mount, path}` or `{name}` for a file p may read, or `{base64, name}`, in pieces under one `upload` id with `more` true until the last. It answers the attachment. `send` takes `images`, a list of attachments or sha256 strings.

`act image.read {message, index?, offset?}` answers `{mime, size, offset, base64, more}`, a chunk at a time, to anyone who resolves the Message. `act image.open {message, index?}` opens an image pane. `act image.show {message, index?}` puts an image on the agent's next turn.

`model/images` true says a Model takes images. A Serialization then holds an `image_ref` part, and the bytes go as an `image_url` data URL at the call only. A Model without it gets one line of text for each image. `evaluation/images` lists what the Evaluation had, each with `sent`.
