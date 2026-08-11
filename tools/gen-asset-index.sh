#!/bin/sh
# gen-asset-index.sh - write an app/assets.index listing every asset path.
#
# WHY THIS EXISTS. The wasmcart ABI can look an asset up by path
# (wc_asset_size / wc_load_asset) but cannot ENUMERATE assets -- there is no
# wc_asset_list. A cart therefore has no way to answer
# love.filesystem.getDirectoryItems, and a great many real Lua libraries
# discover their own modules by listing a directory. 3DreamEngine does it in
# ten places (classes, shaders, jobs, loaders, materials) and cannot load at
# all without it.
#
# The cart knows its own contents at pack time even though it cannot discover
# them at run time, so this writes that knowledge into the bundle as a plain
# text file. The prelude reads it and answers getDirectoryItems and
# getInfo(...).type == "directory" from it.
#
# Run this BEFORE packing, and re-run it whenever assets are added -- a stale
# index is a directory listing that silently omits a file.
#
#   tools/gen-asset-index.sh <app-dir>
#
# The index lists paths RELATIVE to the app directory, which is the same
# namespace wc_asset_read uses, one per line. assets.index excludes itself.
set -eu

APP="${1:-}"
if [ -z "$APP" ] || [ ! -d "$APP" ]; then
  echo "usage: tools/gen-asset-index.sh <app-dir>" >&2
  exit 1
fi

OUT="$APP/assets.index"
# Remove any previous index first so it cannot list itself, and so a rerun
# after deleting assets does not keep their stale entries.
rm -f "$OUT"

# LC_ALL=C for a stable byte-order sort: the index is content the cart ships,
# and a locale-dependent ordering would make two builds of the same tree
# differ for no reason.
( cd "$APP" && find . -type f | sed 's|^\./||' | LC_ALL=C sort ) > "$OUT"

echo "wrote $OUT ($(wc -l < "$OUT" | tr -d ' ') entries)"
