#include "PlaylistProvider.hpp"

#include <android/log.h>

#include <fstream>

#include <nlohmann/json.hpp>

namespace musicmod {
namespace {

constexpr const char *kTag = "MusicMod";

} // namespace

bool PlaylistProvider::available() {
  if (mLoaded) {
    return !mTracks.empty();
  }
  mLoaded = true;

  std::ifstream file(mConfigPath);
  if (!file.is_open()) {
    mLastError = "歌单文件不存在: " + mConfigPath;
    __android_log_print(ANDROID_LOG_WARN, kTag, "%s", mLastError.c_str());
    return false;
  }

  try {
    nlohmann::json root;
    file >> root;

    const auto &list = root.contains("tracks") ? root["tracks"]
                                              : nlohmann::json::array();
    if (!list.is_array()) {
      mLastError = "tracks 字段不是数组";
      return false;
    }

    for (const auto &item : list) {
      if (!item.is_object()) {
        continue;
      }
      Track track;
      track.id = item.value("id", "");
      track.title = item.value("title", "未知曲目");
      track.artist = item.value("artist", "未知");
      track.uri = item.value("uri", "");
      track.lyricPath = item.value("lyric", "");
      track.coverPath = item.value("cover", "");
      if (track.uri.empty()) {
        continue;
      }
      mTracks.push_back(std::move(track));
    }
  } catch (const std::exception &ex) {
    mLastError = std::string("歌单解析失败: ") + ex.what();
    __android_log_print(ANDROID_LOG_ERROR, kTag, "%s", mLastError.c_str());
    return false;
  }

  __android_log_print(ANDROID_LOG_INFO, kTag, "歌单加载完成, 曲目数=%zu",
                      mTracks.size());
  return !mTracks.empty();
}

std::vector<Track> PlaylistProvider::tracks() {
  if (!mLoaded) {
    available();
  }
  return mTracks;
}

std::string PlaylistProvider::lyric(const Track &track) {
  if (track.lyricPath.empty()) {
    return {};
  }
  std::ifstream file(track.lyricPath);
  if (!file.is_open()) {
    return {};
  }
  std::string content((std::istreambuf_iterator<char>(file)),
                      std::istreambuf_iterator<char>());
  return content;
}

} // namespace musicmod