#!/bin/sh
# Type-check web/main.cpp (not websocketchannel.cpp, which only wraps the browser API) with the native compiler (no Emscripten needed): the
# JavaScript parts are stubbed out, SDL comes from the system. Catches typos and
# type errors before a real web build. Usage: web/tools/syntax-check.sh
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
inc=""
for d in src src/core src/render src/terminal src/terminal/vt src/protocol \
         src/transport src/fakeos src/chat src/frontend; do
    inc="$inc -I$root/$d"
done
gen=${GENERATED_DIR:-}
[ -n "$gen" ] && inc="$inc -I$gen"
c++ -std=c++17 -fsyntax-only -Wall -Wno-unused-parameter -Wno-unused-variable -Wno-unused-function \
    -I"$root/web/tools/stubs" $inc $(sdl2-config --cflags) \
    "$root/web/main.cpp"
echo "web: native type check passed"
