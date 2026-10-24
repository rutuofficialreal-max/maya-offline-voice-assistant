#include "text_utils.h"

#include <cctype>
#include <cstdint>
#include <cstring>

namespace maya {

namespace {

inline bool isSpaceC(unsigned char c) { return std::isspace(c) != 0; }

std::string lowerAscii(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// Decode one UTF-8 code point starting at s[i]; advances i. Invalid bytes map to U+FFFD.
uint32_t nextCodePoint(const std::string& s, size_t& i) {
  const unsigned char c = static_cast<unsigned char>(s[i]);
  size_t need = 0;
  uint32_t cp = 0;
  if (c < 0x80) { ++i; return c; }
  if (c >= 0xF0 && c < 0xF8) { need = 3; cp = c & 0x07; }
  else if (c >= 0xE0) { need = 2; cp = c & 0x0F; }
  else if (c >= 0xC0) { need = 1; cp = c & 0x1F; }
  else { ++i; return 0xFFFD; }
  if (i + need >= s.size()) { i = s.size(); return 0xFFFD; }  // truncated sequence
  for (size_t k = 1; k <= need; ++k) {
    const unsigned char cc = static_cast<unsigned char>(s[i + k]);
    if ((cc & 0xC0) != 0x80) { i += k; return 0xFFFD; }
    cp = (cp << 6) | (cc & 0x3F);
  }
  i += need + 1;
  return cp;
}

void appendCodePoint(std::string& out, uint32_t cp) {
  if (cp < 0x80) out += static_cast<char>(cp);
  else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
  else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

bool isEmojiLike(uint32_t cp) {
  return (cp >= 0x1F000 && cp <= 0x1FAFF) || (cp >= 0x2600 && cp <= 0x27BF) ||
         (cp >= 0x2B00 && cp <= 0x2BFF) || cp == 0xFE0F || cp == 0x200D || cp == 0x20E3 ||
         (cp >= 0xE0000 && cp <= 0xE007F) || cp == 0x266A /* music note */ || cp == 0x266B;
}

// Drops a leading list / heading / quote marker from a line: "- ", "* ", "1. ", "2) ", "## ", "> ".
std::string stripLineMarker(const std::string& line) {
  size_t i = 0;
  while (i < line.size() && isSpaceC(static_cast<unsigned char>(line[i]))) ++i;
  size_t j = i;
  if (j < line.size() && (line[j] == '-' || line[j] == '*' || line[j] == '+' || line[j] == '>')) {
    size_t k = j;
    while (k < line.size() && line[k] == line[j]) ++k;
    if (k < line.size() && isSpaceC(static_cast<unsigned char>(line[k]))) return line.substr(k + 1);
  } else if (j < line.size() && line[j] == '#') {
    size_t k = j;
    while (k < line.size() && line[k] == '#') ++k;
    if (k < line.size() && isSpaceC(static_cast<unsigned char>(line[k]))) return line.substr(k + 1);
  } else if (j < line.size() && std::isdigit(static_cast<unsigned char>(line[j]))) {
    size_t k = j;
    while (k < line.size() && std::isdigit(static_cast<unsigned char>(line[k]))) ++k;
    if (k + 1 < line.size() && (line[k] == '.' || line[k] == ')') &&
        isSpaceC(static_cast<unsigned char>(line[k + 1]))) {
      return line.substr(k + 2);
    }
  }
  return line.substr(i);
}

bool isAbbreviation(const std::string& tokenBeforeDot) {
  static const char* kAbbr[] = {"mr", "mrs", "ms", "dr", "prof", "sr", "jr", "st", "vs", "e.g", "i.e", "a.m", "p.m", "u.s"};
  std::string t = lowerAscii(tokenBeforeDot);
  if (t.size() == 1 && std::isalpha(static_cast<unsigned char>(t[0]))) return true;  // initials: "J."
  for (const char* a : kAbbr) if (t == a) return true;
  return false;
}

}  // namespace

std::string Utf8Accumulator::push(const char* d, size_t n) {
  pending_.append(d, n);
  const size_t len = pending_.size();
  size_t cut = len;
  size_t i = len;
  int cont = 0;
  while (i > 0 && cont < 4) {
    const unsigned char c = static_cast<unsigned char>(pending_[i - 1]);
    if ((c & 0xC0) == 0x80) { ++cont; --i; continue; }
    const size_t need = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    const size_t have = static_cast<size_t>(cont) + 1;
    if (have < need) cut = i - 1;
    break;
  }
  std::string out = pending_.substr(0, cut);
  pending_.erase(0, cut);
  return out;
}

std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && isSpaceC(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && isSpaceC(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}

int countWords(const std::string& s) {
  int words = 0;
  bool in = false;
  for (unsigned char c : s) {
    if (isSpaceC(c)) in = false;
    else if (!in) { in = true; ++words; }
  }
  return words;
}

std::string sanitizeForSpeech(const std::string& in) {
  // 1) strip per-line markers, join lines with spaces
  std::string joined;
  size_t pos = 0;
  while (pos <= in.size()) {
    size_t nl = in.find('\n', pos);
    if (nl == std::string::npos) nl = in.size();
    std::string line = stripLineMarker(in.substr(pos, nl - pos));
    if (!trim(line).empty()) {
      if (!joined.empty()) joined += ' ';
      joined += line;
    }
    pos = nl + 1;
  }
  // 2) per-code-point filtering
  std::string out;
  out.reserve(joined.size());
  for (size_t i = 0; i < joined.size();) {
    const uint32_t cp = nextCodePoint(joined, i);
    if (isEmojiLike(cp) || cp == 0xFFFD || cp == '\r') continue;
    switch (cp) {
      case '*': case '#': case '`': case '~': case '|': case '^': case '[': case ']': case '{': case '}': case '_':
        continue;
      case '&': out += " and "; continue;
      case 0x2018: case 0x2019: out += '\''; continue;
      case 0x201C: case 0x201D: out += '"'; continue;
      case 0x2026: out += "..."; continue;
      case '\t': out += ' '; continue;
      default: appendCodePoint(out, cp);
    }
  }
  // 3) collapse whitespace
  std::string res;
  bool prevSpace = true;
  for (unsigned char c : out) {
    if (isSpaceC(c)) { if (!prevSpace) res += ' '; prevSpace = true; }
    else { res += static_cast<char>(c); prevSpace = false; }
  }
  return trim(res);
}

std::string cleanTranscript(const std::string& raw) {
  std::string out;
  int depthSq = 0, depthPar = 0;
  for (size_t i = 0; i < raw.size();) {
    const uint32_t cp = nextCodePoint(raw, i);
    if (cp == '[') { ++depthSq; continue; }
    if (cp == ']') { if (depthSq) --depthSq; continue; }
    if (cp == '(') { ++depthPar; continue; }
    if (cp == ')') { if (depthPar) --depthPar; continue; }
    if (depthSq || depthPar) continue;
    if (cp == 0xFFFD || cp == 0x266A || cp == 0x266B) continue;
    appendCodePoint(out, cp);
  }
  std::string res;
  bool prevSpace = true;
  for (unsigned char c : out) {
    if (isSpaceC(c)) { if (!prevSpace) res += ' '; prevSpace = true; }
    else { res += static_cast<char>(c); prevSpace = false; }
  }
  return trim(res);
}

bool isMeaningfulTranscript(const std::string& cleaned) {
  int alnum = 0;
  std::string key;
  for (unsigned char c : cleaned) {
    if (std::isalnum(c) || c >= 0x80) { ++alnum; key += static_cast<char>(std::tolower(c)); }
    else if (c == ' ' && !key.empty() && key.back() != ' ') key += ' ';
  }
  while (!key.empty() && key.back() == ' ') key.pop_back();
  if (alnum < 2) return false;
  static const char* kNoise[] = {"you", "uh", "um", "hmm", "mm", "huh", "the", "so", "oh", "ah",
                                 "thanks for watching", "thank you for watching", "bye", "bye bye"};
  for (const char* n : kNoise) if (key == n) return false;
  return true;
}

std::string stripWakePhrase(const std::string& t) {
  const std::string src = trim(t);
  size_t i = 0;
  const size_t n = src.size();
  auto skipSep = [&] {
    while (i < n && (isSpaceC(static_cast<unsigned char>(src[i])) || std::strchr(",.!?;:-", src[i]))) ++i;
  };
  auto readWord = [&]() -> std::string {
    const size_t s = i;
    while (i < n && (std::isalnum(static_cast<unsigned char>(src[i])) || src[i] == '\'')) ++i;
    return lowerAscii(src.substr(s, i - s));
  };
  static const char* kGreet[] = {"hey", "hay", "hi", "hello", "okay", "ok"};
  static const char* kName[] = {"maya", "maia", "mya", "mia", "maiya", "myah", "mayah", "maja"};
  auto in = [](const std::string& w, const char* const* list, size_t cnt) {
    for (size_t k = 0; k < cnt; ++k) if (w == list[k]) return true;
    return false;
  };
  skipSep();
  std::string w = readWord();
  if (in(w, kGreet, sizeof kGreet / sizeof *kGreet)) {
    skipSep();
    w = readWord();
  }
  if (in(w, kName, sizeof kName / sizeof *kName)) {
    skipSep();
    return src.substr(i);
  }
  return src;
}

// ---------------------------------------------------------------- SentenceChunker

void SentenceChunker::reset() {
  buf_.clear();
  emitted_ = 0;
}

bool SentenceChunker::extract(bool final, std::string& chunk) {
  const bool first = emitted_ == 0;
  const int minW = first ? opt_.firstMinWords : opt_.laterMinWords;
  const int maxW = first ? opt_.firstMaxWords : opt_.laterMaxWords;
  const int minStrong = first ? opt_.firstSentenceMinWords : opt_.laterSentenceMinWords;  // merge tiny sentences
  const size_t n = buf_.size();
  int words = 0;
  bool inWord = false;
  bool allDigits = true;  // current word consists of digits only (list numbering)

  for (size_t i = 0; i < n; ++i) {
    const unsigned char c = static_cast<unsigned char>(buf_[i]);
    if (isSpaceC(c)) {
      if (c == '\n' && words >= 1) {
        chunk = buf_.substr(0, i);
        buf_.erase(0, i + 1);
        return true;
      }
      if (inWord) {
        inWord = false;
        if (words >= maxW) {
          chunk = buf_.substr(0, i);
          buf_.erase(0, i + 1);
          return true;
        }
      }
      continue;
    }
    if (!inWord) { inWord = true; ++words; allDigits = true; }
    if (!std::isdigit(c)) allDigits = false;

    const bool strong = (c == '.' || c == '!' || c == '?');
    const bool clause = (c == ',' || c == ';' || c == ':');
    if (!strong && !clause) continue;

    size_t j = i + 1;
    while (j < n && (buf_[j] == '"' || buf_[j] == '\'' || buf_[j] == ')')) ++j;
    if (j >= n) {
      if (!final) return false;  // wait: the next character decides ("3.5" vs "3. ")
      break;
    }
    if (!isSpaceC(static_cast<unsigned char>(buf_[j]))) continue;  // e.g. "3.5", "e.g.x"

    if (strong) {
      if (c == '.') {
        size_t k = i;
        while (k > 0 && !isSpaceC(static_cast<unsigned char>(buf_[k - 1]))) --k;
        if (isAbbreviation(buf_.substr(k, i - k))) continue;
        if (allDigits && words == 1) continue;  // "1. First item"
      }
      if (words >= minStrong) {
        chunk = buf_.substr(0, j);
        buf_.erase(0, j);
        return true;
      }
    } else if (words >= minW) {
      chunk = buf_.substr(0, j);
      buf_.erase(0, j);
      return true;
    }
  }

  if (final && countWords(buf_) >= 1) {
    chunk = buf_;
    buf_.clear();
    return true;
  }
  return false;
}

void SentenceChunker::push(const std::string& text, std::vector<std::string>& out) {
  buf_ += text;
  if (opt_.holdAll) return;  // "smooth" style: wait for the complete reply
  std::string chunk;
  while (extract(false, chunk)) {
    chunk = trim(chunk);
    if (!chunk.empty()) { out.push_back(chunk); ++emitted_; }
  }
}

void SentenceChunker::flush(std::vector<std::string>& out) {
  if (opt_.holdAll) {
    std::string all = trim(buf_);
    buf_.clear();
    if (countWords(all) >= 1) {
      if (std::isalnum(static_cast<unsigned char>(all.back()))) all += '.';
      out.push_back(all);
      ++emitted_;
    }
    return;
  }
  std::string chunk;
  bool any = false;
  while (extract(true, chunk)) {
    chunk = trim(chunk);
    if (!chunk.empty()) { out.push_back(chunk); ++emitted_; any = true; }
  }
  buf_.clear();
  if (any && !out.empty()) {
    const unsigned char last = static_cast<unsigned char>(out.back().back());
    if (std::isalnum(last)) out.back() += '.';
  }
}

}  // namespace maya
