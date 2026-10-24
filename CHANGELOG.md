# Changelog

## 1.1.0
Own wake word (trained on your voice)
- New waking engine: a 170k-parameter CNN trained on 180 recordings of *your* "Hey Maya"
  (`hey_maya.onnx` -> `assets/wake_weights.bin`, 708 KiB, shipped in the APK). Held-out result:
  28/28 recall, 0 false wakes/hour on 24 min of held-out noise/music/speech.
- The model runs as a **hand-written C++ forward pass** (`wake_net.cpp`): own Hann window, 512-point
  FFT, 40-band mel, 8 conv layers, 3-of-5 window patience. No ONNX Runtime in the wake path, no
  openWakeWord feature models, nothing to download.
- Settings -> **Wake engine**: `Maya-Wake (trained)` / `sherpa (default)`. Both engines stay loaded
  and the switch is live (`nativeSetWakeEngine`), so trying it costs nothing. sherpa remains the
  default until the new engine has been tested on the phone.
- The sensitivity slider drives whichever engine is active: for Maya-Wake it maps to the trained
  threshold curve (default 55 -> 0.9976, the value picked in validation).
- `tools/export_wake_net.py` folds BatchNorm and proves numerically that the C++ maths equals the
  ONNX model (<=1e-8 on the numpy path, <=3e-5 through the float32 C++ path);
  `cpp/tests/test_wake_net.cpp` re-checks it in CI on every push.

Builds
- **Stable release signing key.** From this version the APK is signed with a real release key kept in
  the repository secrets, so updates install over the previous version instead of needing an uninstall.
  CI falls back to the debug key when the secrets are absent (forks, local builds).

## 1.0.4
Voice recorder for the training data
- New card **Voice dataset (your own wake word)**: `Record a 75 s session` captures "Hey Maya" over and
  over, cuts each utterance with a 20 ms energy detector (150 ms of silence kept on each side) and
  writes one 16 kHz / 16-bit / mono WAV per clip into the app's private folder.
- `Export ZIP` writes `Maya-wake-voice-<date>.zip` (Download/Maya, plus the app's external folder) —
  one session per folder, README with counts. Nothing is uploaded by the app.
- A vibration confirms every captured clip, and the card shows the running total across sessions.

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
