#!/usr/bin/env bash
# Runs the chatd load and soak test (test/chatd_soak_test.cpp) for longer than
# the default CTest run. Usage: tools/chatd-soak.sh [seconds-per-phase] [build-dir]
# The test starts chatd in-process on loopback ports; nothing leaves the machine.
set -euo pipefail
seconds="${1:-60}"
root="$(cd "$(dirname "$0")/.." && pwd)"
build="${2:-$root/build}"
cmake --build "$build" --target chatd_soak_test -j"$(nproc)"
echo "chatd soak: ${seconds} s per phase (about $((seconds * 14)) s in all)"
CHATD_SOAK_SECONDS="$seconds" "$build/test/chatd_soak_test"
