# ADR 0004: ack as a frame kind

Status: accepted, 2026-10-29

## Context
Spec 14.4 requires per-subscription acknowledgement but lists no ack op.
## Decision
A fourth frame kind: {ack <sub-id> seq <n>}, client to world, no response. Acks are transport state and are not logged. attach and detach remain events.
## Consequences
Reattach carries {subs {id acked}}; the world sets each cursor to the client's acked seq.
