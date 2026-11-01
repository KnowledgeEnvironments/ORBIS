# world

The Clojure backend of ORBIS: the log, the fold and index, resolution, the
wire protocol server, kernel-zero boot, the serializer, Evaluations on Models and the
resident loop, actions and approval gates, panes, releases and the supervisor's side of a restart.

## Run

```
export JAVA_HOME=/opt/homebrew/opt/openjdk@25
export PATH=$JAVA_HOME/bin:/opt/homebrew/bin:$PATH
cd world
clojure -M:run                      # ws://127.0.0.1:7777/ws, data in ../data
clojure -M:dev:nrepl                # nREPL on 7888; (go) (halt) (reset) from the user ns
clojure -M:test --focus :unit       # unit tests
clojure -M:test --focus :exit       # attach, drop, reattach, no gap
clojure -M:test --focus :kernel-zero  # spec 17.3 steps; 1, 2, 3, 5, 6 green on the :stub Model, 4 not built
```

With `KE_MODEL_URL` set, `root.model` is a real Model (OpenAI chat completions) and root
answers Messages with it; the test suites never set it, so they run on the :stub Model and
never touch the network. A manual end to end run:

```
export KE_MODEL_URL=http://localhost:8000/v1 KE_MODEL_ID=<model> KE_MODEL_KEY=<key> KE_PORT=7790 KE_DATA=/tmp/world-e2e
clojure -M:run
# attach as root with data/root.token (or as the first person with person.token), then
# resolve {name "root.route" op read}; subscribe {sub "inbox" stream <root> kinds #{delivered}};
# send {to #{<root>} route <root.route> text "what do you know?"}; the reply is the next delivered.
```

On an empty data dir the first start runs kernel-zero and writes `data/root.token` (mode 0600),
the bearer token root attaches with: `attach {token <contents of root.token>}`.

## Environment

| Var | Default | Meaning |
| --- | --- | --- |
| `KE_PORT` | `7777` | WebSocket port (`0` picks a free one) |
| `KE_DATA` | `../data` | data dir, relative to `world/` unless absolute |
| `KE_FSYNC` | `batch` | `batch` (one force per group commit), `always` (per event), `never` |
| `KE_FORMAT` | `transit` | default wire form when `?format=` is absent |
| `KE_LOG` | `info` | timbre level |
| `KE_MODEL_URL` | | base url of the chat-completions endpoint; set, kernel-zero creates `root.model` as a Model on it (residency `:tenant`); unset, `root.model` is the :stub Model |
| `KE_MODEL_ID` | `default` | the model name sent in each request |
| `KE_MODEL_KEY` | | bearer key; read from the environment at each Evaluation, named by the Model's `model/key-env` Field, never written to the log |

## Layout

```
src/knowledge/world/
  config.clj  system.clj  main.clj      env -> config map; start/stop a map of components; -main
  log/        frame segment index blob writer reader core   the only state (ADR 0003, 0006)
  fold/       core (multimethod per event kind -> tx-data), objects (state, names, lookup)
  index/      core (Datascript, snapshot + replay, catch-up), snapshot
  resolve/    core (resolves?, resolve!), mount (stub)
  act/        core: create! bind! set-field! read! grant! revoke! as (world principal args); the wire ops,
              kernel-zero and the loop all come here (one access path)
  route/      core: send! (Message Object, sent, delivered)
  serialize/  core: render, serialize! (spec 8.1 to 8.3), the :openai-chat bytes
  evaluate/   core: evaluate! (queued, running, done or failed; the adapter on a thread)
  loop/       core: the resident loop v1 (spec 9.8); ops: the model acts through protocol ops
  act/        core: the write primitives; perform: act actions on Objects (run, index)
  adapters/   fs (mounts), shell (run), harness (the index), model_api (Models)
  protocol/   codec frames session subscription coalesce ops server
  boot/       kernel_zero (first objects; step! for 17.3 steps 1-6; run-steps!)
  adapters/   model_api (:stub and :real, built from a Model's Fields), shell fs container (stubs)
resources/knowledge/world/boot/instructions/   charter.md ground-rules.md protocol.md route-template.md
test/knowledge/world/   fixtures.clj ws_client.clj and the suites
```

Data dir: `world.edn`, `log/seg-<first-seq>.klog` (+ `.idx` cache), `blobs/sha256/ab/<hex>`,
`snapshots/snap-<seq>.snap`, `root.token`.

## How it fits together

- **Log.** One writer thread assigns global seqs (log position), writes a batch in one buffer,
  forces once, delivers seqs, then notifies subscribers. Every event is validated against
  `:ke/PendingEvent` before and `:ke/Event` after seq assignment. Bytes above 4 KiB go to the blob
  store and the body carries `{:blob/sha256 :blob/size}`. Open scans the last segment: a short or
  corrupt tail is truncated; a corrupt frame followed by more bytes refuses to start.
- **Index.** Datascript rebuilt from the newest snapshot plus replay; kept current by a log
  subscription and by catch-up on every read, so an op always sees its own writes.
- **Resolution.** `(resolves? db principal name op) -> grant-id | nil`. Name forms: a bound name
  in the principal's namespace chain (own, groups, world) or `/<object-id>`. Grants reach the
  principal directly or through Groups (transitively); scope is a set, `{:prefix p}`, or
  `:everything`. Field visibility is a Grant the principal must hold. Ops append `resolved`;
  the push filter uses the pure predicate (see CONTRACT-NEEDS.md).
- **Sessions and subscriptions.** A session is transport state kept 300 s after its socket
  closes; subs are cursors over the log with a window of unacknowledged events. The pump per
  session runs at most every 16 ms and pushes only what the session principal resolves.
- **Kernel-zero.** The world Principal (id = world id, driven-by `:none`) creates root, the root
  Grant, one Object per registry schema (`schema.Event`, `schema.args.attach`, ...; fields
  `schema/name`, `schema/key`, `schema/form` as EDN text, `instructions` where shipped), the
  instruction Fields on root, and root's credential hash. Idempotent on a non-empty log. On a
  fresh log it then runs the six steps of 17.3 as root: 1 binds `root`, `root.workspace` (an
  appliance), `root.model` (from `KE_MODEL_*`, else :stub), `root.route` (a Route on the
  built-in `transport.listener` Adapter, template from `route-template.md`; also bound in the
  world namespace so others can address it) and sets `principal/workspaces|models|routes` on
  root; 2 reads the shipped instruction Fields (root's three and the schema Objects') and writes
  `instructions/own` (reader `:agent`); 3 runs one Evaluation over root's own Fields; 4 is not
  built (logged, skipped); 5 is the loop's; 6 creates the first person, bound `person` with
  `data/person.token`, holding `{:prefix "root"}` for `:read :reference :transmit`.
- **Messages.** `send {to route text tags? in-reply-to?}` needs `:transmit` on the Route
  (`resolve!`); it creates a Message Object (`message/*` Fields), appends `sent` to the sender's
  and the Route's streams and `delivered` to each recipient's and the Route's.
- **Serializer.** `serialize {working-set model depth?}` (and every `evaluate`) renders the
  Objects of the working set that resolve for the principal: a header per Object (class, id, the
  names the principal knows it by), Fields with reader `:agent` or `:both` in set order (each
  introduced by `=== field <name> (<type>)`, authored Fields with `by [[author]] at <time>`),
  references as `[[id|name|class]]` tags, inline at depth > 0 (Route input tags at depth 1),
  bytes as `[[bytes size sha256]]`. Format `:openai-chat`: the bytes are the JSON messages
  array, system = Route template + rendering, user = the Message text. The Serialization Object
  holds working-set, model, depth, format, bytes (inline, or a blob receipt above 4 KiB), size,
  sha256 hash and as-of (stream head per rendered stream); one `field-read` per rendered Field,
  `via` the Evaluation.
- **Evaluations.** `evaluate {working-set model route? policy? depth?}` resolves the Model for
  `:evaluate` (a Model with residency `:external` answers `err residency`), creates the
  Evaluation, serializes, appends `evaluated` queued (with the Serialization id), then on another
  thread running and done (outcome `{:text :usage}`, cost `{:tokens-in :tokens-out}`) or failed,
  each on the Evaluation's, the Model's and the initiator's streams; the fold projects state,
  serialization, outcome and cost onto the Evaluation. The adapter is built from the Model's
  Fields: `model/base-url` set means real mode (POST `{model messages stream false}`, bearer from
  the env var `model/key-env` names, 120 s timeout); else the :stub Model, which answers with
  the names of the Fields it read and the hash it saw.
- **Resident loop.** One thread, woken by `delivered` events seen on an in-process log
  subscription. For a recipient driven by `:process` whose `principal/routes` lists the Route:
  working set = the Message, its sender, the Message tags and the Principal itself (its
  instruction Fields), shaped by the Route's template, serialized for the Principal's first
  Model (`principal/models`), evaluated with policy `:normal`. The model may act
  (`loop/ops.clj`, decisions 2026-10-30): fenced `ops` blocks in its outcome, one JSON object
  `{"op","args"}` per line in the wire's JSON form (or OpenAI tool_calls), are validated against
  `:ke/Request` and run through `protocol.ops/handle` as the Principal, so Grants apply unchanged;
  `attach detach subscribe unsubscribe` answer `err not-allowed-in-loop`. Each iteration is its own
  Evaluation carrying `evaluation/iteration`, `evaluation/ops` and `evaluation/results` as Fields,
  and the next iteration's working set includes the earlier Evaluations of the wake, so the model
  reads what it did from the log. A turn with no ops is the reply: sent on the same Route to the
  sender with `in-reply-to` and `via` the last Evaluation. Caps: 8 iterations per wake, 16 ops per
  iteration. Loop state is Fields on the Principal: `loop/current`, `loop/iteration`, `loop/stalled`
  (the reason, empty after a good cycle); a process Principal needs `:write` on itself to keep
  them. A delivery an Evaluation produced (`event/via` set) does not wake a loop, so a reply
  cannot ping-pong. The serializer puts an Acting section after the Route template; a Principal
  reads any Object it created (`object/created-under`), which is how it renders its own Evaluations.

## What is stubbed or not built

- Ops `search present move cancel release fail arm disarm` answer `err not-implemented`; `act`
  knows `create` for object, principal, group, model, adapter, route.
- Kernel-zero step 4 (the heartbeat Trigger) is not built; `arm` is not implemented.
- The loop does not carry refs between turns, apply Route hard rules or retry; streaming
  partials are not emitted. Budget Triggers are not checked. The world sends no `tools`
  parameter to the Model; ops come from the fenced blocks.
- Mounts (`resolve.mount`), shell, fs and container adapters are documented stubs.
- `read` with `seq` folds the stream in place; `depth` is ignored. `query` subscriptions are ignored
  (filter by stream/streams/kinds only).
- Tokens are bearer strings, not the key pairs of spec 14.2.
