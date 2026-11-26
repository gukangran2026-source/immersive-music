#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "MusicTypes.hpp"

namespace musicmod {

class AudioEngine;

// 游戏内音乐面板，两种形态：
// Mini 是贴在角落的小条，Expanded 是占约四分之一屏的大面板。
// 图形全部用宿主提供的矩形与线段自己画，不依赖字体里的符号。
class HudUi {
public:
  // 真实屏幕像素：把系统触摸坐标换算到宿主绘制面坐标系。
  void setRealScreen(float w, float h);
  ~HudUi();
  void setEngine(AudioEngine *engine);
  void setSearchHandler(std::function<void(const std::string &)> handler);
  // 收藏切换回调。参数是当前曲目的 hash；由 main 去调酷狗接口。
  // 走回调而不是在 HudUi 里直接调 KugouProvider：
  // HudUi 只认引擎，不认具体音源，这条边界不能破。
  void setFavoriteToggleHandler(std::function<void(const std::string &)> handler);
  // 收藏结果回填：成功与否、是否已收藏、提示文字。
  void setFavoriteState(bool favorited, const std::string &trackId);
  // 取出一次「查询收藏状态」请求，返回要查的 hash，空表示没有请求。
  std::string takeFavoriteQuery();
  // 取出一次「重新拉收藏列表」请求。非 0 表示要拉。
  int takeFavoriteListRequest() { return mFavoriteListRequest.exchange(0); }
  // 填入收藏页第一层的歌单列表（由 main 调用）。
  void setFavoritePlaylists(const std::vector<PlaylistInfo> &list);
  // 取出一次「打开某个歌单」的请求，返回 listid，空表示没有。
  std::string takeOpenPlaylistRequest();
  // 进入某个歌单的第二层（main 拉到曲目后调用，同时把曲目塞进列表）。
  void enterFavoritePlaylist(const std::string &id, const std::string &name);
  // 退回歌单列表（第一层）。
  void exitFavoritePlaylist();
  // 已持锁版本：调用方必须已持有 mMutex（触摸路径专用）。
  void exitFavoritePlaylistLocked();
  // ---- 搜索分类 ----
  void setSearchAlbums(const std::vector<AlbumInfo> &list);
  void setSearchArtists(const std::vector<ArtistInfo> &list);
  int searchKind() const { return mSearchKind; }
  // ---- 歌手主页 ----
  // 进入歌手页（main 拉到资料后调用，同时把歌曲塞进列表）。
  void enterArtist(const ArtistInfo &artist);
  // 退出歌手页。
  void exitArtist();
  // 已持锁版本：调用方必须已持有 mMutex（触摸路径专用）。
  void exitArtistLocked();
  bool inArtistView() const { return mArtistView != 0; }
  void setArtistAlbums(const std::vector<AlbumInfo> &list);
  // 取出一次歌手页加载请求：1=歌曲 2=专辑，0=无。
  int takeArtistLoadRequest() { return mArtistLoadRequest.exchange(0); }
  // 取出一次「打开专辑」请求。
  std::string takeOpenAlbumRequest();
  // 待加载的歌手 id（不消费，只读）。
  const std::string &pendingArtistId() const { return mPendingArtistId; }
  // 是否还需要拉歌手资料：名字为空或头像为空时算需要。
  bool needArtistDetail() const {
    return mArtistInfo.name.empty() || mArtistInfo.avatar.empty();
  }
  // 资料拉回来后回填（只更新名字/头像/计数，不动 id）。
  void updateArtistInfo(const ArtistInfo &info);
  bool artistSortByTime() const { return mArtistSortByTime; }
  // 开关当前曲目的收藏。内部走 mFavoriteToggleHandler，联网在锁外。
  void toggleFavoriteOnCurrent();
  bool currentFavorited() const { return mTrackFavorited; }
  void setLoginHandler(std::function<void()> handler);
  void setLoginSendHandler(std::function<void()> handler);
  void setLoginSubmitHandler(std::function<void()> handler);
  // 宿主暂停菜单/设置状态，由 main 通过 JNI 提供。
  void setPauseChecker(std::function<bool()> checker);
  void setMenuChecker(std::function<bool()> checker);
  // 供外部读取登录输入内容。
  std::string loginPhone();
  std::string loginCode();
  // 供外部（工作线程）推送提示文字。
  void notify(const std::string &text, long long durationMs) {
    setNotice(text, durationMs);
  }
  // 面板与圆球位置持久化：路径由外部给出，通常在模组数据目录下。
  void setStatePath(const std::string &path);
  // 设定本模组在宿主里的 lifecycle mod 名（= 模组目录名）。
  // 宿主校验 registerModule 时要求 info.modId 与「owining lifecycle mod」
  // 完全一致，而这个名字就是 mods/ 下的**目录名**，不是 manifest 的 name。
  // 目录名各实例并不统一（例如 MusicMod / 沉浸播放 并存），
  // 所以不能在代码里写死，必须由调用方把真实目录名传进来。
  void setModId(const std::string &id) { mModId = id; }
  void loadState();
  void saveState();
  void setTabHandler(std::function<void(int tab)> handler);
  bool install();
  void uninstall();

  void refresh(bool syncEngine);
  void processPending(AudioEngine &engine);

  void applyConfig(const std::string &key, const std::string &value,
                   bool immediate = false);
  bool autoPlayEnabled();
  bool uploadCover(const std::vector<unsigned char> &rgba, int width, int height);
  bool uploadCoverFrames(
      const std::vector<std::vector<unsigned char>> &frames, int width,
      int height);
  const std::string &coverId() const;
  void setLyric(const std::string &trackId, const std::string &text);
  void clearCover();
  // 换歌时必须立刻清空旧歌词。
  // 歌词是异步拉回来的（网络最长十几秒），不清的话新歌在唱、
  // 界面还在滚上一首的词，看起来就是「歌和词对不上」。
  void clearLyric();
  // 封面现场记录。写文件而不是只打 logcat —— logcat 会被别的进程刷掉。
  // 排查「有些歌封面不换」时全靠这份记录。
  void setCoverLogPath(const std::string &path);
  // 追加一行记录。线程安全，可以从中控线程和渲染线程同时调用。
  void coverLog(const char *fmt, ...);
  // 右上角圆形头像。走独立的图片 id，与封面互不覆盖。
  bool uploadAvatar(const std::vector<unsigned char> &rgba, int width,
                    int height);
  const std::string &avatarId() const;
  // 退出登录后清掉头像，下次登录重新拉取。
  void clearAvatar();
  // 清掉二维码，界面显示“通道不可用”。
  void clearQr();
  // 扫码登录用的二维码图片与提示文字。
  bool uploadQr(const std::vector<unsigned char> &rgba, int width, int height);
  // 按登录 URL 自己生成二维码。酷狗服务器返回的图片里带 appid 参数，
  // 概念版扫到会不认，所以和 md3music 一样只用 key 拼标准链接。
  bool uploadQrUrl(const std::string &url);
  void setQrStatus(const std::string &text);
  // 取出一次「刷新二维码」请求，返回非 0 表示有请求。
  int takeQrRequest();
  // 登录成功后收起登录页。
  void closeLogin();
  // 请求刷新二维码（过期自动换一张时用）。
  void requestQrRefresh();
  // 短信登录页的状态提示，例如键盘是否弹出。
  void setSmsHint(const std::string &text);
  // 同步登录状态：已登录时右上角显示头像，而不是「登录」。
  void setAccountState(bool loggedIn, const std::string &label);
  // 切换菜单时只替换列表显示，不动播放队列。
  void setListTracks(const std::vector<Track> &tracks);
  // 当前正在显示的列表。点歌时必须以它为准，
  // 否则从红心/探索/小众点歌会取到别的子页的缓存。
  const std::vector<Track> &shownTracks() const { return mShownTracks; }
  // 当前推荐子页：0 红心，1 探索，2 小众。
  int currentRecTab() const { return mRecTab; }
  // 某个偏好的强度：0 少推，1 适中，2 多推。可同时给多个偏好设权重。
  int prefWeight(int index) const {
    if (index < 0 || index >= 12) {
      return 0;
    }
    return mPrefWeights[index].load();
  }
  // 当前一级菜单：0 搜索，1 推荐，2 我的。
  int currentTab() const;
  // 点击列表行时回调 index 与当前菜单。
  void setTrackSelectHandler(std::function<void(int, int)> handler);
  // 一级菜单切换回调。
  void setTabChangeHandler(std::function<void(int)> handler);
  // 取出一次退出登录请求，返回非 0 表示有请求。
  int takeLogoutRequest();
  // 退出登录的确认框：两个入口都先走到这里，确认后才真正下发退出。
  void requestLogout();
  void confirmLogout();
  void cancelLogout();
  // 取出一次领取会员请求，返回非 0 表示有请求。
  int takeVipRequest();
  bool uploadListen() const { return mUploadListen.load(); }
  const char *tr(const char *zh, const char *en) const {
    return mEnglish.load() ? en : zh;
  }
  int prefLevel() const { return mPrefLevel.load(); }
  // 发码状态：0 未发 1 发送中 2 已发送 3 失败。hint 为空时沿用原提示。
  void setSmsSendState(int state, const std::string &hint);

private:
// Mini    = 660x192 带封面小面板（中窗口）
  // Expanded = 1560x760 大面板
  // Ball    = 已废弃的圆球形态，仅保留枚举值避免旧存档 mode 读错；
  //           读到时会被迁移为 Bar，不再产生。
  // Bar     = 620x64 单行迷你条（最小形态，替代圆球）
  enum class Mode { Mini, Expanded, Ball, Bar };
  // 底部三个一级入口：搜索、推荐、我的。
  // Favorites 追加在末尾（索引 3），显示顺序由底部菜单单独排：
  // 搜索、推荐、收藏、我的。这样不动 Mine 的索引，风险最小。
  enum class Tab { Search, Recommend, Mine, Favorites };
  enum class Drag { None, Panel, Progress, List, ScrollThumb, Volume, AlphaSlider };

  enum PendingAction : int {
    ActionNone = 0,
    ActionToggle = 1,
    ActionNext = 2,
    ActionPrevious = 3,
    ActionExpand = 4,
    ActionCollapse = 5,
    ActionPlayIndex = 100,
  };

  bool resolveHostApi();
  bool installCjkFont();
  void registerTouch();
  // 提交一次绘制；返回 false 表示这次没提交成功，调用方要保留脏标记。
  bool submit();
  void startSubmitThread();
  void stopSubmitThread();
  bool handleTouch(int action, float x, float y, long long timeMs);
  // 锁内版本：所有会调用宿主的动作（弹键盘、发码、登录）都塞进 deferred，
  // 由 handleTouch 在解锁之后执行，避免宿主回调或网络请求在持锁时进行。
  bool handleTouchLocked(int action, float x, float y, long long timeMs,
                         std::function<void()> &deferred);
  bool handleTextInput(const std::string &text);
  bool handleKey(int keyCode, unsigned int unicodeChar, bool isDown);
  void setNotice(const std::string &text, long long durationMs);
  // 锁内版本。调用方已持 mMutex，避免自锁。
  void setNoticeLocked(const std::string &text, long long durationMs);

  void resolveSurface();
  void placePanel();
  void updateAnim(long long now);
  // 展开面板只有落在控件上才消费触摸，空白要放给游戏。
  bool hitControl(float x, float y) const;

  AudioEngine *mEngine{nullptr};

  void *mRegisterModule{nullptr};
  void *mSubmitDraw{nullptr};
  void *mUnregisterModule{nullptr};
  void *mRegisterTouch{nullptr};
  void *mRegisterImage{nullptr};
  void *mGetSurfaceSize{nullptr};
  void *mRegisterText{nullptr};
  void *mShowKeyboard{nullptr};
  void *mRegisterFont{nullptr};
  bool mCjkReady{false};
  bool mBallPending{false};
  long long mBallDownMs{0};
  // 圆球独立坐标：与迷你条位置解耦，才拖得动。
  float mBallX{0.0f};
  float mBallY{0.0f};
  bool mHasBallPos{false};
  // 拖动时只画虚影，松手才让圆球落位。
  bool mBallGhostActive{false};
  float mBallGhostX{0.0f};
  float mBallGhostY{0.0f};
  long long mLastBallTapMs{0};
  void *mRegisterKey{nullptr};

  std::function<void(const std::string &)> mSearchHandler;
  std::function<void(int)> mTabHandler;
  std::function<void(int, int)> mTrackSelectHandler;
  std::function<void(int)> mTabChangeHandler;
  std::function<void()> mLoginHandler;
  std::function<void()> mLoginSendHandler;
  std::function<void()> mLoginSubmitHandler;
  std::function<bool()> mPauseChecker;
  std::function<bool()> mMenuChecker;
  // 登录页状态：0 未开启，1 手机号，2 验证码。
  bool mLoginPage{false};
  int mLoginField{0};
  // 绘制侧的状态快照，用于定位「状态改了但界面没变」这类问题。
  bool mLastDrawLogin{false};
  bool mLastDrawSms{false};
  bool mLastDrawQr{false};
  // 登录页两态：默认扫码，切到短信才显示手机号输入。
  bool mSmsMode{false};
  bool mQrReady{false};
  bool mQrZoom{false};
  std::string mQrMsg{"正在获取二维码"};
  // 短信登录页顶部状态提示。
  std::string mSmsHint{"点输入框弹键盘"};
  // 发码状态与冷却结束时间，按钮会显示「发送中」「已发送 58s」。
  int mSmsSendState{0};
  long long mSmsSendUntil{0};
  long long mSmsTickMs{0};
  // 登录态：已登录时右上角显示头像与账号首字。
  bool mLoggedIn{false};
  std::string mUserLabel;
  std::atomic<int> mPendingLogout{0};
  // 退出登录确认框是否显示中。
  bool mLogoutConfirm{false};
  // 透明度：三个形态各自独立。0 表示「未单独设置」，此时回落到 mPanelAlpha。
  float mAlphaMini{-1.0f};
  float mAlphaBar{-1.0f};

 public:
  // 列表区上沿。绘制与触摸必须共用这一个函数，否则必然错位。
  // 历史坑：歌手页绘制用 y+104、触摸用 y+140，差 36px，表现为「点哪都不对」。
  float listTopFor() const {
    if (mArtistView == 1) {
      // 歌手页：返回键 y+70~98，子标签 y+100~130，列表从 y+140 起。
      return mTargetY + 140.0f;
    }
    if (mTab == Tab::Search) {
      return mTargetY + (searchHasQuery() ? 134.0f : 104.0f);
    }
    return mTargetY + 104.0f;
  }
  // 搜索页是否已经搜过东西（输入框非空，或有任一分类的结果）。
  bool searchHasQuery() const {
    if (!mInputText.empty()) {
      return true;
    }
    return mTrackCount > 0 || !mSearchAlbums.empty() || !mSearchArtists.empty();
  }
  // 取实际生效的透明度：未单独设置时回落到 mPanelAlpha。
  float effMiniAlpha() const {
    return mAlphaMini >= 0.0f ? mAlphaMini : mPanelAlpha;
  }
  float effBarAlpha() const {
    return mAlphaBar >= 0.0f ? mAlphaBar : mPanelAlpha;
  }

  std::atomic<int> mPendingVip{0};
  std::atomic<bool> mUploadListen{true};
  std::atomic<bool> mEnglish{false};
  std::atomic<int> mPrefLevel{1};
  // 12 个偏好各自的权重，默认全部「适中」。
  std::atomic<int> mPrefWeights[12];
  // 推荐子页切换动画起点。
  long long mRecTabAnimStartMs{0};
  // 换曲时封面滑入的动画起点。
  long long mTrackAnimStartMs{0};
  std::atomic<int> mLyricDelay{0};
  std::atomic<int> mPendingQr{0};
  // 当前显示列表的完整副本。
  std::vector<Track> mShownTracks;
  std::string mLoginPhone;
  std::string mLoginCode;
  std::string mStatePath;
  Tab mTab{Tab::Recommend};
  // 推荐页顶部三个子分类：0 红心 1 探索 2 小众。
  int mRecTab{1};
  // 我的页里选择的推荐偏好，只影响推荐关键词。
  std::string mInputText;
  bool mInputActive{false};
  int mPressedAction{0};
  long long mPressedUntil{0};
  bool mSearchActive{false};
  std::string mNotice;
  long long mNoticeUntil{0};

  std::mutex mMutex;
  Mode mMode{Mode::Mini};
  // 是否已进入过播放状态。初次进游戏时没有歌，迷你条虽然画着，
  // 但触摸不应被它吞掉——否则会挡住游戏主界面的开始按钮。
  bool mHudArmed{false};
  // 是否已经提交过绘制。用于在「无歌」时清掉上一帧残留。
  bool mSubmittedOnce{false};
  // 歌词平滑滚动状态：记录上一步的行下标与插值，
  // 让切句时连续上移而不是瞬间跳一行。
  int mSmoothLyricIndex{-1};
  float mSmoothLyricFrom{0.0f};
  // 上一帧实际画到的行位置（支持小数），切句时以它为动画起点。
  float mSmoothLyricPos{0.0f};
  long long mSmoothLyricStartMs{0};
  bool mInstalled{false};

  float mScreenW{1920.0f};
  float mScreenH{1080.0f};
  // 真实屏幕像素，为 0 表示未知，此时不做坐标换算。
  float mRealW{0.0f};
  float mRealH{0.0f};

  // Mini 形态的固定位置，展开后再收起要回到原处。
  float mMiniX{36.0f};
  float mMiniY{180.0f};

  // 宿主 lifecycle mod 名（= 模组目录名），由 setModId 注入。
  std::string mModId{"MusicMod"};

  // 歌词滚动动画进行中标志。
  // 原来动画期间直接 mDirty = true，会绕过 forceInterval 节流，
  // 导致大面板每帧全量重建、掉帧。改成标志位由统一节拍控制。
  std::atomic<bool> mLyricAnimating{false};

  // 面板位置与尺寸（当前绘制值）
  float mPanelX{36.0f};
  float mPanelY{180.0f};
  float mPanelW{560.0f};
  float mPanelH{176.0f};

  // 动画目标与起点，用于展开、收起的过渡。
  float mTargetX{36.0f};
  float mTargetY{180.0f};
  float mTargetW{560.0f};
  float mTargetH{176.0f};
  float mAnimFromX{36.0f};
  float mAnimFromY{180.0f};
  float mAnimFromW{560.0f};
  float mAnimFromH{176.0f};
  long long mAnimStartMs{0};
  bool mAnimating{false};
  bool mDirty{true};
  // 强制重提交的时间戳，防宿主丢清单导致界面停更。
  long long mLastForceMs{0};
  // 自适应提交节拍：按上一次实际提交耗时决定下一拍间隔，
  // 渲染跟不上时自动放宽，避免越卡越频繁提交。
  long long mSubmitCostMs{0};
  // 分页切换滑动：记录切换起点与时间，用 200ms ease 过渡。
  long long mTabAnimStartMs{0};
  int mTabAnimFrom{0};
  // 选中行淡入：当前行变化时记录时间，透明度从 0 过渡到 1。
  long long mRowAnimStartMs{0};
  std::size_t mRowAnimIndex{static_cast<std::size_t>(-1)};
  // 跑马灯的刷新节拍，避免每拍都重绘。
  long long mMarqueeTickMs{0};

  // 内容状态
  std::string mTitle{"沉浸播放"};
  std::string mArtist{"还没有开始播放"};
  long long mPositionMs{0};
  long long mDurationMs{0};
  // 当前可播放到的毫秒位置；小于总时长时说明只能试听这一段。
  long long mPlayableMs{0};
  // 当前进度条对应的曲目 id，用于切歌时重置旧进度。
  std::string mDisplayedId;
  bool mPaused{false};
  bool mHasTrack{false};
  // 引擎的播放中标志。播完后引擎是 playing=false 且 paused=false，
  // 只看 paused 会让按钮停在暂停图标上，必须同时参考 playing。
  bool mPlaying{false};
  std::size_t mCurrentIndex{0};
  std::size_t mTrackCount{0};
  std::vector<std::string> mTrackTitles;
  std::vector<std::string> mTrackArtists;
  std::vector<std::string> mTrackIds;
  // true 时列表由菜单缓存提供，refresh 不再从播放队列覆盖。
  bool mManualList{false};

  // 配置
  // 配置项是否已从 ui.json 载入。
  // 宿主在注册/打开菜单时会用「默认值」回调 onConfigChanged，
  // 如果无条件接受，磁盘里的设置会被默认值冲掉。
  std::atomic<bool> mConfigLoadedFromDisk{false};
  // 已经用磁盘值压过一次的开关键，后续玩家再改要放行。
  // 开关类回调的暂存队列：宿主回放是批量来的，玩家点击是单条的，
  // 靠批量大小区分。见 applyConfig 里的说明。
  std::unordered_map<std::string, std::string> mCfgPending;
  long long mCfgPendingMs{0};
  // 标题是否需要滚动：决定重绘节拍用 50ms 还是 250ms。
  std::atomic<bool> mTitleScrolling{false};
  bool mShowCover{false};
  bool mShowProgress{true};
  float mFontScale{1.35f};
  bool mAutoPlayOnWorld{true};
  float mVolume{0.7f};

public:
  // 音乐音量（0..1），由 UI 音量条修改，主循环同步给播放器。
  // 不加锁：mMutex 声明在本段之后，且这是一次 4 字节对齐读，
  // 最坏情况读到上一次的旧值，下一拍就追上了，无副作用。
  float volume() const { return mVolume; }

private:
  // 右下方竖向音量条的命中区（绘制时记录，触摸时复用）。
  float mVolBarX{0.0f};
  float mVolBarY{0.0f};
  float mVolBarW{0.0f};
  float mVolBarH{0.0f};

  // 交互
  Drag mDrag{Drag::None};
  // 透明度滑块：拖动时记录是哪一形态（0 大面板 / 1 中面板 / 2 单行条）。
  int mAlphaDragIdx{-1};
  // 滑块几何缓存（绘制侧写入，触摸侧读取），避免两边坐标写死不一致。
  float mAlphaSliderX{0.0f};
  float mAlphaSliderY{0.0f};
  float mAlphaSliderW{0.0f};
  float mAlphaSliderH{0.0f};
  float mAlphaRowY[3]{0.0f, 0.0f, 0.0f};
  // 「我的」页按钮的 y 坐标由绘制侧写入，触摸侧直接读，
  // 避免删改布局后两边硬编码坐标对不上（历史上多次出现「看得见点不到」）。
  float mMineLogoutY{0.0f};
  float mMineVipY{0.0f};
  float mMineUploadY{0.0f};
  float mDownX{0.0f};
  float mDownY{0.0f};
  float mPressRectX{0.0f};
  float mPressRectY{0.0f};
  float mPressRectW{0.0f};
  float mPressRectH{0.0f};
  // 水波纹：记录按下点与时间，绘制时限制在按钮矩形内扩散。
  float mRippleX{0.0f};
  float mRippleY{0.0f};
  long long mRippleStartMs{0};
  // 触摸标定：记录最后一次按下点，叠加刻度与十字用于量测偏移。
  float mCalibX{-1.0f};
  float mCalibY{-1.0f};
  long long mCalibMs{0};
  float mGrabX{0.0f};
  float mGrabY{0.0f};
  bool mTracking{false};
  // 按下点是否落在控件上。只用于反馈，不再用来拦截事件。
  bool mDownOnControl{false};
  bool mMoved{false};
  long long mDownTimeMs{0};
  // 单行迷你条上「轻点歌名」的候选标志：
  // 轻点（未移动、短按）放大到中窗口；长按并拖动则是移动整条。
  // 两者共用歌名区域，靠这个标志在抬手时区分。
  bool mBarTapCandidate{false};

  long long mDragSeekMs{-1};
  float mListScroll{0.0f};

  std::atomic<int> mPendingAction{ActionNone};
  std::atomic<int> mPendingIndex{-1};
  std::atomic<long long> mPendingSeek{-1};
  std::atomic<int> mSearchSubmit{0};
  // 弹键盘不能在宿主触摸回调栈里直接调，交给工作线程下一拍执行。
  std::atomic<int> mWantKeyboard{0};
  std::string mPendingSearchText;

  // 绘制提交线程与其状态。
  std::thread mSubmitThread;
  std::atomic<bool> mSubmitStop{false};
  std::atomic<bool> mSubmitWanted{true};
  // 工作线程当前阶段，卡住时靠它判断卡在哪一步。
  std::atomic<int> mStage{0};

  bool mCoverReady{false};
  int mCoverW{0};
  int mCoverH{0};
  // 图片内容哈希与二维码尺寸，用于跳过重复注册。
  std::size_t mCoverHash{0};
  std::string mCoverId{"musicmod.cover"};
  std::vector<std::string> mCoverIds;
  // 已经成功注册进游戏层的图片 id。
  // 游戏层不接受重复注册同名 id（第二次必返回 false），但图还在里面。
  // 有这份记录才能分辨「已存在」和「真的失败」。
  std::set<std::string> mRegisteredImages;
  long long mCoverRotationMs{0};
  long long mLastCoverTickMs{0};
  // 右上角圆形头像的状态，与封面完全独立。
  bool mAvatarReady{false};
  int mAvatarW{0};
  int mAvatarH{0};
  std::size_t mAvatarHash{0};
  std::string mAvatarImg{"musicmod.avatar"};
  struct LyricLine {
    long long timeMs{0};
    long long durationMs{0};
    std::string text;
    std::vector<std::string> chars;
    std::vector<long long> charStarts;
    std::vector<long long> charEnds;
    // 预计算：每个字的像素宽度与累计右端位置，避免每帧重算。
    std::vector<float> charWidths;
    std::vector<float> charEndsPx;
    float totalWidth{0.0f};
  };
  std::vector<LyricLine> mLyrics;
  std::string mLyricTrackId;
  // 封面现场记录的目标文件与保护锁。
  std::string mCoverLogPath;
  std::mutex mCoverLogMutex;
  std::size_t mCurrentLyric{0};
  // 当前曲目是否已收藏，以及它对应的 hash。
  // 带上 hash 是为了防止「切歌时旧结果晚到」把新歌错误标成已收藏。
  bool mTrackFavorited{false};
  std::string mFavoriteTrackId;
  std::function<void(const std::string &)> mFavoriteToggleHandler;
  // 有非 0 表示需要向音源查询当前曲目的收藏状态。
  std::atomic<int> mFavoriteQueryPending{0};
  // 有非 0 表示需要重新拉取「收藏」页列表。
  std::atomic<int> mFavoriteListRequest{0};
  // 收藏页当前层级：0=歌单列表，1=某歌单的歌曲列表。
  int mFavView{0};
  // 收藏页第一层的数据（歌单摘要）。
  std::vector<PlaylistInfo> mFavPlaylists;
  // 当前打开的歌单 id 与名字，供第二层页头显示。
  std::string mFavOpenId;
  std::string mFavOpenName;
  // 点击歌单时写这里，由 main 去拉曲目。
  std::string mPendingOpenPlaylist;
  // ---- 搜索分类 ----
  // 0=单曲 1=专辑 2=歌手。默认单曲。
  int mSearchKind{0};
  // 搜索结果缓存（专辑/歌手）。歌曲走 mTrackTitles 那一套。
  std::vector<AlbumInfo> mSearchAlbums;
  std::vector<ArtistInfo> mSearchArtists;
  // ---- 歌手主页 ----
  // 0=不在歌手页 1=歌手页
  int mArtistView{0};
  // 歌手页当前子标签：0=单曲 1=专辑。
  int mArtistTab{0};
  // 排序：false=按热度 true=按时间。
  bool mArtistSortByTime{false};
  // 当前歌手。
  // 注意：不能叫 mArtist —— 那个名字已经被「当前歌曲的歌手名」占用。
  ArtistInfo mArtistInfo;
  // 歌手页的专辑缓存。
  std::vector<AlbumInfo> mArtistAlbums;
  // 待加载请求（由 main 消费）。
  std::string mPendingArtistId;
  // 1=要拉歌手歌曲 2=要拉歌手专辑
  std::atomic<int> mArtistLoadRequest{0};
  // 待打开的专辑 id（从歌手页点专辑进去）。
  std::string mPendingAlbumId;
  // 切句时间戳：供歌词滚动做补间，避免切句瞬间跳一行。
  long long mLyricSwitchMs{0};
  // 面板透明度，0.0–1.0；可在「我的」页调节，写入 ui.json。
  float mPanelAlpha{0.85f};
  // 主题模式：0=跟随时间，1=强制日间，2=强制夜间。写入 ui.json 记忆。
  // 0=跟随时间 1=强制日间 2=强制夜间（深色）
  // 默认深色：玩家反馈浅色面板在亮场景里看不清，深色更稳。
  int mThemeMode{2};
  // 当前生效的夜间状态，避免每帧重复套用配色。
  int mAppliedNight{-1};
  std::size_t mQrHash{0};
  int mQrW{0};
  int mQrH{0};
  // 二维码原始像素，直接按模块画方块，不依赖宿主图片接口。
  std::vector<unsigned char> mQrPixels;
  // 直接由 URL 生成时的模块点阵，大小 mQrModules × mQrModules。
  std::vector<unsigned char> mQrCells;
  // 取某个模块是否为黑，点阵优先，没有点阵才回退到图片采样。
  bool qrDarkAt(int row, int col) const;
  int mQrModules{0};
  int mQrMinX{0};
  int mQrMinY{0};
  float mQrPitch{0.0f};
};

} // namespace musicmod