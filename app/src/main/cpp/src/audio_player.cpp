#include "audio_player.h"

#include <chrono>
#include <cstring>
#include <thread>

#include "common.h"

namespace maya {

aaudio_data_callback_result_t AudioPlayer::onData(AAudioStream*, void* user, void* data, int32_t frames) {
  auto* self = static_cast<AudioPlayer*>(user);
  auto* out = static_cast<float*>(data);
  const uint32_t gen = self->flushGen_.load(std::memory_order_acquire);
  if (gen != self->seenGen_) {
    self->ring_.discardAll();
    self->seenGen_ = gen;
  }
  const size_t got = self->ring_.read(out, static_cast<size_t>(frames));
  if (got < static_cast<size_t>(frames)) {
    std::memset(out + got, 0, (static_cast<size_t>(frames) - got) * sizeof(float));  // underrun -> silence
  }
  return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void AudioPlayer::onError(AAudioStream*, void* user, aaudio_result_t error) {
  LOGW("AAudio output error: %s", AAudio_convertResultToText(error));
  static_cast<AudioPlayer*>(user)->broken_.store(true);
}

bool AudioPlayer::begin(int sampleRate) {
  close(true);
  rate_ = sampleRate;
  broken_.store(false);
  ring_.discardAll();
  seenGen_ = flushGen_.load();

  AAudioStreamBuilder* b = nullptr;
  if (AAudio_createStreamBuilder(&b) != AAUDIO_OK || !b) return false;
  AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
  AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
  AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_FLOAT);
  AAudioStreamBuilder_setChannelCount(b, 1);
  AAudioStreamBuilder_setSampleRate(b, sampleRate);
  AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_ASSISTANT);
  AAudioStreamBuilder_setContentType(b, AAUDIO_CONTENT_TYPE_SPEECH);
  AAudioStreamBuilder_setDataCallback(b, &AudioPlayer::onData, this);
  AAudioStreamBuilder_setErrorCallback(b, &AudioPlayer::onError, this);
  const aaudio_result_t r = AAudioStreamBuilder_openStream(b, &stream_);
  AAudioStreamBuilder_delete(b);
  if (r != AAUDIO_OK) {
    LOGE("AAudio output open failed: %s", AAudio_convertResultToText(r));
    stream_ = nullptr;
    return false;
  }
  if (AAudioStream_requestStart(stream_) != AAUDIO_OK) {
    LOGE("AAudio output start failed");
    close(true);
    return false;
  }
  return true;
}

bool AudioPlayer::write(const float* samples, size_t n, const std::atomic<bool>* abort) {
  if (!stream_ || broken_.load()) return false;
  size_t done = 0;
  int64_t lastProgress = nowMs();
  while (done < n) {
    if (abort && abort->load()) return false;
    if (broken_.load()) return false;
    const size_t w = ring_.write(samples + done, n - done);
    done += w;
    if (w > 0) {
      lastProgress = nowMs();
    } else {
      if (nowMs() - lastProgress > 5000) return false;  // device stopped consuming: don't hang the pipeline
      std::this_thread::sleep_for(std::chrono::milliseconds(5));  // back-pressure
    }
  }
  return true;
}

void AudioPlayer::flush() { flushGen_.fetch_add(1, std::memory_order_release); }

void AudioPlayer::drainAndClose(const std::atomic<bool>* abort) {
  if (!stream_) return;
  const int64_t deadline = nowMs() + 90000;
  bool aborted = false;
  while (ring_.size() > 0 && nowMs() < deadline && !broken_.load()) {
    if (abort && abort->load()) { aborted = true; break; }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (aborted) {
    flush();
  } else {
    // Let the device buffer play out.
    const int32_t frames = AAudioStream_getBufferSizeInFrames(stream_);
    const int tailMs = frames > 0 ? static_cast<int>(1000LL * frames / rate_) + 60 : 200;
    std::this_thread::sleep_for(std::chrono::milliseconds(tailMs));
  }
  close(aborted);
}

void AudioPlayer::close(bool immediate) {
  if (!stream_) return;
  if (immediate) flush();
  AAudioStream_requestStop(stream_);
  aaudio_stream_state_t cur = AAudioStream_getState(stream_);
  for (int i = 0; i < 10 && cur != AAUDIO_STREAM_STATE_STOPPED && cur != AAUDIO_STREAM_STATE_DISCONNECTED &&
                  cur != AAUDIO_STREAM_STATE_CLOSED;
       ++i) {
    AAudioStream_waitForStateChange(stream_, cur, &cur, 50LL * 1000 * 1000);
  }
  AAudioStream_close(stream_);
  stream_ = nullptr;
}

}  // namespace maya
