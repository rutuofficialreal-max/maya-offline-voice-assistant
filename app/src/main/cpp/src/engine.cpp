#include "engine.h"

#include <sys/prctl.h>

#include <algorithm>
#include <chrono>
#include <cstdio>

#include "cpu_affinity.h"
#include "text_utils.h"

namespace maya {

using namespace std::chrono_literals;

// ------------------------------------------------------------------ config

EngineConfig EngineConfig::parse(const std::string& text) {
  EngineConfig c;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t nl = text.find('\n', pos);
    if (nl == std::string::npos) nl = text.size();
    std::string line = text.substr(pos, nl - pos);
    pos = nl + 1;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string k = trim(line.substr(0, eq));
    const std::string v = line.substr(eq + 1);

    auto str = [&](const char* key, std::string& out) { if (k == key) out = v; };
    auto i32 = [&](const char* key, int& out) {
      if (k == key) { try { out = std::stoi(v); } catch (...) {} }
    };
    auto f32 = [&](const char* key, float& out) {
      if (k == key) { try { out = std::stof(v); } catch (...) {} }
    };
    auto flag = [&](const char* key, bool& out) { if (k == key) out = (v == "1" || v == "true"); };

    str("kws.encoder", c.kwsEncoder); str("kws.decoder", c.kwsDecoder); str("kws.joiner", c.kwsJoiner);
    str("kws.tokens", c.kwsTokens);   str("vad.model", c.vadModel);     str("whisper.model", c.whisperModel);
    str("llm.model", c.llmModel);     str("tts.model", c.ttsModel);     str("tts.tokens", c.ttsTokens);
    str("tts.data_dir", c.ttsDataDir); str("system_prompt", c.systemPrompt);
    if (k == "kws.phrase" && !v.empty()) c.kwsPhrases.push_back(v);
    f32("kws.threshold", c.kwsThreshold); f32("kws.boost", c.kwsBoost);
    i32("vad.end_silence_ms", c.endSilenceMs); f32("vad.threshold", c.vadThreshold);
    i32("no_speech_timeout_ms", c.noSpeechTimeoutMs); i32("max_command_ms", c.maxCommandMs);
    i32("preroll_ms", c.prerollMs);
    i32("whisper.threads", c.whisperThreads); flag("whisper.gpu", c.whisperGpu); flag("whisper.adaptive_ctx", c.whisperAdaptiveCtx);
    i32("llm.gpu_layers", c.llmGpuLayers); i32("llm.ctx", c.llmCtx); i32("llm.threads", c.llmThreads);
    i32("llm.max_words", c.maxWords); f32("llm.min_gpu_tps", c.minGpuTps);
    f32("tts.speed", c.ttsSpeed); i32("tts.threads", c.ttsThreads);
    flag("pin_big_cores", c.pinBigCores);
  }
  return c;
}

// ------------------------------------------------------------------ lifecycle

MayaEngine::~MayaEngine() { stop(); }

void MayaEngine::setState(State s) {
  const State prev = state_.exchange(s);
  if (prev != s) emit(EVT_STATE, std::to_string(static_cast<int>(s)));
}

bool MayaEngine::init(const EngineConfig& cfg, std::string* err) {
  cfg_ = cfg;
  bigCores_ = cfg.pinBigCores ? detectBigCores(2) : std::vector<int>();
  if (bigCores_.empty()) {
    emit(EVT_INFO, "CPU: no big-core pinning");
  } else {
    std::string s = "CPU: fast cores";
    for (int c : bigCores_) s += " " + std::to_string(c);
    emit(EVT_INFO, s);
  }

  // Threads created while this guard is alive (ONNX Runtime pools) inherit the big-core mask.
  ScopedAffinity pinned(bigCores_);

  emit(EVT_INFO, "Loading voice…");
  TtsOptions to;
  to.model = cfg.ttsModel; to.tokens = cfg.ttsTokens; to.dataDir = cfg.ttsDataDir; to.threads = cfg.ttsThreads;
  if (!tts_.init(to, err)) return false;

  emit(EVT_INFO, "Loading wake word…");
  WakeWordOptions wo;
  wo.encoder = cfg.kwsEncoder; wo.decoder = cfg.kwsDecoder; wo.joiner = cfg.kwsJoiner; wo.tokens = cfg.kwsTokens;
  wo.threshold = cfg.kwsThreshold; wo.boost = cfg.kwsBoost; wo.threads = 1; wo.phrases = cfg.kwsPhrases;
  if (!kws_.init(wo, err)) return false;

  emit(EVT_INFO, "Loading voice detector…");
  VadOptions vo;
  vo.model = cfg.vadModel; vo.threshold = cfg.vadThreshold;
  // Measured on host: the VAD reports end-of-speech ~160 ms after `minSilenceSec`, so subtract it
  // to make `endSilenceMs` the delay the user actually experiences.
  vo.minSilenceSec = std::max(0.3f, (cfg.endSilenceMs - 150) / 1000.0f); vo.threads = 1;
  if (!vad_.init(vo, err)) return false;

  emit(EVT_INFO, "Loading speech recognition…");
  if (!stt_.init(cfg.whisperModel, cfg.whisperThreads, cfg.whisperGpu, cfg.whisperAdaptiveCtx, err)) return false;

  emit(EVT_INFO, "Loading language model (first start can take a while)…");
  LlmOptions lo;
  lo.modelPath = cfg.llmModel; lo.systemPrompt = cfg.systemPrompt; lo.gpuLayers = cfg.llmGpuLayers;
  lo.nCtx = cfg.llmCtx; lo.threads = cfg.llmThreads; lo.maxWords = cfg.maxWords; lo.minGpuTps = cfg.minGpuTps;
  if (!llm_.init(lo, err, &backendInfo_)) return false;
  emit(EVT_INFO, backendInfo_);
  return true;
}

bool MayaEngine::start() {
  if (running_.load()) return true;
  interrupt_.store(false);
  if (!capture_.start()) {
    emit(EVT_ERROR, "Could not open the microphone");
    return false;
  }
  running_.store(true);
  ttsThread_ = std::thread(&MayaEngine::ttsLoop, this);
  pipeline_ = std::thread(&MayaEngine::pipelineLoop, this);
  return true;
}

void MayaEngine::stop() {
  const bool wasRunning = running_.exchange(false);
  interrupt_.store(true);
  player_.flush();
  { std::lock_guard<std::mutex> lk(jobMu_); }
  jobCv_.notify_all();
  capture_.wakeWaiters();
  if (pipeline_.joinable()) pipeline_.join();
  if (ttsThread_.joinable()) ttsThread_.join();
  if (wasRunning) {
    capture_.stop();
    player_.close(true);
    setState(State::Stopped);
  }
}

void MayaEngine::interrupt() {
  interrupt_.store(true);
  player_.flush();
}

// ------------------------------------------------------------------ pipeline thread

void MayaEngine::pipelineLoop() {
  prctl(PR_SET_NAME, "maya-pipe");
  if (cfg_.pinBigCores && !bigCores_.empty()) {
    LOGI("Pipeline thread pinned: %s", pinCurrentThread(bigCores_) ? "yes" : "no");
  }
  std::vector<int16_t> preroll;
  std::vector<float> pcm;
  try {
    while (running_.load()) {
      interrupt_.store(false);
      if (!waitForWake(preroll)) break;
      interrupt_.store(false);
      emit(EVT_WAKE, "");  // -> "Maya is listening..." notification in Kotlin

      if (!captureCommand(preroll, pcm)) continue;

      setState(State::Transcribing);
      int sttMs = 0;
      const std::string raw = stt_.transcribe(pcm.data(), pcm.size(), &interrupt_, &sttMs);
      if (interrupt_.load() || !running_.load()) continue;
      const std::string text = trim(stripWakePhrase(cleanTranscript(raw)));
      if (!isMeaningfulTranscript(text)) {
        emit(EVT_INFO, "Didn't catch that");
        continue;
      }
      emit(EVT_TRANSCRIPT, text);
      respond(text, sttMs);
    }
  } catch (const std::exception& e) {
    LOGE("Pipeline crashed: %s", e.what());
    emit(EVT_ERROR, std::string("Internal error: ") + e.what());
    setState(State::Error);
    return;
  }
  setState(State::Stopped);
}

bool MayaEngine::waitForWake(std::vector<int16_t>& preroll) {
  setState(State::Passive);
  capture_.discard();
  capture_.setActive(true);
  kws_.reset();

  const size_t prSamples = std::max<size_t>(1, static_cast<size_t>(cfg_.prerollMs) * kMicRate / 1000);
  std::vector<int16_t> ring(prSamples, 0);
  size_t wpos = 0, filled = 0;
  std::vector<int16_t> buf(800);  // 50 ms hops
  std::vector<float> f(800);

  while (running_.load()) {
    if (!capture_.restartIfNeeded()) {
      std::this_thread::sleep_for(100ms);
      continue;
    }
    if (!capture_.waitForData(buf.size(), 100)) continue;
    const size_t n = capture_.read(buf.data(), buf.size());
    if (n == 0) continue;
    for (size_t i = 0; i < n; ++i) {
      ring[wpos] = buf[i];
      wpos = (wpos + 1) % ring.size();
      f[i] = static_cast<float>(buf[i]) * (1.0f / 32768.0f);
    }
    filled = std::min(ring.size(), filled + n);
    if (kws_.accept(f.data(), n)) {
      preroll.resize(filled);
      const size_t start = (wpos + ring.size() - filled) % ring.size();
      for (size_t i = 0; i < filled; ++i) preroll[i] = ring[(start + i) % ring.size()];
      return true;
    }
  }
  return false;
}

bool MayaEngine::captureCommand(const std::vector<int16_t>& preroll, std::vector<float>& pcm) {
  setState(State::Listening);
  vad_.reset();
  pcm.clear();
  const size_t maxSamples = static_cast<size_t>(cfg_.maxCommandMs) * kMicRate / 1000;
  pcm.reserve(preroll.size() + maxSamples + kMicRate);
  // Pre-roll: wake-word detection lags the speaker by a few hundred ms; if they kept talking
  // without a pause, the first words of the command are already in here.
  for (int16_t s : preroll) pcm.push_back(static_cast<float>(s) * (1.0f / 32768.0f));

  const int64_t tStart = nowMs();
  bool heard = false;
  std::vector<int16_t> buf(1600);
  std::vector<float> f(1600);

  while (running_.load() && !interrupt_.load()) {
    if (!capture_.restartIfNeeded()) std::this_thread::sleep_for(50ms);
    if (!capture_.waitForData(512, 100)) {
      if (!heard && nowMs() - tStart > cfg_.noSpeechTimeoutMs) return false;
      continue;
    }
    const size_t n = capture_.read(buf.data(), buf.size());
    for (size_t i = 0; i < n; ++i) {
      f[i] = static_cast<float>(buf[i]) * (1.0f / 32768.0f);
      pcm.push_back(f[i]);
    }
    vad_.accept(f.data(), n);  // only live audio drives the VAD, never the pre-roll

    if (vad_.speechActive()) {
      heard = true;
    } else if (heard) {
      break;  // auto-stop: the VAD saw endSilenceMs of silence after speech
    }
    if (!heard && nowMs() - tStart > cfg_.noSpeechTimeoutMs) return false;
    if (pcm.size() - preroll.size() >= maxSamples) break;
  }
  if (!heard || interrupt_.load() || !running_.load()) return false;

  // Freeze the buffer: trim most of the trailing silence the VAD already waited for.
  const size_t trailing = static_cast<size_t>(std::max(0, cfg_.endSilenceMs - 250)) * kMicRate / 1000;
  if (pcm.size() > trailing + preroll.size()) pcm.resize(pcm.size() - trailing);
  return true;
}

void MayaEngine::respond(const std::string& text, int sttMs) {
  setState(State::Thinking);
  auto job = std::make_shared<Utterance>();
  job->t0 = nowMs();
  {
    std::lock_guard<std::mutex> lk(jobMu_);
    job_ = job;
  }
  jobCv_.notify_one();

  capture_.setActive(false);  // half-duplex: don't let Maya hear herself
  if (!player_.begin(tts_.sampleRate())) emit(EVT_ERROR, "Audio output unavailable");

  SentenceChunker chunker;
  std::vector<std::string> chunks;
  auto handOver = [&] {
    if (chunks.empty()) return;
    {
      std::lock_guard<std::mutex> lk(job->mu);
      for (auto& c : chunks) job->chunks.push_back(std::move(c));
    }
    job->cv.notify_all();
    chunks.clear();
  };

  LlmStats st;
  const bool ok = llm_.generate(
      text,
      [&](const std::string& piece) {
        emit(EVT_REPLY_DELTA, piece);          // text for the UI
        chunker.push(piece, chunks);           // speakable chunks for TTS, as soon as they form
        handOver();
        return !interrupt_.load();
      },
      &st, &interrupt_);
  if (!ok && !interrupt_.load()) emit(EVT_ERROR, "The language model failed on that request");
  chunker.flush(chunks);
  handOver();
  {
    std::lock_guard<std::mutex> lk(job->mu);
    job->closed = true;
  }
  job->cv.notify_all();
  {
    std::unique_lock<std::mutex> lk(job->mu);
    job->cv.wait(lk, [&] { return job->done; });
  }
  player_.drainAndClose(&interrupt_);

  char m[256];
  std::snprintf(m, sizeof m, "stt %d ms | prefill %d ms (%d/%d cached) | 1st token %d ms | %.1f tok/s | 1st audio %d ms%s%s",
                sttMs, st.prefillMs, st.reusedTokens, st.promptTokens, st.ttftMs, st.tps, job->firstAudioMs,
                st.capped ? " | word cap" : "", st.aborted ? " | interrupted" : "");
  emit(EVT_METRIC, m);
  emit(EVT_REPLY_DONE, "");

  std::this_thread::sleep_for(250ms);  // let the room echo die before listening again
  capture_.discard();
  capture_.setActive(true);
}

// ------------------------------------------------------------------ TTS thread

void MayaEngine::ttsLoop() {
  prctl(PR_SET_NAME, "maya-tts");
  if (cfg_.pinBigCores && !bigCores_.empty()) pinCurrentThread(bigCores_);
  for (;;) {
    std::shared_ptr<Utterance> u;
    {
      std::unique_lock<std::mutex> lk(jobMu_);
      jobCv_.wait(lk, [&] { return !running_.load() || job_ != nullptr; });
      if (!job_) return;  // shutting down and nothing pending
      u = std::move(job_);
      job_.reset();
    }
    for (;;) {
      std::string chunk;
      {
        std::unique_lock<std::mutex> lk(u->mu);
        u->cv.wait_for(lk, 50ms, [&] { return u->closed || !u->chunks.empty() || interrupt_.load(); });
        if (interrupt_.load()) break;
        if (u->chunks.empty()) {
          if (u->closed) break;
          continue;
        }
        chunk = std::move(u->chunks.front());
        u->chunks.pop_front();
      }
      const std::string speech = sanitizeForSpeech(chunk);
      if (speech.empty()) continue;
      tts_.synthesize(speech, cfg_.ttsSpeed, [&](const float* samples, int n) {
        if (interrupt_.load()) return false;
        if (u->firstAudioMs < 0) {
          u->firstAudioMs = static_cast<int>(nowMs() - u->t0);
          setState(State::Speaking);
        }
        return player_.write(samples, static_cast<size_t>(n), &interrupt_);
      });
      if (interrupt_.load()) break;
    }
    {
      std::lock_guard<std::mutex> lk(u->mu);
      u->done = true;
    }
    u->cv.notify_all();
  }
}

}  // namespace maya
