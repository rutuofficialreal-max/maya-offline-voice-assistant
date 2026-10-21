// Offline neural TTS: sherpa-onnx VITS runtime with a Piper female voice (Jenny).
#pragma once
#include <functional>
#include <memory>
#include <string>

#include "sherpa-onnx/c-api/c-api.h"

namespace maya {

struct TtsOptions {
  std::string model, tokens, dataDir;  // dataDir = espeak-ng-data
  int threads = 2;
};

class Tts {
 public:
  Tts() = default;
  ~Tts() = default;
  Tts(const Tts&) = delete;
  Tts& operator=(const Tts&) = delete;

  bool init(const TtsOptions& opt, std::string* err);
  int sampleRate() const { return sampleRate_; }

  // Blocking. `sink` receives PCM as soon as it is generated; return false to abort.
  bool synthesize(const std::string& text, float speed, const std::function<bool(const float*, int)>& sink);

 private:
  struct Del {
    void operator()(const SherpaOnnxOfflineTts* p) const { if (p) SherpaOnnxDestroyOfflineTts(p); }
  };
  struct AudioDel {
    void operator()(const SherpaOnnxGeneratedAudio* p) const { if (p) SherpaOnnxDestroyOfflineTtsGeneratedAudio(p); }
  };
  std::unique_ptr<const SherpaOnnxOfflineTts, Del> tts_;
  int sampleRate_ = 22050;
};

}  // namespace maya
