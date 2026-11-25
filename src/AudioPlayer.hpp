#pragma once

#include <jni.h>

#include <string>

namespace musicmod {

// 通过 JNI 调用系统 android.media.MediaPlayer。
// 既支持本地文件路径，也支持 http/https 直链，MediaPlayer 本身不区分两者。
class AudioPlayer {
public:
  explicit AudioPlayer(JavaVM *vm) : mJavaVm(vm) {}
  ~AudioPlayer();

  AudioPlayer(const AudioPlayer &) = delete;
  AudioPlayer &operator=(const AudioPlayer &) = delete;

  // uri 可以是本地绝对路径，也可以是 http(s) 地址。
  bool play(const std::string &uri);
  // 异步准备：setDataSource + prepareAsync，不阻塞调用线程。
  bool beginPrepare(const std::string &uri);
  // 准备完成后调用 start。
  bool startPlayback();
  // 异步准备是否已完成。用 duration 是否可知来判断。
  bool isPrepared() const;
  void pause();
  void resume();
  void stop();
  // 0.0 到 1.0，只改这一个播放器实例的音量。
  void setVolume(float volume);
  void seekTo(long long positionMs);

  bool hasTrack() const { return mPlayer != nullptr; }
  bool isPlayingNow() const;
  long long positionMs() const;
  long long durationMs() const;

private:
  long long callLong(const char *method) const;

  JavaVM *mJavaVm{};
  jobject mPlayer{nullptr}; // 全局引用
};

} // namespace musicmod