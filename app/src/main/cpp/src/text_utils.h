// Pure text helpers (no Android / model dependencies, unit-testable on the host).
#pragma once
#include <string>
#include <vector>

namespace maya {

// Streaming UTF-8 guard: LLM tokens can end in the middle of a multi-byte character.
// push() returns only complete code points and keeps the dangling tail for next time.
class Utf8Accumulator {
 public:
  std::string push(const char* data, size_t n);
  void clear() { pending_.clear(); }

 private:
  std::string pending_;
};

std::string trim(const std::string& s);
int countWords(const std::string& s);

// Remove markdown, list markers, emoji and other things a TTS voice should not read out.
std::string sanitizeForSpeech(const std::string& in);

// Whisper output -> plain command text: drops "[BLANK_AUDIO]", "(music)", notes, extra spaces.
std::string cleanTranscript(const std::string& raw);
// False for empty text and for the usual Whisper hallucinations on silence ("you", "uh"...).
bool isMeaningfulTranscript(const std::string& cleaned);
// Removes a leading "hey maya," (and spelling variants Whisper produces) from the command.
std::string stripWakePhrase(const std::string& transcript);

// Cuts a token stream into speakable chunks so TTS can start before the LLM is done:
// the first chunk is released early (short clause), later chunks wait for sentence ends.
class SentenceChunker {
 public:
  struct Options {
    int firstMinWords = 4;           // clause break (, ; :) allowed once the first chunk has this many words
    int laterMinWords = 8;           // same for later chunks (999 disables clause breaks)
    int firstMaxWords = 12;          // force a cut after this many words without any punctuation
    int laterMaxWords = 26;
    int firstSentenceMinWords = 1;   // a sentence end closes the first chunk only with at least this many words
    int laterSentenceMinWords = 3;   // short sentences are merged into the next one
    bool holdAll = false;            // release nothing until flush(), then the whole reply as ONE chunk
  };
  SentenceChunker() = default;
  explicit SentenceChunker(const Options& o) : opt_(o) {}

  void reset();
  void push(const std::string& text, std::vector<std::string>& out);
  void flush(std::vector<std::string>& out);

 private:
  bool extract(bool final, std::string& chunk);

  Options opt_;
  std::string buf_;
  int emitted_ = 0;
};

}  // namespace maya
