// Voice styles: how an answer is cut into pieces for the TTS and how the pieces are joined.
#pragma once
#include <string>

#include "text_utils.h"

namespace maya {

enum class VoiceStyle {
  Fast,      // first words after ~4 words / first comma; lowest latency, prosody restarts at clause seams
  Balanced,  // whole sentences only; natural prosody, starts after the first sentence
  Smooth,    // waits for the complete reply, one TTS call; no gaps even if the LLM is slower than speech
};

struct TtsStyle {
  SentenceChunker::Options chunker;
  int seamSentenceMs = 260;  // pause inserted between sentences
  int seamClauseMs = 130;    // ... after a chunk that ended at a comma / semicolon / colon
  int seamForcedMs = 40;     // ... after a run-on that had to be cut without punctuation
  float silenceScale = 0.5f; // pauses inside the audio: Jenny's comma pause 350-400 ms -> ~180 ms
};

inline VoiceStyle parseVoiceStyle(const std::string& s) {
  if (s == "fast") return VoiceStyle::Fast;
  if (s == "smooth") return VoiceStyle::Smooth;
  return VoiceStyle::Balanced;
}

inline TtsStyle styleFor(VoiceStyle v) {
  TtsStyle t;
  SentenceChunker::Options& c = t.chunker;
  switch (v) {
    case VoiceStyle::Fast:
      c.firstMinWords = 4; c.laterMinWords = 8; c.firstMaxWords = 12; c.laterMaxWords = 26;
      c.firstSentenceMinWords = 1; c.laterSentenceMinWords = 3; c.holdAll = false;
      t.seamSentenceMs = 200; t.seamClauseMs = 90; t.seamForcedMs = 30;
      break;
    case VoiceStyle::Smooth:
      c.holdAll = true;
      break;
    case VoiceStyle::Balanced:
    default:
      c.firstMinWords = 10; c.laterMinWords = 999; c.firstMaxWords = 18; c.laterMaxWords = 40;
      c.firstSentenceMinWords = 6; c.laterSentenceMinWords = 4; c.holdAll = false;
      break;
  }
  return t;
}

}  // namespace maya
