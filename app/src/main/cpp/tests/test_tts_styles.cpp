// Pure tests (no models): chunk boundaries per voice style, seam rules, silence trimming, and a
// first-chunk latency simulation with an LLM streaming at 10 tokens/s.
#include <cmath>
#include <cstdio>
#include <regex>
#include <string>
#include <vector>
#include "speech_utils.h"
#include "tts_style.h"
using namespace maya;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { ++fails; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

// split like a BPE tokenizer would: " word" pieces, long words in two, punctuation separate
static std::vector<std::string> pieces(const std::string& text) {
  std::vector<std::string> out; std::regex re(R"(\s?[A-Za-z0-9']+|[^\sA-Za-z0-9'])"); 
  for (std::sregex_iterator it(text.begin(), text.end(), re), end; it != end; ++it) {
    std::string p = it->str(); if (p.size() > 7 && std::isalpha((unsigned char)p.back())) { out.push_back(p.substr(0, p.size() - 3)); out.push_back(p.substr(p.size() - 3)); } else out.push_back(p); }
  return out; }
struct Run { std::vector<std::string> chunks; std::vector<double> at; };
static Run stream(const std::string& reply, VoiceStyle v, double tokPerSec) {
  SentenceChunker c(styleFor(v).chunker); Run r; auto ps = pieces(reply); double t = 0;
  for (auto& p : ps) { t += 1.0 / tokPerSec; std::vector<std::string> out; c.push(p, out); for (auto& x : out) { r.chunks.push_back(x); r.at.push_back(t); } }
  std::vector<std::string> out; c.flush(out); for (auto& x : out) { r.chunks.push_back(x); r.at.push_back(t); } return r; }

int main() {
  const std::string R1 = "Sure, I can help with that. The capital of France is Paris, and it sits on the Seine river. It is lovely in spring, especially in April, when the trees bloom.";
  const std::string R2 = "Yes! I think it should take about ten minutes, give or take. Let me know if you need anything else.";
  const std::string R3 = "The weather today looks mostly sunny with a high of twenty four degrees, so a light jacket should be plenty.";
  const std::string R4 = "Okay. Setting a timer for five minutes now.";
  const std::string replies[] = {R1, R2, R3, R4};
  const char* names[] = {"fast", "balanced", "smooth"};
  const VoiceStyle styles[] = {VoiceStyle::Fast, VoiceStyle::Balanced, VoiceStyle::Smooth};

  // ---- structural expectations
  { auto r = stream(R1, VoiceStyle::Balanced, 10);   // sentences only; "Sure, ..." must NOT be cut at the comma
    CHECK(r.chunks.size() == 3);
    if (r.chunks.size() == 3) { CHECK(r.chunks[0] == "Sure, I can help with that."); CHECK(r.chunks[1] == "The capital of France is Paris, and it sits on the Seine river."); CHECK(r.chunks[2] == "It is lovely in spring, especially in April, when the trees bloom."); } }
  { auto r = stream(R1, VoiceStyle::Fast, 10); CHECK(r.chunks.size() >= 4); CHECK(!r.chunks.empty() && r.chunks[0] == "Sure, I can help with that."); }   // fast: cuts at clauses later on
  { auto r = stream(R1, VoiceStyle::Smooth, 10); CHECK(r.chunks.size() == 1); if (r.chunks.size() == 1) CHECK(r.chunks[0] == R1); CHECK(!r.at.empty()); }
  { auto r = stream(R4, VoiceStyle::Balanced, 10); CHECK(r.chunks.size() == 1); if (!r.chunks.empty()) CHECK(r.chunks[0] == R4); }   // "Okay." merged with the next sentence (first chunk needs >= 6 words)
  { auto r = stream(R2, VoiceStyle::Balanced, 10); CHECK(r.chunks.size() == 2); if (r.chunks.size() == 2) CHECK(r.chunks[0] == "Yes! I think it should take about ten minutes, give or take."); }
  { auto r = stream(R3, VoiceStyle::Balanced, 10);   // one long sentence: the first chunk may cut at a comma once it has >= 10 words
    CHECK(r.chunks.size() == 2); if (r.chunks.size() == 2) CHECK(r.chunks[0] == "The weather today looks mostly sunny with a high of twenty four degrees,"); }
  // ---- seams
  TtsStyle b = styleFor(VoiceStyle::Balanced);
  CHECK(seamAfterChunk("It is done.", b) == b.seamSentenceMs); CHECK(seamAfterChunk("Well,", b) == b.seamClauseMs);
  CHECK(seamAfterChunk("He said \"stop!\"", b) == b.seamSentenceMs); CHECK(seamAfterChunk("and then the", b) == b.seamForcedMs); CHECK(seamAfterChunk("", b) == b.seamForcedMs);
  // ---- trimming
  { std::vector<float> x(16000, 0.0f); for (int i = 6000; i < 9000; ++i) x[i] = 0.3f; size_t a, bb; trimSilenceBounds(x.data(), x.size(), 16000, 15, &a, &bb);
    CHECK(a == 6000 - 240 && bb == 9000 + 240);
    std::vector<float> z(100, 0.0f); trimSilenceBounds(z.data(), z.size(), 16000, 15, &a, &bb); CHECK(a == bb);
    std::vector<float> e(50, 0.5f); trimSilenceBounds(e.data(), e.size(), 16000, 15, &a, &bb); CHECK(a == 0 && bb == 50); }

  printf("\nTime until the first chunk can be spoken (LLM at 10 tok/s; excludes TTS time):\n%-10s", "reply");
  for (auto n : names) printf(" %9s", n); printf("   (chunks)\n");
  for (int i = 0; i < 4; ++i) { printf("R%-9d", i + 1); std::string counts;
    for (int s = 0; s < 3; ++s) { auto r = stream(replies[i], styles[s], 10); printf(" %7.1f s", r.at.empty() ? 0.0 : r.at[0]); counts += " " + std::to_string(r.chunks.size()); }
    printf("   %s\n", counts.c_str()); }
  printf(fails ? "\nSOME STYLE TESTS FAILED\n" : "\nall voice-style tests passed\n"); return fails;
}
