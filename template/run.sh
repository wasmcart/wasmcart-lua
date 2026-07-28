#!/bin/bash
# Play this directory as a dev cart. Edit app/main.lua, rerun.
set -e
cd "$(dirname "$0")"
[ -f main.wasm ] || cp ../build/engine.wasm main.wasm
npx wasmcart .
