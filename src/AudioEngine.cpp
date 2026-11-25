#include "AudioEngine.hpp"

#include <fstream>

#include <android/log.h>

#include <cstdio>

namespace musicmod {
namespace {

constexpr const char *kTag = "MusicMod";

} // namespace

bool AudioEngine::loadQueue() {
  if (mProvider == nullptr) {
    __android_log_print(ANDROID_LOG_ERROR, kTag, "队列加载失败: 未设置音源");
    return false;
  }
  if (!mProvider->available()) {
    __android_log_print(ANDROID_LOG_WARN, kTag, "队列加载失败: 音源不可用");
    return false;
  }
  mTracks = mProvider->tracks();
  mIndex = 0;
  mSilentTicks = 0;
  __android_log_print(ANDROID_LOG_INFO, kTag, "队列就绪, 音源=%s 曲目数=%zu",
                      mProvider->name().c_str(), mTracks.size());
  return !mTracks.empty();
}

std::string AudioEngine::resolveUri(const Track &track) {
  if (mProvider == nullptr) {
    return track.uri;
  }
  // 平台地址通常带时效，每次播放前重新解析。
  const std::string uri = mProvider->streamUrl(track);
  return uri.empty() ? track.uri : uri;
}

void AudioEngine::setQueueKeepCurrent(const std::vector<Track> &tracks,
                                      const std::string &currentId) {
  if (!currentId.empty()) {
    for (std::size_t i = 0; i < tracks.size(); ++i) {
      if (tracks[i].id == currentId) {
        mTracks = tracks;
        mIndex = i;
        mPendingTracks.clear();
        mHasPendingQueue = false;
        return;
      }
    }
  }
  if (mPlaying || mPaused) {
    // 当前歌不在新歌单里：先记下新队列，等播完或点下一首再切。
    mPendingTracks = tracks;
    mHasPendingQueue = true;
    return;
  }
  mTracks = tracks;
  mIndex = 0;
  mPendingTracks.clear();
  mHasPendingQueue = false;
}

std::size_t AudioEngine::appendQueue(const std::vector<Track> &tracks) {
  const std::size_t oldSize = mTracks.size();
  for (const auto &track : tracks) {
    bool exists = false;
    for (const auto &current : mTracks) {
      if (current.id == track.id) {
        exists = true;
        break;
      }
    }
    if (!exists) {
      mTracks.push_back(track);
    }
  }
  return oldSize;
}

bool AudioEngine::playIndex(std::size_t index) {
  if (mTracks.empty()) {
    return false;
  }
  mIndex = index % mTracks.size();
  // 换曲一律作废上一首的重试计划。
  mPendingRetry = false;
  mRetryLeft = 2;
  const Track &track = mTracks[mIndex];
  const std::string uri = resolveUri(track);
  if (uri.empty()) {
    __android_log_print(ANDROID_LOG_ERROR, kTag, "无可用播放地址: %s",
                        track.title.c_str());
    mPlaying = false;
    mPaused = false;
    mPendingStart = false;
    mPendingRetry = false;
    mRetryLeft = 0;
    mError = "当前曲目没有播放版权";
    return true;
  }
  mError.clear();

  __android_log_print(ANDROID_LOG_INFO, kTag, "准备播放 [%zu/%zu] %s - %s",
                      mIndex + 1, mTracks.size(), track.title.c_str(),
                      track.artist.c_str());
  __android_log_print(ANDROID_LOG_INFO, kTag, "音源地址: %s", uri.c_str());

  const bool ok = mPlayer.beginPrepare(uri);
  if (!ok) {
    __android_log_print(ANDROID_LOG_ERROR, kTag, "发起播放失败: %s",
                        track.title.c_str());
    mPlaying = false;
    mPaused = false;
    mPendingStart = false;
    scheduleRetry();
    return true;
  }

  // 异步准备：不在这里等，交给 tick 轮询。
  mPendingStart = true;
  mPlaying = false;
  mPaused = false;
  mSilentTicks = 0;
  mPrepareDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  return true;
}

void AudioEngine::togglePause() {
  if (!mPlaying) {
    return;
  }
  if (mPaused) {
    mPlayer.resume();
    mPaused = false;
    __android_log_print(ANDROID_LOG_INFO, kTag, "继续播放");
  } else {
    mPlayer.pause();
    mPaused = true;
    __android_log_print(ANDROID_LOG_INFO, kTag, "暂停");
  }
  saveProgress();
}

void AudioEngine::setVolume(float volume) { mPlayer.setVolume(volume); }

bool AudioEngine::playIndexPaused(std::size_t index) {
  if (!playIndex(index)) {
    return false;
  }
  mResumePaused = true;
  return true;
}

void AudioEngine::next() {
  if (mHasPendingQueue) {
    mTracks = std::move(mPendingTracks);
    mPendingTracks.clear();
    mHasPendingQueue = false;
    mIndex = 0;
    if (!mTracks.empty()) {
      playIndex(0);
    }
    return;
  }
  if (mTracks.empty()) {
    return;
  }
  if (mIndex + 1 >= mTracks.size()) {
    __android_log_print(ANDROID_LOG_INFO, kTag, "歌单到尾部, 等待续接");
    mNeedMore = true;
    stop();
    return;
  }
  playIndex(mIndex + 1);
}

void AudioEngine::previous() {
  if (mTracks.empty()) {
    return;
  }
  playIndex((mIndex + mTracks.size() - 1) % mTracks.size());
}

void AudioEngine::stop() {
  mPlayer.stop();
  mPlaying = false;
  mPaused = false;
  mPendingStart = false;
  mSilentTicks = 0;
}

void AudioEngine::seek(long long positionMs) {
  if (mPlaying || mPaused) {
    mPlayer.seekTo(positionMs);
    __android_log_print(ANDROID_LOG_INFO, kTag, "跳转到 %lld 毫秒", positionMs);
  }
}
void AudioEngine::saveProgress() {
  if (mStatePath.empty() || mTracks.empty()) {
    return;
  }
  std::ofstream output(mStatePath);
  if (!output.is_open()) {
    return;
  }
  output << "index=" << mIndex << "\n";
  output << "position=" << positionMs() << "\n";
  output << "paused=" << (mPaused ? 1 : 0) << "\n";
}

bool AudioEngine::loadProgress(std::size_t &index, long long &positionMs,
                               bool *paused) const {
  if (mStatePath.empty()) {
    return false;
  }
  std::ifstream input(mStatePath);
  if (!input.is_open()) {
    return false;
  }
  std::string line;
  bool hasIndex = false;
  while (std::getline(input, line)) {
    const std::size_t eq = line.find('=');
    if (eq == std::string::npos) {
      continue;
    }
    const std::string key = line.substr(0, eq);
    try {
      if (key == "index") {
        index = static_cast<std::size_t>(std::stoul(line.substr(eq + 1)));
        hasIndex = true;
      } else if (key == "position") {
        positionMs = std::stoll(line.substr(eq + 1));
      } else if (key == "paused" && paused != nullptr) {
        *paused = std::stoi(line.substr(eq + 1)) != 0;
      }
    } catch (const std::exception &) {
    }
  }
  return hasIndex;
}

void AudioEngine::scheduleRetry() {
  if (mRetryLeft > 0) {
    --mRetryLeft;
    mPendingRetry = true;
    mRetryAtMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now().time_since_epoch())
                     .count() +
                 900;
    __android_log_print(ANDROID_LOG_WARN, kTag,
                        "本曲未就绪, 900 毫秒后重试, 剩余 %d 次", mRetryLeft);
    return;
  }
  __android_log_print(ANDROID_LOG_ERROR, kTag, "重试用尽, 跳过该曲目");
  mRetryLeft = 2;
  mPendingRetry = false;
  next();
}

long long AudioEngine::positionMs() const { return mPlayer.positionMs(); }

long long AudioEngine::durationMs() const { return mPlayer.durationMs(); }

void AudioEngine::tick() {
  const long long tickNow =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count();
  // 每 15 秒落一次进度。外部存储在安卓上走转发层，写太勤会拖时间。
  if (mPlaying && tickNow - mLastSaveMs > 15000) {
    mLastSaveMs = tickNow;
    saveProgress();
  }
  if (mPendingRetry) {
    const long long nowMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    if (nowMs >= mRetryAtMs) {
      mPendingRetry = false;
      playIndex(mIndex);
    }
    return;
  }
  if (mPendingStart) {
    if (mPlayer.isPrepared()) {
      if (mResumePaused) {
        mPlayer.startPlayback();
        if (mPendingSeekMs > 0) {
          mPlayer.seekTo(mPendingSeekMs);
        }
        mPlayer.pause();
        mPlayer.setVolume(mVolume);
        mPendingStart = false;
        mPlaying = true;
        mPaused = true;
        mResumePaused = false;
        mPendingSeekMs = 0;
        saveProgress();
        __android_log_print(ANDROID_LOG_INFO, kTag, "已恢复暂停状态: %s",
                            mTracks[mIndex].title.c_str());
      } else if (mPlayer.startPlayback()) {
        mPendingStart = false;
        mPlaying = true;
        mPaused = false;
        mRetryLeft = 2;
        mSilentTicks = 0;
        mPlayer.setVolume(mVolume);
        if (mPendingSeekMs > 0) {
          mPlayer.seekTo(mPendingSeekMs);
          __android_log_print(ANDROID_LOG_INFO, kTag, "续播跳转到 %lld 毫秒",
                              mPendingSeekMs);
          mPendingSeekMs = 0;
        }
        __android_log_print(ANDROID_LOG_INFO, kTag, "开始播放: %s",
                            mTracks[mIndex].title.c_str());
      } else {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "启动播放失败");
        mPendingStart = false;
        scheduleRetry();
      }
    } else if (std::chrono::steady_clock::now() > mPrepareDeadline) {
      __android_log_print(ANDROID_LOG_ERROR, kTag, "准备超时");
      mPendingStart = false;
      mPlayer.stop();
      scheduleRetry();
    }
    return;
  }

  if (!mPlaying || mPaused) {
    return;
  }

  if (mPlayer.isPlayingNow()) {
    mSilentTicks = 0;
    return;
  }

  // isPlaying 在刚启动的一瞬间可能为 false，连续两次确认才算播放结束。
  if (++mSilentTicks < 2) {
    return;
  }

  mSilentTicks = 0;
  __android_log_print(ANDROID_LOG_INFO, kTag, "当前曲目结束, 自动接续下一首");
  next();
}

std::string AudioEngine::statusLine() const {
  if (mTracks.empty()) {
    return "未播放";
  }
  const Track &track = mTracks[mIndex];
  if (mPendingStart) {
    return track.title + " - " + track.artist + " [缓冲中]";
  }
  if (!mPlaying) {
    return "未播放";
  }
  std::string line = track.title + " - " + track.artist;
  if (mPaused) {
    line += " [已暂停]";
  }
  return line;
}

std::string AudioEngine::progressText() const {
  if (mTracks.empty()) {
    return "未播放";
  }
  if (mPendingStart) {
    return "缓冲中";
  }
  if (!mPlaying) {
    return "未播放";
  }

  const auto format = [](long long ms) {
    if (ms < 0) {
      ms = 0;
    }
    const long long totalSeconds = ms / 1000;
    const long long minutes = totalSeconds / 60;
    const long long seconds = totalSeconds % 60;
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%02lld:%02lld", minutes, seconds);
    return std::string(buffer);
  };

  const long long position = mPlayer.positionMs();
  const long long duration = mPlayer.durationMs();
  const std::string state = mPaused ? "已暂停" : "播放中";
  return state + " " + format(position) + " / " + format(duration);
}

const Track *AudioEngine::currentTrack() const {
  if (mTracks.empty()) {
    return nullptr;
  }
  return &mTracks[mIndex];
}

} // namespace musicmod