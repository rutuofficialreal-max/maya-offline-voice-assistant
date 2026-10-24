// Trained "Hey Maya" wake-word model, run as a hand-written forward pass (no ONNX Runtime).
//
// The model is the 170k-parameter CNN trained on the user's own recordings (see tools/export_wake_net.py
// and the Kaggle notebook). Weights arrive in a flat float32 blob; this file does:
//
//   int16/float PCM -> AGC -> 96 x 512-point FFT -> 40-band mel -> log -> 8 conv layers -> score
//
// Keeping the mel front end here (instead of a second ONNX graph) means the numbers are produced by
// the same code path that was validated on the host: tools/export_wake_net.py prints a numeric proof
// that this maths matches the ONNX model to ~1e-8, and cpp/tests/test_wake_net.cpp re-checks it in CI.
//
// Threading: NOT thread-safe, like the rest of the wake path. Only the pipeline thread touches it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace maya {

struct WakeNetOptions {
  std::string weightsPath;      // wake_weights.bin (exported from hey_maya.onnx)
  float threshold = 0.9976f;    // from training: 0 false accepts / hour on the held-out negatives
  int patience = 3;             // how many of the last 5 windows must exceed the threshold
  float agcTargetRms = 0.0501f; // -26 dBFS, the level the model was trained at
};

class WakeNet {
 public:
  WakeNet() = default;
  WakeNet(const WakeNet&) = delete;
  WakeNet& operator=(const WakeNet&) = delete;

  // Loads the weight blob. Returns false (with a message) if the file is missing or malformed,
  // in which case the caller must fall back to the sherpa spotter.
  bool init(const WakeNetOptions& opt, std::string* err);

  bool ok() const { return ok_; }
  void reset();                                  // forget the audio history and the patience counter
  bool accept(const float* pcm, size_t n);       // true => "Hey Maya" was just heard
  float lastScore() const { return lastScore_; }
  void setThreshold(float t) { opt_.threshold = t; }
  float threshold() const { return opt_.threshold; }

  // Exposed for the host test (and for a debug log line): one score for a 0.98 s window.
  float score(const float* pcm, size_t n);

 private:
  bool load(const std::string& path, std::string* err);
  void push(const float* pcm, size_t n);
  float forward(const float* window /* kWindow samples, AGC applied by caller */);
  void mel(const float* window, std::vector<float>* out /* kNMels*kFrames */);
  void fft(const float* re, const float* im, float* ore, float* oim) const;

  WakeNetOptions opt_;
  bool ok_ = false;

  // front end
  std::vector<float> melFb_;    // (40, 257)
  std::vector<float> window_;   // (512) hann, zero-padded 400 -> 512
  std::vector<float> cosTab_, sinTab_;      // twiddles for the 512-point FFT
  std::vector<float> frameRe_, frameIm_;
  std::vector<float> melBuf_;                       // (40,96) log-mel
  std::vector<float> actA_, actB_, pooled_;         // reusable activation scratch (no alloc in accept())

  // model
  struct Conv { std::vector<float> w, b; int ci = 0, co = 0; };
  Conv convs_[8];
  std::vector<float> fc1w_, fc1b_, fc2w_, fc2b_;

  // streaming state
  std::vector<float> ring_;
  size_t wpos_ = 0, filled_ = 0, sinceEval_ = 0;
  int hits_ = 0;
  float scoreHist_[5] = {0, 0, 0, 0, 0};
  int histPos_ = 0, histLen_ = 0;
  float lastScore_ = 0.0f;
};

}  // namespace maya
