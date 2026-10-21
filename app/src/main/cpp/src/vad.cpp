#include "vad.h"

#include <sys/stat.h>

#include <cstring>

#include "common.h"

namespace maya {

bool Vad::init(const VadOptions& o, std::string* err) {
  struct stat st;
  if (::stat(o.model.c_str(), &st) != 0 || st.st_size <= 0) {
    *err = "Missing VAD model: " + o.model;
    return false;
  }
  SherpaOnnxVadModelConfig c;
  std::memset(&c, 0, sizeof c);
  c.silero_vad.model = o.model.c_str();
  c.silero_vad.threshold = o.threshold;
  c.silero_vad.min_silence_duration = o.minSilenceSec;
  c.silero_vad.min_speech_duration = o.minSpeechSec;
  c.silero_vad.window_size = 512;  // required for 16 kHz
  c.silero_vad.max_speech_duration = 30.0f;
  c.sample_rate = kMicRate;
  c.num_threads = o.threads;
  c.provider = "cpu";
  c.debug = 0;
  vad_.reset(SherpaOnnxCreateVoiceActivityDetector(&c, 30.0f));
  if (!vad_) {
    *err = "Could not create the Silero VAD";
    return false;
  }
  return true;
}

void Vad::reset() {
  if (vad_) SherpaOnnxVoiceActivityDetectorReset(vad_.get());
}

void Vad::accept(const float* pcm, size_t n) {
  if (!vad_ || n == 0) return;
  SherpaOnnxVoiceActivityDetectorAcceptWaveform(vad_.get(), pcm, static_cast<int32_t>(n));
  // We only use the live "speech active" flag; drop finished segments so memory stays flat.
  while (!SherpaOnnxVoiceActivityDetectorEmpty(vad_.get())) SherpaOnnxVoiceActivityDetectorPop(vad_.get());
}

bool Vad::speechActive() const { return vad_ && SherpaOnnxVoiceActivityDetectorDetected(vad_.get()) != 0; }

}  // namespace maya
