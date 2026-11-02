# The protocol, for agents

One WebSocket, `ws://host:port/ws?format=transit|json`; frames are maps: request `{id op args}`,
response `{id ok result seq}` or `{id ok=false err seq}`, push `{sub events as-of}`, ack `{ack <sub> seq}`.
`seq` on every response is the log head; every write returns the seq of its event.

- `attach {token}` first; `detach {}`; a dropped socket keeps the session 300 s and
  `attach {token session subs}` resumes with no gap. `subscribe {sub stream after? window?}` then `{ack}`.
- Read through resolution: `read {object}` or `read {name}` (a mounted name materializes its file),
  `read {object field range [first last]}` pages any text or bytes Field; `history {stream after? limit}` (limit ≤ 1000)
  pages from the oldest; for the newest, `history {last n}`, and `before` for the page before that.
- Write through resolution: `set-field {object field type value reader}`, `bind {name object}`,
  `grant {grantee scope ops}`, `send {to route text}`, `move {task to}`,
  `act {class action create args}` to create, `act {object action args}` for `run`, `index`, `cancel`,
  `act {action "<name>" args}` for a named action (principal.create, workspace.mount, route.create,
  route.join, harness.index, task.run, task.approve, task.deny; the Acting section lists the ones you may call, with their args).
  An op named in your policy/approval becomes a pending approval Task (err approval-pending) that a person decides.
- Grant ops are `read reference transmit write delegate evaluate create` only; wire ops such as
  `history`, `subscribe`, `set-field`, `act` are not Grant ops (`err invalid-ops`). A scope is
  `{prefix "name"}`, a list of ids, or `everything`.
- `err unresolved`: the name does not resolve for you (a missing name looks the same).
  `err not-implemented`: registered, not built; say so, do not work around it.

The full contract with every op's args: read `harness/docs/protocol.md`.
