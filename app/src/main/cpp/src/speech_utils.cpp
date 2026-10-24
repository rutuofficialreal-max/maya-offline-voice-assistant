#include "speech_utils.h"

#include <algorithm>
#include <cmath>

namespace maya {

void trimSilenceBounds(const float* s, size_t n, int sampleRate, int keepMs, size_t* a, size_t* b) {
  const float thr = 0.01f;
  size_t first = 0;
  while (first < n && std::fabs(s[first]) <= thr) ++first;
  if (first == n) {
    *a = *b = 0;
    return;
  }
  size_t last = n;
  while (last > first && std::fabs(s[last - 1]) <= thr) --last;
  const size_t keep = static_cast<size_t>(sampleRate) * static_cast<size_t>(keepMs) / 1000;
  *a = first > keep ? first - keep : 0;
  *b = std::min(n, last + keep);
}

int seamAfterChunk(const std::string& chunk, const TtsStyle& style) {
  size_t i = chunk.size();
  while (i > 0) {  // skip closing quotes / brackets / spaces
    const char c = chunk[i - 1];
    if (c == '"' || c == '\'' || c == ')' || c == ' ' || c == '\n') --i; else break;
  }
  if (i == 0) return style.seamForcedMs;
  switch (chunk[i - 1]) {
    case '.': case '!': case '?': return style.seamSentenceMs;
    case ',': case ';': case ':': return style.seamClauseMs;
    default: return style.seamForcedMs;
  }
}

}  // namespace maya
