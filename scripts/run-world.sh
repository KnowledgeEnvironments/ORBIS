#!/bin/zsh
# Start the ORBIS world on this machine.
#   scripts/run-world.sh          keep the existing log in data/
#   scripts/run-world.sh --fresh  wipe data/ first (kernel-zero re-creates root, root.model as a Model, root.token)
#   KE_PORT / KE_DATA pick the port and data dir; a second world on another port and dir runs alongside (world pass 12)
# Run it from Terminal.app (e.g. `open -a Terminal scripts/run-world.sh`): the JVM needs macOS Local Network
# permission to reach a model on another machine, and that permission belongs to the launching app. A session
# started elsewhere may not have it, so Python and Java there get "No route to host" while curl works.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# toolchain: an existing JAVA_HOME wins; then a user-local ~/opt/env.sh; then Homebrew
[ -z "$JAVA_HOME" ] && [ -f "$HOME/opt/env.sh" ] && source "$HOME/opt/env.sh"
export JAVA_HOME="${JAVA_HOME:-/opt/homebrew/opt/openjdk@25}"
export PATH="$JAVA_HOME/bin:$HOME/opt/clojure/bin:/opt/homebrew/bin:$PATH"
export KE_PORT="${KE_PORT:-7777}"
export KE_DATA="${KE_DATA:-$ROOT/data}"
# the model key lives in ORBIS's own git-ignored .envrc (export KE_MODEL_KEY=...); nothing is read from other tools
[ -f "$ROOT/.envrc" ] && source "$ROOT/.envrc"
[ -n "$KE_MODEL_URL" ] && [ -n "$KE_MODEL_ID" ] || { echo "set KE_MODEL_URL and KE_MODEL_ID in $ROOT/.envrc: see .env.example"; exit 1; }
[ -n "$KE_MODEL_KEY" ] || { echo "KE_MODEL_KEY is empty: put export KE_MODEL_KEY=... in $ROOT/.envrc"; exit 1; }
# stop only the world of this data dir (data/world.pid, written by the world at start); other worlds on this machine keep running
"$ROOT/scripts/world-stop.sh" "$KE_DATA" || true
if [ "$1" = "--fresh" ]; then setopt null_glob; rm -rf "$KE_DATA"/log "$KE_DATA"/blobs "$KE_DATA"/snapshots "$KE_DATA"/world.edn "$KE_DATA"/*.token; fi
mkdir -p "$KE_DATA" "$ROOT/logs"
cd "$ROOT/world"
echo "world -> ws://${KE_BIND:-127.0.0.1}:$KE_PORT/ws  data=$KE_DATA  model=$KE_MODEL_ID @ $KE_MODEL_URL  log=$ROOT/logs/world.log"
nohup clojure -M:run > "$ROOT/logs/world.log" 2>&1 &
sleep 20; tail -3 "$ROOT/logs/world.log"
