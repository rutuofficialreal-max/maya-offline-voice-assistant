#include <cassert>
#include <iostream>
#include "text_utils.h"
using namespace maya;

static std::vector<std::string> chunksOf(const std::string& full, size_t step) {
  SentenceChunker c; std::vector<std::string> out;
  for (size_t i = 0; i < full.size(); i += step) c.push(full.substr(i, step), out);
  c.flush(out);
  return out;
}
#define CHECK(c) do { if(!(c)) { std::cerr << "FAIL line " << __LINE__ << ": " #c "\n"; fails++; } } while(0)
int main() {
  int fails = 0;
  // --- Utf8Accumulator: split a 3-byte char (€ = E2 82 AC) and a 4-byte emoji across pushes
  { Utf8Accumulator a; std::string s = "a\xE2\x82\xAC" "b\xF0\x9F\x98\x80" "c";
    std::string got; for (size_t i = 0; i < s.size(); ++i) got += a.push(&s[i], 1);
    CHECK(got == s); }
  // --- sanitizer
  CHECK(sanitizeForSpeech("**Hello** there! \xF0\x9F\x98\x80") == "Hello there!");
  CHECK(sanitizeForSpeech("1. First thing\n2. Second thing") == "First thing Second thing");
  CHECK(sanitizeForSpeech("- apples\n- pears & plums") == "apples pears and plums");
  CHECK(sanitizeForSpeech("## Title\nText `code` here") == "Title Text code here");
  CHECK(sanitizeForSpeech("It\xE2\x80\x99s fine\xE2\x80\xA6") == "It's fine...");
  // --- transcript cleaning
  CHECK(cleanTranscript(" [BLANK_AUDIO] ") == "");
  CHECK(cleanTranscript("What time is it? (coughs)") == "What time is it?");
  CHECK(!isMeaningfulTranscript("")); CHECK(!isMeaningfulTranscript("you.")); CHECK(!isMeaningfulTranscript("Uh,"));
  CHECK(isMeaningfulTranscript("Thank you.")); CHECK(isMeaningfulTranscript("What's the weather?"));
  // --- wake phrase stripping
  CHECK(stripWakePhrase("Hey Maya, what time is it?") == "what time is it?");
  CHECK(stripWakePhrase("hey, Maia. Turn on the light") == "Turn on the light");
  CHECK(stripWakePhrase("Maya what's 2 plus 2") == "what's 2 plus 2");
  CHECK(stripWakePhrase("Hey Maya") == "");
  CHECK(stripWakePhrase("Hey what time is it") == "Hey what time is it");
  CHECK(stripWakePhrase("Tell me about Maya Angelou") == "Tell me about Maya Angelou");
  // --- chunker: first chunk released early on a clause, regardless of token granularity
  for (size_t step : {1, 3, 7, 1000}) {
    auto c = chunksOf("Sure, I can help with that. The capital of France is Paris, and it sits on the Seine river. It is lovely in spring!", step);
    CHECK(c.size() == 3);
    if (c.size() == 3) {
      CHECK(c[0] == "Sure, I can help with that.");
      CHECK(c[1] == "The capital of France is Paris, and it sits on the Seine river.");
      CHECK(c[2] == "It is lovely in spring!");
    } else { std::cerr << "step " << step << " got " << c.size() << ":"; for (auto& x : c) std::cerr << " [" << x << "]"; std::cerr << "\n"; }
  }
  { auto c = chunksOf("Well, it is a very nice day outside today and the sun is out", 2);
    CHECK(c.size() >= 1); CHECK(c.back().back() == '.'); }                 // flush adds the terminal period
  { auto c = chunksOf("The value is 3.5 percent. Dr. Smith agrees.", 1);
    CHECK(c.size() == 2); if (c.size()==2) { CHECK(c[0] == "The value is 3.5 percent."); CHECK(c[1] == "Dr. Smith agrees."); } }
  { auto c = chunksOf("Here you go:\n1. Apples\n2. Pears\nEnjoy.", 4);
    for (auto& x : c) std::cout << "  list chunk: [" << x << "]\n"; }
  { auto c = chunksOf("one two three four five six seven eight nine ten eleven twelve thirteen fourteen", 5);
    CHECK(c.size() >= 2); }                                                  // forced flush on long run-on text
  std::cout << (fails ? "SOME TESTS FAILED\n" : "all text_utils tests passed\n");
  return fails;
}
