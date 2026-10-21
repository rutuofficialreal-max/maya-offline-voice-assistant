#include "stt_whisper.h"

#include <sys/stat.h>

#include <algorithm>
#include <vector>

#include "common.h"
#include "whisper.h"

namespace maya {

WhisperStt::~WhisperStt() {
  if (ctx_) {
    whisper_free(ctx_);
    ctx_ = nullptr;
  }
}

bool WhisperStt::init(const std::string& modelPath, int threads, bool useGpu, bool adaptiveCtx, std::string* err) {
  struct stat st;
  if (::stat(modelPath.c_str(), &st) != 0 || st.st_size <= 0) {
    *err = "Missing Whisper model: " + modelPath;
    return false;
  }
  threads_ = std::max(1, threads);
  adaptiveCtx_ = adaptiveCtx;
  whisper_context_params cp = whisper_context_default_params();
  cp.use_gpu = useGpu;      // CPU by default: the LLM owns the GPU
  cp.flash_attn = false;
  ctx_ = whisper_init_from_file_with_params(modelPath.c_str(), cp);
  if (!ctx_) {
    *err = "whisper.cpp could not load the model";
    return false;
  }
  return true;
}

std::string WhisperStt::transcribe(const float* pcm, size_t n, const std::atomic<bool>* abort, int* elapsedMs) {
  if (!ctx_ || n == 0) return {};
  const int64_t t0 = nowMs();

  // whisper refuses clips shorter than 1 s: pad with silence.
  std::vector<float> padded(pcm, pcm + n);
  const size_t minSamples = static_cast<size_t>(kMicRate * 1.2);
  if (padded.size() < minSamples) padded.resize(minSamples, 0.0f);

  whisper_full_params wp = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
  wp.n_threads = threads_;
  wp.language = "en";
  wp.translate = false;
  wp.no_context = true;
  wp.no_timestamps = true;
  wp.single_segment = true;
  wp.print_special = false;
  wp.print_progress = false;
  wp.print_realtime = false;
  wp.print_timestamps = false;
  wp.suppress_blank = true;
  wp.suppress_nst = true;
  wp.temperature = 0.0f;
  wp.temperature_inc = 0.0f;  // no fallback re-decodes: keeps latency predictable
  wp.greedy.best_of = 1;
  wp.no_speech_thold = 0.6f;
  // The encoder cost is proportional to the audio window. Voice commands are short, so
  // shrink the 30 s window to what we actually have (50 positions per second + margin).
  if (adaptiveCtx_) {
    const int ctxPositions = static_cast<int>(padded.size() / 320) + 128;
    wp.audio_ctx = std::min(1500, std::max(384, ctxPositions));
  } else {
    wp.audio_ctx = 0;  // full 30 s window: slowest, the model's native setting
  }
  wp.abort_callback = [](void* user) -> bool {
    auto* flag = static_cast<const std::atomic<bool>*>(user);
    return flag && flag->load();
  };
  wp.abort_callback_user_data = const_cast<std::atomic<bool>*>(abort);

  std::string text;
  if (whisper_full(ctx_, wp, padded.data(), static_cast<int>(padded.size())) == 0) {
    const int segs = whisper_full_n_segments(ctx_);
    for (int i = 0; i < segs; ++i) {
      const char* t = whisper_full_get_segment_text(ctx_, i);
      if (t) text += t;
    }
  } else {
    LOGW("whisper_full failed or was aborted");
  }
  if (elapsedMs) *elapsedMs = static_cast<int>(nowMs() - t0);
  return text;
}

}  // namespace maya
