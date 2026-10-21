// Helpers to keep latency-critical work on the fast ("big") CPU cores.
// Snapdragon 4 Gen 5 = 2x Cortex-A78 @ 2.4 GHz + 6x Cortex-A55 @ 2.0 GHz.
#pragma once
#include <sched.h>

#include <vector>

namespace maya {

// Indices of the `count` fastest cores (by max frequency). Empty if the CPU is
// homogeneous or the topology cannot be read.
std::vector<int> detectBigCores(int count);

// Pin the calling thread to `cores`, intersected with the cores the scheduler
// currently allows for this process (Android cpusets can restrict them).
bool pinCurrentThread(const std::vector<int>& cores);

// Pins the calling thread for the lifetime of the object, then restores the old mask.
// Threads created while it is alive (e.g. ONNX Runtime pools) inherit the mask.
class ScopedAffinity {
 public:
  explicit ScopedAffinity(const std::vector<int>& cores);
  ~ScopedAffinity();
  ScopedAffinity(const ScopedAffinity&) = delete;
  ScopedAffinity& operator=(const ScopedAffinity&) = delete;

 private:
  cpu_set_t old_;
  bool active_ = false;
};

}  // namespace maya
