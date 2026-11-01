# source this: . scripts/env.sh
export JAVA_HOME=/opt/homebrew/opt/openjdk@25
export PATH="$JAVA_HOME/bin:/opt/homebrew/bin:$PATH"
[ -f "$(dirname "$0")/../.envrc" ] && . "$(dirname "$0")/../.envrc"
