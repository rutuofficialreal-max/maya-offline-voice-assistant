#include "tts_sherpa.h"

#include <sys/stat.h>

#include <cstring>

#include "common.h"

namespace maya {

namespace {
bool exists(const std::string& p) {
  struct stat st;
  return ::stat(p.c_str(), &st) == 0;
}

struct CbCtx {
  const std::function<bool(const float*, int)>* sink;
  bool aborted = false;
  bool delivered = false;
};

int32_t onSamples(const float* samples, int32_t n, float /*progress*/, void* arg) {
  auto* ctx = static_cast<CbCtx*>(arg);
  if (n > 0) {
    ctx->delivered = true;
    if (!(*ctx->sink)(samples, n)) {
      ctx->aborted = true;
      return 0;  // stop generating
    }
  }
  return 1;  // continue
}
}  // namespace

bool Tts::init(const TtsOptions& o, std::string* err) {
  if (!exists(o.model) || !exists(o.tokens) || !exists(o.dataDir)) {
    *err = "Missing TTS files (model / tokens / espeak-ng-data)";
    return false;
  }
  SherpaOnnxOfflineTtsConfig c;
  std::memset(&c, 0, sizeof c);  // zero = library defaults for every field we don't set
  c.model.vits.model = o.model.c_str();
  c.model.vits.tokens = o.tokens.c_str();
  c.model.vits.data_dir = o.dataDir.c_str();
  c.model.num_threads = o.threads;
  c.model.provider = "cpu";
  c.model.debug = 0;
  c.max_num_sentences = 1;  // hand audio over sentence by sentence
  tts_.reset(SherpaOnnxCreateOfflineTts(&c));
  if (!tts_) {
    *err = "Could not create the TTS engine";
    return false;
  }
  sampleRate_ = SherpaOnnxOfflineTtsSampleRate(tts_.get());
  return true;
}

bool Tts::synthesize(const std::string& text, float speed, float silenceScale,
                     const std::function<bool(const float*, int)>& sink) {
  if (!tts_ || text.empty()) return false;
  CbCtx ctx;
  ctx.sink = &sink;
  SherpaOnnxGenerationConfig gc;
  std::memset(&gc, 0, sizeof gc);
  gc.sid = 0;
  gc.speed = speed;
  gc.silence_scale = silenceScale;
  std::unique_ptr<const SherpaOnnxGeneratedAudio, AudioDel> audio(
      SherpaOnnxOfflineTtsGenerateWithConfig(tts_.get(), text.c_str(), &gc, &onSamples, &ctx));
  // Safety net: if the library returned audio without calling back, play it now.
  if (!ctx.aborted && !ctx.delivered && audio && audio->n > 0) {
    if (!sink(audio->samples, audio->n)) ctx.aborted = true;
  }
  return !ctx.aborted;
}

}  // namespace maya
