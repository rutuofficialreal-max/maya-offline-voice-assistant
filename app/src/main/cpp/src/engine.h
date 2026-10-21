// The voice loop:
//   Passive (wake word) -> Listening (Silero VAD) -> Transcribing (Whisper)
//     -> Thinking (Vulkan LLM, streaming) -> Speaking (sherpa TTS, overlapped) -> Passive
#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio_capture.h"
#include "audio_player.h"
#include "common.h"
#include "llm.h"
#include "stt_whisper.h"
#include "tts_sherpa.h"
#include "vad.h"
#include "wake_word.h"

namespace maya {

struct EngineConfig {
  // model files
  std::string kwsEncoder, kwsDecoder, kwsJoiner, kwsTokens;
  std::string vadModel, whisperModel, llmModel;
  std::string ttsModel, ttsTokens, ttsDataDir;
  std::string systemPrompt =
      "You are Maya, a fast local assistant. Your answers must be conversational, natural, and strictly capped at a "
      "50-word limit.";

  // wake word
  float kwsThreshold = 0.07f;
  float kwsBoost = 1.5f;
  std::vector<std::string> kwsPhrases;  // calibrated pronunciations (token strings)
  // capture / VAD
  int endSilenceMs = 800;         // silence after speech that ends the command
  int noSpeechTimeoutMs = 6000;   // give up if nobody speaks after the wake word
  int maxCommandMs = 15000;
  int prerollMs = 600;            // audio kept from just before the detection (wake-word latency)
  float vadThreshold = 0.5f;
  // models
  int whisperThreads = 2;
  bool whisperGpu = false;
  bool whisperAdaptiveCtx = true;
  int llmGpuLayers = 99;
  int llmCtx = 2048;
  int llmThreads = 2;
  int maxWords = 50;
  float minGpuTps = 4.0f;
  float ttsSpeed = 1.0f;
  int ttsThreads = 2;
  bool pinBigCores = true;

  // "key=value" per line (see NativeBridge.kt / ModelManager.kt).
  static EngineConfig parse(const std::string& text);
};

class MayaEngine {
 public:
  explicit MayaEngine(EventSink sink) : sink_(std::move(sink)) {}
  ~MayaEngine();
  MayaEngine(const MayaEngine&) = delete;
  MayaEngine& operator=(const MayaEngine&) = delete;

  bool init(const EngineConfig& cfg, std::string* err);  // loads every model (slow: call off the UI thread)
  bool start();                                          // opens the mic and starts the loop
  void stop();                                           // joins threads, closes audio (idempotent)
  void interrupt();                                      // cut the current answer short

 private:
  // One spoken answer: the pipeline thread produces text chunks, the TTS thread consumes them.
  struct Utterance {
    std::mutex mu;
    std::condition_variable cv;
    std::deque<std::string> chunks;
    bool closed = false;
    bool done = false;
    int64_t t0 = 0;
    int firstAudioMs = -1;
  };

  void pipelineLoop();
  void ttsLoop();
  bool waitForWake(std::vector<int16_t>& preroll);
  bool captureCommand(const std::vector<int16_t>& preroll, std::vector<float>& pcm);
  void respond(const std::string& text, int sttMs);
  void setState(State s);
  void emit(int type, const std::string& payload) const {
    if (sink_) sink_(type, payload);
  }

  EventSink sink_;
  EngineConfig cfg_;
  std::vector<int> bigCores_;

  AudioCapture capture_;
  AudioPlayer player_;
  WakeWord kws_;
  Vad vad_;
  WhisperStt stt_;
  Llm llm_;
  Tts tts_;

  std::atomic<bool> running_{false};
  std::atomic<bool> interrupt_{false};
  std::atomic<State> state_{State::Stopped};
  std::string backendInfo_;

  std::thread pipeline_;
  std::thread ttsThread_;
  std::mutex jobMu_;
  std::condition_variable jobCv_;
  std::shared_ptr<Utterance> job_;
};

}  // namespace maya
