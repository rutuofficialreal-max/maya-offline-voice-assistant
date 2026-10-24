#include "speech_assembler.h"

#include <algorithm>
#include <cmath>

#include "text_utils.h"

namespace maya {

bool SpeechAssembler::emit(const float* s, size_t n) {
  if (n == 0) return true;
  emitted_ += n;
  return sink_(s, n);
}

bool SpeechAssembler::silence(int ms) {
  if (ms <= 0) return true;
  const size_t n = static_cast<size_t>(tts_.sampleRate()) * static_cast<size_t>(ms) / 1000;
  if (zeros_.size() < n) zeros_.assign(n, 0.0f);
  return emit(zeros_.data(), n);
}

bool SpeechAssembler::speak(const std::string& chunk) {
  const std::string text = sanitizeForSpeech(chunk);
  if (text.empty()) return true;  // nothing speakable (e.g. only markdown); keep the owed pause
  bool firstInChunk = true;
  bool stopped = false;
  const int rate = tts_.sampleRate();
  const bool ok = tts_.synthesize(text, speed_, style_.silenceScale, [&](const float* s, int n) {
    size_t a = 0, b = 0;
    trimSilenceBounds(s, static_cast<size_t>(n), rate, 15, &a, &b);
    if (b <= a) return true;
    if (anyAudio_ && !silence(firstInChunk ? pendingSeamMs_ : style_.seamSentenceMs)) { stopped = true; return false; }
    if (!emit(s + a, b - a)) { stopped = true; return false; }
    anyAudio_ = true;
    firstInChunk = false;
    return true;
  });
  pendingSeamMs_ = seamAfterChunk(chunk, style_);
  return ok && !stopped;
}

}  // namespace maya
