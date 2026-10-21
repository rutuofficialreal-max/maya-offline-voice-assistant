// Shared types and helpers for the Maya native core.
#pragma once
#include <android/log.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

#define MAYA_TAG "Maya"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, MAYA_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, MAYA_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, MAYA_TAG, __VA_ARGS__)

namespace maya {

constexpr int kMicRate = 16000;  // wake word, VAD and Whisper all run at 16 kHz mono

enum class State : int {
  Stopped = 0,
  Passive = 1,       // listening only for "Hey Maya"
  Listening = 2,     // wake word heard, recording the command (Silero VAD)
  Transcribing = 3,  // Whisper
  Thinking = 4,      // LLM running, nothing spoken yet
  Speaking = 5,      // TTS audio is playing
  Error = 6,
};

// Events delivered to Kotlin through a single callback: onEvent(type, utf8Bytes).
enum EventType : int {
  EVT_STATE = 1,        // payload: decimal State value
  EVT_WAKE = 2,         // payload: empty
  EVT_TRANSCRIPT = 3,   // payload: what the user said
  EVT_REPLY_DELTA = 4,  // payload: streamed reply text
  EVT_REPLY_DONE = 5,   // payload: empty
  EVT_ERROR = 6,        // payload: message
  EVT_INFO = 7,         // payload: status line
  EVT_METRIC = 8,       // payload: timing summary
};

using EventSink = std::function<void(int type, const std::string& payload)>;

inline int64_t nowMs() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace maya
