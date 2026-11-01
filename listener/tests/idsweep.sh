#!/bin/sh
# Id sweep: mockworld on a private port, the windowed listener with LISTENER_IDCHECK walking a
# synthetic mouse over every pane view; exit 3 on any ImGui id conflict or recoverable error.
set -eu
MOCK="$1"; LISTENER="$2"; PORT="${3:-7786}"; STEP="${4:-12}"
"$MOCK" --port "$PORT" --period-ms 200 &
MOCK_PID=$!
trap 'kill $MOCK_PID 2>/dev/null || true' EXIT INT TERM
sleep 0.5
LISTENER_IDCHECK="sweep:$STEP" LISTENER_IDCHECK_AFTER_MS=2500 LISTENER_STRICT=1 \
  "$LISTENER" --url "ws://127.0.0.1:$PORT/ws" --token sweep-token
