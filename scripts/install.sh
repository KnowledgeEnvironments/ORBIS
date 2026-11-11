#!/bin/zsh
# From a clean checkout to a running world and an open listener, on macOS. Each step is one of the
# commands in docs/install.md, so what this does by itself can be done by hand.
#   scripts/install.sh                       check tools, write .envrc, build, make a world in data/, start it, open the listener
#   scripts/install.sh --check               only report the toolchain
#   scripts/install.sh --data <dir>          the world's data directory (default <repo>/data)
#   scripts/install.sh --port 7777 --name home
#   scripts/install.sh --no-listener         build but do not open the client window
#   scripts/install.sh --brew                install what is missing with Homebrew without asking
#   scripts/install.sh --lan                 listen on every interface, for listeners on other machines (default: this machine only)
# Run it from Terminal.app (`open -a Terminal scripts/install.sh`) when the Model is on another machine:
# the world needs macOS Local Network permission, which belongs to the app that launched it.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DATA="$ROOT/data"; PORT=7777; NAME=home; LISTENER=1; CHECK=""; BREW=""; LAN=""
while [ $# -gt 0 ]; do
  case "$1" in
    --data) DATA="$2"; shift 2 ;;
    --port) PORT="$2"; shift 2 ;;
    --name) NAME="$2"; shift 2 ;;
    --no-listener) LISTENER=""; shift ;;
    --check) CHECK=1; shift ;;
    --brew) BREW=1; shift ;;
    --lan) LAN=1; shift ;;
    *) echo "unknown argument $1" >&2; exit 2 ;;
  esac
done
say() { printf '%s\n' "$*"; }
ok() { say "  ok    $*"; }
miss() { say "  MISSING $*"; MISSING+=("$2"); }

# ---------------------------------------------------------------- tools
say "ORBIS install: $ROOT"
say "toolchain"
MISSING=()
[ -z "$JAVA_HOME" ] && [ -f "$HOME/opt/env.sh" ] && source "$HOME/opt/env.sh"
export JAVA_HOME="${JAVA_HOME:-/opt/homebrew/opt/openjdk@25}"
export PATH="$JAVA_HOME/bin:$HOME/opt/clojure/bin:/opt/homebrew/bin:$PATH"
if xcode-select -p >/dev/null 2>&1 && command -v clang >/dev/null; then ok "clang ($(xcode-select -p))"; else say "  MISSING clang: run  xcode-select --install"; MISSING+=(xcode); fi
if [ -x "$JAVA_HOME/bin/java" ] && "$JAVA_HOME/bin/java" -version 2>&1 | grep -q 'version "25'; then ok "java 25 ($JAVA_HOME)"; else miss "java 25: brew install openjdk@25" openjdk@25; fi
if command -v clojure >/dev/null; then ok "clojure $(clojure --version 2>/dev/null | awk '{print $NF}')"; else miss "clojure CLI: brew install clojure/tools/clojure" clojure/tools/clojure; fi
if command -v cmake >/dev/null && [ "$(cmake --version | head -1 | awk '{print $3}' | cut -d. -f1-2 | tr -d .)" -ge 328 ] 2>/dev/null; then ok "cmake $(cmake --version | head -1 | awk '{print $3}')"; else miss "cmake 3.28 or later: brew install cmake" cmake; fi
if command -v ninja >/dev/null; then ok "ninja $(ninja --version)"; else miss "ninja: brew install ninja" ninja; fi
PRESET=macos-source
if [ -d /opt/homebrew/opt/sdl3 ] && [ -d /opt/homebrew/opt/freetype ]; then PRESET=macos-debug; ok "SDL3 and FreeType from Homebrew (preset macos-debug)"; else ok "SDL3 and FreeType will be built from source (preset macos-source; brew install sdl3 freetype for the faster build)"; fi
command -v zig >/dev/null && ok "zig (the terminal holder can be built: make holder)" || say "  note  no zig: terminals run inside the world and do not survive a restart (make holder needs Zig, holder/PIN)"
if [ ${#MISSING[@]} -gt 0 ]; then
  FORMULAE=(${MISSING:#xcode})
  if [ ${#FORMULAE[@]} -gt 0 ] && command -v brew >/dev/null; then
    if [ -n "$BREW" ]; then yes=y; else printf 'install with Homebrew: brew install %s ? [y/N] ' "${FORMULAE[*]}"; read -r yes; fi
    if [ "$yes" = y ] || [ "$yes" = Y ]; then
      for f in "${FORMULAE[@]}"; do case "$f" in */*) brew tap "${f%/*}" >/dev/null 2>&1 || true ;; esac; done
      brew install "${FORMULAE[@]}"
      exec "$0" "$@"
    fi
  fi
  say "install what is missing and run this again"; exit 1
fi
[ -n "$CHECK" ] && exit 0

# ---------------------------------------------------------------- the Model
if [ ! -f "$ROOT/.envrc" ]; then
  say ""
  say "the Model root thinks with: any OpenAI-compatible chat endpoint. Leave the URL empty for none now:"
  say "the world then boots with a stub, and you sign in to ChatGPT from the listener's model picker, or add a Model later."
  printf 'model URL (e.g. http://localhost:8000/v1): '; read -r MURL
  MID=""; MKEY=""
  if [ -n "$MURL" ]; then
    printf 'key (not shown; empty if the endpoint takes none): '; read -rs MKEY; echo
    # the endpoint says what it serves: one model is taken, several are offered, none means type it
    MODELS="$(/usr/bin/curl -s -m 5 ${MKEY:+-H "Authorization: Bearer $MKEY"} "${MURL%/}/models" 2>/dev/null | python3 -c 'import json,sys
try:
    d=json.load(sys.stdin); print("\n".join(str(m.get("id") or m.get("slug") or "") for m in (d.get("data") or d.get("models") or []) if isinstance(m, dict)))
except Exception: pass' 2>/dev/null)"
    if [ -z "$MODELS" ]; then
      say "  (no model list at ${MURL%/}/models: the endpoint may be down, or need the key; type the id)"
      printf 'model id: '; read -r MID
    elif [ "$(printf '%s\n' "$MODELS" | grep -c .)" -eq 1 ]; then
      MID="$MODELS"; say "  the endpoint serves one model: $MID"
    else
      say "  the endpoint serves:"; printf '%s\n' "$MODELS" | nl -w3 -s'  '
      printf 'model id (a number from the list, or the id): '; read -r PICK
      case "$PICK" in ''|*[!0-9]*) MID="$PICK" ;; *) MID="$(printf '%s\n' "$MODELS" | sed -n "${PICK}p")" ;; esac
    fi
  fi
  ( umask 077; {
      echo "# written by scripts/install.sh; see .env.example"
      [ -n "$MURL" ] && echo "export KE_MODEL_URL=$MURL"
      [ -n "$MID" ] && echo "export KE_MODEL_ID=$MID"
      [ -n "$MKEY" ] && echo "export KE_MODEL_KEY=$MKEY"
      echo "export KE_FSYNC=batch"
    } > "$ROOT/.envrc" )
  ok ".envrc written (git-ignored)"
else
  ok ".envrc present"
fi

# ---------------------------------------------------------------- build
say ""
say "resolving Clojure dependencies"
( cd "$ROOT" && make -s deps )
say "building the listener ($PRESET)"
mkdir -p "$ROOT/logs"
( cd "$ROOT/listener" && cmake --preset "$PRESET" > "$ROOT/logs/cmake.log" 2>&1 || { cat "$ROOT/logs/cmake.log"; exit 1; }
  cmake --build --preset "$PRESET" > "$ROOT/logs/build.log" 2>&1 || { tail -40 "$ROOT/logs/build.log"; exit 1; } )
BIN="$ROOT/listener/build/$PRESET/listener"
[ -x "$BIN" ] || { say "no listener at $BIN"; exit 1; }
ok "listener $BIN"

# ---------------------------------------------------------------- the world
say ""
mkdir -p "$DATA"; DATA="$(cd "$DATA" && pwd)"
if [ ! -f "$DATA/world.env" ]; then
  ( umask 077; {
      echo "export KE_WORLD_NAME=$NAME"
      echo "export KE_PORT=$PORT"
      # the address clients dial: the LAN IPv4 when there is one, since a phone cannot always resolve the host's name
      if [ -n "$LAN" ]; then echo "export KE_BIND=0.0.0.0"; echo "export KE_PUBLIC_URL=ws://$(ipconfig getifaddr en0 2>/dev/null || ipconfig getifaddr en1 2>/dev/null || hostname -s):$PORT/ws"; else echo "export KE_PUBLIC_URL=ws://127.0.0.1:$PORT/ws"; fi
      grep '^export KE_' "$ROOT/.envrc" | grep -v '^export KE_PORT=\|^export KE_DATA='
    } > "$DATA/world.env" )
  ok "$DATA/world.env written: name $NAME, port $PORT$([ -n "$LAN" ] && echo ", every interface" || echo ", this machine only: --lan or KE_BIND=0.0.0.0 for others")"
fi
if [ -f "$DATA/supervisor.pid" ] && kill -0 "$(cat "$DATA/supervisor.pid")" 2>/dev/null; then
  ok "the world already runs (supervisor $(cat "$DATA/supervisor.pid"))"
else
  [ -f "$DATA/release" ] || { say "making the first release"; "$ROOT/scripts/release.sh" --data "$DATA" >/dev/null; }
  # Terminal.app runs a file and passes it no arguments, so each world gets a two-line starter of its own
  printf '#!/bin/zsh\nexec "%s/scripts/world.sh" "%s"\n' "$ROOT" "$DATA" > "$DATA/start.sh"; chmod +x "$DATA/start.sh"
  say "starting the world through Terminal.app (it keeps running when that window closes)"
  mkdir -p "$DATA/logs"
  open -a Terminal "$DATA/start.sh" 2>/dev/null || { nohup "$DATA/start.sh" > "$DATA/logs/start.log" 2>&1 & }
  for i in $(seq 1 90); do /usr/bin/curl -s -m 1 -o /dev/null "http://127.0.0.1:$PORT/" && break; sleep 1; done
  /usr/bin/curl -s -m 1 -o /dev/null "http://127.0.0.1:$PORT/" || { say "the world did not answer on $PORT in 90 s: see $DATA/logs/world.log"; exit 1; }
  ok "world $NAME on ws://127.0.0.1:$PORT/ws, data $DATA"
fi
for i in $(seq 1 30); do [ -f "$DATA/person.token" ] && break; sleep 1; done
[ -f "$DATA/person.token" ] && ok "your token: $DATA/person.token (a second machine's listener takes a copy)"

# ---------------------------------------------------------------- the listener
if [ -n "$LISTENER" ]; then
  mkdir -p "$ROOT/logs"
  nohup "$BIN" --url "ws://127.0.0.1:$PORT/ws" --token-file "$DATA/person.token" > "$ROOT/logs/listener.log" 2>&1 &
  disown
  ok "listener opening (log $ROOT/logs/listener.log)"
fi
say ""
KE_DATA="$DATA" "$ROOT/start.sh" --status
say "next time:  ./start.sh   (starts the world if it is down, --restart after a change to world.env, --status to look), then"
say "            $BIN --url ws://127.0.0.1:$PORT/ws --token-file $DATA/person.token"
say "stop:       scripts/supervise.sh --stop $DATA"
say "docs:       docs/install.md"
