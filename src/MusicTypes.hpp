#pragma once

#include <string>
#include <vector>

namespace musicmod {

// 一首曲目的最小描述。uri 可以是本地绝对路径，也可以是 http(s) 直链。
struct Track {
  std::string id;
  std::string title;
  std::string artist;
  std::string uri;
  std::string lyricPath;
  std::string coverPath;
  // 封面接口需要的专辑与音频编号。
  std::string albumId;
  std::string audioId;
  // 整首歌完整时长，毫秒；0 表示未知。
  long long durationMs{0};
};

// 音源接口。每个平台实现一份，上层只认这个接口。
// 具体平台的鉴权与协议实现刻意留空，由后续按需接入。
class IMusicProvider {
public:
  virtual ~IMusicProvider() = default;

  virtual std::string name() const = 0;

  // 当前音源是否可用。例如未登录、网络不可达时返回 false。
  virtual bool available() = 0;

  // 拉取当前可播放的曲目列表。
  virtual std::vector<Track> tracks() = 0;

  // 把曲目解析成真正可播放的 uri。平台返回的地址通常有时效，
  // 所以每次播放前都走一次，而不是缓存到 Track 里。
  virtual std::string streamUrl(const Track &track) { return track.uri; }

  // 取歌词文本，空表示没有。
  virtual std::string lyric(const Track &) { return {}; }
};

} // namespace musicmod