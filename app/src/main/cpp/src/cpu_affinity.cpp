#include "cpu_affinity.h"

#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <utility>

#include "common.h"

namespace maya {

static long readLongFile(const char* path) {
  FILE* f = std::fopen(path, "r");
  if (!f) return -1;
  long v = -1;
  if (std::fscanf(f, "%ld", &v) != 1) v = -1;
  std::fclose(f);
  return v;
}

std::vector<int> detectBigCores(int count) {
  const long n = sysconf(_SC_NPROCESSORS_CONF);
  if (n <= 0 || n > 64) return {};
  std::vector<std::pair<long, int>> freq;  // (max frequency, core index)
  for (int i = 0; i < n; ++i) {
    char path[96];
    std::snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", i);
    long f = readLongFile(path);
    if (f <= 0) {
      std::snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpu_capacity", i);
      f = readLongFile(path);
    }
    if (f > 0) freq.emplace_back(f, i);
  }
  if (freq.empty()) return {};
  std::sort(freq.begin(), freq.end(), [](const auto& a, const auto& b) {
    return a.first != b.first ? a.first > b.first : a.second > b.second;
  });
  if (freq.front().first == freq.back().first) return {};  // homogeneous CPU
  std::vector<int> out;
  for (size_t i = 0; i < freq.size() && static_cast<int>(i) < count; ++i) out.push_back(freq[i].second);
  std::sort(out.begin(), out.end());
  return out;
}

bool pinCurrentThread(const std::vector<int>& cores) {
  if (cores.empty()) return false;
  cpu_set_t allowed;
  CPU_ZERO(&allowed);
  if (sched_getaffinity(0, sizeof allowed, &allowed) != 0) return false;
  cpu_set_t want;
  CPU_ZERO(&want);
  int usable = 0;
  for (int c : cores) {
    if (c >= 0 && c < CPU_SETSIZE && CPU_ISSET(c, &allowed)) {
      CPU_SET(c, &want);
      ++usable;
    }
  }
  if (usable == 0) {
    LOGW("Big cores are outside this process' cpuset; not pinning");
    return false;
  }
  return sched_setaffinity(0, sizeof want, &want) == 0;
}

ScopedAffinity::ScopedAffinity(const std::vector<int>& cores) {
  CPU_ZERO(&old_);
  if (cores.empty()) return;
  if (sched_getaffinity(0, sizeof old_, &old_) != 0) return;
  active_ = pinCurrentThread(cores);
}

ScopedAffinity::~ScopedAffinity() {
  if (active_) sched_setaffinity(0, sizeof old_, &old_);
}

}  // namespace maya
