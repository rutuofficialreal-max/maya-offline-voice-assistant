// Speech-to-text: whisper.cpp with a quantized ggml model.
#pragma once
#include <atomic>
#include <string>

struct whisper_context;

namespace maya {

class WhisperStt {
 public:
  WhisperStt() = default;
  ~WhisperStt();
  WhisperStt(const WhisperStt&) = delete;
  WhisperStt& operator=(const WhisperStt&) = delete;

  // adaptiveCtx: shrink Whisper's 30 s window to the clip length (much faster, tiny accuracy risk).
  bool init(const std::string& modelPath, int threads, bool useGpu, bool adaptiveCtx, std::string* err);
  // 16 kHz mono float PCM -> text. Empty on failure / abort.
  std::string transcribe(const float* pcm, size_t n, const std::atomic<bool>* abort, int* elapsedMs);

 private:
  whisper_context* ctx_ = nullptr;
  int threads_ = 2;
  bool adaptiveCtx_ = true;
};

}  // namespace maya
