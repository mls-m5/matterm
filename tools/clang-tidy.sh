#!/bin/sh
# Runs clang-tidy over matterm's own sources (not lib/, matperf/, test/mls-unit-test).
#   tools/clang-tidy.sh [build-dir] [extra run-clang-tidy args]
# Needs a configured build dir (compile_commands.json is exported by default).
set -eu
build=${1:-build}; [ $# -gt 0 ] && shift
root=$(cd "$(dirname "$0")/.." && pwd)
exec run-clang-tidy -p "$build" -quiet "$@" "^$root/(src|app|demo|test)/(?!mls-unit-test).*\\.cpp\$"
