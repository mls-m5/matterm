#!/bin/sh
# Runs clang-tidy over matterm's own sources (not lib/, matperf/, test/mls-unit-test).
#   tools/clang-tidy.sh [build-dir] [--changed [base]] [extra run-clang-tidy args]
# Needs a configured build dir (compile_commands.json is exported by default).
# The full run takes minutes. --changed checks only the .cpp files that differ from
# `base` (default: origin/main) or are modified in the working tree; a changed header
# re-checks every .cpp that includes it only in a full run.
set -eu
build=${1:-build}; [ $# -gt 0 ] && shift
root=$(cd "$(dirname "$0")/.." && pwd)
if [ "${1:-}" = "--changed" ]; then
    shift
    base=origin/main
    case "${1:-}" in -*|"") ;; *) base=$1; shift ;; esac
    files=$( (cd "$root" && { git diff --name-only "$base"...HEAD; git diff --name-only; git ls-files --others --exclude-standard; }) |
        sort -u | grep -E '^(src|app|demo|test)/.*\.cpp$' | grep -v '^test/mls-unit-test' || true)
    if [ -z "$files" ]; then echo "clang-tidy: no changed .cpp files"; exit 0; fi
    # shellcheck disable=SC2086
    set -- $(for f in $files; do printf '%s ' "$root/$f"; done) "$@"
    exec run-clang-tidy -p "$build" -quiet "$@"
fi
exec run-clang-tidy -p "$build" -quiet "$@" "^$root/(src|app|demo|test)/(?!mls-unit-test).*\\.cpp\$"
