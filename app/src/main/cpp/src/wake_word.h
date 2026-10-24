// "Hey Maya" wake-word detection: sherpa-onnx streaming keyword spotter (Zipformer, 3.3M params).
// Open-vocabulary, Apache-2.0, fully offline, no access key / account needed.
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "sherpa-onnx/c-api/c-api.h"

namespace maya {

struct WakeWordOptions {
  std::string encoder, decoder, joiner, tokens;
  float threshold = 0.07f;  // trigger probability: lower = more sensitive (more false accepts)
  float boost = 1.5f;       // keyword score boost (1.5-2.0 worked best in host tests; higher hurts)
  int threads = 1;
  // Pronunciation variants as BPE token strings, e.g. "▁HE Y ▁MY R". These come from the
  // calibration step ("say Hey Maya 3 times"): the tokens the model *actually emits* for the
  // user's voice. They are added to the built-in factory phrases ("MAY-uh" / "MY-uh" pronunciations).
  std::vector<std::string> phrases;
};

// Calibration helper (also validates it): decodes one recording (16 kHz mono int16) with the wake-word model used as
// a tiny streaming ASR. `text` = what the model heard ("HEY MARIA"), `tokens` = BPE pieces in
// keyword-file syntax ("▁HE Y ▁MAR IA").
bool decodeWakeCalibration(const WakeWordOptions& model, const int16_t* pcm, size_t n, std::string* text,
                           std::string* tokens, std::string* err);

class WakeWord {
 public:
  WakeWord() = default;
  ~WakeWord() = default;  // stream_ is destroyed before spotter_ (reverse declaration order)
  WakeWord(const WakeWord&) = delete;
  WakeWord& operator=(const WakeWord&) = delete;

  bool init(const WakeWordOptions& opt, std::string* err);
  // Rebuilds the spotter with new phrases / threshold. On failure the previous one stays active.
  // Call from the thread that calls accept().
  bool reconfigure(const WakeWordOptions& opt, std::string* err);
  void reset();                              // start a fresh decoding stream
  bool accept(const float* pcm, size_t n);   // true => "Hey Maya" was just heard

 private:
  struct SpotterDel {
    void operator()(const SherpaOnnxKeywordSpotter* p) const { if (p) SherpaOnnxDestroyKeywordSpotter(p); }
  };
  struct StreamDel {
    void operator()(const SherpaOnnxOnlineStream* p) const { if (p) SherpaOnnxDestroyOnlineStream(p); }
  };
  std::unique_ptr<const SherpaOnnxKeywordSpotter, SpotterDel> spotter_;
  std::unique_ptr<const SherpaOnnxOnlineStream, StreamDel> stream_;
};

}  // namespace maya
