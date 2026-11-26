#pragma once

#include <string>
#include <vector>

#include "MusicTypes.hpp"

namespace musicmod {

// 酷狗概念版音源。接口实现来自 MD3Music 的本地服务组件
// libkugou_server.so：在进程内起一个只监听回环的 HTTP 服务，
// 本类只通过 127.0.0.1 调它，不涉及任何签名逻辑。
//
// 该组件为 AGPL-3.0 授权，仅本地自用；对外分发时必须一并开源。
class KugouProvider : public IMusicProvider {
public:
  ~KugouProvider() override;

  bool init(const std::string &libPath, const std::string &dataDir);
  void shutdown();

  std::string name() const override { return "酷狗概念版"; }
  bool available() override;
  std::vector<Track> tracks() override;
  std::string streamUrl(const Track &track) override;
  std::string lyric(const Track &track) override;

  // 按关键词搜索歌曲，成功后替换内部曲目列表。
  bool search(const std::string &keywords);
  // ---- 多分类搜索 ----
  // 按类型搜索。Song 走 /search，Album 走 /search/album，
  // Artist 走 /search/artist（歌手）。结果分别放 mAlbums / mArtists。
  bool searchByKind(SearchKind kind, const std::string &keywords);
  const std::vector<AlbumInfo> &albums() const { return mAlbums; }
  const std::vector<ArtistInfo> &artists() const { return mArtists; }
  // ---- 歌手主页 ----
  // 拉歌手资料（名字、头像、作品数）。
  bool loadArtistDetail(const std::string &artistId);
  // 拉歌手单曲。sort: "hot" 按热度，其余按时间。
  bool loadArtistSongs(const std::string &artistId,
                       const std::string &sort = "hot");
  // 拉歌手专辑。
  bool loadArtistAlbums(const std::string &artistId,
                        const std::string &sort = "hot");
  // 最近一次拉到的歌手资料。
  const ArtistInfo &currentArtist() const { return mCurrentArtist; }
  // 拉某个专辑的歌曲。
  bool loadAlbumSongs(const std::string &albumId);
  // 加载「我喜欢」歌单，替换内部曲目列表。
  bool loadFavorites();
  // 每日自动推荐：variant=0 探索，variant=1 小众。
  bool loadDailyRecommend(int variant);
  // 按一首歌继续生成相似推荐，用于歌单播完后的自动续接。
  bool loadSimilar(const Track &seed);
  // 下载曲目封面到 path，成功返回 true。
  bool fetchCover(const Track &track, const std::string &path);
  // ---- 收藏（收进酷狗「我喜欢」）----
  // 把一首歌加进「我喜欢」。成功返回 true，失败原因写 lastError。
  bool addToFavorites(const Track &track);
  // 从「我喜欢」移除。
  bool removeFromFavorites(const Track &track);
  // 查询「我喜欢」里是否已有这首歌。未知时返回 false。
  bool isFavorite(const Track &track);
  // 按 hash 在收藏歌单里查数字 fileid。删除收藏必须用它。
  bool findFavoriteFileId(const std::string &hash, std::string &fileId);
  // 取「我喜欢」的 listid，内部有缓存。
  std::string favoritesListId();
  // ---- 歌单列表（收藏页第一层）----
  // 拉账号下所有歌单。成功返回 true，结果放 mPlaylists。
  bool loadPlaylists();
  // 当前缓存里的歌单列表。
  const std::vector<PlaylistInfo> &playlists() const { return mPlaylists; }
  // 按 listid 拉某个歌单的全部歌曲，成功返回 true，结果放 tracks()。
  bool loadPlaylistTracks(const std::string &listId);
  // 记住玩家选中的歌单，之后 addToFavorites 就往这个歌单里加。
  void setActivePlaylist(const std::string &listId) { mFavListId = listId; }
  const std::string &activePlaylistId() const { return mFavListId; }

  // 拉账号资料：昵称与头像图片地址。成功返回 true。
  // 头像字段名没有公开文档，按常见候选逐个试探并打日志。
  bool fetchUserInfo(std::string &nickname, std::string &avatarUrl);
  // 按完整 URL 下载头像到 path，成功返回 true。
  bool fetchAvatar(const std::string &url, const std::string &path);

  // ---- 登录（扫码 / 手机号）----
  // 直接向酷狗官方接口申请扫码 key 与官方二维码图。
  // image 为 data:image/png;base64,...，与 key 一次返回。
  bool loginQrStart(std::string &key, std::string &image);
  // 轮询扫码状态：1 等待扫描 2 已扫描待确认 4 成功 0 过期 -1 请求失败。
  int loginQrCheck(const std::string &key, std::string &token,
                   std::string &userId);

  // 上报真实播放事件，用于听歌领会员。
  void reportListen(const Track &track, const std::string &event,
                    long long durationMs);
  // 领取一天会员，结果写入 message。
  bool claimVip(std::string &message);

  // 手机号登录：先发验证码，再提交登录码。
  bool loginSmsSend(const std::string &mobile);
  bool loginSmsSubmit(const std::string &mobile, const std::string &code);

  // 登录态持久化到 data 目录的 session.json。
  bool loadSession();
  bool saveSession();
  bool loggedIn() const { return !mToken.empty(); }
  const std::string &token() const { return mToken; }
  const std::string &userId() const { return mUserId; }
  // 最近一次登录相关失败的原因，用于界面提示。
  const std::string &lastError() const { return mLastError; }
  // 退出登录：清空令牌并删除本地会话文件。
  void logout();

  bool running() const { return mPort > 0; }
  // 本地服务实际监听的端口，供上层把状态落盘，便于日志滚动后复盘。
  int port() const { return mPort; }

private:
  void *mLib{nullptr};
  int mPort{0};
  std::string mDataDir;
  std::vector<Track> mTracks;
  std::string mToken;
  std::string mUserId;
  std::string mVipToken;
  std::string mVipType;
  std::string mLastError;
  // 当前选中的收藏歌单 listid。空表示还没选，用默认（我喜欢）。
  std::string mFavListId;
  // 收藏目标歌单名（日志用）。
  std::string mFavListName;
  // 歌单列表缓存。
  std::vector<PlaylistInfo> mPlaylists;
  // 多分类搜索缓存。
  std::vector<AlbumInfo> mAlbums;
  std::vector<ArtistInfo> mArtists;
  // 当前打开的歌手资料。
  ArtistInfo mCurrentArtist;
};

} // namespace musicmod