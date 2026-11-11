#!/bin/zsh
# Start a supervised world from its data directory (docs/self-hosting.md): the first run makes a release of
# this checkout, later runs start whatever <data>/release names. Everything the world needs is in
# <data>/world.env (name, port, Model), written by scripts/install.sh or by hand from .env.example.
#   scripts/world.sh <data-dir>                 run under the supervisor; closing the Terminal does not stop it
#   scripts/supervise.sh --stop <data-dir>      stop it
# On macOS start it through Terminal.app (`open -a Terminal scripts/world.sh <data-dir>`) when the Model runs
# on another machine: the JVM needs Local Network permission, and that belongs to the launching app.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DATA="${1:?usage: world.sh <data-dir>}"
mkdir -p "$DATA"
DATA="$(cd "$DATA" && pwd)"
[ -f "$DATA/world.env" ] || { echo "no $DATA/world.env: scripts/install.sh writes one, or copy .env.example there as exports"; exit 2; }
[ -f "$DATA/release" ] || "$ROOT/scripts/release.sh" --data "$DATA"
exec "$ROOT/scripts/supervise.sh" "$DATA"
