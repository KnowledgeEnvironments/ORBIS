# Contract needs from the listener (sprint 0)

Findings while building the listener against `docs/protocol.md` (frozen at `protocol-v0`). None of
these blocks sprint 0; the listener takes the least-deviating path noted under each item. They are
raised here per ground rule 10 so they can go through `docs/decisions.md`.

## 1. The `lagging` marker has no place in the Push schema

`docs/protocol.md` "Subscriptions": "When the window is full the sub is marked `lagging` in the next
push". `listener/generated/schema/ke.Push.json` only has `sub`, `events`, `as-of` (all required).

Listener path: `Push` parses an optional boolean `lagging` key when present and never emits it unless
true, so every frame in `docs/protocol-examples/` round-trips byte-for-byte. The mock sends
`"lagging": true` on a full window. Need: either add `lagging?` (boolean) to `:ke/Push`, or state
that lagging is conveyed another way (for example an `err lagging` response to the next ack).

## 2. `bind` has nothing to bind until the host Object exists

Spec 15.6 says the listener binds its host as a workspace and a shell Model. `bind {name object}`
needs an Object uuid, and the only way to create one is `act {class action create}` (ADR 0008), which
sprint 0 supports only for `object`, `principal`, `group`, not `adapter` or `model`.

Listener path: the sprint-0 stubs (`src/workspace/LocalFs.cpp`, `Shell.cpp`) send `bind` with a
deterministic placeholder uuid derived from the hostname and show whatever the world answers
(`not-implemented`, `unresolved`, or `ok`). Need for sprint 1: `act create` for classes `adapter` and
`model` (or a ruling that the host is a plain `object` with fields), so the listener can create the
host Objects before binding them.

## 3. `subscribe` without `after` on a first attach

The spec leaves the initial cursor of a subscribe without `after` to the world. The listener omits
`after` on the very first attach of a fresh install (nothing acked yet) and sends `after = acked`
ever after. If the intended first-run behaviour is "the whole log from 0", say so and the listener
will send `after 0`; the mock currently starts at the head (first push is 43 on a fresh world, as in
`attach.response.json`).

## Pins

All pinned tags from ADR 0007 existed upstream and are used unchanged: imgui `v1.92.9b-docking`,
IXWebSocket `v12.0.1`, nlohmann/json `v3.12.0`, readerwriterqueue `v1.0.7`, Catch2 `v3.16.0`.
No fallback was needed for the renderer (SDL_GPU on Metal works) or the fonts (downloaded).

## 4. `send`: what the world answers before the inbox Route exists

Added 2026-10-30 with the input line. `send {to route text tags}` is sent with `to` as a JSON array
(the set form of ADR 0002) and `route` the first id of the principal's `principal/routes`. Three
answers were seen or read in the source and the listener handles them:

- `err not-implemented` (the running world at the time of writing): shown once as
  `[ SEND: NOT BUILT ]`.
- `err unresolved` (`route/send!` when the Route does not resolve or a recipient does not exist):
  shown as `[ SEND: UNRESOLVED ]`.
- `err invalid "frame does not validate"` when `route` is not a uuid: the listener never sends a
  non-uuid route, but note that a missing route cannot be probed with a placeholder string.

Need: root's inbox Route. Until the world creates one (or `act create` for class `route` lands),
`principal/routes` is absent on root and the input line shows `NO ROUTE`, re-reading the principal
every 5 s. `--smoke` then probes `send` with the principal id standing in for the route so the op
is at least exercised (`unresolved` or `not-implemented` both count as answered).

## 5. `read` folds class attributes into Fields

`read {object <principal>}` on the real world answers
`{object/id object/class object/stream object/names object/fields}` with the class attributes
(`principal/driven-by`, `principal/routes`, ...) as entries of `object/fields` carrying
`field/value`, while `ke.Principal.json` lists them as top-level keys. The listener reads both
places (`chat::stateValue`: top-level first, then `object/fields.<key>.field/value`), and the mock
answers in the folded form. Need: say which form is the contract for `read`, or regenerate the JSON
schema mirror for the state map.

## 6. `sent` / `delivered` carry the Message id, not its text

Confirmed from `knowledge.schema.events/bodies`: `sent {message route}`,
`delivered {message route recipient}`. A chat line therefore costs one `read {object <message>}`
per Message (cached by id; `sent` and `delivered` of the same Message share it), plus the
`field-read` events that read appends on the reader's stream. For a busy inbox that doubles the
rows. Need (later): either a `depth` on subscribe that inlines the referenced Message, or a
`present` form for Messages, so the listener can render chat without a read per row.

## 7. Streaming Evaluations

`docs/listener-style.md` wants a spinner on a row while its Evaluation streams. No event shape
says "streaming" yet; the listener's hook treats an `evaluated` event whose body has
`state "streaming"` as the start and any later `evaluated` for the same `evaluation` as the end.
Need: the actual body of `evaluated` for partial output when the world streams.
