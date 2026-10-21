#!/usr/bin/env bash
# Downloads the native dependencies into app/src/main/cpp/third_party (git-ignored).
# Safe to re-run: anything already at the pinned version is skipped.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck disable=SC1091
source "$ROOT/scripts/versions.env"
TP="$ROOT/app/src/main/cpp/third_party"
mkdir -p "$TP"

clone() {  # clone <name> <url> <tag>
  local name="$1" url="$2" tag="$3" dir="$TP/$1"
  if [ -f "$dir/.maya-tag" ] && [ "$(cat "$dir/.maya-tag")" = "$tag" ]; then
    echo "[deps] $name $tag (already present)"
    return
  fi
  echo "[deps] cloning $name $tag"
  rm -rf "$dir"
  git clone --quiet --depth 1 --branch "$tag" "$url" "$dir"
  echo "$tag" > "$dir/.maya-tag"
}

clone llama.cpp      https://github.com/ggml-org/llama.cpp.git          "$LLAMA_CPP_TAG"
clone whisper.cpp    https://github.com/ggml-org/whisper.cpp.git        "$WHISPER_CPP_TAG"
clone Vulkan-Headers https://github.com/KhronosGroup/Vulkan-Headers.git "$VULKAN_HEADERS_TAG"
clone SPIRV-Headers  https://github.com/KhronosGroup/SPIRV-Headers.git  "$SPIRV_HEADERS_TAG"

# SPIRV-Headers is header-only. ggml-vulkan wants find_package(SPIRV-Headers CONFIG), which the
# NDK toolchain only resolves from an install tree, so install it once.
SPV_CFG="$TP/spirv-headers-install/share/cmake/SPIRV-Headers/SPIRV-HeadersConfig.cmake"
if [ ! -f "$SPV_CFG" ]; then
  echo "[deps] installing SPIRV-Headers"
  cmake -S "$TP/SPIRV-Headers" -B "$TP/SPIRV-Headers/build" \
        -DCMAKE_INSTALL_PREFIX="$TP/spirv-headers-install" >/dev/null
  cmake --install "$TP/SPIRV-Headers/build" >/dev/null
fi
[ -f "$SPV_CFG" ] || { echo "SPIRV-Headers install layout changed: $SPV_CFG not found" >&2; exit 1; }

# sherpa-onnx: prebuilt Android C API + ONNX Runtime (wake word, VAD and TTS live here).
SH="$TP/sherpa-onnx"
if [ ! -f "$SH/.maya-tag" ] || [ "$(cat "$SH/.maya-tag")" != "$SHERPA_ONNX_TAG" ]; then
  echo "[deps] downloading sherpa-onnx $SHERPA_ONNX_TAG"
  rm -rf "$SH"
  mkdir -p "$SH/jniLibs/arm64-v8a" "$SH/include/sherpa-onnx/c-api"
  tmp="$(mktemp -d)"
  curl -fL --retry 3 --retry-delay 3 -o "$tmp/sherpa.tar.bz2" \
    "https://github.com/k2-fsa/sherpa-onnx/releases/download/$SHERPA_ONNX_TAG/sherpa-onnx-$SHERPA_ONNX_TAG-android.tar.bz2"
  tar -xjf "$tmp/sherpa.tar.bz2" -C "$tmp"
  # Only the two libraries we link: the C API and ONNX Runtime (skip the Kotlin JNI/C++ API libs).
  cp "$tmp/jniLibs/arm64-v8a/libonnxruntime.so" "$tmp/jniLibs/arm64-v8a/libsherpa-onnx-c-api.so" \
     "$SH/jniLibs/arm64-v8a/"
  curl -fL --retry 3 --retry-delay 3 -o "$SH/include/sherpa-onnx/c-api/c-api.h" \
    "https://raw.githubusercontent.com/k2-fsa/sherpa-onnx/$SHERPA_ONNX_TAG/sherpa-onnx/c-api/c-api.h"
  echo "$SHERPA_ONNX_TAG" > "$SH/.maya-tag"
  rm -rf "$tmp"
fi

printf 'set(MAYA_LLAMA_CPP_TAG "%s")\nset(MAYA_WHISPER_CPP_TAG "%s")\n' \
  "$LLAMA_CPP_TAG" "$WHISPER_CPP_TAG" > "$TP/versions.cmake"
echo "[deps] done"
