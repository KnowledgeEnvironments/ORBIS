# ADR 0001: http-kit as the WebSocket server

Status: accepted, 2026-10-29

## Context
The world needs one WebSocket endpoint with per-subscription backpressure.
## Decision
http-kit 2.8.1: single jar, no transitive deps, as-channel/send!/on-close. Backpressure is implemented at the protocol level (per-sub window over the log cursor), not at the socket, so Netty/aleph or Jetty buy nothing.
## Consequences
Max message 4 MiB by default; push frames are capped at 256 events or 1 MiB.
