#!/usr/bin/env bash
# Host unit tests for the pure C++ logic (no models, no Android, no network): ~20 s.
# text utils/chunker, SPSC ring buffer (ThreadSanitizer), wake-phrase gate, voice styles + pause/trim helpers.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$ROOT/app/src/main/cpp/src"
TESTS="$ROOT/app/src/main/cpp/tests"
OUT="$(mktemp -d)"
CXX="${CXX:-g++}"

run() {  # run <test name> <sanitizers> <extra sources...>
  local name="$1" san="$2"
  shift 2
  echo "== $name (-fsanitize=$san)"
  "$CXX" -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -fsanitize="$san" -fno-omit-frame-pointer \
         -I "$SRC" "$TESTS/test_$name.cpp" "$@" -o "$OUT/$name" -pthread
  "$OUT/$name"
}

run text_utils   address,undefined "$SRC/text_utils.cpp"
run ring_buffer  thread
run wake_gate    address,undefined "$SRC/wake_gate.cpp"
# needs the exported weights blob; the test also proves the C++ maths equals the ONNX model
"$CXX" -std=c++17 -O2 -Wall -Wextra -I "$SRC" "$TESTS/test_wake_net.cpp" "$SRC/wake_net.cpp" \
       -o "$OUT/wake_net"
(cd "$ROOT" && "$OUT/wake_net" app/src/main/assets/wake_weights.bin)
run tts_styles   address,undefined "$SRC/speech_utils.cpp" "$SRC/text_utils.cpp"
echo "all host tests passed"
