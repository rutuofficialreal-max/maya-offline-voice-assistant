#include "audio_capture.h"

#include <chrono>
#include <thread>

#include "common.h"

namespace maya {

aaudio_data_callback_result_t AudioCapture::onData(AAudioStream*, void* user, void* data, int32_t frames) {
  auto* self = static_cast<AudioCapture*>(user);
  if (self->active_.load(std::memory_order_relaxed)) {
    self->ring_.write(static_cast<const int16_t*>(data), static_cast<size_t>(frames));
    if (self->ring_.size() >= self->notifyAt_.load(std::memory_order_relaxed)) {
      { std::lock_guard<std::mutex> lk(self->mu_); }  // closes the lost-wakeup window
      self->cv_.notify_one();
    }
  }
  return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void AudioCapture::onError(AAudioStream*, void* user, aaudio_result_t error) {
  auto* self = static_cast<AudioCapture*>(user);
  LOGW("AAudio input error: %s", AAudio_convertResultToText(error));
  // Must not close/reopen from the error callback thread: flag it for the pipeline thread.
  self->needRestart_.store(true);
  { std::lock_guard<std::mutex> lk(self->mu_); }
  self->cv_.notify_all();
}

bool AudioCapture::tryOpen(bool preferred) {
  AAudioStreamBuilder* b = nullptr;
  if (AAudio_createStreamBuilder(&b) != AAUDIO_OK || !b) return false;
  AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_INPUT);
  AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
  AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
  AAudioStreamBuilder_setChannelCount(b, 1);
  AAudioStreamBuilder_setSampleRate(b, kMicRate);
  if (preferred) {
    // Passive listening runs all day: favour battery. The wake-word model needs ~320 ms of
    // context anyway, so a few ms of extra capture latency is irrelevant.
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_POWER_SAVING);
    AAudioStreamBuilder_setInputPreset(b, AAUDIO_INPUT_PRESET_VOICE_RECOGNITION);
  }
  AAudioStreamBuilder_setDataCallback(b, &AudioCapture::onData, this);
  AAudioStreamBuilder_setErrorCallback(b, &AudioCapture::onError, this);

  const aaudio_result_t r = AAudioStreamBuilder_openStream(b, &stream_);
  AAudioStreamBuilder_delete(b);
  if (r != AAUDIO_OK) {
    LOGW("AAudio input open failed (%s): %s", preferred ? "preferred" : "plain", AAudio_convertResultToText(r));
    stream_ = nullptr;
    return false;
  }
  if (AAudioStream_getSampleRate(stream_) != kMicRate || AAudioStream_getChannelCount(stream_) != 1 ||
      AAudioStream_getFormat(stream_) != AAUDIO_FORMAT_PCM_I16) {
    LOGW("AAudio input gave an unexpected format (%d Hz, %d ch)", AAudioStream_getSampleRate(stream_),
         AAudioStream_getChannelCount(stream_));
    closeStream();
    return false;
  }
  const aaudio_result_t s = AAudioStream_requestStart(stream_);
  if (s != AAUDIO_OK) {
    LOGW("AAudio input start failed: %s", AAudio_convertResultToText(s));
    closeStream();
    return false;
  }
  LOGI("Mic stream open (%s): %d Hz, burst %d frames", preferred ? "preferred" : "plain",
       AAudioStream_getSampleRate(stream_), AAudioStream_getFramesPerBurst(stream_));
  return true;
}

bool AudioCapture::openStream() {
  // Some devices reject low-power mode / the voice-recognition preset: retry with plain settings.
  return tryOpen(true) || tryOpen(false);
}

void AudioCapture::closeStream() {
  if (!stream_) return;
  AAudioStream_requestStop(stream_);
  aaudio_stream_state_t cur = AAudioStream_getState(stream_);
  for (int i = 0; i < 10 && cur != AAUDIO_STREAM_STATE_STOPPED && cur != AAUDIO_STREAM_STATE_DISCONNECTED &&
                  cur != AAUDIO_STREAM_STATE_CLOSED;
       ++i) {
    AAudioStream_waitForStateChange(stream_, cur, &cur, 50LL * 1000 * 1000);
  }
  AAudioStream_close(stream_);  // after this returns the callbacks are guaranteed finished
  stream_ = nullptr;
}

bool AudioCapture::start() {
  needRestart_.store(false);
  ring_.discardAll();
  return openStream();
}

void AudioCapture::stop() {
  closeStream();
  wakeWaiters();
}

bool AudioCapture::restartIfNeeded() {
  if (!needRestart_.load()) return true;
  const int64_t now = nowMs();
  if (now - lastRestartMs_ < 1000) return false;  // rate-limit retries
  lastRestartMs_ = now;
  LOGI("Restarting mic stream");
  closeStream();
  if (openStream()) {
    needRestart_.store(false);
    return true;
  }
  return false;
}

size_t AudioCapture::read(int16_t* dst, size_t n) { return ring_.read(dst, n); }

void AudioCapture::discard() { ring_.discardAll(); }

void AudioCapture::wakeWaiters() {
  { std::lock_guard<std::mutex> lk(mu_); }
  cv_.notify_all();
}

bool AudioCapture::waitForData(size_t minSamples, int timeoutMs) {
  notifyAt_.store(minSamples, std::memory_order_relaxed);
  if (ring_.size() >= minSamples) return true;
  std::unique_lock<std::mutex> lk(mu_);
  cv_.wait_for(lk, std::chrono::milliseconds(timeoutMs),
               [&] { return ring_.size() >= minSamples || needRestart_.load(); });
  return ring_.size() >= minSamples;
}

}  // namespace maya
