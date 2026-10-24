#include "wake_net.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace maya {
namespace {

constexpr int kSampleRate = 16000;
constexpr int kWinLen = 400;      // 25 ms analysis window (zero-padded into 512)
constexpr int kNfft = 512;
constexpr int kHop = 160;         // 10 ms
constexpr int kMels = 40;
constexpr int kFrames = 96;       // 0.96 s of context
constexpr int kWindow = (kFrames - 1) * kHop + kNfft;   // 15712 samples
constexpr int kEvalHop = 1280;    // score every 80 ms
constexpr int kBins = kNfft / 2 + 1;
constexpr int kMaxDim = 4;

bool readBlob(const std::string& path, std::vector<float>* flat, std::vector<int>* dims,
              std::vector<size_t>* offsets, std::string* err) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    *err = "wake model not found: " + path;
    return false;
  }
  char magic[8] = {0};
  f.read(magic, 8);
  if (std::memcmp(magic, "MAYAWK1\0", 8) != 0) {
    *err = "wake model has a bad header";
    return false;
  }
  uint32_t n = 0;
  f.read(reinterpret_cast<char*>(&n), 4);
  if (!f || n == 0 || n > 64) {
    *err = "wake model has a bad tensor count";
    return false;
  }
  for (uint32_t t = 0; t < n; ++t) {
    uint32_t ndim = 0;
    f.read(reinterpret_cast<char*>(&ndim), 4);
    if (!f || ndim == 0 || ndim > kMaxDim) {
      *err = "wake model has a bad tensor rank";
      return false;
    }
    size_t count = 1;
    for (uint32_t d = 0; d < ndim; ++d) {
      uint32_t dim = 0;
      f.read(reinterpret_cast<char*>(&dim), 4);
      count *= dim;
      dims->push_back(static_cast<int>(dim));
    }
    offsets->push_back(flat->size());
    flat->resize(flat->size() + count);
    f.read(reinterpret_cast<char*>(flat->data() + offsets->back()), static_cast<std::streamsize>(count * 4));
    if (!f) {
      *err = "wake model is truncated";
      return false;
    }
  }
  return true;
}

}  // namespace

bool WakeNet::init(const WakeNetOptions& opt, std::string* err) {
  opt_ = opt;
  ok_ = load(opt.weightsPath, err);
  return ok_;
}

bool WakeNet::load(const std::string& path, std::string* err) {
  std::vector<float> flat;
  std::vector<int> dims;
  std::vector<size_t> off;
  if (!readBlob(path, &flat, &dims, &off, err)) return false;

  // mel filterbank (40,257) and hann window (512) come first, then 8 convs (w,b), then fc1, fc2.
  size_t t = 0, d = 0;
  auto take = [&](std::vector<float>* dst, size_t want, const char* what) -> bool {
    if (t >= off.size()) {
      *err = std::string("wake model: missing ") + what;
      return false;
    }
    size_t count = (t + 1 < off.size() ? off[t + 1] : flat.size()) - off[t];
    if (count != want) {
      char m[96];
      std::snprintf(m, sizeof m, "wake model: %s has %zu values, expected %zu", what, count, want);
      *err = m;
      return false;
    }
    dst->assign(flat.begin() + static_cast<long>(off[t]), flat.begin() + static_cast<long>(off[t] + count));
    ++t;
    return true;
  };

  if (!take(&melFb_, static_cast<size_t>(kMels) * kBins, "mel filterbank")) return false;
  if (!take(&window_, kNfft, "hann window")) return false;
  const int channels[8] = {16, 16, 32, 32, 64, 64, 64, 64};
  const int inCh[8] = {1, 16, 16, 32, 32, 64, 64, 64};
  for (int i = 0; i < 8; ++i) {
    convs_[i].co = channels[i];
    convs_[i].ci = inCh[i];
    char m[48];
    std::snprintf(m, sizeof m, "conv%d weights", i + 1);
    if (!take(&convs_[i].w, static_cast<size_t>(channels[i]) * inCh[i] * 9, m)) return false;
    std::snprintf(m, sizeof m, "conv%d bias", i + 1);
    if (!take(&convs_[i].b, static_cast<size_t>(channels[i]), m)) return false;
  }
  if (!take(&fc1w_, 64 * 384, "fc1 weights")) return false;
  if (!take(&fc1b_, 64, "fc1 bias")) return false;
  if (!take(&fc2w_, 64, "fc2 weights")) return false;
  if (!take(&fc2b_, 1, "fc2 bias")) return false;
  (void)d;

  // 512-point FFT twiddles (radix-2, iterative, bit-reversed input order).
  cosTab_.resize(kNfft / 2);
  sinTab_.resize(kNfft / 2);
  for (int i = 0; i < kNfft / 2; ++i) {
    const double a = -2.0 * M_PI * i / kNfft;
    cosTab_[i] = static_cast<float>(std::cos(a));
    sinTab_[i] = static_cast<float>(std::sin(a));
  }

  ring_.assign(kWindow, 0.0f);
  frameRe_.assign(kNfft, 0.0f);
  frameIm_.assign(kNfft, 0.0f);
  melBuf_.assign(static_cast<size_t>(kMels) * kFrames, 0.0f);
  actA_.assign(static_cast<size_t>(64) * kMels * kFrames, 0.0f);
  actB_.assign(static_cast<size_t>(64) * kMels * kFrames, 0.0f);
  pooled_.assign(static_cast<size_t>(64) * kMels * kFrames, 0.0f);
  reset();
  ok_ = true;
  return true;
}

void WakeNet::reset() {
  std::fill(ring_.begin(), ring_.end(), 0.0f);
  wpos_ = filled_ = sinceEval_ = 0;
  hits_ = 0;
  lastScore_ = 0.0f;
  for (int i = 0; i < 5; ++i) scoreHist_[i] = 0.0f;
  histPos_ = histLen_ = 0;
}

// ------------------------------------------------------------------ front end

void WakeNet::fft(const float* re, const float* im, float* ore, float* oim) const {
  // bit-reversal permutation
  for (int i = 0; i < kNfft; ++i) {
    int j = 0;
    for (int b = 0; b < 9; ++b)
      if (i & (1 << b)) j |= 1 << (8 - b);
    ore[j] = re[i];
    oim[j] = im[i];
  }
  for (int len = 2; len <= kNfft; len <<= 1) {
    const int halfLen = len / 2;
    const int step = kNfft / len;
    for (int i = 0; i < kNfft; i += len) {
      for (int k = 0; k < halfLen; ++k) {
        const float wr = cosTab_[k * step], wi = sinTab_[k * step];
        const int a = i + k, b = i + k + halfLen;
        const float tr = ore[b] * wr - oim[b] * wi;
        const float ti = ore[b] * wi + oim[b] * wr;
        ore[b] = ore[a] - tr;
        oim[b] = oim[a] - ti;
        ore[a] += tr;
        oim[a] += ti;
      }
    }
  }
}

void WakeNet::mel(const float* w, std::vector<float>* out) {
  // 96 frames, each = 512 samples starting at i*160 (matches torch.stft with center=False),
  // shaped by the 400-sample hann window zero-padded into the middle of 512.
  for (int f = 0; f < kFrames; ++f) {
    const float* src = w + f * kHop;
    for (int i = 0; i < kNfft; ++i) frameRe_[i] = src[i] * window_[i];
    for (int i = 0; i < kNfft; ++i) frameIm_[i] = 0.0f;
    fft(frameRe_.data(), frameIm_.data(), frameRe_.data(), frameIm_.data());
    // power spectrum (257 bins) -> mel -> log10 dB clamped to [-100, 0] -> (x+100)/50
    for (int m = 0; m < kMels; ++m) {
      const float* row = melFb_.data() + static_cast<size_t>(m) * kBins;
      float acc = 0.0f;
      for (int b = 0; b < kBins; ++b) {
        const float p = frameRe_[b] * frameRe_[b] + frameIm_[b] * frameIm_[b];
        acc += row[b] * p;
      }
      float db = 10.0f * std::log10(acc > 1e-10f ? acc : 1e-10f);
      if (db < -100.0f) db = -100.0f;
      if (db > 0.0f) db = 0.0f;
      (*out)[static_cast<size_t>(m) * kFrames + f] = (db + 100.0f) / 50.0f;
    }
  }
}

// ------------------------------------------------------------------ forward pass

namespace {

// x: (ci, H, W) -> y: (co, H, W), 3x3 zero-padded, bias added, ReLU applied by the caller.
void conv3x3(const float* x, int ci, int H, int W, const float* w, const float* b, int co,
             float* out, bool relu) {
  for (int o = 0; o < co; ++o) {
    float* dst = out + static_cast<size_t>(o) * H * W;
    std::fill(dst, dst + static_cast<size_t>(H) * W, b[o]);
    const float* wrow = w + static_cast<size_t>(o) * ci * 9;
    for (int c = 0; c < ci; ++c) {
      const float* src = x + static_cast<size_t>(c) * H * W;
      const float* wc = wrow + static_cast<size_t>(c) * 9;
      for (int ky = 0; ky < 3; ++ky) {
        for (int kx = 0; kx < 3; ++kx) {
          const float kv = wc[ky * 3 + kx];
          if (kv == 0.0f) continue;
          for (int y = 0; y < H; ++y) {
            const int sy = y + ky - 1;
            if (sy < 0 || sy >= H) continue;
            const float* s = src + static_cast<size_t>(sy) * W;
            float* dsti = dst + static_cast<size_t>(y) * W;
            for (int xx = 0; xx < W; ++xx) {
              const int sx = xx + kx - 1;
              if (sx < 0 || sx >= W) continue;
              dsti[xx] += kv * s[sx];
            }
          }
        }
      }
    }
    if (relu)
      for (size_t i = 0; i < static_cast<size_t>(H) * W; ++i) dst[i] = dst[i] > 0 ? dst[i] : 0.0f;
  }
}

// max pool over (ph, pw)
void maxPool(const float* x, int c, int H, int W, int ph, int pw, float* out) {
  const int oh = H / ph, ow = W / pw;
  for (int ch = 0; ch < c; ++ch) {
    const float* src = x + static_cast<size_t>(ch) * H * W;
    float* dst = out + static_cast<size_t>(ch) * oh * ow;
    for (int y = 0; y < oh; ++y)
      for (int xx = 0; xx < ow; ++xx) {
        float m = -INFINITY;
        for (int dy = 0; dy < ph; ++dy)
          for (int dx = 0; dx < pw; ++dx) {
            const float v = src[static_cast<size_t>(y * ph + dy) * W + (xx * pw + dx)];
            if (v > m) m = v;
          }
        dst[static_cast<size_t>(y) * ow + xx] = m;
      }
  }
}

}  // namespace

float WakeNet::forward(const float* window) {
  mel(window, &melBuf_);

  // 40x96 -> conv(1->16) -> conv(16->16) -> pool  -> 20x48
  //         -> conv(16->32) -> conv(32->32) -> pool -> 10x24
  //         -> conv(32->64) -> conv(64->64) -> pool -> 5x12
  //         -> conv(64->64) -> conv(64->64) -> pool(1,2) -> 5x6
  //         -> mean over frequency -> 384 -> fc -> 64 -> fc -> 1 -> sigmoid
  // Scratch is allocated once in load() and reused: no allocation in the audio path.
  int ch = 1, H = kMels, W = kFrames;
  std::copy(melBuf_.begin(), melBuf_.end(), actA_.begin());

  const int poolH[4] = {2, 2, 2, 1};
  const int poolW[4] = {2, 2, 2, 2};
  for (int s = 0; s < 4; ++s) {
    for (int k = 0; k < 2; ++k) {
      const Conv& cv = convs_[s * 2 + k];
      conv3x3(actA_.data(), cv.ci, H, W, cv.w.data(), cv.b.data(), cv.co, actB_.data(), true);
      std::copy(actB_.begin(), actB_.begin() + static_cast<long>(cv.co) * H * W, actA_.begin());
      ch = cv.co;
    }
    maxPool(actA_.data(), ch, H, W, poolH[s], poolW[s], pooled_.data());
    H /= poolH[s];
    W /= poolW[s];
    std::copy(pooled_.begin(), pooled_.begin() + static_cast<long>(ch) * H * W, actA_.begin());
  }

  // mean over frequency: (ch, H, W) -> (ch, W)
  std::vector<float> feat(static_cast<size_t>(ch) * W, 0.0f);
  for (int cc = 0; cc < ch; ++cc) {
    const float* src = actA_.data() + static_cast<size_t>(cc) * H * W;
    for (int t = 0; t < W; ++t) {
      float acc = 0.0f;
      for (int y = 0; y < H; ++y) acc += src[static_cast<size_t>(y) * W + t];
      feat[static_cast<size_t>(cc) * W + t] = acc / static_cast<float>(H);
    }
  }

  // fc1: (384 -> 64), relu
  std::vector<float> h1(64, 0.0f);
  for (int o = 0; o < 64; ++o) {
    const float* row = fc1w_.data() + static_cast<size_t>(o) * feat.size();
    float acc = fc1b_[o];
    for (size_t i = 0; i < feat.size(); ++i) acc += row[i] * feat[i];
    h1[o] = acc > 0 ? acc : 0.0f;
  }
  // fc2: (64 -> 1), sigmoid
  float logit = fc2b_[0];
  for (int i = 0; i < 64; ++i) logit += fc2w_[i] * h1[i];
  return 1.0f / (1.0f + std::exp(-logit));
}

// ------------------------------------------------------------------ streaming

void WakeNet::push(const float* pcm, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    ring_[wpos_] = pcm[i];
    wpos_ = (wpos_ + 1) % ring_.size();
  }
  filled_ = std::min(ring_.size(), filled_ + n);
  sinceEval_ += n;
}

float WakeNet::score(const float* pcm, size_t n) {
  // Copy the window in time order, then AGC to the training level (the phone's mic gain is much
  // lower than a studio recording, and the model was trained at -26 dBFS RMS).
  std::vector<float> w(kWindow, 0.0f);
  if (n >= static_cast<size_t>(kWindow)) {
    std::memcpy(w.data(), pcm + (n - kWindow), sizeof(float) * kWindow);
  } else {
    std::memcpy(w.data() + (kWindow - n), pcm, sizeof(float) * n);
  }
  double acc = 0.0;
  for (float v : w) acc += static_cast<double>(v) * v;
  const float rms = static_cast<float>(std::sqrt(acc / kWindow));
  if (rms > 1e-6f) {
    const float g = opt_.agcTargetRms / rms;
    for (float& v : w) v *= g;
  }
  return forward(w.data());
}

bool WakeNet::accept(const float* pcm, size_t n) {
  if (!ok_) return false;
  push(pcm, n);
  if (filled_ < static_cast<size_t>(kWindow) || sinceEval_ < kEvalHop) return false;
  sinceEval_ = 0;

  std::vector<float> w(static_cast<size_t>(kWindow));
  const size_t start = wpos_;   // ring is full, so the oldest sample sits at wpos_
  for (size_t i = 0; i < w.size(); ++i) w[i] = ring_[(start + i) % ring_.size()];
  lastScore_ = score(w.data(), w.size());

  scoreHist_[histPos_] = lastScore_;
  histPos_ = (histPos_ + 1) % 5;
  if (histLen_ < 5) ++histLen_;
  hits_ = 0;
  for (int i = 0; i < histLen_; ++i)
    if (scoreHist_[i] >= opt_.threshold) ++hits_;

  if (hits_ >= opt_.patience) {
    reset();
    return true;
  }
  return false;
}

}  // namespace maya
