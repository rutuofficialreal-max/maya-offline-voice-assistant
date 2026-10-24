// Quality gate for wake-word calibration samples (pure C++, no model dependencies).
#pragma once
#include <string>

namespace maya {

struct WakeSampleVerdict {
  bool ok = false;
  std::string reason;   // user-facing explanation when !ok (mentions what was heard)
  std::string heard;    // what the model heard, e.g. "HEY MYA"
};

// `tokens` uses keyword-file syntax ("▁HE Y ▁MY A"; "▁" starts a word). A sample is accepted when it
// sounds like "<greeting> <name starting with M>": 2-4 words, the name part (everything after the
// first word) starts with M, has at least 3 letters and is not a very common word, because the
// phrase becomes a trigger and "hey my" / "hey may" occur in everyday speech.
WakeSampleVerdict evaluateWakeSample(const std::string& tokens);

}  // namespace maya
