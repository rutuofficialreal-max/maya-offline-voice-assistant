#include "wake_gate.h"

#include <cctype>
#include <sstream>
#include <vector>

namespace maya {

namespace {

const char kMarker[] = "▁";  // U+2581, SentencePiece word-start marker (3 bytes in UTF-8)

bool startsWithMarker(const std::string& tok) { return tok.compare(0, 3, kMarker) == 0; }

// Words that would fire all the time in normal speech if used as "hey <word>".
bool tooCommon(const std::string& w) {
  static const char* kCommon[] = {"MY",   "ME",   "MAY",  "MAN",  "MEN",   "MAKE", "MADE", "MORE", "MOST", "MUCH",
                                  "MUST", "MIGHT", "MIND", "MINE", "MISS", "MANY", "MOVE", "MEAN", "MOM",  "MUM",
                                  "MAMA", "MR",   "MRS",  "MS",   "MAYBE", "MAD",  "MAP",  "MAT",  "MET",  "MIX"};
  for (const char* c : kCommon) if (w == c) return true;
  return false;
}

}  // namespace

WakeSampleVerdict evaluateWakeSample(const std::string& tokens) {
  WakeSampleVerdict v;
  std::vector<std::string> words;
  std::istringstream in(tokens);
  std::string tok;
  while (in >> tok) {
    std::string piece = startsWithMarker(tok) ? tok.substr(3) : tok;
    if (startsWithMarker(tok) || words.empty()) words.push_back(std::string());
    for (char c : piece) words.back() += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  for (size_t i = 0; i < words.size(); ++i) {
    if (i) v.heard += ' ';
    v.heard += words[i];
  }
  const std::string quoted = "“" + v.heard + "”";

  if (words.empty()) {
    v.reason = "Nothing was heard.";
    return v;
  }
  if (words.size() < 2) {
    v.reason = "Heard " + quoted + ". Please say “Hey Maya”, including the “Hey”.";
    return v;
  }
  // "Hey Maya" = greeting + name. The name may be split by a pause ("MA YA", "MY A") into at most
  // two words, the second being a short syllable.
  if (words.size() > 3 || (words.size() == 3 && words[2].size() > 3)) {
    v.reason = "Heard " + quoted + ". Please say just “Hey Maya”.";
    return v;
  }
  if (words[0].size() > 5) {
    v.reason = "Heard " + quoted + ". That does not sound like “Hey”. Try again, a little slower.";
    return v;
  }
  std::string name;
  for (size_t i = 1; i < words.size(); ++i) name += words[i];
  if (name.empty() || name[0] != 'M') {
    v.reason = "Heard " + quoted + ". I could not hear the “M” of Maya. Try again, a little slower.";
    return v;
  }
  if (name.size() < 3 || name.size() > 8 || tooCommon(name)) {
    v.reason = "Heard " + quoted + ". That is a very common phrase and would trigger by accident. "
               "Say “Hey Maya” again with a clear “ya” at the end.";
    return v;
  }
  v.ok = true;
  return v;
}

}  // namespace maya
