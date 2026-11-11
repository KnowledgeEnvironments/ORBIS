#!/bin/zsh
# One command: make sure the world is up (with the model), then open the listener window.
#   scripts/go.sh            start the world if nothing answers on the port, then the listener
#   scripts/go.sh --fresh    wipe data/ first (new root token), then as above
# Run it from Terminal.app (`open -a Terminal scripts/go.sh`): the world needs macOS Local Network permission,
# which belongs to the launching app; see the note in run-world.sh.
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PORT="${KE_PORT:-7777}"
if [ "$1" = "--fresh" ] || ! /usr/bin/curl -s -m 2 -o /dev/null "http://127.0.0.1:$PORT/"; then
  "$ROOT/scripts/run-world.sh" "$1" || exit 1
fi
for i in $(seq 1 30); do /usr/bin/curl -s -m 1 -o /dev/null "http://127.0.0.1:$PORT/" && break; sleep 1; done
# whichever macOS build exists: Homebrew-based (hostone) or from-source (vantage)
LISTENER=""; for b in macos-debug macos-source macos-release; do [ -x "$ROOT/listener/build/$b/listener" ] && { LISTENER="$ROOT/listener/build/$b/listener"; break; }; done
[ -n "$LISTENER" ] || { echo "listener not built: cd listener && cmake --preset macos-debug && cmake --build --preset macos-debug  (or preset macos-source without Homebrew)"; exit 1; }
pkill -x listener 2>/dev/null || true
mkdir -p "$ROOT/logs"
nohup "$LISTENER" --token-file "$ROOT/data/person.token" > "$ROOT/logs/listener.log" 2>&1 &
disown
echo "listener window opening (log: $ROOT/logs/listener.log). Click the box after the > at the bottom, type, press Return."
