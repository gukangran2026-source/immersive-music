#include "AudioPlayer.hpp"

#include <android/log.h>

namespace musicmod {
namespace {

constexpr const char *kTag = "MusicMod";

bool drainException(JNIEnv *env, const char *where) {
  if (env->ExceptionCheck() == JNI_FALSE) {
    return false;
  }
  __android_log_print(ANDROID_LOG_ERROR, kTag, "%s: Java 侧抛异常", where);
  env->ExceptionDescribe();
  env->ExceptionClear();
  return true;
}

// 取得当前线程 JNIEnv，析构时按需 detach。
class EnvGuard {
public:
  explicit EnvGuard(JavaVM *vm) : mVm(vm) {
    if (mVm == nullptr) {
      return;
    }
    if (mVm->GetEnv(reinterpret_cast<void **>(&mEnv), JNI_VERSION_1_6) == JNI_OK) {
      return;
    }
    if (mVm->AttachCurrentThread(&mEnv, nullptr) == JNI_OK) {
      mAttached = true;
    } else {
      mEnv = nullptr;
    }
  }

  ~EnvGuard() {
    if (mAttached && mVm != nullptr) {
      mVm->DetachCurrentThread();
    }
  }

  EnvGuard(const EnvGuard &) = delete;
  EnvGuard &operator=(const EnvGuard &) = delete;

  JNIEnv *env() const { return mEnv; }
  bool ok() const { return mEnv != nullptr; }

private:
  JavaVM *mVm{nullptr};
  JNIEnv *mEnv{nullptr};
  bool mAttached{false};
};

} // namespace

AudioPlayer::~AudioPlayer() { stop(); }

bool AudioPlayer::play(const std::string &uri) {
  if (mJavaVm == nullptr) {
    __android_log_print(ANDROID_LOG_ERROR, kTag, "播放失败: JavaVM 为空");
    return false;
  }

  stop();

  EnvGuard guard(mJavaVm);
  if (!guard.ok()) {
    __android_log_print(ANDROID_LOG_ERROR, kTag, "播放失败: 线程附加失败");
    return false;
  }
  JNIEnv *env = guard.env();

  bool ok = false;
  jclass playerClass = nullptr;
  jobject player = nullptr;
  jmethodID releaseMethod = nullptr;

  do {
    playerClass = env->FindClass("android/media/MediaPlayer");
    if (playerClass == nullptr) {
      __android_log_print(ANDROID_LOG_ERROR, kTag, "播放失败: 找不到 MediaPlayer");
      drainException(env, "FindClass");
      break;
    }

    jmethodID ctor = env->GetMethodID(playerClass, "<init>", "()V");
    if (ctor == nullptr) {
      drainException(env, "GetMethodID <init>");
      break;
    }

    player = env->NewObject(playerClass, ctor);
    if (player == nullptr) {
      drainException(env, "NewObject MediaPlayer");
      break;
    }

    releaseMethod = env->GetMethodID(playerClass, "release", "()V");
    jmethodID setDataSource =
        env->GetMethodID(playerClass, "setDataSource", "(Ljava/lang/String;)V");
    jmethodID prepare = env->GetMethodID(playerClass, "prepare", "()V");
    jmethodID start = env->GetMethodID(playerClass, "start", "()V");
    if (releaseMethod == nullptr || setDataSource == nullptr ||
        prepare == nullptr || start == nullptr) {
      drainException(env, "GetMethodID 播放方法");
      break;
    }

    jstring jUri = env->NewStringUTF(uri.c_str());
    if (jUri == nullptr) {
      drainException(env, "NewStringUTF");
      break;
    }
    env->CallVoidMethod(player, setDataSource, jUri);
    env->DeleteLocalRef(jUri);
    if (drainException(env, "setDataSource")) {
      break;
    }

    env->CallVoidMethod(player, prepare);
    if (drainException(env, "prepare")) {
      break;
    }

    env->CallVoidMethod(player, start);
    if (drainException(env, "start")) {
      break;
    }

    mPlayer = env->NewGlobalRef(player);
    if (mPlayer == nullptr) {
      drainException(env, "NewGlobalRef");
      break;
    }
    ok = true;
  } while (false);

  // 已创建但未成功接管的播放器必须 release，避免留下无人控制的实例。
  if (!ok && player != nullptr && releaseMethod != nullptr) {
    env->CallVoidMethod(player, releaseMethod);
    drainException(env, "release 清理");
  }
  if (player != nullptr) {
    env->DeleteLocalRef(player);
  }
  if (playerClass != nullptr) {
    env->DeleteLocalRef(playerClass);
  }

  return ok;
}

bool AudioPlayer::beginPrepare(const std::string &uri) {
  if (mJavaVm == nullptr) {
    return false;
  }

  stop();

  EnvGuard guard(mJavaVm);
  if (!guard.ok()) {
    return false;
  }
  JNIEnv *env = guard.env();

  bool ok = false;
  jclass playerClass = nullptr;
  jobject player = nullptr;
  jmethodID releaseMethod = nullptr;

  do {
    playerClass = env->FindClass("android/media/MediaPlayer");
    if (playerClass == nullptr) {
      drainException(env, "FindClass");
      break;
    }

    jmethodID ctor = env->GetMethodID(playerClass, "<init>", "()V");
    if (ctor == nullptr) {
      drainException(env, "GetMethodID <init>");
      break;
    }

    player = env->NewObject(playerClass, ctor);
    if (player == nullptr) {
      drainException(env, "NewObject MediaPlayer");
      break;
    }

    releaseMethod = env->GetMethodID(playerClass, "release", "()V");
    jmethodID setDataSource =
        env->GetMethodID(playerClass, "setDataSource", "(Ljava/lang/String;)V");
    jmethodID prepareAsync = env->GetMethodID(playerClass, "prepareAsync", "()V");
    if (releaseMethod == nullptr || setDataSource == nullptr ||
        prepareAsync == nullptr) {
      drainException(env, "GetMethodID 异步准备方法");
      break;
    }

    jstring jUri = env->NewStringUTF(uri.c_str());
    if (jUri == nullptr) {
      drainException(env, "NewStringUTF");
      break;
    }
    env->CallVoidMethod(player, setDataSource, jUri);
    env->DeleteLocalRef(jUri);
    if (drainException(env, "setDataSource")) {
      break;
    }

    // 异步准备不阻塞调用线程，网络源卡住也不会拖死工作线程。
    env->CallVoidMethod(player, prepareAsync);
    if (drainException(env, "prepareAsync")) {
      break;
    }

    mPlayer = env->NewGlobalRef(player);
    if (mPlayer == nullptr) {
      drainException(env, "NewGlobalRef");
      break;
    }
    ok = true;
  } while (false);

  if (!ok && player != nullptr && releaseMethod != nullptr) {
    env->CallVoidMethod(player, releaseMethod);
    drainException(env, "release 清理");
  }
  if (player != nullptr) {
    env->DeleteLocalRef(player);
  }
  if (playerClass != nullptr) {
    env->DeleteLocalRef(playerClass);
  }
  return ok;
}

bool AudioPlayer::isPrepared() const {
  // MediaPlayer 没有直接查询 prepared 状态的接口，
  // 时长可知就说明异步准备已经完成。
  return durationMs() > 0;
}

bool AudioPlayer::startPlayback() {
  if (mPlayer == nullptr || mJavaVm == nullptr) {
    return false;
  }
  EnvGuard guard(mJavaVm);
  if (!guard.ok()) {
    return false;
  }
  JNIEnv *env = guard.env();
  jclass cls = env->GetObjectClass(mPlayer);
  if (cls == nullptr) {
    return false;
  }
  bool ok = false;
  jmethodID startMethod = env->GetMethodID(cls, "start", "()V");
  if (startMethod != nullptr) {
    env->CallVoidMethod(mPlayer, startMethod);
    ok = !drainException(env, "start");
  }
  env->DeleteLocalRef(cls);
  return ok;
}

void AudioPlayer::pause() {
  if (mPlayer == nullptr || mJavaVm == nullptr) {
    return;
  }
  EnvGuard guard(mJavaVm);
  if (!guard.ok()) {
    return;
  }
  JNIEnv *env = guard.env();
  jclass cls = env->GetObjectClass(mPlayer);
  if (cls == nullptr) {
    return;
  }
  jmethodID pauseMethod = env->GetMethodID(cls, "pause", "()V");
  if (pauseMethod != nullptr) {
    env->CallVoidMethod(mPlayer, pauseMethod);
    drainException(env, "pause");
  }
  env->DeleteLocalRef(cls);
}

void AudioPlayer::resume() {
  if (mPlayer == nullptr || mJavaVm == nullptr) {
    return;
  }
  EnvGuard guard(mJavaVm);
  if (!guard.ok()) {
    return;
  }
  JNIEnv *env = guard.env();
  jclass cls = env->GetObjectClass(mPlayer);
  if (cls == nullptr) {
    return;
  }
  jmethodID startMethod = env->GetMethodID(cls, "start", "()V");
  if (startMethod != nullptr) {
    env->CallVoidMethod(mPlayer, startMethod);
    drainException(env, "resume");
  }
  env->DeleteLocalRef(cls);
}

void AudioPlayer::setVolume(float volume) {
  if (mPlayer == nullptr || mJavaVm == nullptr) {
    return;
  }
  if (volume < 0.0f) {
    volume = 0.0f;
  }
  if (volume > 1.0f) {
    volume = 1.0f;
  }
  EnvGuard guard(mJavaVm);
  if (!guard.ok()) {
    return;
  }
  JNIEnv *env = guard.env();
  jclass cls = env->GetObjectClass(mPlayer);
  if (cls == nullptr) {
    return;
  }
  jmethodID method = env->GetMethodID(cls, "setVolume", "(FF)V");
  if (method != nullptr) {
    env->CallVoidMethod(mPlayer, method, volume, volume);
    drainException(env, "setVolume");
  }
  env->DeleteLocalRef(cls);
}

void AudioPlayer::stop() {
  if (mPlayer == nullptr || mJavaVm == nullptr) {
    return;
  }

  EnvGuard guard(mJavaVm);
  if (!guard.ok()) {
    __android_log_print(ANDROID_LOG_ERROR, kTag, "停止失败: 线程附加失败，保留现状");
    return;
  }
  JNIEnv *env = guard.env();

  jclass cls = env->GetObjectClass(mPlayer);
  if (cls != nullptr) {
    jmethodID stopMethod = env->GetMethodID(cls, "stop", "()V");
    jmethodID releaseMethod = env->GetMethodID(cls, "release", "()V");
    if (stopMethod != nullptr) {
      env->CallVoidMethod(mPlayer, stopMethod);
      drainException(env, "stop");
    }
    if (releaseMethod != nullptr) {
      env->CallVoidMethod(mPlayer, releaseMethod);
      drainException(env, "release");
    }
    env->DeleteLocalRef(cls);
  }

  env->DeleteGlobalRef(mPlayer);
  mPlayer = nullptr;
}

void AudioPlayer::seekTo(long long positionMs) {
  if (mPlayer == nullptr || mJavaVm == nullptr || positionMs < 0) {
    return;
  }
  EnvGuard guard(mJavaVm);
  if (!guard.ok()) {
    return;
  }
  JNIEnv *env = guard.env();
  jclass cls = env->GetObjectClass(mPlayer);
  if (cls == nullptr) {
    return;
  }
  jmethodID seekMethod = env->GetMethodID(cls, "seekTo", "(I)V");
  if (seekMethod != nullptr) {
    env->CallVoidMethod(mPlayer, seekMethod, static_cast<jint>(positionMs));
    drainException(env, "seekTo");
  }
  env->DeleteLocalRef(cls);
}

bool AudioPlayer::isPlayingNow() const {
  if (mPlayer == nullptr || mJavaVm == nullptr) {
    return false;
  }
  EnvGuard guard(mJavaVm);
  if (!guard.ok()) {
    return false;
  }
  JNIEnv *env = guard.env();
  jclass cls = env->GetObjectClass(mPlayer);
  if (cls == nullptr) {
    return false;
  }
  bool playing = false;
  jmethodID method = env->GetMethodID(cls, "isPlaying", "()Z");
  if (method != nullptr) {
    playing = env->CallBooleanMethod(mPlayer, method) == JNI_TRUE;
    drainException(env, "isPlaying");
  }
  env->DeleteLocalRef(cls);
  return playing;
}

long long AudioPlayer::callLong(const char *method) const {
  if (mPlayer == nullptr || mJavaVm == nullptr || method == nullptr) {
    return -1;
  }
  EnvGuard guard(mJavaVm);
  if (!guard.ok()) {
    return -1;
  }
  JNIEnv *env = guard.env();
  jclass cls = env->GetObjectClass(mPlayer);
  if (cls == nullptr) {
    return -1;
  }
  long long value = -1;
  jmethodID fn = env->GetMethodID(cls, method, "()I");
  if (fn != nullptr) {
    value = static_cast<long long>(env->CallIntMethod(mPlayer, fn));
    drainException(env, method);
  }
  env->DeleteLocalRef(cls);
  return value;
}

long long AudioPlayer::positionMs() const { return callLong("getCurrentPosition"); }

long long AudioPlayer::durationMs() const { return callLong("getDuration"); }

} // namespace musicmod