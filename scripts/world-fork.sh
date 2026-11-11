#!/bin/zsh
# World as files (world pass 9): see world/src/knowledge/world/tools.clj.
#   world-export.sh <data-dir> <archive.tgz> [--live]   world-import.sh <archive.tgz> <data-dir>
#   world-fork.sh <parent-dir> <child-dir> [--at N]     world-promote.sh <child-dir> <parent-dir>
#   world-diff.sh <child-dir>
# Stop the world first for export/import/promote; fork and diff read a live parent in place.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
[ -f "$HOME/opt/env.sh" ] && source "$HOME/opt/env.sh"
export JAVA_HOME="${JAVA_HOME:-/opt/homebrew/opt/openjdk@25}"
export PATH="$JAVA_HOME/bin:/opt/homebrew/bin:$PATH"
cd "$ROOT/world"
exec clojure -M -m knowledge.world.tools fork "$@"
