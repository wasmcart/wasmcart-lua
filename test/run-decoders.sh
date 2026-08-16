#!/bin/bash
# Build and run the native decoder harness (test/decoders.c).
#
# Native, not wasm: the decoders are pure memory-in/PCM-out C, so running them
# under the host compiler asserts on the actual samples with no cart, no engine
# and no emscripten in the way. The wasm build is verified separately by
# runtime/build.sh linking the same translation unit.
#
# Opus is included automatically when runtime/build.sh has fetched and built it
# (vendor/libopus.a); otherwise the harness reports opus as skipped.
set -e
cd "$(dirname "$0")/.."
RT=runtime
OUT="${TMPDIR:-/tmp}/wcl-decoder-test"

# Fixtures are generated, not committed: five encodes of one ffmpeg-generated
# tone. Regenerate if absent so a fresh clone can run this.
if [ ! -f test/audio-fixtures/tone.mp3 ]; then
  echo "generating fixtures with ffmpeg..."
  mkdir -p test/audio-fixtures
  ( cd test/audio-fixtures
    ffmpeg -hide_banner -loglevel error -y -f lavfi \
      -i "sine=frequency=440:sample_rate=48000:duration=0.5" -ac 2 -c:a pcm_s16le tone.wav
    ffmpeg -hide_banner -loglevel error -y -i tone.wav -c:a libmp3lame -b:a 128k tone.mp3
    ffmpeg -hide_banner -loglevel error -y -i tone.wav -c:a flac            tone.flac
    ffmpeg -hide_banner -loglevel error -y -i tone.wav -c:a libopus -b:a 96k tone.opus
    ffmpeg -hide_banner -loglevel error -y -i tone.wav -c:a libvorbis -q:a 5 tone.ogg )
fi

# Opus needs a NATIVE archive: runtime/vendor/libopus.a is a wasm archive built
# by emcc and the host linker rejects it outright ("file format not
# recognized"). Same sources, same defines, host compiler, cached separately.
OPUS_FLAGS=""
NATIVE_OPUS="$RT/vendor/libopus-native.a"
if [ -d "$RT/vendor/opus" ]; then
  if [ ! -f "$NATIVE_OPUS" ]; then
    echo "building native libopus for the harness (one time)..."
    V="$PWD/$RT/vendor"
    OPUS_INC="-I$V/opus/celt -I$V/opus/silk -I$V/opus/silk/float \
              -I$V/opus/include -I$V/opus -I$V/ogg/include \
              -I$V/opusfile/include -I$V/opusfile/src"
    SRC="$(ls $V/opus/src/*.c $V/opus/celt/*.c $V/opus/silk/*.c \
              $V/opus/silk/float/*.c 2>/dev/null \
           | grep -v -E 'demo|_test|opus_compare|repacketizer_demo|/tests?/')"
    SRC="$SRC $V/ogg/src/bitwise.c $V/ogg/src/framing.c"
    SRC="$SRC $V/opusfile/src/opusfile.c $V/opusfile/src/info.c \
         $V/opusfile/src/internal.c $V/opusfile/src/stream.c"
    OBJ="${TMPDIR:-/tmp}/wcl-opus-native-obj"
    rm -rf "$OBJ" && mkdir -p "$OBJ"
    ( cd "$OBJ" && cc -O2 -c $SRC \
        -DOPUS_BUILD=1 -DVAR_ARRAYS=1 -DFLOATING_POINT=1 \
        -DOP_ENABLE_HTTP=0 -DOP_FIXED_POINT=0 -w $OPUS_INC )
    ar rcs "$PWD/$NATIVE_OPUS" "$OBJ"/*.o
    rm -rf "$OBJ"
  fi
  OPUS_FLAGS="-DWCL_ENABLE_OPUS -I$RT/vendor/opusfile/include -I$RT/vendor/ogg/include \
              -I$RT/vendor/opus/include $NATIVE_OPUS"
  echo "Opus: ENABLED"
else
  echo "Opus: disabled (run WCL_OPUS=1 runtime/build.sh to fetch it)"
fi

# -msse2 for dr_mp3's SIMD path: decoders.c defines __i386__ around the header
# to select it (see the comment there), and that dispatch needs real SSE2 when
# building for a native x86 host.
cc -O2 -msse2 -o "$OUT" test/decoders.c "$RT/decoders.c" "$RT/vorbis.c" \
   -I "$RT" $OPUS_FLAGS -lm
"$OUT" test/audio-fixtures
