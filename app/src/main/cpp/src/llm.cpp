#include "llm.h"

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#include "common.h"
#include "ggml-backend.h"
#include "text_utils.h"

namespace maya {

namespace {

std::atomic<int> g_backendRefs{0};
constexpr size_t kMaxHistoryMessages = 8;  // last 4 exchanges

void llamaLog(ggml_log_level level, const char* text, void*) {
  if (!text) return;
  int prio = ANDROID_LOG_INFO;
  if (level == GGML_LOG_LEVEL_ERROR) prio = ANDROID_LOG_ERROR;
  else if (level == GGML_LOG_LEVEL_WARN) prio = ANDROID_LOG_WARN;
  else if (level == GGML_LOG_LEVEL_DEBUG) return;  // too chatty
  std::string s(text);
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
  if (!s.empty()) __android_log_print(prio, "Maya.ggml", "%s", s.c_str());
}

std::string chatmlFallback(const std::string& system, const std::deque<std::string>& roles,
                           const std::deque<std::string>& texts, const std::string& user) {
  std::string p = "<|im_start|>system\n" + system + "<|im_end|>\n";
  for (size_t i = 0; i < roles.size(); ++i) p += "<|im_start|>" + roles[i] + "\n" + texts[i] + "<|im_end|>\n";
  p += "<|im_start|>user\n" + user + "<|im_end|>\n<|im_start|>assistant\n";
  return p;
}

}  // namespace

Llm::~Llm() {
  unload();
  if (backendHeld_ && g_backendRefs.fetch_sub(1) == 1) llama_backend_free();
}

bool Llm::init(const LlmOptions& opt, std::string* err, std::string* backendInfo) {
  opt_ = opt;
  struct stat st;
  if (::stat(opt.modelPath.c_str(), &st) != 0 || st.st_size <= 0) {
    *err = "Missing LLM model: " + opt.modelPath;
    return false;
  }
  if (g_backendRefs.fetch_add(1) == 0) {
    llama_log_set(llamaLog, nullptr);
    llama_backend_init();
  }
  backendHeld_ = true;

  // What did ggml find? (Vulkan registers an iGPU/GPU device when a usable driver exists.)
  bool haveGpu = false;
  std::string devices;
  for (size_t i = 0; i < ggml_backend_dev_count(); ++i) {
    ggml_backend_dev_t d = ggml_backend_dev_get(i);
    const enum ggml_backend_dev_type t = ggml_backend_dev_type(d);  // 'enum': the function shares the name
    const bool gpu = (t == GGML_BACKEND_DEVICE_TYPE_GPU || t == GGML_BACKEND_DEVICE_TYPE_IGPU);
    haveGpu = haveGpu || gpu;
    const char* name = ggml_backend_dev_name(d);
    const char* desc = ggml_backend_dev_description(d);
    if (!devices.empty()) devices += ", ";
    devices += std::string(name ? name : "?") + (gpu ? " [GPU: " : " [") + (desc ? desc : "") + "]";
  }
  LOGI("ggml devices: %s", devices.c_str());

  int layers = (haveGpu && opt.gpuLayers != 0) ? opt.gpuLayers : 0;
  std::string note;
  if (!load(layers, err)) {
    if (layers == 0) return false;
    LOGW("GPU load failed (%s); retrying on CPU", err->c_str());
    unload();
    layers = 0;
    note = " (GPU load failed, using CPU)";
    if (!load(0, err)) return false;
  }
  gpuActive_ = layers > 0;

  if (gpuActive_ && opt.benchmark) {
    const float tps = benchmark();
    LOGI("GPU decode benchmark: %.1f tok/s (threshold %.1f)", tps, opt.minGpuTps);
    if (tps > 0.0f && tps < opt.minGpuTps) {
      unload();
      if (!load(0, err)) return false;
      gpuActive_ = false;
      char buf[96];
      std::snprintf(buf, sizeof buf, " (GPU too slow: %.1f tok/s, using CPU)", tps);
      note = buf;
    } else if (tps > 0.0f) {
      char buf[48];
      std::snprintf(buf, sizeof buf, " (%.1f tok/s)", tps);
      note = buf;
    }
  }
  if (!gpuActive_ && note.empty()) note = haveGpu ? " (CPU by configuration)" : " (no GPU backend found, using CPU)";

  // Prime the KV cache with the system prompt so the first real question starts fast.
  GenOptions prime;
  prime.maxNewTokens = 1;
  prime.remember = false;
  generate("Hi", nullptr, nullptr, nullptr, prime);

  if (backendInfo) *backendInfo = std::string("LLM backend: ") + (gpuActive_ ? "Vulkan GPU" : "CPU") + note + " | " + devices;
  return true;
}

bool Llm::load(int gpuLayers, std::string* err) {
  llama_model_params mp = llama_model_default_params();
  mp.n_gpu_layers = gpuLayers;
  model_ = llama_model_load_from_file(opt_.modelPath.c_str(), mp);
  if (!model_) {
    *err = "llama.cpp could not load the model";
    return false;
  }
  vocab_ = llama_model_get_vocab(model_);

  llama_context_params cp = llama_context_default_params();
  cp.n_ctx = static_cast<uint32_t>(opt_.nCtx);
  cp.n_batch = static_cast<uint32_t>(opt_.nBatch);
  cp.n_ubatch = static_cast<uint32_t>(opt_.nBatch);
  cp.n_threads = opt_.threads;
  cp.n_threads_batch = opt_.threads;
  cp.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_DISABLED;  // most conservative path on mobile GPU drivers
  ctx_ = llama_init_from_model(model_, cp);
  if (!ctx_) {
    *err = "llama.cpp could not create a context (out of memory?)";
    unload();
    return false;
  }

  llama_sampler_chain_params sp = llama_sampler_chain_default_params();
  smpl_ = llama_sampler_chain_init(sp);
  llama_sampler_chain_add(smpl_, llama_sampler_init_penalties(llama_vocab_n_tokens(vocab_), 64, 1.1f, 0.0f, 0.0f));
  llama_sampler_chain_add(smpl_, llama_sampler_init_top_k(40));
  llama_sampler_chain_add(smpl_, llama_sampler_init_top_p(0.9f, 1));
  llama_sampler_chain_add(smpl_, llama_sampler_init_temp(0.7f));
  llama_sampler_chain_add(smpl_, llama_sampler_init_dist(static_cast<uint32_t>(nowMs())));
  cache_.clear();
  return true;
}

void Llm::unload() {
  if (smpl_) { llama_sampler_free(smpl_); smpl_ = nullptr; }
  if (ctx_) { llama_free(ctx_); ctx_ = nullptr; }
  if (model_) { llama_model_free(model_); model_ = nullptr; }
  vocab_ = nullptr;
  cache_.clear();
}

void Llm::clearKv() {
  if (ctx_) llama_memory_clear(llama_get_memory(ctx_), true);
  cache_.clear();
}

void Llm::resetConversation() {
  history_.clear();
  // The KV cache stays valid: the next prompt just shares less of its prefix.
}

float Llm::benchmark() {
  GenOptions go;
  go.maxNewTokens = 24;
  go.maxWords = 200;
  go.remember = false;
  LlmStats warm, timed;
  // Run 1 compiles the Vulkan pipelines; run 2 is the measurement.
  if (!generate("Count slowly from one to twenty.", nullptr, &warm, nullptr, go)) return -1.0f;
  if (!generate("Count slowly from one to thirty.", nullptr, &timed, nullptr, go)) return -1.0f;
  return timed.tps;
}

std::string Llm::renderPrompt(const std::string& user) const {
  std::vector<llama_chat_message> msgs;
  msgs.reserve(history_.size() + 2);
  msgs.push_back({"system", opt_.systemPrompt.c_str()});
  for (const Turn& t : history_) msgs.push_back({t.role.c_str(), t.text.c_str()});
  msgs.push_back({"user", user.c_str()});

  const char* tmpl = llama_model_chat_template(model_, nullptr);
  std::string buf(2048 + user.size() * 2 + opt_.systemPrompt.size() * 2, '\0');
  int32_t n = llama_chat_apply_template(tmpl ? tmpl : "chatml", msgs.data(), msgs.size(), true, &buf[0],
                                        static_cast<int32_t>(buf.size()));
  if (n > static_cast<int32_t>(buf.size())) {
    buf.assign(static_cast<size_t>(n), '\0');
    n = llama_chat_apply_template(tmpl ? tmpl : "chatml", msgs.data(), msgs.size(), true, &buf[0], n);
  }
  if (n < 0) {  // template not recognised: use ChatML
    std::deque<std::string> roles, texts;
    for (const Turn& t : history_) { roles.push_back(t.role); texts.push_back(t.text); }
    return chatmlFallback(opt_.systemPrompt, roles, texts, user);
  }
  buf.resize(static_cast<size_t>(n));
  return buf;
}

std::vector<llama_token> Llm::tokenize(const std::string& text) const {
  const int32_t need = -llama_tokenize(vocab_, text.c_str(), static_cast<int32_t>(text.size()), nullptr, 0, true, true);
  if (need <= 0) return {};
  std::vector<llama_token> toks(static_cast<size_t>(need));
  const int32_t got = llama_tokenize(vocab_, text.c_str(), static_cast<int32_t>(text.size()), toks.data(),
                                     static_cast<int32_t>(toks.size()), true, true);
  if (got < 0) return {};
  toks.resize(static_cast<size_t>(got));
  return toks;
}

bool Llm::decodeRange(const llama_token* t, int n) {
  for (int i = 0; i < n; i += opt_.nBatch) {
    const int cnt = std::min(opt_.nBatch, n - i);
    llama_batch b = llama_batch_get_one(const_cast<llama_token*>(t + i), cnt);
    const int rc = llama_decode(ctx_, b);
    if (rc != 0) {
      LOGE("llama_decode failed (rc=%d)", rc);
      return false;
    }
  }
  return true;
}

bool Llm::generate(const std::string& user, const std::function<bool(const std::string&)>& onPiece, LlmStats* stats,
                   const std::atomic<bool>* abort, const GenOptions& go) {
  if (!ctx_ || !model_) return false;
  const int64_t t0 = nowMs();
  LlmStats st;
  const int maxNew = go.maxNewTokens > 0 ? go.maxNewTokens : opt_.maxNewTokens;
  const int maxWords = go.maxWords > 0 ? go.maxWords : opt_.maxWords;
  const int softWords = std::max(1, std::min(maxWords - 10, maxWords * 3 / 4));

  // 1) Build the prompt; drop the oldest exchange while it does not fit the context.
  std::vector<llama_token> toks;
  for (;;) {
    toks = tokenize(renderPrompt(user));
    if (toks.empty()) return false;
    if (static_cast<int>(toks.size()) + maxNew + 4 <= opt_.nCtx) break;
    if (history_.size() < 2) {
      LOGE("Prompt does not fit in the context (%zu tokens)", toks.size());
      return false;
    }
    history_.pop_front();
    history_.pop_front();
  }

  // 2) Reuse whatever prefix is already in the KV cache (system prompt + earlier turns).
  size_t common = 0;
  const size_t lim = std::min(cache_.size(), toks.size());
  while (common < lim && cache_[common] == toks[common]) ++common;
  if (common >= toks.size()) common = toks.size() - 1;  // always decode >= 1 token to get logits
  if (common < cache_.size()) {
    if (llama_memory_seq_rm(llama_get_memory(ctx_), 0, static_cast<llama_pos>(common), -1)) {
      cache_.resize(common);
    } else {
      clearKv();
      common = 0;
    }
  }
  st.promptTokens = static_cast<int>(toks.size());
  st.reusedTokens = static_cast<int>(common);

  const int64_t tPrefill = nowMs();
  if (!decodeRange(toks.data() + common, static_cast<int>(toks.size() - common))) {
    clearKv();
    return false;
  }
  cache_.insert(cache_.end(), toks.begin() + static_cast<std::ptrdiff_t>(common), toks.end());
  st.prefillMs = static_cast<int>(nowMs() - tPrefill);

  // 3) Generate.
  llama_sampler_reset(smpl_);
  Utf8Accumulator utf8;
  std::string reply;
  int words = 0;
  bool inWord = false;
  int64_t tFirst = 0;
  bool decodeFailed = false;

  for (int i = 0; i < maxNew; ++i) {
    if (abort && abort->load()) { st.aborted = true; break; }
    llama_token tok = llama_sampler_sample(smpl_, ctx_, -1);
    if (llama_vocab_is_eog(vocab_, tok)) break;
    if (st.genTokens == 0) { tFirst = nowMs(); st.ttftMs = static_cast<int>(tFirst - t0); }
    ++st.genTokens;

    char piece[256];
    const int32_t pn = llama_token_to_piece(vocab_, tok, piece, sizeof piece, 0, false);
    std::string text = pn > 0 ? utf8.push(piece, static_cast<size_t>(pn)) : std::string();

    // Word accounting with a hard cap, and a soft stop at a sentence end once close to it.
    std::string emit;
    bool capped = false;
    for (unsigned char c : text) {
      if (std::isspace(c)) { inWord = false; emit += static_cast<char>(c); continue; }
      if (!inWord) {
        if (words >= maxWords) { capped = true; break; }
        inWord = true;
        ++words;
      }
      emit += static_cast<char>(c);
    }
    if (!emit.empty()) {
      reply += emit;
      if (onPiece && !onPiece(emit)) { st.aborted = true; break; }
    }
    if (capped) { st.capped = true; break; }
    if (words >= softWords && !emit.empty()) {
      const std::string tail = trim(emit);
      if (!tail.empty() && (tail.back() == '.' || tail.back() == '!' || tail.back() == '?')) break;
    }

    llama_token one = tok;
    if (!decodeRange(&one, 1)) { decodeFailed = true; break; }
    cache_.push_back(tok);
  }

  if (decodeFailed) clearKv();
  const int64_t tEnd = nowMs();
  st.totalMs = static_cast<int>(tEnd - t0);
  if (st.genTokens > 2 && tEnd > tFirst) st.tps = (st.genTokens - 1) * 1000.0f / static_cast<float>(tEnd - tFirst);

  if (go.remember && !st.aborted && !decodeFailed) {
    const std::string r = trim(reply);
    if (!r.empty()) {
      history_.push_back({"user", user});
      history_.push_back({"assistant", r});
      while (history_.size() > kMaxHistoryMessages) history_.pop_front();
    }
  }
  if (stats) *stats = st;
  return !decodeFailed;
}

}  // namespace maya
