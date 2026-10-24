// Host test for the trained wake-word runtime (wake_net.cpp): no Android, no ONNX Runtime.
//
// 1. Rebuilds the exact signal used by tools/export_wake_net.py and checks the score against the
//    value the ONNX model produced for it (0.0003608167). This covers the whole chain — Hann window,
//    512-point FFT, mel filterbank, log, the 8 convolutions, pooling, the head and the sigmoid — so a
//    mistake anywhere shows up here instead of on the phone.
// 2. Feeds the same signal through the streaming API in 50 ms chunks (what the audio thread does) and
//    checks it never fires, and that the ring buffer/patience bookkeeping keeps working.
// 3. If a WAV/A 16-bit mono path is given as argv[2], prints per-window scores for manual comparison.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "wake_net.h"

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kSR = 16000;
constexpr int kWindow = 15712;
constexpr double kTargetRms = 0.0501;

std::vector<float> referenceSignal() {
  std::vector<float> x(kWindow);
  for (int i = 0; i < kWindow; ++i) {
    const double t = static_cast<double>(i) / kSR;
    x[i] = static_cast<float>(0.3 * std::sin(2 * kPi * 220 * t) + 0.2 * std::sin(2 * kPi * 660 * t + 0.5) +
                              0.1 * std::sin(2 * kPi * 1500 * t));
  }
  return x;
}

bool readWav16(const char* path, std::vector<float>* out) {
  FILE* f = fopen(path, "rb");
  if (!f) return false;
  char hdr[44];
  if (fread(hdr, 1, 44, f) != 44 || std::memcmp(hdr, "RIFF", 4) != 0 || std::memcmp(hdr + 8, "WAVE", 4) != 0) {
    fclose(f);
    return false;
  }
  int channels = *reinterpret_cast<int16_t*>(hdr + 22);
  int rate = *reinterpret_cast<int*>(hdr + 24);
  int bits = *reinterpret_cast<int16_t*>(hdr + 34);
  if (channels != 1 || rate != kSR || bits != 16) {
    fclose(f);
    return false;
  }
  out->clear();
  int16_t s;
  while (fread(&s, 2, 1, f) == 1) out->push_back(static_cast<float>(s) / 32768.0f);
  fclose(f);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  const char* weights = argc > 1 ? argv[1] : "app/src/main/assets/wake_weights.bin";
  int failures = 0;

  maya::WakeNetOptions opt;
  opt.weightsPath = weights;
  maya::WakeNet net;
  std::string err;
  if (!net.init(opt, &err)) {
    std::printf("FAIL init: %s\n", err.c_str());
    return 1;
  }
  std::printf("ok  weights loaded\n");

  // ---- 1. single-window score must match the ONNX model that Kaggle validated
  const std::vector<float> sig = referenceSignal();
  const float got = net.score(sig.data(), sig.size());
  const float expect = 0.0003608167f;
  std::printf("    score %.7f (expected %.7f, diff %.2e)\n", got, expect, std::fabs(got - expect));
  if (std::fabs(got - expect) > 2e-4f) {
    std::printf("FAIL score does not match the ONNX model\n");
    ++failures;
  }

  // ---- 2. streaming: 75 s of the reference signal must never trigger
  net.reset();
  int fires = 0;
  std::vector<float> chunk(800);
  for (int i = 0; i < 75000 * 16 / 800; ++i) {
    for (size_t k = 0; k < chunk.size(); ++k) {
      const int idx = static_cast<int>((i * chunk.size() + k) % sig.size());
      chunk[k] = sig[idx];
    }
    if (net.accept(chunk.data(), chunk.size())) ++fires;
  }
  std::printf("    streaming: %d false wakes in 75 s, last score %.6f\n", fires, net.lastScore());
  if (fires > 0) {
    std::printf("FAIL the reference signal must not be detected as \"Hey Maya\"\n");
    ++failures;
  }

  // ---- 3. optional manual comparison against a real recording
  if (argc > 2) {
    std::vector<float> pcm;
    if (!readWav16(argv[2], &pcm)) {
      std::printf("FAIL could not read %s (expects 16 kHz mono 16-bit WAV)\n", argv[2]);
      return 1;
    }
    net.reset();
    for (size_t off = 0; off + 800 <= pcm.size(); off += 800) {
      net.accept(pcm.data() + off, 800);
    }
    std::printf("    %s: last score %.6f\n", argv[2], net.lastScore());
  }

  if (failures) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("all wake-net tests passed\n");
  return 0;
}
