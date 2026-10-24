# Changelog

## 1.0.3
Calibration
- Gate rewritten (`wake_gate.cpp`): accepts "<greeting> <name starting with M>" in 2-3 words, refuses names that are
  everyday words ("hey my", "hey may", "hey mom") and tells the user what it heard. The v1.0.2 gate demanded exactly the
  tokens `HEY` + one word and rejected most genuine Indian-English attempts.
- No beep during calibration (it bled into the clip): a short vibration is the cue. Each clip is energy-trimmed
  (`SampleTrimmer`) before recognition.
- Keeps the **newest** four phrases (`PhraseBook.merge`); the old code kept the oldest four, so re-calibrating changed nothing.
- "HEY MYA" (the pronunciation that worked on the real phone) is now a built-in factory phrase.
- Calibrating while Maya runs pauses and restarts her automatically. The sensitivity slider applies live
  (`nativeSetWake` -> `WakeWord::reconfigure`, no model reload).

Voice
- Voice styles Fast / Balanced / Smooth (Settings). Balanced is the default.
- `silence_scale` is now 0.5 instead of sherpa's 0.2, which collapsed every pause longer than 0.2 s inside the
  audio (measured: no audible pause at all in an 8 s answer). Pauses between sentences/clauses are inserted
  explicitly by `SpeechAssembler`, after trimming each sentence's near-silent edges.

Quality
- `scripts/check_interfaces.py`: lints Kotlin format strings, Kotlin<->C++ config keys and JNI signatures. It flags the
  v1.0.0 crash line (`DuplicateFormatFlagsException`) on the old commits.
- All UI strings built with `format()` moved to `UiText.kt` and are exercised by plain-JVM tests.
- CI: new fast `checks` job (lint, C++ host tests incl. ThreadSanitizer, JVM tests); builds only on pushes to `main`
  (not for docs), cancel superseded runs.

## 1.0.2 (changes made by the person who ran v1.0.0 on the phone)
- Fixed the launch crash: the literal `%` in the sensitivity label (`DuplicateFormatFlagsException` in `render()`).

## 1.0.1
- `MayaApp` crash recorder with start-up breadcrumbs, on-screen error report with Share/Copy, guarded lifecycle,
  `OrbView` zero-size guard.

## 1.0.0
- Vulkan: `ggml-vulkan` includes `<spirv/unified1/spirv.hpp>` but llama.cpp b11748 never attaches the SPIRV-Headers include
  directory to the target; added in `CMakeLists.txt`.
- First release: wake word, Silero VAD, Whisper, Llama 3.2 on Vulkan, streaming Piper voice.
