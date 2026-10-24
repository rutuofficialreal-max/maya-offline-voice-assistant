// JNI bridge: Kotlin <-> MayaEngine. Registered with RegisterNatives (no symbol-name coupling).
#include <jni.h>

#include <memory>
#include <string>
#include <vector>

#include "common.h"
#include "engine.h"

namespace {

JavaVM* g_vm = nullptr;

struct Bridge {
  std::unique_ptr<maya::MayaEngine> engine;
  jobject listener = nullptr;  // global ref to MayaListener
  jmethodID onEvent = nullptr; // void onEvent(int, byte[])
};

// Native worker threads attach to the JVM on first use and detach when the thread exits.
struct ThreadDetacher {
  bool attached = false;
  ~ThreadDetacher() {
    if (attached && g_vm) g_vm->DetachCurrentThread();
  }
};

JNIEnv* currentEnv() {
  if (!g_vm) return nullptr;
  JNIEnv* env = nullptr;
  if (g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) return env;
  JavaVMAttachArgs args{JNI_VERSION_1_6, const_cast<char*>("maya-native"), nullptr};
  if (g_vm->AttachCurrentThread(&env, &args) != JNI_OK) return nullptr;
  thread_local ThreadDetacher detacher;
  detacher.attached = true;
  return env;
}

void dispatch(Bridge* b, int type, const std::string& payload) {
  JNIEnv* env = currentEnv();
  if (!env || !b->listener) return;
  jbyteArray arr = nullptr;
  if (!payload.empty()) {
    arr = env->NewByteArray(static_cast<jsize>(payload.size()));
    if (arr) env->SetByteArrayRegion(arr, 0, static_cast<jsize>(payload.size()), reinterpret_cast<const jbyte*>(payload.data()));
  }
  env->CallVoidMethod(b->listener, b->onEvent, static_cast<jint>(type), arr);
  if (env->ExceptionCheck()) env->ExceptionClear();
  if (arr) env->DeleteLocalRef(arr);
}

// ---- native methods ------------------------------------------------------------------

jlong nativeCreate(JNIEnv* env, jobject /*thiz*/, jstring jconfig, jobject listener) {
  auto bridge = std::make_unique<Bridge>();
  jclass cls = env->GetObjectClass(listener);
  bridge->onEvent = env->GetMethodID(cls, "onEvent", "(I[B)V");
  env->DeleteLocalRef(cls);
  if (!bridge->onEvent) return 0;
  bridge->listener = env->NewGlobalRef(listener);

  const char* cfgChars = env->GetStringUTFChars(jconfig, nullptr);
  const std::string cfgText = cfgChars ? cfgChars : "";
  if (cfgChars) env->ReleaseStringUTFChars(jconfig, cfgChars);

  Bridge* raw = bridge.get();
  try {
    bridge->engine = std::make_unique<maya::MayaEngine>(
        [raw](int type, const std::string& payload) { dispatch(raw, type, payload); });
    std::string err;
    if (!bridge->engine->init(maya::EngineConfig::parse(cfgText), &err)) {
      LOGE("Engine init failed: %s", err.c_str());
      dispatch(raw, maya::EVT_ERROR, err);
      bridge->engine.reset();
      env->DeleteGlobalRef(bridge->listener);
      return 0;
    }
  } catch (const std::exception& e) {
    LOGE("Engine init threw: %s", e.what());
    dispatch(raw, maya::EVT_ERROR, std::string("Init failed: ") + e.what());
    bridge->engine.reset();
    env->DeleteGlobalRef(bridge->listener);
    return 0;
  }
  return reinterpret_cast<jlong>(bridge.release());
}

jboolean nativeStart(JNIEnv*, jobject, jlong handle) {
  auto* b = reinterpret_cast<Bridge*>(handle);
  if (!b || !b->engine) return JNI_FALSE;
  try {
    return b->engine->start() ? JNI_TRUE : JNI_FALSE;
  } catch (const std::exception& e) {
    LOGE("start threw: %s", e.what());
    return JNI_FALSE;
  }
}

void nativeStop(JNIEnv*, jobject, jlong handle) {
  auto* b = reinterpret_cast<Bridge*>(handle);
  if (b && b->engine) b->engine->stop();
}

void nativeInterrupt(JNIEnv*, jobject, jlong handle) {
  auto* b = reinterpret_cast<Bridge*>(handle);
  if (b && b->engine) b->engine->interrupt();
}

// Live wake-word update: threshold + calibrated phrases ('\n' separated token strings).
void nativeSetWake(JNIEnv* env, jobject, jlong handle, jfloat threshold, jstring jphrases) {
  auto* b = reinterpret_cast<Bridge*>(handle);
  if (!b || !b->engine) return;
  const char* chars = jphrases ? env->GetStringUTFChars(jphrases, nullptr) : nullptr;
  const std::string all = chars ? chars : "";
  if (chars) env->ReleaseStringUTFChars(jphrases, chars);
  std::vector<std::string> phrases;
  size_t pos = 0;
  while (pos < all.size()) {
    size_t nl = all.find('\n', pos);
    if (nl == std::string::npos) nl = all.size();
    std::string line = all.substr(pos, nl - pos);
    pos = nl + 1;
    if (!line.empty()) phrases.push_back(line);
  }
  b->engine->setWakeOptions(threshold, std::move(phrases));
}

// Switch the wake engine at runtime: 0 = sherpa keyword spotter, 1 = trained "Hey Maya" model.
void nativeSetWakeEngine(JNIEnv*, jobject, jlong handle, jint engine) {
  auto* b = reinterpret_cast<Bridge*>(handle);
  if (!b || !b->engine) return;
  b->engine->setWakeEngine(static_cast<int>(engine));
}

void nativeDestroy(JNIEnv* env, jobject, jlong handle) {
  auto* b = reinterpret_cast<Bridge*>(handle);
  if (!b) return;
  if (b->engine) b->engine->stop();   // joins the worker threads
  b->engine.reset();                  // frees every model, stream and buffer (RAII)
  if (b->listener) env->DeleteGlobalRef(b->listener);
  delete b;
}

// Calibration: decode one recording -> "OK\t<text>\t<tokens>" or "ERR\t<message>".
jstring nativeCalibrateDecode(JNIEnv* env, jobject, jstring jconfig, jshortArray jpcm) {
  const char* cfgChars = env->GetStringUTFChars(jconfig, nullptr);
  const std::string cfgText = cfgChars ? cfgChars : "";
  if (cfgChars) env->ReleaseStringUTFChars(jconfig, cfgChars);
  const jsize n = env->GetArrayLength(jpcm);
  std::vector<int16_t> pcm(static_cast<size_t>(n));
  if (n > 0) env->GetShortArrayRegion(jpcm, 0, n, reinterpret_cast<jshort*>(pcm.data()));

  std::string result;
  try {
    const maya::EngineConfig cfg = maya::EngineConfig::parse(cfgText);
    maya::WakeWordOptions wo;
    wo.encoder = cfg.kwsEncoder; wo.decoder = cfg.kwsDecoder; wo.joiner = cfg.kwsJoiner; wo.tokens = cfg.kwsTokens;
    std::string text, tokens, err;
    if (maya::decodeWakeCalibration(wo, pcm.data(), pcm.size(), &text, &tokens, &err)) result = "OK\t" + text + "\t" + tokens;
    else result = "ERR\t" + err;
  } catch (const std::exception& e) {
    result = std::string("ERR\t") + e.what();
  }
  return env->NewStringUTF(result.c_str());
}

const JNINativeMethod kMethods[] = {
    {"nativeCreate", "(Ljava/lang/String;Lcom/maya/assistant/MayaListener;)J", reinterpret_cast<void*>(nativeCreate)},
    {"nativeStart", "(J)Z", reinterpret_cast<void*>(nativeStart)},
    {"nativeStop", "(J)V", reinterpret_cast<void*>(nativeStop)},
    {"nativeInterrupt", "(J)V", reinterpret_cast<void*>(nativeInterrupt)},
    {"nativeDestroy", "(J)V", reinterpret_cast<void*>(nativeDestroy)},
    {"nativeSetWake", "(JFLjava/lang/String;)V", reinterpret_cast<void*>(nativeSetWake)},
    {"nativeSetWakeEngine", "(JI)V", reinterpret_cast<void*>(nativeSetWakeEngine)},
    {"nativeCalibrateDecode", "(Ljava/lang/String;[S)Ljava/lang/String;", reinterpret_cast<void*>(nativeCalibrateDecode)},
};

}  // namespace

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
  g_vm = vm;
  JNIEnv* env = nullptr;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
  jclass cls = env->FindClass("com/maya/assistant/NativeBridge");
  if (!cls) return JNI_ERR;
  if (env->RegisterNatives(cls, kMethods, sizeof(kMethods) / sizeof(kMethods[0])) != JNI_OK) return JNI_ERR;
  env->DeleteLocalRef(cls);
  return JNI_VERSION_1_6;
}
