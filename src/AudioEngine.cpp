#include "AudioEngine.hpp"

#include <fstream>
#include <algorithm>
#include <chrono>

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
  // 空列表直接忽略。
  // 切到一个还没加载内容的 tab（例如没搜过的搜索页）时，
  // 上层会拿空列表进来；如果照单全收，会把 pending 队列设成空，
  // 等当前歌播完 next() 一应用，队列就空了，播放器随即空转。
  if (tracks.empty()) {
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "忽略空列表的队列替换 (当前队列=%zu)", mTracks.size());
    return;
  }
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

bool AudioEngine::insertNext(const Track &track) {
  if (track.id.empty()) {
    return false;
  }
  if (mTracks.empty()) {
    mTracks.push_back(track);
    mIndex = 0;
    return true;
  }
  // 插到当前曲目之后。mIndex 保持不变，正在播的这首歌不会被中断。
  std::size_t pos = mIndex + 1;
  if (pos > mTracks.size()) {
    pos = mTracks.size();
  }
  mTracks.insert(mTracks.begin() + static_cast<std::ptrdiff_t>(pos), track);
  return true;
}

bool AudioEngine::playIndex(std::size_t index) {
  // 换曲时清掉卡死计数，避免上一首的累计影响这一首。
  mStallTicks = 0;
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

void AudioEngine::setPlayMode(PlayMode mode) {
  if (mPlayMode == mode) {
    return;
  }
  mPlayMode = mode;
  __android_log_print(ANDROID_LOG_INFO, kTag, "播放模式切换为 %s",
                      playModeName());
  // 切到随机时重排一次，保证从当前位置重新开始不重复。
  if (mode == PlayMode::Shuffle) {
    resetShuffleHistory();
  }
  saveProgress();
}

void AudioEngine::cyclePlayMode() {
  switch (mPlayMode) {
    case PlayMode::Sequential:
      setPlayMode(PlayMode::Shuffle);
      break;
    case PlayMode::Shuffle:
      setPlayMode(PlayMode::RepeatOne);
      break;
    case PlayMode::RepeatOne:
      setPlayMode(PlayMode::Sequential);
      break;
  }
}

const char *AudioEngine::playModeName() const {
  switch (mPlayMode) {
    case PlayMode::Shuffle:
      return "随机";
    case PlayMode::RepeatOne:
      return "单曲循环";
    case PlayMode::Sequential:
    default:
      return "顺序";
  }
}

void AudioEngine::resetShuffleHistory() {
  mShuffleOrder.clear();
  mShufflePos = 0;
  if (mTracks.empty()) {
    return;
  }
  // 生成一个 0..n-1 的排列。用固定的线性同余发生器打乱，
  // 不引 <random>：产物要极小，且这里对随机质量要求不高。
  mShuffleOrder.resize(mTracks.size());
  for (std::size_t i = 0; i < mShuffleOrder.size(); ++i) {
    mShuffleOrder[i] = i;
  }
  unsigned int seed = static_cast<unsigned int>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  for (std::size_t i = mShuffleOrder.size(); i > 1; --i) {
    seed = seed * 1103515245u + 12345u;
    const std::size_t j = (seed >> 8) % i;
    std::swap(mShuffleOrder[i - 1], mShuffleOrder[j]);
  }
  // 起点对齐到当前曲目，避免「切到随机立刻跳到别处」。
  for (std::size_t i = 0; i < mShuffleOrder.size(); ++i) {
    if (mShuffleOrder[i] == mIndex) {
      std::swap(mShuffleOrder[0], mShuffleOrder[i]);
      break;
    }
  }
  mShufflePos = 0;
}

void AudioEngine::advanceAuto() {
  if (mTracks.empty()) {
    mPlaying = false;
    return;
  }
  // 单曲循环：自动接续时回到本曲。
  if (mPlayMode == PlayMode::RepeatOne && !mTracks.empty() &&
      mIndex < mTracks.size()) {
    playIndex(mIndex);
    return;
  }
  next();
}

void AudioEngine::next() {
  if (mHasPendingQueue) {
    mHasPendingQueue = false;
    if (mPendingTracks.empty()) {
      // pending 是空的（来自某次空列表切换）：直接丢弃，保留现有队列，
      // 继续往下走正常的「下一首」逻辑。
      // 旧实现会先用空列表覆盖 mTracks，导致队列清空且 mPlaying 悬空。
      __android_log_print(ANDROID_LOG_INFO, kTag,
                          "丢弃空的待切队列, 继续正常接续");
    } else {
      mTracks = std::move(mPendingTracks);
      mPendingTracks.clear();
      mIndex = 0;
      playIndex(0);
      return;
    }
  }
  if (mTracks.empty()) {
    // 空队列时把播放态也清掉，避免上层以为还在播。
    mPlaying = false;
    mPaused = false;
    return;
  }
  // 随机：按预生成的顺序表走。
  if (mPlayMode == PlayMode::Shuffle) {
    if (mShuffleOrder.size() != mTracks.size()) {
      resetShuffleHistory();
    }
    if (mShufflePos + 1 < mShuffleOrder.size()) {
      ++mShufflePos;
      playIndex(mShuffleOrder[mShufflePos]);
      return;
    }
    // 一轮播完：重新洗牌，进入下一轮。
    __android_log_print(ANDROID_LOG_INFO, kTag, "随机一轮播完, 重新洗牌");
    resetShuffleHistory();
    if (!mShuffleOrder.empty()) {
      mShufflePos = 0;
      playIndex(mShuffleOrder[0]);
    }
    return;
  }
  if (mIndex + 1 >= mTracks.size()) {
    // 单曲循环下自动接续不会走到这里；走到这里说明是手动点下曲。
    if (mPlayMode == PlayMode::RepeatOne) {
      playIndex(0);
      return;
    }
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
  // 随机模式下按顺序表回退一站，和「下一首」对称。
  if (mPlayMode == PlayMode::Shuffle) {
    if (mShuffleOrder.size() != mTracks.size()) {
      resetShuffleHistory();
    }
    if (mShufflePos > 0) {
      --mShufflePos;
      playIndex(mShuffleOrder[mShufflePos]);
      return;
    }
    // 已经在表头：回到表尾。
    if (!mShuffleOrder.empty()) {
      mShufflePos = mShuffleOrder.size() - 1;
      playIndex(mShuffleOrder[mShufflePos]);
      return;
    }
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
  // 播放模式也要记住：玩家选了随机，重进游戏不该又回到顺序。
  output << "playmode=" << static_cast<int>(mPlayMode) << "\n";
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
      } else if (key == "playmode") {
        // 这个方法是 const，但模式是纯内存状态，用 const_cast 写回。
        // 换别的做法要改一整套接口，收益不值。
        const int mode = std::stoi(line.substr(eq + 1));
        if (mode >= 0 && mode <= 2) {
          const_cast<AudioEngine *>(this)->mPlayMode =
              static_cast<PlayMode>(mode);
        }
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

  // 队列空了就别再判「曲目结束」——playIndex 会因为空队列直接返回，
  // mPlaying 永远清不掉，于是每两拍就 advanceAuto 一次，
  // 表现为日志疯狂刷「当前曲目结束」而音乐早就没了。
  if (mTracks.empty()) {
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "队列为空, 停止空转 (mIndex=%zu)", mIndex);
    mPlaying = false;
    mPaused = false;
    mPendingStart = false;
    mSilentTicks = 0;
    return;
  }

  if (mPlayer.isPlayingNow()) {
    mSilentTicks = 0;
    return;
  }

  // isPlaying() 会间歇性报 false（缓冲、音频焦点抖动），
  // 实测音频明明还在写（AudioTrack 计时持续增长），但这里已经判成「结束」，
  // 结果每 6 秒就切一首。所以必须用播放位置复核：
  //   位置离末尾还远 → 只是卡了一下，等它恢复，不切歌。
  const long long pos = mPlayer.positionMs();
  const long long dur = mPlayer.durationMs();
  if (dur > 0 && pos + 1500 < dur) {
    // 还没到末尾。累计一个宽容计数，避免永久卡死：
    // 连续 40 拍（约 6.4 秒）都没有任何进展，才认为真的卡死并切歌。
    if (++mStallTicks < 40) {
      static long long lastStallLog = 0;
      const long long nowLog =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now().time_since_epoch())
              .count();
      if (nowLog - lastStallLog > 3000) {
        lastStallLog = nowLog;
        __android_log_print(
            ANDROID_LOG_INFO, kTag,
            "缓冲抖动, 不切歌 (位置=%lld/%lld 宽容计数=%d)",
            pos, dur, mStallTicks);
      }
      return;
    }
    __android_log_print(ANDROID_LOG_WARN, kTag,
                        "位置长时间不动, 判定卡死并切歌 (位置=%lld/%lld)", pos,
                        dur);
    mStallTicks = 0;
    mSilentTicks = 0;
    advanceAuto();
    return;
  }
  mStallTicks = 0;

  // 位置已到末尾（或时长未知）：按旧逻辑，连续两次确认才算结束。
  if (++mSilentTicks < 2) {
    return;
  }

  mSilentTicks = 0;
  __android_log_print(ANDROID_LOG_INFO, kTag,
                      "当前曲目结束, 自动接续下一首 (队列=%zu 当前=%zu 模式=%s "
                      "位置=%lld/%lld)",
                      mTracks.size(), mIndex, playModeName(), pos, dur);
  // 走 advanceAuto：单曲循环时回到本曲，其余转 next()。
  advanceAuto();
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