#pragma once

#include <string>
#include <vector>

#include "MusicTypes.hpp"

namespace musicmod {

// 从模组数据目录的 playlist.json 读取歌单。
// 这是框架的默认实现，用来验证"非本地写死"的播放链路：
// uri 允许填 http(s) 地址，因此网络音频走的是同一条路径。
class PlaylistProvider : public IMusicProvider {
public:
  explicit PlaylistProvider(std::string configPath)
      : mConfigPath(std::move(configPath)) {}

  std::string name() const override { return "本地歌单"; }

  bool available() override;

  std::vector<Track> tracks() override;

  std::string streamUrl(const Track &track) override { return track.uri; }

  std::string lyric(const Track &track) override;

  const std::string &lastError() const { return mLastError; }

private:
  std::string mConfigPath;
  std::string mLastError;
  std::vector<Track> mTracks;
  bool mLoaded{false};
};

} // namespace musicmod