#!/bin/zsh
# Clean-checkout CI, on the machine it runs on. Tests one commit from a bundle or a local repo, in a
# throwaway directory, so nothing depends on a working tree another agent is editing.
#   scripts/ci.sh [repo-or-bundle] [rev]     default: this repo, HEAD
# Writes logs/ci/<sha>.log and prints one PASS or FAIL line per suite; exit 0 only if all pass.
[ -f "$HOME/opt/env.sh" ] && source "$HOME/opt/env.sh"
[ -d /opt/homebrew/opt/openjdk@25 ] && export JAVA_HOME=/opt/homebrew/opt/openjdk@25 PATH=/opt/homebrew/opt/openjdk@25/bin:/opt/homebrew/bin:$PATH
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="${1:-$ROOT}"; REV="${2:-HEAD}"
W="$(mktemp -d /tmp/orbis-ci.XXXXXX)"; trap 'rm -rf "$W"' EXIT
git clone -q "$SRC" "$W/r" && cd "$W/r" && git checkout -q "$REV" || { echo "FAIL checkout"; exit 2; }
SHA="$(git rev-parse --short HEAD)"; mkdir -p "$ROOT/logs/ci"; LOG="$ROOT/logs/ci/$SHA.log"; : > "$LOG"
fail=0
step() { name=$1; shift; if "$@" >> "$LOG" 2>&1; then echo "PASS $name"; else echo "FAIL $name"; fail=1; fi; }
step schema make test-schema
step schema-export make schema-check
step unit make test-unit
step exit make test-exit
step kernel-zero make kz
PRESET=macos-source; [ -d /opt/homebrew/opt/sdl3 ] && PRESET=macos-debug
cd listener
step listener-build zsh -c "cmake --preset $PRESET && cmake --build --preset $PRESET"
step listener-test ctest --preset "$PRESET" -E "listener_ux|idsweep" --output-on-failure
echo "$SHA $([ $fail = 0 ] && echo GREEN || echo RED) log=$LOG"
exit $fail
