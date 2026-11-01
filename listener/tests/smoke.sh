#!/bin/sh
# End-to-end: start mockworld on a private port, run the listener's --smoke (network + session +
# store, no window) for a few seconds with a mid-run connection drop, and require no gap.
set -eu
MOCK="$1"; LISTENER="$2"; PORT="${3:-7781}"
"$MOCK" --port "$PORT" --period-ms 200 &
MOCK_PID=$!
trap 'kill $MOCK_PID 2>/dev/null || true' EXIT INT TERM
sleep 0.5
"$LISTENER" --smoke 4 --url "ws://127.0.0.1:$PORT/ws" --token smoke-token
