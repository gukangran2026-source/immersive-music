#pragma once

#include <jni.h>

#include <chrono>
#include <string>
#include <vector>

#include "AudioPlayer.hpp"
#include "MusicTypes.hpp"

namespace musicmod {

// 播放队列与状态机。上层只需要给它一个音源和曲目列表，
// 它负责解析地址、串起播放、自动接续下一首。
class AudioEngine {
public:
  explicit AudioEngine(JavaVM *vm) : mPlayer(vm) {}

  void setProvider(IMusicProvider *provider) { mProvider = provider; }

  bool loadQueue();
  // 直接替换播放队列，不触发播放；用于切菜单时只换列表。
  void setQueue(const std::vector<Track> &tracks) {
    mTracks = tracks;
    mIndex = 0;
    mHasPendingQueue = false;
  }
  // 切歌单但保持当前歌曲继续播放；下一首从新歌单接续。
  void setQueueKeepCurrent(const std::vector<Track> &tracks,
                           const std::string &currentId);
  // 追加曲目并去重，返回追加前的队列长度。
  std::size_t appendQueue(const std::vector<Track> &tracks);
  bool takeNeedMore() {
    const bool value = mNeedMore;
    mNeedMore = false;
    return value;
  }
  const std::vector<Track> &tracks() const { return mTracks; }
  bool playIndex(std::size_t index);
  // 把当前曲目准备好但保持暂停，供“记住暂停状态”启动时使用。
  bool playIndexPaused(std::size_t index);
  void setVolume(float volume);
  void togglePause();
  void next();
  void previous();
  void stop();
  // 跳到指定毫秒位置。
  void seek(long long positionMs);

  long long positionMs() const;
  long long durationMs() const;

  // 供界面读取队列信息。
  std::size_t trackCount() const { return mTracks.size(); }
  std::size_t currentIndex() const { return mIndex; }
  const Track *trackAt(std::size_t index) const {
    if (index >= mTracks.size()) {
      return nullptr;
    }
    return &mTracks[index];
  }

  // 由工作线程周期调用，负责检测播放结束并自动接续。
  void tick();

  // 播放进度持久化：记住上次是哪首、播到第几毫秒。
  void setStatePath(const std::string &path) { mStatePath = path; }
  void saveProgress();
  bool loadProgress(std::size_t &index, long long &positionMs,
                    bool *paused = nullptr) const;
  // 续播时先登记目标位置，等真正开始播放后再跳转。
  void prepareResume(long long positionMs) { mPendingSeekMs = positionMs; }

  bool playing() const { return mPlaying; }
  bool paused() const { return mPaused; }
  std::string statusLine() const;
  // 最近一次播放失败原因，读取后清空。
  std::string takeError() {
    std::string value = mError;
    mError.clear();
    return value;
  }
  // 形如 "播放中 01:23 / 03:45"，用于界面第三行。
  std::string progressText() const;
  const Track *currentTrack() const;

private:
  std::string resolveUri(const Track &track);
  // 首曲常在世界刚加载时因服务或网络未就绪而失败，
  // 直接跳过会表现为“第一首永远播不了”。改为同曲延时重试。
  void scheduleRetry();

  IMusicProvider *mProvider{nullptr};
  AudioPlayer mPlayer;
  std::vector<Track> mTracks;
  std::vector<Track> mPendingTracks;
  bool mHasPendingQueue{false};
  bool mNeedMore{false};
  std::size_t mIndex{0};
  bool mPlaying{false};
  bool mPaused{false};
  bool mResumePaused{false};
  float mVolume{0.7f};
  // 已发起异步准备但还没真正开始播放。
  bool mPendingStart{false};
  // 同一曲目的剩余重试次数。
  int mRetryLeft{2};
  bool mPendingRetry{false};
  long long mRetryAtMs{0};
  std::string mStatePath;
  std::string mError;
  long long mPendingSeekMs{0};
  long long mLastSaveMs{0};
  std::chrono::steady_clock::time_point mPrepareDeadline;
  int mSilentTicks{0};
};

} // namespace musicmod