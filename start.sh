#!/bin/zsh
# The world under data/: bring it up, bring it back after a failure, and say how clients connect.
#   ./start.sh              start the world if it is down, then show its state and the client tokens
#   ./start.sh --restart    stop it first (a wedged supervisor, a change to data/world.env)
#   ./start.sh --status     only show
# On a Mac the world starts through Terminal.app, so it has Local Network permission for a Model on
# another machine and keeps running when the window closes (docs/install.md). Everything the world
# needs is data/world.env; the supervisor (scripts/supervise.sh) keeps it up and rolls a bad release
# back to the last good one by itself (docs/self-hosting.md).
set -u
ROOT="$(cd "$(dirname "$0")" && pwd)"
DATA="${KE_DATA:-$ROOT/data}"
MODE="${1:-start}"
say() { printf '%s\n' "$*"; }

[ -f "$DATA/world.env" ] || { say "no $DATA/world.env: scripts/install.sh makes the world first"; exit 2; }
PORT="$(sed -n 's/^export KE_PORT=//p' "$DATA/world.env" | head -1)"; PORT="${PORT:-7777}"
URL="$(sed -n 's/^export KE_PUBLIC_URL=//p' "$DATA/world.env" | head -1)"; URL="${URL:-ws://127.0.0.1:$PORT/ws}"

running() { [ -f "$DATA/supervisor.pid" ] && kill -0 "$(cat "$DATA/supervisor.pid")" 2>/dev/null; }
answers() { /usr/bin/curl -s -m 1 -o /dev/null "http://127.0.0.1:$PORT/"; }

if [ "$MODE" = "--restart" ] && running; then
  say "stopping the world"
  "$ROOT/scripts/supervise.sh" --stop "$DATA"
fi
if [ "$MODE" != "--status" ] && ! running; then
  # a supervisor that died leaves its pidfile; a world that died leaves its port free: both are starts
  rm -f "$DATA/supervisor.pid"
  printf '#!/bin/zsh\nexec "%s/scripts/world.sh" "%s"\n' "$ROOT" "$DATA" > "$DATA/start.sh"; chmod +x "$DATA/start.sh"
  say "starting the world"
  if [ "$(uname)" = "Darwin" ]; then
    open -a Terminal "$DATA/start.sh" 2>/dev/null || { mkdir -p "$DATA/logs"; nohup "$DATA/start.sh" > "$DATA/logs/start.log" 2>&1 & }
  else
    mkdir -p "$DATA/logs"; nohup "$DATA/start.sh" > "$DATA/logs/start.log" 2>&1 &
  fi
  for _ in $(seq 1 120); do answers && break; sleep 1; done
fi

say ""
if answers; then say "world     up on port $PORT"; else say "world     NOT answering on port $PORT: see $DATA/logs/world.log and $DATA/logs/supervise.log"; fi
say "release   $(cut -c1-12 "$DATA/release" 2>/dev/null)   good $(cut -c1-12 "$DATA/release.good" 2>/dev/null)"
[ -f "$DATA/logs/supervise.log" ] && say "last      $(tail -1 "$DATA/logs/supervise.log")"
say "clients   dial $URL"
say "token     $DATA/person.token  (you, the first person: a listener on another machine takes a copy, 0600)"
say "            $(cat "$DATA/person.token" 2>/dev/null)"
say "root      $DATA/root.token  (the operator's key: leave it where it is)"
say "phones    pair from a listener: DEVICES on your own page, pair phone, begin pairing, then scan the QR"
# the device tokens the world has issued, by label: revocable from DEVICES; secrets are never shown again
if answers && [ -f "$DATA/person.token" ] && [ -f "$ROOT/scripts/bench/ws.py" ]; then
  python3 - "$DATA/person.token" "$PORT" "$ROOT/scripts/bench" <<'PY' 2>/dev/null
import sys
sys.path.insert(0, sys.argv[3])
from ws import World
w = World(token_file=sys.argv[1], port=int(sys.argv[2]))
r = w.act("token.list", {})
rows = ((r.get("result") or {}).get("result") or {}).get("tokens") or ((r.get("result") or {}).get("result") or {}).get("rows") or []
if rows:
    print("devices")
    for t in rows:
        print("            " + " ".join(str(t.get(k) or "") for k in ("label", "principal-name", "issued-at", "last-used") if t.get(k)))
PY
fi
say ""
say "stop      $ROOT/scripts/supervise.sh --stop $DATA"
say "logs      $DATA/logs/world.log   $DATA/logs/supervise.log"
