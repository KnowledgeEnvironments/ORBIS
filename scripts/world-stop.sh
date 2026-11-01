#!/bin/zsh
# Stop the world that owns a data directory, and no other: the world writes data/world.pid at start.
#   scripts/world-stop.sh [data-dir]      default: <repo>/data
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DATA="${1:-${KE_DATA:-$ROOT/data}}"
PIDFILE="$DATA/world.pid"
# a supervised world stops through its supervisor, or it would be started again
if [ -f "$DATA/supervisor.pid" ] && kill -0 "$(cat "$DATA/supervisor.pid")" 2>/dev/null; then
  exec "$ROOT/scripts/supervise.sh" --stop "$DATA"
fi
PORT="${KE_PORT:-7777}"
if [ -f "$PIDFILE" ]; then
  PID="$(cat "$PIDFILE")"
else
  # a world started before pidfiles existed: the one listening on this data dir's port, if it is a world
  PID="$(lsof -nP -ti "tcp:$PORT" -sTCP:LISTEN 2>/dev/null | head -1)"
  # the port alone proves nothing: a data dir with no pidfile once stopped the main world on the default port
  [ -n "$PID" ] && [ -d "$DATA/log" ] || { echo "no world for $DATA"; exit 0; }
  REAL="$(cd "$DATA" && pwd -P)"
  lsof -nP -p "$PID" 2>/dev/null | grep -qF "$REAL/log/" || { echo "pid $PID on port $PORT does not hold $DATA; leaving it"; exit 0; }
  [ -n "$PID" ] || { echo "no world.pid in $DATA and nothing listens on $PORT (nothing to stop)"; exit 0; }
  ps -o command= -p "$PID" | grep -q knowledge.world.main || { echo "pid $PID on port $PORT is not a world; leaving it"; exit 1; }
  echo "no world.pid; stopping the pre-pidfile world $PID on port $PORT"
fi
if ! kill -0 "$PID" 2>/dev/null; then echo "world.pid $PID is stale; removing"; rm -f "$PIDFILE"; exit 0; fi
if ! ps -o command= -p "$PID" | grep -q knowledge.world.main; then echo "pid $PID is not a world; leaving it"; exit 1; fi
kill "$PID"
for i in $(seq 1 30); do kill -0 "$PID" 2>/dev/null || { echo "world $PID stopped ($DATA)"; rm -f "$PIDFILE"; exit 0; }; sleep 0.5; done
echo "world $PID did not stop in 15 s; killing"; kill -9 "$PID" 2>/dev/null; rm -f "$PIDFILE"
