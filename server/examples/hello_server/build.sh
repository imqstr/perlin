#!/bin/sh
# freestanding build: clang with the wasm32 target and wasm-ld, no libc, no emscripten needed
set -e
here=$(cd "$(dirname "$0")" && pwd)
out=${1:-$here/hello_server.wasm}
clang --target=wasm32 -O2 -nostdlib -fno-builtin -fvisibility=hidden \
  -Wl,--no-entry -Wl,--export-dynamic -Wl,--allow-undefined \
  -o "$out" "$here/hello_server.c"
echo "built $out"
