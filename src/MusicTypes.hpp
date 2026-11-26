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
  // 歌单内的数字 fileid。删除收藏必须用它——实测传 hash 无效。
  // 搜索结果里没有这个字段，为空时删除前会去歌单里查一次。
  std::string fileId;
};

// 搜索结果的三种类型。
enum class SearchKind { Song = 0, Album = 1, Artist = 2 };

// 歌手摘要。搜索结果里的歌手条目，也用于歌手主页页头。
struct ArtistInfo {
  std::string id;       // author_id
  std::string name;
  std::string avatar;   // 头像地址，可能为空
  int songCount{0};
  int albumCount{0};
};

// 专辑摘要。搜索结果里的专辑条目。
struct AlbumInfo {
  std::string id;       // album_id
  std::string name;
  std::string artist;
  std::string cover;
  int songCount{0};
  // 从专辑进歌曲列表时要用到的歌手 id（可能为空）。
  std::string artistId;
};

// 酷狗云歌单的一条摘要。收藏页先列这个，点进去再看歌曲。
struct PlaylistInfo {
  std::string id;          // list_create_listid
  std::string gid;         // list_create_gid（部分接口需要）
  std::string name;        // 歌单名
  std::string cover;       // 封面地址，可能为空
  int count{0};            // 曲目数 m_count
  int type{0};             // 0=自建（含「我喜欢」） 1=收藏
  // 「我喜欢」的标志：酷狗把默认收藏那个歌单的 is_pri 置 1。
  bool isDefault{false};
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