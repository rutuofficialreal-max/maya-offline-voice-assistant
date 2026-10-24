#include "wake_word.h"

#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unordered_set>

#include "common.h"
#include "wake_gate.h"

namespace maya {

namespace {
bool fileExists(const std::string& p) {
  struct stat st;
  return !p.empty() && ::stat(p.c_str(), &st) == 0 && st.st_size > 0;
}

// Factory phrases = how the tiny model tends to *hear* "Hey Maya" before calibration
// (found with the model's own ASR decoder on synthetic speech):
//   MAY-uh -> "HEY MAYA",  MY-uh (British) -> "HEY MYR" / "HEY MYER".
// "▁" is U+2581, SentencePiece's word-start marker. Calibration replaces these with the
// user's own pronunciation, which is far more reliable for accents such as Indian English.
const char* const kFactoryPhrases[] = {
    "▁HE Y ▁MA Y A",  // MAY-uh
    "▁HE Y ▁MY A",    // MY-ah: what Indian-English "Maya" was heard as on the real phone
    "▁HE Y ▁MY R",    // MY-uh (British)
    "▁HE Y ▁MY ER",
};

// Every token must exist in tokens.txt: sherpa-onnx calls exit() on an unknown token.
std::unordered_set<std::string> loadTokenSet(const std::string& path) {
  std::unordered_set<std::string> set;
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    const size_t sp = line.rfind(' ');
    set.insert(sp == std::string::npos ? line : line.substr(0, sp));
  }
  return set;
}

bool phraseIsValid(const std::string& phrase, const std::unordered_set<std::string>& vocab) {
  if (phrase.empty() || phrase.size() > 160 || phrase.find('\n') != std::string::npos) return false;
  std::istringstream ss(phrase);
  std::string tok;
  int n = 0;
  while (ss >> tok) {
    if (!vocab.count(tok)) return false;
    ++n;
  }
  return n >= 2 && n <= 16;
}

struct RecognizerDel {
  void operator()(const SherpaOnnxOnlineRecognizer* p) const { if (p) SherpaOnnxDestroyOnlineRecognizer(p); }
};
}  // namespace

bool WakeWord::init(const WakeWordOptions& o, std::string* err) { return reconfigure(o, err); }

bool WakeWord::reconfigure(const WakeWordOptions& o, std::string* err) {
  for (const std::string* p : {&o.encoder, &o.decoder, &o.joiner, &o.tokens}) {
    if (!fileExists(*p)) {
      *err = "Missing wake-word model file: " + *p;
      return false;
    }
  }
  // keywords_buf format: "<tokens> :<boost> #<threshold> @<name>" per line.
  const std::unordered_set<std::string> vocab = loadTokenSet(o.tokens);
  std::vector<std::string> phrases;
  for (const std::string& p : o.phrases) {
    if (phraseIsValid(p, vocab)) phrases.push_back(p);
    else LOGW("Ignoring invalid wake phrase: %s", p.c_str());
  }
  const size_t calibratedCount = phrases.size();
  // Factory phrases are always active: calibration only ever *adds* pronunciations.
  for (const char* f : kFactoryPhrases) {
    if (phraseIsValid(f, vocab) && std::find(phrases.begin(), phrases.end(), f) == phrases.end()) phrases.push_back(f);
  }
  if (phrases.empty()) {
    *err = "No usable wake phrase (tokens.txt does not match the model?)";
    return false;
  }
  std::string keywords;
  for (const std::string& phrase : phrases) {
    char tail[64];
    std::snprintf(tail, sizeof tail, " :%.2f #%.3f @HEY_MAYA\n", o.boost, o.threshold);
    keywords += phrase;
    keywords += tail;
  }
  LOGI("Wake word: %zu phrase(s) (%zu calibrated), threshold %.3f", phrases.size(), calibratedCount, o.threshold);

  SherpaOnnxKeywordSpotterConfig c;
  std::memset(&c, 0, sizeof c);
  c.feat_config.sample_rate = kMicRate;
  c.feat_config.feature_dim = 80;
  c.model_config.transducer.encoder = o.encoder.c_str();
  c.model_config.transducer.decoder = o.decoder.c_str();
  c.model_config.transducer.joiner = o.joiner.c_str();
  c.model_config.tokens = o.tokens.c_str();
  c.model_config.num_threads = o.threads;
  c.model_config.provider = "cpu";
  c.model_config.debug = 0;
  c.max_active_paths = 4;
  c.num_trailing_blanks = 1;  // fire as early as possible after the last token
  c.keywords_score = o.boost;
  c.keywords_threshold = o.threshold;
  c.keywords_buf = keywords.c_str();
  c.keywords_buf_size = static_cast<int32_t>(keywords.size());

  std::unique_ptr<const SherpaOnnxKeywordSpotter, SpotterDel> fresh(SherpaOnnxCreateKeywordSpotter(&c));
  if (!fresh) {
    *err = "Could not create the wake-word spotter (check the model files)";
    return false;  // an existing spotter keeps working untouched
  }
  stream_.reset();                 // the old stream must die before the old spotter
  spotter_ = std::move(fresh);
  reset();
  return stream_ != nullptr;
}

void WakeWord::reset() {
  if (!spotter_) return;
  stream_.reset(SherpaOnnxCreateKeywordStream(spotter_.get()));
}

bool WakeWord::accept(const float* pcm, size_t n) {
  if (!spotter_ || !stream_ || n == 0) return false;
  SherpaOnnxOnlineStreamAcceptWaveform(stream_.get(), kMicRate, pcm, static_cast<int32_t>(n));
  while (SherpaOnnxIsKeywordStreamReady(spotter_.get(), stream_.get())) {
    SherpaOnnxDecodeKeywordStream(spotter_.get(), stream_.get());
    const SherpaOnnxKeywordResult* r = SherpaOnnxGetKeywordResult(spotter_.get(), stream_.get());
    bool hit = false;
    if (r) {
      hit = r->keyword && r->keyword[0] != '\0';
      if (hit) LOGI("Wake word: %s", r->keyword);
      SherpaOnnxDestroyKeywordResult(r);
    }
    if (hit) return true;
  }
  return false;
}

bool decodeWakeCalibration(const WakeWordOptions& o, const int16_t* pcm, size_t n, std::string* text,
                           std::string* tokens, std::string* err) {
  for (const std::string* p : {&o.encoder, &o.decoder, &o.joiner, &o.tokens}) {
    if (!fileExists(*p)) {
      *err = "Missing wake-word model file: " + *p;
      return false;
    }
  }
  if (n < static_cast<size_t>(kMicRate / 4)) {
    *err = "Recording too short";
    return false;
  }
  SherpaOnnxOnlineRecognizerConfig c;
  std::memset(&c, 0, sizeof c);
  c.feat_config.sample_rate = kMicRate;
  c.feat_config.feature_dim = 80;
  c.model_config.transducer.encoder = o.encoder.c_str();
  c.model_config.transducer.decoder = o.decoder.c_str();
  c.model_config.transducer.joiner = o.joiner.c_str();
  c.model_config.tokens = o.tokens.c_str();
  c.model_config.num_threads = 1;
  c.model_config.provider = "cpu";
  c.decoding_method = "greedy_search";
  c.max_active_paths = 4;
  std::unique_ptr<const SherpaOnnxOnlineRecognizer, RecognizerDel> rec(SherpaOnnxCreateOnlineRecognizer(&c));
  if (!rec) {
    *err = "Could not create the recognizer";
    return false;
  }
  std::vector<float> f(n + kMicRate / 2, 0.0f);  // + 0.5 s of silence flushes the last tokens
  for (size_t i = 0; i < n; ++i) f[i] = static_cast<float>(pcm[i]) * (1.0f / 32768.0f);

  const SherpaOnnxOnlineStream* stream = SherpaOnnxCreateOnlineStream(rec.get());
  SherpaOnnxOnlineStreamAcceptWaveform(stream, kMicRate, f.data(), static_cast<int32_t>(f.size()));
  SherpaOnnxOnlineStreamInputFinished(stream);
  while (SherpaOnnxIsOnlineStreamReady(rec.get(), stream)) SherpaOnnxDecodeOnlineStream(rec.get(), stream);
  const SherpaOnnxOnlineRecognizerResult* r = SherpaOnnxGetOnlineStreamResult(rec.get(), stream);

  text->clear();
  tokens->clear();
  if (r) {
    if (r->text) *text = r->text;
    // tokens_arr gives word-start pieces with a leading space instead of "▁".
    for (int32_t i = 0; i < r->count; ++i) {
      std::string t = r->tokens_arr[i] ? r->tokens_arr[i] : "";
      if (t.empty()) continue;
      if (t[0] == ' ') t = std::string("\xE2\x96\x81") + t.substr(1);
      if (!tokens->empty()) *tokens += ' ';
      *tokens += t;
    }
    SherpaOnnxDestroyOnlineRecognizerResult(r);
  }
  SherpaOnnxDestroyOnlineStream(stream);
  if (tokens->empty()) {
    *err = "Nothing was heard";
    return false;
  }
  // Quality gate (see wake_gate.cpp): a mumbled attempt must not become a wake phrase.
  const WakeSampleVerdict verdict = evaluateWakeSample(*tokens);
  if (!verdict.ok) {
    *err = verdict.reason;
    return false;
  }
  return true;
}

}  // namespace maya
