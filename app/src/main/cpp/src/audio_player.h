// TTS playback through AAudio. One stream per spoken answer: begin() -> write()... -> drainAndClose().
#pragma once
#include <aaudio/AAudio.h>

#include <atomic>

#include "ring_buffer.h"

namespace maya {

class AudioPlayer {
 public:
  AudioPlayer() : ring_(1 << 19) {}  // 524288 float samples = 23 s at 22.05 kHz
  ~AudioPlayer() { close(true); }
  AudioPlayer(const AudioPlayer&) = delete;
  AudioPlayer& operator=(const AudioPlayer&) = delete;

  bool begin(int sampleRate);  // open + start (plays silence until data arrives)
  // Producer thread: blocks while the ring is full. False if aborted or no stream.
  bool write(const float* samples, size_t n, const std::atomic<bool>* abort);
  void flush();  // any thread: discard queued audio immediately
  // Wait until queued audio has been played (or abort), then stop and close the stream.
  void drainAndClose(const std::atomic<bool>* abort);
  void close(bool immediate);

 private:
  static aaudio_data_callback_result_t onData(AAudioStream* s, void* user, void* data, int32_t frames);
  static void onError(AAudioStream* s, void* user, aaudio_result_t error);

  AAudioStream* stream_ = nullptr;
  int rate_ = 22050;
  SpscRing<float> ring_;
  std::atomic<uint32_t> flushGen_{0};
  uint32_t seenGen_ = 0;  // audio-callback thread only
  std::atomic<bool> broken_{false};
};

}  // namespace maya
