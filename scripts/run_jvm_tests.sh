#!/usr/bin/env bash
# Plain-JVM tests for the pure Kotlin logic: every formatted UI string, phrase merging, calibration-clip trimming.
# Needs a JDK. Downloads the Kotlin compiler once (or set KOTLINC=/path/to/kotlinc).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
J="$ROOT/app/src/main/java/com/maya/assistant"
KV=2.0.21
if [ -n "${KOTLINC:-}" ]; then
  STDLIB="$(dirname "$KOTLINC")/../lib/kotlin-stdlib.jar"
else
  CACHE="${KOTLIN_CACHE:-$HOME/.cache/maya-kotlinc}"
  if [ ! -x "$CACHE/kotlinc/bin/kotlinc" ]; then
    mkdir -p "$CACHE"
    curl -fL --retry 3 -o "$CACHE/kotlin.zip" "https://github.com/JetBrains/kotlin/releases/download/v$KV/kotlin-compiler-$KV.zip"
    unzip -q -o "$CACHE/kotlin.zip" -d "$CACHE"
  fi
  KOTLINC="$CACHE/kotlinc/bin/kotlinc"
  STDLIB="$CACHE/kotlinc/lib/kotlin-stdlib.jar"
fi
OUT="$(mktemp -d)"
"$KOTLINC" -nowarn -d "$OUT" "$J/PhraseBook.kt" "$J/SampleTrimmer.kt" "$J/UiText.kt" "$ROOT/app/src/test-jvm/PureTests.kt"
java -cp "$OUT:$STDLIB" com.maya.assistant.PureTestsKt
