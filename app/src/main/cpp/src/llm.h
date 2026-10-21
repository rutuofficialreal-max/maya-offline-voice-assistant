// Local LLM: llama.cpp (ggml) with the Vulkan backend. Falls back to the CPU automatically
// if no GPU is exposed, loading fails, or the GPU turns out to be slower than `minGpuTps`.
#pragma once
#include <atomic>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "llama.h"

namespace maya {

struct LlmOptions {
  std::string modelPath;
  std::string systemPrompt;
  int gpuLayers = 99;       // 0 = CPU only
  int nCtx = 2048;
  int nBatch = 512;
  int threads = 2;          // CPU threads (sampling / dispatch on GPU, matmuls on CPU)
  int maxWords = 50;        // hard cap on the spoken answer
  int maxNewTokens = 128;
  float minGpuTps = 4.0f;   // below this decode speed the GPU is abandoned for the CPU
  bool benchmark = true;
};

struct LlmStats {
  int promptTokens = 0;
  int reusedTokens = 0;   // prompt tokens served from the KV cache
  int genTokens = 0;
  int prefillMs = 0;
  int ttftMs = 0;         // request start -> first token
  int totalMs = 0;
  float tps = 0.0f;       // decode speed
  bool capped = false;    // stopped by the word cap
  bool aborted = false;
};

struct GenOptions {
  int maxNewTokens = -1;  // -1 = use LlmOptions
  int maxWords = -1;
  bool remember = true;   // keep this turn in the conversation history
};

class Llm {
 public:
  Llm() = default;
  ~Llm();
  Llm(const Llm&) = delete;
  Llm& operator=(const Llm&) = delete;

  bool init(const LlmOptions& opt, std::string* err, std::string* backendInfo);

  // Streams the reply through onPiece (UTF-8, complete code points only). onPiece may be
  // empty. Returning false from it stops generation. `abort` is polled once per token.
  bool generate(const std::string& user, const std::function<bool(const std::string&)>& onPiece, LlmStats* stats,
                const std::atomic<bool>* abort, const GenOptions& go = GenOptions());

  void resetConversation();
  bool usingGpu() const { return gpuActive_; }

 private:
  struct Turn {
    std::string role;
    std::string text;
  };

  bool load(int gpuLayers, std::string* err);
  void unload();
  float benchmark();
  std::string renderPrompt(const std::string& user) const;
  std::vector<llama_token> tokenize(const std::string& text) const;
  bool decodeRange(const llama_token* t, int n);
  void clearKv();

  LlmOptions opt_;
  llama_model* model_ = nullptr;
  llama_context* ctx_ = nullptr;
  llama_sampler* smpl_ = nullptr;
  const llama_vocab* vocab_ = nullptr;
  std::vector<llama_token> cache_;  // tokens currently resident in the KV cache
  std::deque<Turn> history_;
  bool gpuActive_ = false;
  bool backendHeld_ = false;
};

}  // namespace maya
