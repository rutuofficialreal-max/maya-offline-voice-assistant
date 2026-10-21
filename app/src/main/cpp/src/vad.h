// Silero voice-activity detection (through sherpa-onnx / ONNX Runtime).
#pragma once
#include <memory>
#include <string>

#include "sherpa-onnx/c-api/c-api.h"

namespace maya {

struct VadOptions {
  std::string model;
  float threshold = 0.5f;
  float minSilenceSec = 0.8f;   // silence that ends an utterance (the "auto-stop" delay)
  float minSpeechSec = 0.15f;   // ignore blips shorter than this
  int threads = 1;
};

class Vad {
 public:
  Vad() = default;
  ~Vad() = default;
  Vad(const Vad&) = delete;
  Vad& operator=(const Vad&) = delete;

  bool init(const VadOptions& opt, std::string* err);
  void reset();
  // Any chunk size is fine; the detector buffers internally to 512-sample windows.
  void accept(const float* pcm, size_t n);
  // True while an utterance is in progress. Turns false once `minSilenceSec` of
  // silence followed speech, i.e. the speaker has finished.
  bool speechActive() const;

 private:
  struct Del {
    void operator()(const SherpaOnnxVoiceActivityDetector* p) const {
      if (p) SherpaOnnxDestroyVoiceActivityDetector(p);
    }
  };
  std::unique_ptr<const SherpaOnnxVoiceActivityDetector, Del> vad_;
};

}  // namespace maya
