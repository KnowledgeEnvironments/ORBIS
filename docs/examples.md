
# Worked examples and backed objects

## 11. Worked examples, unix style

Every unix thing has one counterpart here. The crib first, then four cases traced event by event.

| Unix | Here | Note |
| --- | --- | --- |
| user | Principal, driven\_by person |  |
| daemon | Principal, driven\_by nil | An office appliance. |
| group | Group |  |
| file | Field, type bytes | On the workspace that holds the path. |
| process | Evaluation | With its exact input bytes named. |
| cron | Trigger, schedule |  |
| inotify | Trigger, state |  |
| pipe | Route | With a template and a format at each end. |
| env | Route template | The system prompt is `$ENV` for an agent. |
| chmod, chown | Grant |  |
| sudo | Grant with op delegate | Every use is an event naming both actors. |
| syslog | stream | One per Object. |
| tail -f | subscribe |  |
| make | Evaluation with field-read lineage | What it read is recorded, so the dependency graph is a query. |
| ssh host cmd | Evaluation on a remote shell Model |  |
| sbatch | Evaluation on a queue Model |  |
| docker run | A workspace appliance created, plus one Evaluation |  |
| root | the root agent | Infinite scope out of the box, every act on the log. |
| init, PID 1 | kernel-zero | The root agent's first Evaluations bring the world up. |

### 11.1 A container with a startup command

`docker run --rm -v src:/src builder ./build.sh`

1. A Principal `agent-one` asks the container Adapter for a workspace with image `builder` and mount `src`. Event: created, on the new appliance's stream. Its holder is `agent-one`. Its files under `/src` are byte-stream Fields, lazily read.
2. `agent-one` submits an Evaluation on the workspace's shell Model with command `./build.sh`. The Serialization is the command line plus the Route template, which here is the environment. Event: evaluated, state queued.
3. The shell runs. Each file the build reads appends a field-read event with via set to the Evaluation. Each file it writes appends a field-set event authored by `agent-one` via the Evaluation. stdout streams to subscribers.
4. Exit. Event: evaluated, state done, outcome `{exit 0, stdout, stderr, changed [/src/out/a.bin]}`, cost in seconds.
5. `--rm` is a Trigger with a completion predicate on the Evaluation, firing a retire on the workspace as `agent-one`.

The log now answers: what did the build read, what did it write, how long did it take, and run it again on the same inputs.

### 11.2 An ssh call to a remote host

`ssh build-box make -C /work all`

1. The host `build-box` is bound once by the ssh Adapter as a Model of kind shell with residency `tenant`. The Adapter holds the key. The kernel holds a Grant to the Model.
2. `agent-one` submits an Evaluation on that Model with command `make -C /work all`. Resolution checks `agent-one` has op evaluate on the Model. Serialization is the command line. State queued, then running as the session opens.
3. The host has no filesystem Adapter in this example, so no field-read lineage. stdout and stderr stream. Exit code closes it.
4. Event: evaluated, done, outcome `{exit, stdout, stderr}`. The Model's stream records the call. `agent-one`'s stream records the delegation.

Bind `/work` with a filesystem Adapter over sftp and step 3 gains lineage: `make` becomes the same thing as in 11.1, on a different box.

### 11.3 An FPGA synthesis job on a slurm queue

`sbatch --partition=fpga synth.sh top.v`

1. The slurm cluster is bound by the slurm Adapter as a Model of kind runner. Its capacity Field holds partitions and limits. Its pricing Field holds node-hour cost. Its residency is `tenant`.
2. An appliance `fpga-lab` is a workspace on the shared filesystem, so `top.v` is a byte-stream Field with a hash.
3. `ada`, an agent, submits an Evaluation on the slurm Model: command `synth.sh`, input the `top.v` Field at its current as\_of, policy one\_shot. Serialization is the script plus the input hash. State queued. The Adapter runs `sbatch` and records the slurm job id as a Field on the Evaluation.
4. The Adapter polls `squeue` and `sacct`. Each slurm state change appends an evaluated event: queued stays queued while pending, running when it starts. Nothing else in the world blocks.
5. Done. The bitstream `top.bit` lands as a byte-stream Field on `fpga-lab`'s filesystem, set via the Evaluation. Cost is node-hours from `sacct`. A hard-rule validator on the Route checks timing closure from the report before anything downstream reads the bitstream; a miss leaves the Evaluation gated.
6. A budget Trigger with a threshold on `ada`'s node-hours this week fires when crossed and sets approval\_required on `ada`.

Rerun is the same Serialization on the same Model. Diff the two bitstreams by hash.

### 11.4 A call into a long-running service

A Postgres container that stays up, and a query against it.

1. The service is an appliance `pg`, driven\_by nil: a workspace in state running, the container itself. The container Adapter created it. `pg`'s stream records start, health checks, restarts.
2. The database itself is bound by the database Adapter as a Model of kind storage on `pg`, with the schema and tables as Objects whose Grants map from Postgres roles.
3. `agent-one` submits an Evaluation on that Model with a query. Resolution checks the table Objects the query names. Serialization is the query text plus the as\_of of the schema Objects. The call runs against the live service; the Evaluation records it.
4. Outcome is a row-view Object, a Field on the Evaluation, with the rows as its content. Cost is milliseconds.
5. A state Trigger on `pg`'s health Field fires a restart Evaluation as `pg` when health goes bad. A schedule Trigger runs `VACUUM` nightly as `pg`.

A call into a service is an Evaluation on a Model the service exposes. The service lives as an appliance so its uptime, its restarts, and every call into it share one stream. `tail -f` on `pg` is a subscription to that stream.

### 11.5 A model behind a vLLM endpoint, and where contexts live

`curl http://192.0.2.50:8000/v1/chat/completions`

1. The vLLM server is an appliance `tier1`, driven\_by nil: a workspace in state running, the container with the weights mounted. Its stream records start, health, restarts, GPU memory as a metered Field.
2. The endpoint is a Model of kind model on `tier1`, bound by the model-api Adapter. Its Fields: `base_url`, `api_mode` chat\_completions, `format` the tag syntax and template dialect for this model, `residency` local, `capacity` context length and concurrency, `pricing` per token or per second. The Adapter holds any key. A hosted model is the same Model with residency external and a different Adapter.
3. An agent `ada` sends a turn on its Route. The kernel resolves her working set, renders a Serialization for the `tier1` Model, and submits an Evaluation. The Serialization is the context: the bytes, the working set, the depth map, the as\_of per stream, the hash.
4. The Adapter posts the bytes. Partial output streams as events on the Evaluation. The reply is parsed by the Route's output format. Cost lands as tokens in and out.
5. Next turn, `ada`'s loop carries a reference to the previous Serialization, not its bytes. The new working set is the Route's shaping plus the references carried, re-rendered at the new as\_of. Unchanged prefix, same hash prefix, prompt cache hit on `tier1`.

Where things live, by name.

| Thing | Lives as | Where to look |
| --- | --- | --- |
| A model | Model, kind model, on an appliance | the Model's Fields and stream |
| An API endpoint | `base_url` and `api_mode` Fields on that Model | the Model |
| The server process | the appliance itself, a workspace | the appliance's stream |
| A context | Serialization | the Evaluation that used it |
| A conversation | the chain of Serializations an agent's Evaluations reference on a Route | the Route's stream |
| The system prompt | the Route's template Field | the Route |
| What the model knew | the Serialization's working set at its as\_of | one click from the Evaluation |
| Model routing | Grant policy over Models: residency, cost, capability | the Grants |
| The prompt cache | the Model's own; keyed by the Serialization hash | the Model, by hash |

A context has a name, so two agents can share one, a person can read what an agent read, and an Evaluation can be rerun on a different Model by rendering the same working set for it.

### 11.6 What the five share

In every case the thing is bound once by an Adapter, the work is an Evaluation with a named Serialization, the result is a Field, and the lifecycle is a stream. The container, the ssh host, the queue, the daemon, and the model differ only in the Model kind and in whether a filesystem Adapter gives lineage.


## 12. Backed Objects: files, databases, stores, processes

A thing whose bytes live outside the log is a backed Object. The log holds its identity, its Fields, its events, and a receipt for its content. The Adapter holds the content, or knows where it is. Every operation on it is an action.

### 12.1 Where a Field's value lives

| Mode | The log holds | Used for | Resolution returns |
| --- | --- | --- | --- |
| inline | the literal | small values: strings, numbers, refs | the value |
| blob | hash, size | bytes the world mirrors: source files, outputs, Serializations | the bytes from the content-addressed store |
| external | model, key, hash if known, last\_seen | bytes the world does not copy: a large dataset, a table, a remote file | the bytes through the Adapter, with a freshness flag, and a field-read event either way |

A Field's mode is a property of the Field, set by the Adapter that bound it. The reader cannot tell the modes apart except by cost and freshness. Changing mode is a field-set event: mirror an external file and it becomes blob; stop mirroring and it goes back.

### 12.2 Actions, the command table

An action is a named operation an Object's class exposes: an args schema, an instruction Field for agents and humans, and the Model that performs it. Actions are Fields on the class schema, so they resolve like anything else: a Principal sees the actions it may invoke. `act` on the wire submits an Evaluation on the action's Model with the args as working set, and the outcome is parsed into events on the Object. The listener's command table of 12.3 is the set of actions that resolve. The natural-language command table is the same set with its instruction Fields rendered.

### 12.3 A file

Bound by a filesystem Adapter on a workspace.

| Field | Mode | Note |
| --- | --- | --- |
| path | inline |  |
| bytes | blob or external | hash always; content mirrored or not |
| mode, mtime, size | inline | refreshed by the Adapter |

| Action | Does | Events |
| --- | --- | --- |
| read | returns bytes at as\_of | field-read |
| lines(from, to) | returns a range, counted by the Adapter | field-read with the range |
| write(bytes) | replaces content | field-set with the new hash; the old hash stays in the stream |
| edit(from, to, text) | replaces a range | field-set with the new hash and the diff in the body |
| append(bytes) | adds to the end | field-set; the diff is the tail |
| watch | arms a state Trigger on the Field | fired on change |

`sed -i '3s/foo/bar/' config` is edit on line 3. The stream holds the diff, the blob holds the result, the Evaluation holds who and why. `grep -n foo config` is lines after a search Evaluation, or a query on the index when the file is mirrored.

### 12.4 A database

Bound by a database Adapter as a Model of kind storage. Schema, tables, and views are Objects with inline Fields. Rows are not Objects; one row is too small to name. A row-view is: an Object whose rows Field is external, with the query as its key.

| Action | Does | Events |
| --- | --- | --- |
| query(text) | returns a row-view | evaluated; field-read on each table named |
| lookup(table, key) | returns one row as a map | field-read on the table |
| edit(table, key, changes) | an update statement | field-set on the table with the statement and the count |
| pin(row-view) | mirrors the rows as blob | mode change |

Grants on tables map from the database's own roles at bind time. A query that names a table the Principal cannot resolve fails as `unresolved` before any statement runs.

### 12.5 An external store with transforms

A key-value service, an object store, a search index, a vector store. Bound as a Model of kind storage with four actions: store, recall, lookup, edit. Each is an Evaluation on that Model. The Field that names the key is external, with the Model as its backing.

A transform is a derived Field: a Field whose value is produced by an Evaluation from other Fields, declared with the Evaluation template and a recompute rule. `on-read` recomputes at resolution. `on-change` recomputes when an input changes, by a state Trigger. An embedding of a document is a derived Field, external, keyed into a vector store. A summary of a stream is a derived Field, blob, recomputed nightly. The transform's inputs are the field-read events of the Evaluation that produced it, so lineage is a query.

Store this: `store` on the Model and a field-set on the Object. Recall this: resolution of the Field, which fetches through the Adapter and appends a field-read. A memory system such as Hindsight binds this way: its observations become authored Fields, its retrieval becomes a recall action, and the kernel never holds the embeddings.

### 12.6 A unix process

A long-running process is bound by the process Adapter as a Model of kind shell on a workspace, with the pid as a Field. Actions: signal(sig), stdin(bytes), exec(argv), stop. A FIFO or socket the process reads is a Route whose transport is the process Adapter: a Message sent on the Route is written to the process, and what it writes back arrives as Messages on the same Route.

Fire this event to a unix process is one of three, by what the process expects: `act signal` for a `kill -HUP`; a Message on its Route for a line on stdin; an exec Evaluation for a fresh invocation. A state Trigger on any Object can do all three as its Evaluation, so a changed config Field reloads the daemon with no code outside the graph.

### 12.7 What they share

A backed Object is an Object with a receipt. Its content lives in the cheapest honest place. Its operations are actions on its class, run as Evaluations on the backing Model, recorded as events with who, under what Grant, and what changed. The kernel never special-cases a file, a table, a store, or a process. The Adapter does, once, at bind time.

