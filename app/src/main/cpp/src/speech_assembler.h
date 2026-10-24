// Turns text chunks into one continuous audio stream: sanitises the text, synthesises it, trims the
// near-silent edges of every sentence and inserts deliberate pauses between sentences / clauses.
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "speech_utils.h"
#include "tts_sherpa.h"
#include "tts_style.h"

namespace maya {

class SpeechAssembler {
 public:
  using Sink = std::function<bool(const float* samples, size_t n)>;  // false => stop

  SpeechAssembler(Tts& tts, const TtsStyle& style, float speed, Sink sink)
      : tts_(tts), style_(style), speed_(speed), sink_(std::move(sink)) {}

  // Speaks one chunk. Returns false if the sink asked to stop.
  bool speak(const std::string& chunk);

  size_t samplesEmitted() const { return emitted_; }

 private:
  bool emit(const float* s, size_t n);
  bool silence(int ms);

  Tts& tts_;
  TtsStyle style_;
  float speed_;
  Sink sink_;
  bool anyAudio_ = false;
  int pendingSeamMs_ = 0;
  size_t emitted_ = 0;
  std::vector<float> zeros_;
};

}  // namespace maya
