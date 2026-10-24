// Pure audio/text helpers for the voice output (no model or Android dependencies, host-testable).
#pragma once
#include <cstddef>
#include <string>

#include "tts_style.h"

namespace maya {

// Index range [*a, *b) of `s` without leading/trailing near-silence (|x| <= 0.01, the threshold sherpa
// uses for its own silence detection), keeping `keepMs` of margin. a == b means "all silent".
void trimSilenceBounds(const float* s, size_t n, int sampleRate, int keepMs, size_t* a, size_t* b);

// Pause owed after `chunk`, from how it ends.
int seamAfterChunk(const std::string& chunk, const TtsStyle& style);

}  // namespace maya
