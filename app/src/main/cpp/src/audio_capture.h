// Always-on microphone capture through AAudio (16 kHz, mono, 16-bit).
// The AAudio callback only copies into a lock-free ring; all processing happens on the
// pipeline thread, so the real-time audio thread never touches models or the heap.
#pragma once
#include <aaudio/AAudio.h>

#include <atomic>
#include <condition_variable>
#include <mutex>

#include "ring_buffer.h"

namespace maya {

class AudioCapture {
 public:
  AudioCapture() : ring_(1 << 16) {}  // 65536 samples = 4.1 s of headroom
  ~AudioCapture() { stop(); }
  AudioCapture(const AudioCapture&) = delete;
  AudioCapture& operator=(const AudioCapture&) = delete;

  bool start();
  void stop();

  // Consumer side (pipeline thread).
  size_t read(int16_t* dst, size_t n);                  // non-blocking
  bool waitForData(size_t minSamples, int timeoutMs);   // true if >= minSamples are ready
  void discard();                                       // drop buffered audio
  bool restartIfNeeded();                               // reopen after route change / disconnect
  void wakeWaiters();                                   // unblock waitForData (shutdown)

  // false => the callback drops frames (used while Maya is thinking or speaking).
  void setActive(bool on) { active_.store(on, std::memory_order_relaxed); }

 private:
  bool openStream();
  bool tryOpen(bool preferred);
  void closeStream();
  static aaudio_data_callback_result_t onData(AAudioStream* s, void* user, void* data, int32_t frames);
  static void onError(AAudioStream* s, void* user, aaudio_result_t error);

  AAudioStream* stream_ = nullptr;
  SpscRing<int16_t> ring_;
  std::atomic<bool> active_{true};
  std::atomic<bool> needRestart_{false};
  std::atomic<size_t> notifyAt_{800};
  int64_t lastRestartMs_ = 0;
  std::mutex mu_;
  std::condition_variable cv_;
};

}  // namespace maya
