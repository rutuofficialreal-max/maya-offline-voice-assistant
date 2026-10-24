# Maya: a fully offline voice assistant for Android

Say **"Hey Maya"** and ask anything. Everything runs on the phone: wake word, speech recognition,
the language model and the voice. The APK is small; the AI models (~950 MB) are downloaded once after install.

```
mic ─▶ wake word ─▶ "Maya is listening..." ─▶ Silero VAD ─▶ Whisper ─▶ LLM (Vulkan GPU) ─▶ TTS ─▶ speaker
       sherpa-onnx    notification            auto-stop      whisper.cpp  llama.cpp          sherpa-onnx
       keyword spotter                        on silence                  streams text       VITS "Jenny"
                                                                          into TTS per clause
```

Built for a Redmi 17 5G (Snapdragon 4 Gen 5: 2× Cortex-A78 + 6× Cortex-A55, Adreno GPU, 8 GB RAM).

## 1. Build the APK on GitHub (nothing to install on a PC)

1. Create an empty GitHub repository and push this whole folder to it
   (from a PC, or on the phone with Termux: `git init && git add -A && git commit -m maya && git push`).
2. Open the repository's **Actions** tab → **Build Maya APK**. A cold run takes about 5 minutes (measured: llama.cpp with the
   Vulkan shaders builds in ~3.5 min); a fast **checks** job (lint + unit tests) runs next to it.
3. When it is green, open the run and download the artifact **Maya-apk**.
   `Maya-vulkan.apk` has the GPU backend. If the Vulkan toolchain fails on the GitHub runner, the workflow
   automatically builds `Maya-cpu-only.apk` instead, so you always get an installable app.
4. Install it (allow "Install unknown apps" for your browser/files app).

If a build fails, open the failed step, copy the last ~40 log lines and send them to me.

## 2. First run

1. **Download models**: ~950 MB, resumable, with a progress notification (Wi-Fi only by default).
2. **Calibrate "Hey Maya"** (about 20 seconds): when the phone gives a short vibration, say it. Three accepted samples
   are needed. This matters: the wake-word model is a tiny English recogniser, and different accents are *heard* as
   different spellings ("Maya" may come out as "Maya", "Mya", "Myer" or "Maria"). Calibration records the spelling the
   model hears for *your* voice and adds it as a trigger phrase. It shows what it heard after every sample; samples
   that would cause false triggers ("hey my", "hey may") or that are not "hey + a name starting with M" are refused with
   a hint. The newest four phrases are kept. If Maya is running she is paused and restarted automatically.
3. **Start Maya**, grant the microphone and notification permissions, then tap **Allow background running**.
   On Xiaomi/HyperOS also enable *Autostart*, set Battery to *No restrictions*, and lock the app in Recents,
   otherwise the system will kill the listener.
4. Say: *"Hey Maya, what's the capital of France?"*

After a reboot, open the app and tap Start once (Android 14+ does not allow a microphone service to be
started from the background).

## 3. What is where

| Area | Files |
|---|---|
| Voice loop / state machine | `cpp/src/engine.cpp` (Passive → Listening → Transcribing → Thinking → Speaking) |
| Microphone / speaker (AAudio) | `audio_capture.cpp`, `audio_player.cpp`, lock-free `ring_buffer.h` |
| Wake word + calibration | `wake_word.cpp` (sherpa-onnx streaming keyword spotter) |
| Silero VAD | `vad.cpp` |
| Whisper.cpp STT | `stt_whisper.cpp` (audio window shrunk to the clip length for speed) |
| Vulkan LLM | `llm.cpp` (llama.cpp, KV prefix cache, 50-word cap, GPU benchmark → CPU fallback) |
| VITS voice | `tts_sherpa.cpp`, `text_utils.cpp` (clause chunker streams LLM text into TTS) |
| 2 big cores | `cpu_affinity.cpp` (pins threads to the A78 cores that Android allows) |
| JNI | `maya_jni.cpp` ↔ `NativeBridge.kt` |
| Android | `MainActivity`, `MayaService` (microphone FGS), `ModelDownloadService` (dataSync FGS), `Downloader` |

All native objects are RAII (`unique_ptr` with the libraries' destroy functions); threads are joined in
`MayaEngine::stop()`; the JNI global reference is released in `nativeDestroy`.

## 4. Tuning

* **Wake sensitivity**: slider in the app (threshold 0.30 → 0.02, log scale). Applied live while Maya runs.
* **Voice style** (Settings): *Fast* starts after the first clause (the voice restarts at each clause); *Balanced* (default)
  speaks whole sentences with natural pauses and starts after the first sentence; *Smooth* waits for the full answer and
  speaks it in one go, which avoids gaps if the CPU fallback generates text more slowly than it is spoken. Pauses are
  inserted deliberately (about 260 ms between sentences, 130 ms after a comma), and `silence_scale` is 0.5: sherpa's
  default of 0.2 shrinks every pause longer than 0.2 s inside the audio, so commas disappeared completely.
* **Pause before Maya answers**: `vad.end_silence_ms` in `ModelManager.buildConfig` (default 800 ms).
* **Whisper accuracy vs speed**: `whisper.adaptive_ctx=0` (in `ModelManager.buildConfig`) uses Whisper's full 30 s window: slower, but the model's native mode.
* **Other model**: edit `ModelManager.ITEMS` (e.g. Qwen2.5-1.5B-Instruct Q4_K_M GGUF). The chat template is read
  from the GGUF. Whisper tiny is faster, `base.en` more accurate.
* **GPU**: the status line shows the backend in use. At start-up a short benchmark compares the GPU against
  `llm.min_gpu_tps` and falls back to the CPU if it is slower. If the Vulkan driver *crashes* during loading, the
  next start disables the GPU automatically (switch it back on in Settings to retry).
* Logs: `adb logcat -s Maya Maya.ggml MayaService`.

## 5. Things you should know

* **Wake word engine**: I used sherpa-onnx's open-vocabulary keyword spotter instead of Porcupine because Porcupine
  needs a Picovoice account, an access key and a console-trained keyword file; this one needs nothing and shares
  the runtime with VAD and TTS. It is less accurate than Porcupine, which is why calibration exists.
* **Whisper is 5-bit** (`base.en-q5_1`): the official quantised files are q5_1 and q8_0. For true 4-bit, quantise with
  whisper.cpp's `quantize` tool and host the file yourself.
* **System prompt** is exactly the one you specified; replies are also hard-capped at 50 words in code.
* The microphone is ignored while Maya thinks and speaks (no echo self-trigger). Use the notification's
  **Silence** button to stop an answer.
* "Always listening" shows the green microphone dot and costs battery; the wake word itself is light.
* Adreno + Vulkan + llama.cpp is not guaranteed to beat the CPU on a mid-range chip; the app measures instead of assuming.
* **Known upstream leak**: espeak-ng inside sherpa-onnx leaks about 1.3 KB per *spoken sentence* (reproduced with
  LeakSanitizer; it is in the prebuilt library, not in Maya's code). That is roughly 1 MB per few hundred sentences.
  Stopping the app's process (Settings → Force stop) clears it.

## 6. What was and was not verified

Checked on a Linux host against the exact pinned library versions: the whole native core compiles and links through
`CMakeLists.txt` (CPU build, zero unresolved symbols); the wake-word, calibration, VAD and TTS code ran with the real
models (8/8 clean "Hey Maya" detections and 0/33 false accepts at the default sensitivity on synthetic speech; the
VAD ended 850 ms after speech stopped; the voice-style assembler produced the intended pauses); text, ring buffer, VAD
and wake-word code are clean under AddressSanitizer/UBSan/ThreadSanitizer and a 20k-string fuzz; all Kotlin
type-checks against the API 35 `android.jar`, and the pure Kotlin logic runs on a plain JVM.

Reported from a real Redmi 17 5G (Android 16) after v1.0.2: the Vulkan build compiles, the whole loop works, and the
first answer arrives about 7.8 s after speaking (see CHANGELOG.md).

**Not** verified here: anything that needs the phone's screen, GPU or microphone (UI rendering, Vulkan behaviour, the
wake word with a real voice), and Whisper/LLM accuracy. Type-checking cannot catch runtime-only bugs, which is how the
`%` crash in v1.0.0 slipped through; `scripts/check_interfaces.py` and the JVM tests now guard that class of bug.

## 7. Checks and tests (also run by CI on every push)

```
python3 scripts/check_interfaces.py    # format strings, Kotlin<->C++ config keys, JNI signatures (<1 s)
bash scripts/run_host_tests.sh         # C++: text/chunker, ring buffer (TSan), wake gate, voice styles
bash scripts/run_jvm_tests.sh          # Kotlin pure logic on a plain JVM (downloads kotlinc once)
```

Third-party: llama.cpp, whisper.cpp (MIT), sherpa-onnx (Apache-2.0), Silero VAD (MIT), Piper "Jenny" voice,
Llama 3.2 (Llama 3.2 Community License; review it before sharing the model).
