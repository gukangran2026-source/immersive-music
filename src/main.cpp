#include <android/log.h>
#include <dirent.h>
#include <dlfcn.h>
#include <unistd.h>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "AudioEngine.hpp"
#include "HudUi.hpp"
#include "HttpClient.hpp"
#include "PlaylistProvider.hpp"
#include "KugouProvider.hpp"
#include "RenderHook.hpp"
#include "pl/Mod.hpp"

namespace {

constexpr const char *kTag = "MusicMod";
constexpr const char *kVersion = "0.4.0";

// 启动后等待游戏进入稳定状态的延时，单位秒。
constexpr int kStartDelaySeconds = 10;
// 主菜单阶段不播放。世界是否已加载不能靠计时猜，改用下面 worldLoaded() 判断。
// 世界加载后最多再等这么久仍未检测到，就放弃自动播放，避免误在菜单里发声。
constexpr int kWorldWaitSeconds = 600;
// 主循环节拍，界面刷新用它保证跟手。50 毫秒在手机上明显发涩，改成 16 毫秒。
constexpr int kTickMs = 16;
// 每几拍轮询一次播放器状态。节拍变快后同步间隔也缩短，状态跟得更紧。
constexpr int kEngineTickTicks = 10;

// base64 解码：忽略换行与非法字符，遇到 '=' 结束。
int base64Value(char c) {
  if (c >= 'A' && c <= 'Z') {
    return c - 'A';
  }
  if (c >= 'a' && c <= 'z') {
    return c - 'a' + 26;
  }
  if (c >= '0' && c <= '9') {
    return c - '0' + 52;
  }
  if (c == '+') {
    return 62;
  }
  if (c == '/') {
    return 63;
  }
  return -1;
}

std::vector<unsigned char> base64Decode(const std::string &text) {
  std::vector<unsigned char> out;
  out.reserve(text.size() * 3 / 4 + 4);
  int buffer = 0;
  int bits = 0;
  for (char c : text) {
    if (c == '=') {
      break;
    }
    const int value = base64Value(c);
    if (value < 0) {
      continue;
    }
    buffer = (buffer << 6) | value;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<unsigned char>((buffer >> bits) & 0xFF));
    }
  }
  return out;
}

// 世界存档的 LevelDB 文件只在真正加载世界时才会被游戏打开。
// 主菜单不碰这些句柄，因此用它判断是否已进入世界，比按时间猜可靠。
bool worldLoaded() {
  DIR *dir = opendir("/proc/self/fd");
  if (dir == nullptr) {
    return false;
  }
  bool found = false;
  char link[64];
  char target[512];
  while (dirent *ent = readdir(dir)) {
    if (ent->d_name[0] == '.') {
      continue;
    }
    std::snprintf(link, sizeof(link), "/proc/self/fd/%s", ent->d_name);
    const ssize_t n = readlink(link, target, sizeof(target) - 1);
    if (n <= 0) {
      continue;
    }
    target[n] = '\0';
    if (std::strstr(target, "minecraftWorlds/") == nullptr) {
      continue;
    }
    if (std::strstr(target, "/db/") != nullptr ||
        std::strstr(target, ".ldb") != nullptr ||
        std::strstr(target, "MANIFEST") != nullptr ||
        std::strstr(target, "/CURRENT") != nullptr) {
      found = true;
      break;
    }
  }
  closedir(dir);
  return found;
}

// 用系统 BitmapFactory 解码封面并转成 RGBA，供 HUD 注册图片使用。
bool decodeCover(JavaVM *vm, const std::string &path,
                 std::vector<unsigned char> &rgba, int &width, int &height) {
  if (vm == nullptr || path.empty()) {
    return false;
  }

  JNIEnv *env = nullptr;
  bool attached = false;
  if (vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK) {
    if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
      return false;
    }
    attached = true;
  }

  bool ok = false;
  jclass factoryClass = nullptr;
  jobject bitmap = nullptr;
  jintArray pixels = nullptr;

  do {
    factoryClass = env->FindClass("android/graphics/BitmapFactory");
    if (factoryClass == nullptr) {
      break;
    }
    jmethodID decodeFile = env->GetStaticMethodID(
        factoryClass, "decodeFile",
        "(Ljava/lang/String;)Landroid/graphics/Bitmap;");
    if (decodeFile == nullptr) {
      break;
    }
    jstring jPath = env->NewStringUTF(path.c_str());
    if (jPath == nullptr) {
      break;
    }
    bitmap = env->CallStaticObjectMethod(factoryClass, decodeFile, jPath);
    env->DeleteLocalRef(jPath);
    if (bitmap == nullptr) {
      env->ExceptionClear();
      break;
    }

    jclass bitmapClass = env->GetObjectClass(bitmap);
    jmethodID getWidth = env->GetMethodID(bitmapClass, "getWidth", "()I");
    jmethodID getHeight = env->GetMethodID(bitmapClass, "getHeight", "()I");
    jmethodID getPixels = env->GetMethodID(bitmapClass, "getPixels",
                                           "([IIIIIII)V");
    if (getWidth == nullptr || getHeight == nullptr || getPixels == nullptr) {
      env->DeleteLocalRef(bitmapClass);
      break;
    }

    const int w = env->CallIntMethod(bitmap, getWidth);
    const int h = env->CallIntMethod(bitmap, getHeight);
    if (w <= 0 || h <= 0 || w > 2048 || h > 2048) {
      env->DeleteLocalRef(bitmapClass);
      break;
    }

    pixels = env->NewIntArray(w * h);
    if (pixels == nullptr) {
      env->DeleteLocalRef(bitmapClass);
      break;
    }
    env->CallVoidMethod(bitmap, getPixels, pixels, 0, w, 0, 0, w, h);

    std::vector<jint> argb(static_cast<std::size_t>(w) * h);
    env->GetIntArrayRegion(pixels, 0, w * h, argb.data());

    rgba.resize(static_cast<std::size_t>(w) * h * 4);
    for (std::size_t i = 0; i < argb.size(); ++i) {
      const jint c = argb[i];
      rgba[i * 4 + 0] = static_cast<unsigned char>((c >> 16) & 0xFF);
      rgba[i * 4 + 1] = static_cast<unsigned char>((c >> 8) & 0xFF);
      rgba[i * 4 + 2] = static_cast<unsigned char>(c & 0xFF);
      rgba[i * 4 + 3] = static_cast<unsigned char>((c >> 24) & 0xFF);
    }
    width = w;
    height = h;
    ok = true;
    env->DeleteLocalRef(bitmapClass);
  } while (false);

  if (pixels != nullptr) {
    env->DeleteLocalRef(pixels);
  }
  if (bitmap != nullptr) {
    env->DeleteLocalRef(bitmap);
  }
  if (factoryClass != nullptr) {
    env->DeleteLocalRef(factoryClass);
}
  if (attached) {
    vm->DetachCurrentThread();
  }
  return ok;
}

// 把方形封面裁成圆形并预生成若干旋转帧。HUD 图片命令不支持旋转，
// 只能在 RGBA 层按角度重采样，后续按时间轮换纹理。
// 生成圆形封面。frameCount 传 1 即静态圆图（不再旋转）。
// 早期用 64 帧做旋转，每 60ms 换一帧只有 16fps，观感是持续闪烁，
// 而且 64 张 128×128 的 RGBA 常驻显存。改为静态单帧后闪烁消失。
// 封面裁切：把方形封面裁成「圆角正方形」并缩放到 outSize。
// 原来这里是裁成圆形 + 旋转动画。用户要求：不转、圆角方形。
// 所以去掉了角度旋转，只保留圆角裁切。
std::vector<std::vector<unsigned char>> makeRoundedCoverFrames(
    const std::vector<unsigned char> &src, int width, int height, int outSize,
    int frameCount, float cornerRatio = 0.28f) {
  std::vector<std::vector<unsigned char>> frames;
  if (src.empty() || width <= 0 || height <= 0 || outSize <= 0 ||
      frameCount <= 0) {
    return frames;
  }
  const int side = std::min(width, height);
  const int offsetX = (width - side) / 2;
  const int offsetY = (height - side) / 2;
  // 圆角半径：边长的 28%。
  // 原来 18% 在 128px 输出上只有 23px 圆角，缩放到 220px 显示后
  // 圆角被插值抹平，看起来接近方块。现在同时加大半径和输出分辨率。
  // 圆角半径比例：封面用 0.28，头像传 0.5（正好是正圆的半径）。
  const float corner = static_cast<float>(outSize) * cornerRatio;
  frames.reserve(frameCount);
  for (int frame = 0; frame < frameCount; ++frame) {
    std::vector<unsigned char> out(
        static_cast<std::size_t>(outSize) * outSize * 4, 0);
    for (int y = 0; y < outSize; ++y) {
      for (int x = 0; x < outSize; ++x) {
        // 圆角矩形判定：到四角圆心的距离决定是否在内部。
        const float fx = static_cast<float>(x) + 0.5f;
        const float fy = static_cast<float>(y) + 0.5f;
        const float cx = std::min(std::max(fx, corner),
                                  static_cast<float>(outSize) - corner);
        const float cy = std::min(std::max(fy, corner),
                                  static_cast<float>(outSize) - corner);
        const float dx = fx - cx;
        const float dy = fy - cy;
        const float dist = std::sqrt(dx * dx + dy * dy);
        float alpha = 1.0f;
        if (dist > corner) {
          continue;  // 圆角外，完全透明
        }
        if (dist > corner - 1.0f) {
          // 边缘 1 像素做反锯齿，避免锯齿状硬边。
          alpha = corner - dist;
        }
        const int sx = offsetX +
                       std::min(side - 1, std::max(0, x * side / outSize));
        const int sy = offsetY +
                       std::min(side - 1, std::max(0, y * side / outSize));
        const std::size_t srcIndex =
            (static_cast<std::size_t>(sy) * width + sx) * 4;
        const std::size_t dstIndex =
            (static_cast<std::size_t>(y) * outSize + x) * 4;
        out[dstIndex + 0] = src[srcIndex + 0];
        out[dstIndex + 1] = src[srcIndex + 1];
        out[dstIndex + 2] = src[srcIndex + 2];
        const float srcAlpha =
            static_cast<float>(src[srcIndex + 3]) / 255.0f;
        out[dstIndex + 3] = static_cast<unsigned char>(
            std::min(255.0f, std::max(0.0f, alpha * srcAlpha)) * 255.0f);
      }
    }
    frames.push_back(std::move(out));
  }
  return frames;
}
// 探测：游戏进程内能否拿到 Android Context / 创建 WebView。
// 只读探测，不改变任何状态；用于判断 HTML 路线是否可行。
void probeWebViewSupport(JavaVM *vm) {
  if (vm == nullptr) {
    __android_log_print(ANDROID_LOG_WARN, kTag, "probe: JavaVM 为空");
    return;
  }
  JNIEnv *env = nullptr;
  bool attached = false;
  if (vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK) {
    if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
      __android_log_print(ANDROID_LOG_WARN, kTag, "probe: attach 失败");
      return;
    }
    attached = true;
  }
  // 1) ActivityThread.currentActivityThread()
  jclass atClass = env->FindClass("android/app/ActivityThread");
  if (atClass == nullptr) {
    __android_log_print(ANDROID_LOG_WARN, kTag, "probe: 找不到 ActivityThread");
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (attached) vm->DetachCurrentThread();
    return;
  }
  jmethodID cur = env->GetStaticMethodID(
      atClass, "currentActivityThread", "()Landroid/app/ActivityThread;");
  jobject at = cur ? env->CallStaticObjectMethod(atClass, cur) : nullptr;
  __android_log_print(ANDROID_LOG_INFO, kTag, "probe: ActivityThread=%p",
                      reinterpret_cast<void *>(at));
  // 2) Application
  jobject app = nullptr;
  if (at != nullptr) {
    jmethodID getApp = env->GetMethodID(
        atClass, "getApplication", "()Landroid/app/Application;");
    if (getApp != nullptr) {
      app = env->CallObjectMethod(at, getApp);
    }
  }
  __android_log_print(ANDROID_LOG_INFO, kTag, "probe: Application=%p",
                      reinterpret_cast<void *>(app));
  // 3) 尝试创建 WebView（需要 UI 线程，先看类能否加载）
  jclass wvClass = env->FindClass("android/webkit/WebView");
  __android_log_print(ANDROID_LOG_INFO, kTag, "probe: WebView 类=%p",
                      reinterpret_cast<void *>(wvClass));
  if (env->ExceptionCheck()) env->ExceptionClear();
  if (attached) vm->DetachCurrentThread();
}
// 读取真实屏幕像素。宿主绘制面的分辨率可能和屏幕不同，触摸坐标要按它换算。
bool queryScreenSize(JavaVM *vm, int &width, int &height) {
  if (vm == nullptr) {
    return false;
  }
  JNIEnv *env = nullptr;
  bool attached = false;
  if (vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK) {
    if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
      return false;
    }
    attached = true;
  }
  bool ok = false;
  do {
    jclass resClass = env->FindClass("android/content/res/Resources");
    if (resClass == nullptr) {
      break;
    }
    jmethodID getSystem = env->GetStaticMethodID(
        resClass, "getSystem", "()Landroid/content/res/Resources;");
    if (getSystem == nullptr) {
      break;
    }
    jobject res = env->CallStaticObjectMethod(resClass, getSystem);
    if (res == nullptr) {
      break;
    }
    jmethodID getMetrics = env->GetMethodID(
        resClass, "getDisplayMetrics", "()Landroid/util/DisplayMetrics;");
    if (getMetrics == nullptr) {
      break;
    }
    jobject metrics = env->CallObjectMethod(res, getMetrics);
    if (metrics == nullptr) {
      break;
    }
    jclass metricsClass = env->FindClass("android/util/DisplayMetrics");
    if (metricsClass == nullptr) {
      break;
    }
    jfieldID wField = env->GetFieldID(metricsClass, "widthPixels", "I");
    jfieldID hField = env->GetFieldID(metricsClass, "heightPixels", "I");
    if (wField == nullptr || hField == nullptr) {
      break;
    }
    width = env->GetIntField(metrics, wField);
    height = env->GetIntField(metrics, hField);
    ok = width > 0 && height > 0;
  } while (false);
  if (attached) {
    vm->DetachCurrentThread();
  }
  return ok;
}

class MusicMod {
public:
  bool load(pl::mod::ModContext &ctx) {
    mJavaVm = ctx.javaVm();
    musicmod::setHttpJavaVm(mJavaVm);
    mDataDir = ctx.dataDir().string();
    // 模组目录名 = 宿主认定的 lifecycle mod 名。
    // registerModule 要求 info.modId 与它完全一致，否则模块注册失败、
    // 游戏内 Mod Menu 里根本看不到这个模组。
    // 各实例目录名不统一（MusicMod / 沉浸播放 并存），必须动态取。
    {
      const std::string root = ctx.modRootPath().string();
      const std::size_t slash = root.find_last_of('/');
      const std::string dirName =
          (slash == std::string::npos) ? root : root.substr(slash + 1);
      if (!dirName.empty()) {
        mModDirName = dirName;
        __android_log_print(ANDROID_LOG_INFO, kTag, "模组目录名(modId)=%s",
                            dirName.c_str());
      } else {
        __android_log_print(ANDROID_LOG_WARN, kTag,
                            "取不到模组目录名，沿用默认 modId");
      }
    }
    // ── 数据/令牌防丢机制 ───────────────────────────────────────
    // 问题：data 目录就在模组目录里，LeviLauncher 更新模组时
    // 会把整个 mods/MusicMod 替换掉，ui.json、session.json 跟着没了，
    // 玩家的面板位置/透明度/主题/推送强度/登录令牌全部丢失。
    // 对策：在【模组目录之外】再存一份镜像，
    //   启动时若主数据缺失或为空，就从镜像恢复。
    //   每次保存都同步写镜像。
    {
      const std::string base = mDataDir;
      // 从 .../mods/MusicMod/data 往回退两级，得到 .../minecraft
      std::string root = base;
      for (int i = 0; i < 2; ++i) {
        const std::size_t slash = root.find_last_of('/');
        if (slash == std::string::npos) {
          break;
        }
        root = root.substr(0, slash);
      }
      mBackupDir = root + "/MusicModData";
      std::error_code ec;
      std::filesystem::create_directories(mBackupDir, ec);
      // 保存后把主数据镜像到 mBackupDir。
      // 这样即使下一次更新把 mods/MusicMod 整个替换掉，
      // 镜像仍然在 mods 同级目录里，启动时能被恢复。
      auto mirrorOut = [this]() {
        std::error_code ec2;
        const char *items[] = {"ui.json", "session.json", "progress.json",
                              "device_info.json"};
        for (const char *name : items) {
          const std::string src = mDataDir + "/" + name;
          const std::string dst = mBackupDir + "/" + name;
          if (std::filesystem::exists(src, ec2)) {
            std::filesystem::copy_file(
                src, dst,
                std::filesystem::copy_options::overwrite_existing, ec2);
          }
        }
      };
      mMirrorOut = mirrorOut;
      // 主数据缺失时从镜像恢复。
      const char *keep[] = {"ui.json", "session.json", "progress.json",
                            "device_info.json"};
      for (const char *name : keep) {
        const std::string mainPath = base + "/" + name;
        const std::string bakPath = mBackupDir + "/" + name;
        const bool mainOk = std::filesystem::exists(mainPath, ec);
        const bool bakOk = std::filesystem::exists(bakPath, ec);
        if (!mainOk && bakOk) {
          std::filesystem::copy_file(
              bakPath, mainPath,
              std::filesystem::copy_options::overwrite_existing, ec);
          __android_log_print(ANDROID_LOG_INFO, "MusicMod",
                              "数据恢复: %s <- 镜像", name);
        } else if (mainOk && !bakOk) {
          std::filesystem::copy_file(
              mainPath, bakPath,
              std::filesystem::copy_options::overwrite_existing, ec);
        }
      }
    }
    {
      // 版本戳：每次启动写一次，用于确认当前加载的是哪一版模组。
      std::ofstream stamp(mDataDir + "/build_stamp.txt");
      if (stamp) {
        stamp << "ui_version=55\nbuild=2026-11-26 17:05\n"
                 "fix=modid_from_dirname,dark_theme_default,cover_toggle_batch,"
                 "cover_switch_saved,marquee_slow_smooth,ascii_width_062,"
                 "bar_3btn_shift,expanded_fixed_no_drag,modmenu_pos_removed,"
                 "expanded_fps_cap,bar_compact,avatar_circle_real,"
                 "logout_gate_relaxed,dup_avatar_removed,confirm_escape,"
                 "avatar_thread_nonblock,playmode_shuffle_repeat,"
                 "favorite_kugou_cloud,favorites_tab,tabs4,"
                 "lyric_two_segment_front,playlist_picker,search_kinds,"
                 "artist_page,sort_toggle,list_top_unified,"
                 "search_kind_gated,bar_btns_right40,queue_empty_guard,"
                 "skip_jitter_guard,empty_playlist_ok,isdef_default,"
                 "fav_fileids_del,playlist_pagesize_fix,exp_btn_order,fix_back_deadlock,fav_status_data,no_playlist_drift,cover_lyric_guard,restore_mini,bar_no_mode_btn,lyric_engine_guard,lyric_thread_clear,remove_fav_status,mixsongid_field,mini_heart_btn,cover_thread_retry,cover_img_validate,mode_btn_flat,cover_debug_log,mode_btn_align,cover_id_reuse\n";
      }
    }

    std::error_code ec;
    std::filesystem::create_directories(mDataDir, ec);
    // 旧版本留下的本地测试歌单不再使用，避免登录后列表里混进测试曲目。
    mProvider = std::make_unique<musicmod::PlaylistProvider>(mDataDir +
                                                             "/playlist.disabled.json");
    // 酷狗音源是首选，初始化放到工作线程里做，避免阻塞游戏启动。
    mKugou = std::make_unique<musicmod::KugouProvider>();

    __android_log_print(ANDROID_LOG_INFO, kTag, "load: v%s 数据目录=%s", kVersion,
                        mDataDir.c_str());
    return true;
  }

  bool enable() {
    __android_log_print(ANDROID_LOG_INFO, kTag, "enable: 进入");
    if (mRunning.exchange(true)) {
      return true;
    }

    // 渲染入口只做探测，不改写函数头。
    mRenderHook.install();
    __android_log_print(ANDROID_LOG_INFO, kTag, "步骤: 渲染探测完成");
    probeWebViewSupport(mJavaVm);
    // 界面由宿主绘制，注册失败不影响播放。
    if (!mModDirName.empty()) {
      mUi.setModId(mModDirName);
    }
    mUi.setStatePath(mDataDir + "/ui.json");
    __android_log_print(ANDROID_LOG_INFO, kTag, "步骤: 开始注册界面");
    const bool uiOk = mUi.install();
    __android_log_print(ANDROID_LOG_INFO, kTag, "步骤: 界面注册结果=%d",
                        uiOk ? 1 : 0);
    // 记录真实屏幕尺寸，触摸坐标要按它换算到绘制面，否则面板会「看得见点不中」。
    {
      int realW = 0;
      int realH = 0;
      const bool sizeOk = queryScreenSize(mJavaVm, realW, realH);
      __android_log_print(ANDROID_LOG_INFO, kTag, "步骤: 屏幕=%dx%d ok=%d",
                          realW, realH, sizeOk ? 1 : 0);
      if (sizeOk) {
        mUi.setRealScreen(static_cast<float>(realW),
                          static_cast<float>(realH));
      }
    }

    __android_log_print(ANDROID_LOG_INFO, kTag, "步骤: 创建音频引擎");
    mEngine = std::make_unique<musicmod::AudioEngine>(mJavaVm);
    __android_log_print(ANDROID_LOG_INFO, kTag, "步骤: 音频引擎已建");
    mEngine->setProvider(mProvider.get());
    mEngine->setStatePath(mDataDir + "/progress.json");
    mUi.setEngine(mEngine.get());
    mUi.setSearchHandler([this](const std::string &keyword) {
      std::lock_guard<std::mutex> lock(mSearchMutex);
      mSearchKeyword = keyword;
      mLastKeyword = keyword;
      mSearchTarget = 0;
      mSearchPending = true;
    });
    // 登录页的两个动作交给工作线程执行，避免卡住渲染。
    mUi.setLoginSendHandler([this] {
      std::lock_guard<std::mutex> lock(mLoginMutex);
      mLoginAction = 1;
    });
    mUi.setLoginSubmitHandler([this] {
      std::lock_guard<std::mutex> lock(mLoginMutex);
      mLoginAction = 2;
    });
    // 暂停菜单/设置打开时屏蔽 HUD 触摸，避免点到后面的面板。
    mUi.setPauseChecker([this] {
      if (mJavaVm == nullptr) {
        return false;
      }
      JNIEnv *env = nullptr;
      bool attached = false;
      if (mJavaVm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) !=
          JNI_OK) {
        if (mJavaVm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
          return false;
        }
        attached = true;
      }
      bool open = false;
      jclass cls = env->FindClass("org/levimc/launcher/preloader/PreloaderInput");
      if (cls != nullptr) {
        jmethodID method = env->GetStaticMethodID(
            cls, "isPauseMenuOpen", "()Z");
        if (method != nullptr) {
          open = env->CallStaticBooleanMethod(cls, method) == JNI_TRUE;
        }
        env->DeleteLocalRef(cls);
      }
      if (env->ExceptionCheck()) {
        env->ExceptionClear();
      }
      if (attached) {
        mJavaVm->DetachCurrentThread();
      }
      return open;
    });
    mUi.setMenuChecker([this] {
      if (mJavaVm == nullptr) {
        return false;
      }
      JNIEnv *env = nullptr;
      bool attached = false;
      if (mJavaVm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) !=
          JNI_OK) {
        if (mJavaVm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
          return false;
        }
        attached = true;
      }
      bool open = false;
      jclass cls = env->FindClass("org/levimc/launcher/preloader/PreloaderInput");
      if (cls != nullptr) {
        jmethodID method = env->GetStaticMethodID(cls, "isShowingMenu", "()Z");
        if (method != nullptr) {
          open = env->CallStaticBooleanMethod(cls, method) == JNI_TRUE;
        }
        env->DeleteLocalRef(cls);
      }
      if (env->ExceptionCheck()) {
        env->ExceptionClear();
      }
      if (attached) {
        mJavaVm->DetachCurrentThread();
      }
      return open;
    });
    // 分页先用不同关键词喂数据，真实频道接口留到登录接入后替换。
    mUi.setTabHandler([this](int tab) {
      // 10..12 推荐子分类红心/探索/小众；20..31 我的页推荐偏好。
      static const char *kPrefKeywords[12] = {
          "华语流行", "欧美流行", "日韩流行", "粤语歌曲",
          "电子音乐", "摇滚音乐", "民谣",     "说唱",
          "轻音乐",   "古典音乐", "爵士音乐", "二次元音乐"};
      std::string keyword;
      if (tab == 0) {
        keyword = mLastKeyword;
        if (keyword.empty()) {
          return;
        }
      } else if (tab == 10) {
        // 先显示红心缓存，再异步拉取，避免停留在上一个子页的列表上。
        mUi.setListTracks(mRecCache[0]);
        mFavoritesPending.store(true);
        return;
      } else if (tab >= 11 && tab <= 12) {
        const int sub = tab - 10;
        mUi.setListTracks(mRecCache[sub]);
        std::lock_guard<std::mutex> lock(mSearchMutex);
        mSearchKeyword.clear();
        mSearchTarget = 2 + (tab - 11);
        mSearchPending = true;
        return;
      } else if (tab >= 20 && tab <= 31) {
        keyword = kPrefKeywords[tab - 20];
        const int level = mUi.prefLevel();
        if (level <= 0) {
          keyword += " 经典";
        } else if (level >= 2) {
          keyword += " 热门";
        }
      } else {
        return;
      }
      std::lock_guard<std::mutex> lock(mSearchMutex);
      mSearchKeyword = keyword;
      mSearchTarget = 1;
      mSearchPending = true;
    });

    // 切菜单只换显示列表，播放队列和当前歌曲保持不变。
    mUi.setTabChangeHandler([this](int tab) {
      const musicmod::Track *cur = mEngine->currentTrack();
      const std::string currentId = cur != nullptr ? cur->id : std::string();
      // 列表为空时只换显示，不动播放队列。
      // 没搜过的搜索页、还没拉到的推荐子页都属于这种，拿空列表去替换
      // 会把正在播的歌弄没（历史上就是这样把队列清空的）。
      if (tab == 0) {
        mUi.setListTracks(mSearchList);
        if (!mSearchList.empty()) {
          mEngine->setQueueKeepCurrent(mSearchList, currentId);
        }
      } else if (tab == 1) {
        const int sub = std::max(0, std::min(2, mUi.currentRecTab()));
        const std::vector<musicmod::Track> &list = mRecCache[sub];
        mUi.setListTracks(list);
        if (!list.empty()) {
          mEngine->setQueueKeepCurrent(list, currentId);
        }
      }
    });
    // 点列表行才真正替换播放队列并播放。
    // 收藏切换：点爱心时调用，联网在 main 线程里做（玩家主动操作，
    // 一次性等待可接受），结果回填给 UI。
    mUi.setFavoriteToggleHandler([this](const std::string &trackId) {
      if (mKugou == nullptr || trackId.empty()) {
        return;
      }
      if (!mKugou->loggedIn()) {
        mUi.notify("请先登录", 3000);
        return;
      }
      // 找到当前曲目，收藏接口要 title/albumId/audioId。
      const musicmod::Track *cur = mEngine ? mEngine->currentTrack() : nullptr;
      if (cur == nullptr || cur->id != trackId) {
        mUi.notify("曲目已切换, 请重试", 3000);
        return;
      }
      const bool wasFav = mUi.currentFavorited();
      const bool ok = wasFav ? mKugou->removeFromFavorites(*cur)
                             : mKugou->addToFavorites(*cur);
      if (ok) {
        mUi.setFavoriteState(!wasFav, trackId);
        mUi.notify(wasFav ? "已取消收藏" : "已收藏到我喜欢", 2600);
        __android_log_print(ANDROID_LOG_INFO, kTag, "收藏切换成功 新状态=%d",
                            wasFav ? 0 : 1);
        // 收藏变化会影响收藏页内容，标记一下，切过去时会重新拉。
        mFavListDirty = true;
      } else {
        mUi.notify("操作失败: " + mKugou->lastError(), 4000);
        __android_log_print(ANDROID_LOG_WARN, kTag, "收藏切换失败: %s",
                            mKugou->lastError().c_str());
      }
    });

    mUi.setTrackSelectHandler([this](int index, int tab) {
      (void)tab;
      const std::vector<musicmod::Track> &list = mUi.shownTracks();
      if (index < 0 || static_cast<std::size_t>(index) >= list.size()) {
        return;
      }
      mEngine->setQueue(list);
      mEngine->playIndex(static_cast<std::size_t>(index));
    });

    try {
      mWorker = std::thread([this] { run(); });
      __android_log_print(ANDROID_LOG_INFO, kTag, "步骤: 工作线程已启动");
      // 头像抓取独立成线程：网络阻塞只影响它自己，主循环始终 16ms 稳定。
      mAvatarStop.store(false);
      mAvatarWorker = std::thread([this] { avatarLoop(); });
      __android_log_print(ANDROID_LOG_INFO, kTag, "步骤: 头像线程已启动");
      // 歌词也独立成线程。lyric() 内部是阻塞 httpGet（超时 12 秒，
      // 失败还会再去掉 keywords 重试一次），绝不能再放主循环里。
      mLyricStop.store(false);
      mLyricWorker = std::thread([this] { lyricLoop(); });
      __android_log_print(ANDROID_LOG_INFO, kTag, "步骤: 歌词线程已启动");
      // 封面下载同样独立成线程。fetchCover 内部是 8s + 10s 两次阻塞请求，
      // 放主循环里最坏卡 18 秒，且失败后旧代码不重试，封面就永久不换。
      // 封面现场记录落到数据目录，不受 logcat 环形缓冲影响。
      mUi.setCoverLogPath(mDataDir + "/cover_debug.log");
      __android_log_print(ANDROID_LOG_INFO, kTag, "封面记录文件: %s",
                          (mDataDir + "/cover_debug.log").c_str());
      mCoverStop.store(false);
      mCoverWorker = std::thread([this] { coverLoop(); });
      __android_log_print(ANDROID_LOG_INFO, kTag, "步骤: 封面线程已启动");
    } catch (const std::system_error &e) {
      mRunning.store(false);
      __android_log_print(ANDROID_LOG_ERROR, kTag, "线程创建失败: %s", e.what());
      teardown();
      return false;
    }
    return true;
  }

  bool disable() {
    __android_log_print(ANDROID_LOG_INFO, kTag, "disable");
    stopWorker();
    teardown();
    return true;
  }

  bool unload() {
    stopWorker();
    teardown();
    __android_log_print(ANDROID_LOG_INFO, kTag, "unload");
    return true;
  }

private:
  void teardown() {
    if (mEngine) {
      mEngine->stop();
      mEngine.reset();
    }
    if (mKugou) {
      mKugou->shutdown();
    }
    mUi.uninstall();
    mRenderHook.remove();
  }

  void stopWorker() {
    {
      std::lock_guard<std::mutex> lock(mExitMutex);
      mRunning.store(false);
    }
    mExitCv.notify_all();
    if (mWorker.joinable()) {
      mWorker.join();
    }
    // 头像线程必须在卸载前收掉，否则它会持有已销毁的 mKugou。
    mAvatarStop.store(true);
    if (mAvatarWorker.joinable()) {
      mAvatarWorker.join();
    }
    // 歌词线程同样要在 mKugou 被销毁前收掉。
    mLyricStop.store(true);
    if (mLyricWorker.joinable()) {
      mLyricWorker.join();
    }
    mCoverStop.store(true);
    if (mCoverWorker.joinable()) {
      mCoverWorker.join();
    }
  }

  void run() {
    {
      std::unique_lock<std::mutex> lock(mExitMutex);
      if (mExitCv.wait_for(lock, std::chrono::seconds(kStartDelaySeconds),
                           [this] { return !mRunning.load(); })) {
        return;
      }
    }
    if (!mRunning.load() || !mEngine) {
      return;
    }

    __android_log_print(ANDROID_LOG_INFO, kTag, "游戏库已映射=%d",
                        isGameLibraryMapped() ? 1 : 0);

    // 优先接酷狗：拉起本地服务并搜一批热门歌曲，失败再退回本地歌单。
    bool kugouReady = false;
    if (mKugou != nullptr &&
        mKugou->init(mDataDir + "/libkugou_server.so", mDataDir)) {
      kugouReady = mKugou->search("热门歌曲");
    }
    if (kugouReady) {
      mEngine->setProvider(mKugou.get());
      mRecommendList = mKugou->tracks();
      mUi.setListTracks(mRecommendList);
      // 登录后首屏就用官方每日推荐，避免每次启动都看到同一批热门歌。
      if (mKugou->loggedIn()) {
        std::lock_guard<std::mutex> lock(mSearchMutex);
        mSearchKeyword.clear();
        mSearchTarget = 2;
        mSearchPending = true;
      }
      __android_log_print(ANDROID_LOG_INFO, kTag, "音源切换为酷狗概念版");
    } else {
      mEngine->setProvider(mProvider.get());
      __android_log_print(ANDROID_LOG_WARN, kTag, "酷狗不可用, 回退本地歌单");
    }
    // 启动时同步一次账号状态，右上角据此决定显示登录还是头像。
    mUi.setAccountState(mKugou != nullptr && mKugou->loggedIn(),
                        mKugou != nullptr ? mKugou->userId() : std::string());

    if (!mEngine->loadQueue()) {
      __android_log_print(ANDROID_LOG_WARN, kTag, "没有可播放曲目");
      mUi.refresh(true);
    return;
  }

  // 读取上次进度与暂停状态，但先不播：等进入世界后再恢复。
  std::size_t resumeIndex = 0;
  long long resumePos = 0;
  bool resumePaused = false;
  const bool hasResume = mEngine->loadProgress(resumeIndex, resumePos, &resumePaused) &&
                         resumeIndex < mEngine->trackCount();
  if (hasResume) {
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "待续播: 第 %zu 首, %lld 毫秒, 上次%s", resumeIndex + 1,
                        resumePos, resumePaused ? "暂停" : "播放");
  }

    // 进游戏时保持安静，世界真正加载完成后再开始播放。
    bool autoStarted = false;
    int worldHits = 0;
    bool worldSeen = false;
    int worldTick = 0;
    // 离开世界检测：和进入用同一判据，连续两次读不到世界存档句柄
    // 就认定已经退回主菜单，暂停播放并记住进度。
    int worldGoneHits = 0;
    int worldGoneTick = 0;
    bool worldGone = false;
    // 只有「因为离开世界而被我们暂停」的情况，回到世界才自动续播。
    // 用户自己按的暂停不能被这里覆盖。
    bool pausedByLeaving = false;
    int worldBackHits = 0;
    const auto bootAt = std::chrono::steady_clock::now();
    int tickCounter = 0;
    int beatTicks = 0;
    auto lastBeat = std::chrono::steady_clock::now();
    while (mRunning.load()) {
      {
        std::unique_lock<std::mutex> lock(mExitMutex);
        mExitCv.wait_for(lock, std::chrono::milliseconds(kTickMs));
      }
      if (!mRunning.load() || !mEngine) {
        break;
      }

      // 用户已经手动开始过就不再自动接管。
      if (!autoStarted && mUi.autoPlayEnabled()) {
        if (mEngine->playing()) {
          autoStarted = true;
        } else {
          // 每 500 毫秒查一次世界存档句柄，连续两次命中才认定真的进了世界。
          if (++worldTick >= 10) {
            worldTick = 0;
            if (worldLoaded()) {
              ++worldHits;
            } else {
              worldHits = 0;
            }
            if (!worldSeen && worldHits >= 2) {
              worldSeen = true;
              __android_log_print(ANDROID_LOG_INFO, kTag, "检测到世界已加载");
            }
          }
          if (worldSeen) {
            autoStarted = true;
            __android_log_print(ANDROID_LOG_INFO, kTag, "世界内自动播放");
            if (hasResume) {
              mEngine->prepareResume(resumePos);
              if (resumePaused) {
                mEngine->playIndexPaused(resumeIndex);
              } else {
                mEngine->playIndex(resumeIndex);
              }
            } else {
              mEngine->playIndex(0);
            }
          } else if (std::chrono::steady_clock::now() - bootAt >=
                     std::chrono::seconds(kWorldWaitSeconds)) {
            autoStarted = true;
            __android_log_print(ANDROID_LOG_INFO, kTag, "未检测到世界, 不自动播放");
          }
        }
      }

      // 离开世界：世界内的存档句柄消失，说明已退回主菜单。
      // 此时必须暂停播放并保存进度，否则音乐会在主菜单继续响。
      // 只在「曾经进过世界」之后才检测，避免启动阶段误判。
      if (worldSeen && !worldGone && mEngine->playing()) {
        if (++worldGoneTick >= 10) {
          worldGoneTick = 0;
          if (worldLoaded()) {
            worldGoneHits = 0;
          } else {
            ++worldGoneHits;
          }
          if (worldGoneHits >= 2) {
            worldGone = true;
            if (!mEngine->paused()) {
              mEngine->togglePause();
              // 只有确实是我们暂停的，才允许回世界时自动续播。
              pausedByLeaving = true;
            }
            mEngine->saveProgress();
            __android_log_print(ANDROID_LOG_INFO, kTag,
                                "已离开世界, 暂停播放并保存进度");
          }
        }
      }
      // 回到世界：再次检测到存档句柄，且上次是我们暂停的，就接着放。
      if (worldGone && pausedByLeaving && mEngine->paused()) {
        if (++worldGoneTick >= 10) {
          worldGoneTick = 0;
          if (worldLoaded()) {
            ++worldBackHits;
          } else {
            worldBackHits = 0;
          }
          if (worldBackHits >= 2) {
            worldBackHits = 0;
            worldGone = false;
            worldGoneHits = 0;
            pausedByLeaving = false;
            mEngine->togglePause();
            __android_log_print(ANDROID_LOG_INFO, kTag, "回到世界, 继续播放");
          }
        }
      }

      // 播放器状态每 5 拍同步一次，其余拍只刷新绘制，保证点击与拖动跟手。
      const bool syncEngine = (++tickCounter >= kEngineTickTicks);
      if (syncEngine) {
        tickCounter = 0;
        // 把 UI 音量条的值同步给播放器（音乐音量，不是系统音量）。
        // 每 5 拍同步一次足够，避免每帧都调底层。
        {
          const float uiVol = mUi.volume();
          if (std::fabs(uiVol - mLastAppliedVolume) > 0.005f) {
            mLastAppliedVolume = uiVol;
            mEngine->setVolume(uiVol);
          }
        }
        mEngine->tick();
        // 每 5 拍把主数据镜像一次。
        // 覆盖玩家所有设置与登录令牌，下次更新后能直接恢复。
        if (mMirrorOut) {
          mMirrorOut();
        }
        const std::string playError = mEngine->takeError();
        if (!playError.empty()) {
          mUi.notify(playError, 4000);
        }
        // 歌词：主循环只负责「告诉线程该拉哪首」和「把结果放到界面上」，
        // 真正的网络请求在 lyricLoop() 线程里做。
        // 原来这里直接调 mKugou->lyric()，那是阻塞调用（超时 12 秒，
        // 失败还会再试一次），阻塞期间 mPositionMs 不推进，歌词行号就卡住不动。
        const musicmod::Track *lyricTrack = mEngine->currentTrack();
        if (lyricTrack != nullptr && lyricTrack->id != mLastLyricId) {
          mLastLyricId = lyricTrack->id;
          // 先清掉旧词：宁可空白，也不能拿上一首的词配这一首。
          mUi.clearLyric();
          {
            std::lock_guard<std::mutex> lock(mLyricMutex);
            mLyricReqTrack = *lyricTrack;
            mLyricReqId = lyricTrack->id;
          }
          __android_log_print(ANDROID_LOG_INFO, kTag, "换歌 请求歌词 %.16s",
                              lyricTrack->id.c_str());
        }
        {
          std::string readyId;
          std::string readyText;
          {
            std::lock_guard<std::mutex> lock(mLyricMutex);
            if (mLyricHasResult) {
              readyId = mLyricReadyId;
              readyText = mLyricReadyText;
              mLyricHasResult = false;
            }
          }
          if (!readyText.empty()) {
            // setLyric 内部还会用引擎当前曲目再校验一次，双保险。
            mUi.setLyric(readyId, readyText);
          }
        }

        if (mEngine->takeNeedMore()) {
          const musicmod::Track *seed = mEngine->currentTrack();
          bool appended = false;
          if (seed != nullptr && mKugou != nullptr &&
              mKugou->loadSimilar(*seed)) {
            const std::size_t start = mEngine->appendQueue(mKugou->tracks());
            const auto &full = mEngine->tracks();
            if (start < full.size()) {
              if (mUi.currentTab() == 0) {
                mSearchList = full;
              } else {
                mRecommendList = full;
              }
              mUi.setListTracks(full);
              mEngine->playIndex(start);
              appended = true;
            }
          }
          if (!appended) {
            mUi.notify("没有更多推荐", 3000);
          }
        }
        // 上报开关语义（这段原本就是对的，不要改动）：
        //   开启 -> 切歌时先给上一首补 end，再给新歌发 start；
        //   关闭 -> 若还有「已发 start 未结账」的歌，补一个 end 收尾后清空，
        //           之后不再发任何事件。
        // 关闭时补发 end 是必要的：不补的话服务端会留下永不完结的记录。
        if (!mUi.uploadListen()) {
          if (!mReportedId.empty()) {
            const long long nowMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count();
            mKugou->reportListen(mReportedTrack, "end",
                                 nowMs - mReportStartedMs);
            mReportedId.clear();
          }
        } else if (mKugou != nullptr && mEngine->playing()) {
          const musicmod::Track *cur = mEngine->currentTrack();
          if (cur != nullptr && cur->id != mReportedId) {
            const long long nowMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count();
            if (!mReportedId.empty()) {
              mKugou->reportListen(mReportedTrack, "end",
                                   nowMs - mReportStartedMs);
            }
            mKugou->reportListen(*cur, "start", 0);
            mReportedId = cur->id;
            mReportedTrack = *cur;
            mReportStartedMs = nowMs;
          }
        }
        refreshCover();
        refreshAvatar();
        // 切歌后查一次收藏状态。只在曲目真的换了时才发起（UI 侧置的标志），
        // 不是每拍都查。
        const std::string favQuery = mUi.takeFavoriteQuery();
        if (!favQuery.empty() && mKugou != nullptr && mKugou->loggedIn()) {
          const musicmod::Track *cur = mEngine->currentTrack();
          if (cur != nullptr && cur->id == favQuery) {
            const bool fav = mKugou->isFavorite(*cur);
            mUi.setFavoriteState(fav, favQuery);
          }
        }
        // 进收藏页时重新拉歌单列表（第一层）。
        // 收藏页不做缓存：玩家刚点过爱心，切过来必须看到最新结果。
        if (mUi.takeFavoriteListRequest() != 0 && mKugou != nullptr &&
            mKugou->loggedIn()) {
          if (mKugou->loadPlaylists()) {
            mUi.setFavoritePlaylists(mKugou->playlists());
            __android_log_print(ANDROID_LOG_INFO, kTag, "收藏页歌单 %zu 个",
                                mKugou->playlists().size());
          } else {
            mUi.setFavoritePlaylists({});
            __android_log_print(ANDROID_LOG_INFO, kTag, "歌单列表为空: %s",
                                mKugou->lastError().c_str());
          }
        }
        // ---- 歌手页加载 ----
        const int artistReq = mUi.takeArtistLoadRequest();
        if (artistReq != 0 && mKugou != nullptr && mKugou->loggedIn()) {
          const std::string aid = mUi.pendingArtistId();
          if (!aid.empty()) {
            // 资料只在第一次进歌手页时拉；切排序不重复拉资料。
            if (mUi.needArtistDetail()) {
              mKugou->loadArtistDetail(aid);
              mUi.updateArtistInfo(mKugou->currentArtist());
            }
            const std::string sortBy = mUi.artistSortByTime() ? "new" : "hot";
            if (artistReq == 2) {
              if (mKugou->loadArtistAlbums(aid, sortBy)) {
                mUi.setArtistAlbums(mKugou->albums());
              } else {
                mUi.notify("歌手专辑加载失败", 3000);
                __android_log_print(ANDROID_LOG_WARN, kTag,
                                    "歌手专辑加载失败: %s",
                                    mKugou->lastError().c_str());
              }
            } else {
              if (mKugou->loadArtistSongs(aid, sortBy)) {
                const std::vector<musicmod::Track> songs = mKugou->tracks();
                mUi.setListTracks(songs);
                __android_log_print(ANDROID_LOG_INFO, kTag,
                                    "歌手歌曲已加载 %zu 首", songs.size());
              } else {
                mUi.notify("歌手歌曲加载失败", 3000);
                __android_log_print(ANDROID_LOG_WARN, kTag,
                                    "歌手歌曲加载失败: %s",
                                    mKugou->lastError().c_str());
              }
            }
          }
        }
        // ---- 打开专辑 ----
        const std::string openAlbum = mUi.takeOpenAlbumRequest();
        if (!openAlbum.empty() && mKugou != nullptr && mKugou->loggedIn()) {
          if (mKugou->loadAlbumSongs(openAlbum)) {
            const std::vector<musicmod::Track> songs = mKugou->tracks();
            mUi.setListTracks(songs);
            mEngine->setQueue(songs);
            mUi.notify("已载入专辑 " + std::to_string(songs.size()) + " 首",
                       2600);
            __android_log_print(ANDROID_LOG_INFO, kTag, "专辑已载入 %zu 首",
                                songs.size());
          } else {
            mUi.notify("专辑加载失败: " + mKugou->lastError(), 4000);
          }
        }
        // 点了某个歌单：拉它的曲目，然后进第二层。
        const std::string openId = mUi.takeOpenPlaylistRequest();
        if (!openId.empty() && mKugou != nullptr && mKugou->loggedIn()) {
          if (mKugou->loadPlaylistTracks(openId)) {
            const std::vector<musicmod::Track> songs = mKugou->tracks();
            mFavoriteList = songs;
            mUi.setListTracks(songs);
            // 不要用「正在浏览的歌单」当收藏目标。
            // 玩家打开某个歌单只是看看，不代表他要往那儿收藏；
            // 之前这一行会让爱心加到你正在浏览的歌单，
            // 然后你去「默认收藏」里找当然找不到。
            // 收藏目标固定由 is_def==1 决定，见 favoritesListId()。
            // 从歌单列表里找名字，交给 UI 显示在页头。
            std::string name = openId;
            for (const auto &pl : mKugou->playlists()) {
              if (pl.id == openId) {
                name = pl.name;
                break;
              }
            }
            mUi.enterFavoritePlaylist(openId, name);
            __android_log_print(ANDROID_LOG_INFO, kTag,
                                "歌单已打开 %s 共 %zu 首", name.c_str(),
                                songs.size());
          } else {
            mUi.notify("打开歌单失败: " + mKugou->lastError(), 4000);
            __android_log_print(ANDROID_LOG_WARN, kTag, "打开歌单失败: %s",
                                mKugou->lastError().c_str());
          }
        }
      }

      // 处理界面提交的搜索请求。
      bool doSearch = false;
      std::string keyword;
      int target = 0;
      // 搜索分类：0=单曲 1=专辑 2=歌手。
      const int searchKind = mUi.searchKind();
      if (mFavoritesPending.exchange(false) && mKugou != nullptr) {
        if (mKugou->loadFavorites()) {
          // 只有拉取成功才覆盖缓存。失败时 tracks() 里仍是上一次搜索
          // 的结果，直接赋值会让红心列表变成搜索列表。
          mFavoriteList = mKugou->tracks();
          mRecCache[0] = mFavoriteList;
          if (mUi.currentTab() == 1 && mUi.currentRecTab() == 0) {
            const std::string keepId =
                mEngine->currentTrack() != nullptr ? mEngine->currentTrack()->id
                                                   : std::string();
            mUi.setListTracks(mRecCache[0]);
            mEngine->setQueueKeepCurrent(mRecCache[0], keepId);
            const musicmod::Track *cur = mEngine->currentTrack();
            mEngine->setQueueKeepCurrent(
                mRecCache[0], cur != nullptr ? cur->id : std::string());
          }
        }
        mUi.refresh(true);
      }
      {
        std::lock_guard<std::mutex> lock(mSearchMutex);
        if (mSearchPending) {
          mSearchPending = false;
          doSearch = true;
          keyword = mSearchKeyword;
          target = mSearchTarget;
        }
      }
      if (doSearch && mKugou != nullptr) {
        const bool daily = (target >= 2);
        // 搜索页的非单曲分类：走 searchByKind，结果不进歌曲列表。
        if (!daily && target == 0 && searchKind != 0) {
          const musicmod::SearchKind kind =
              (searchKind == 1) ? musicmod::SearchKind::Album
                                : musicmod::SearchKind::Artist;
          const bool kOk = mKugou->searchByKind(kind, keyword);
          if (kOk) {
            if (kind == musicmod::SearchKind::Album) {
              mUi.setSearchAlbums(mKugou->albums());
              __android_log_print(ANDROID_LOG_INFO, kTag,
                                  "搜索分类=专辑 命中 %zu",
                                  mKugou->albums().size());
            } else {
              mUi.setSearchArtists(mKugou->artists());
              __android_log_print(ANDROID_LOG_INFO, kTag,
                                  "搜索分类=歌手 命中 %zu",
                                  mKugou->artists().size());
            }
          } else {
            // 失败时清空对应缓存，避免旧结果留在屏幕上误导玩家。
            if (kind == musicmod::SearchKind::Album) {
              mUi.setSearchAlbums({});
            } else {
              mUi.setSearchArtists({});
            }
            mUi.notify("搜索失败: " + mKugou->lastError(), 3000);
            __android_log_print(ANDROID_LOG_WARN, kTag,
                                "分类搜索失败 kind=%d err=%s", searchKind,
                                mKugou->lastError().c_str());
          }
        } else {
        const bool ok =
            daily ? mKugou->loadDailyRecommend(target - 2)
                  : mKugou->search(keyword);
        if (ok) {
          auto list = mKugou->tracks();
          if (target == 0) {
            mSearchList = std::move(list);
            if (mUi.currentTab() == 0) {
              mUi.setListTracks(mSearchList);
              const musicmod::Track *cur = mEngine->currentTrack();
              mEngine->setQueueKeepCurrent(
                  mSearchList, cur != nullptr ? cur->id : std::string());
            }
          } else {
            mRecommendList = std::move(list);
            // 按目标子页写入对应缓存，切页时不再互相污染。
            // target 2 是「探索」、3 是「小众」，对应缓存槽 1 和 2。
            const int sub = std::max(0, std::min(2, target - 1));
            mRecCache[sub] = mRecommendList;
            if (mUi.currentTab() == 1 && mUi.currentRecTab() == sub) {
              mUi.setListTracks(mRecCache[sub]);
              const musicmod::Track *cur = mEngine->currentTrack();
              mEngine->setQueueKeepCurrent(
                  mRecCache[sub], cur != nullptr ? cur->id : std::string());
            }
          }
        } else {
          __android_log_print(ANDROID_LOG_WARN, kTag, "加载失败: %s",
                              daily ? "每日推荐" : keyword.c_str());
        }
        }
        mUi.refresh(true);
      }

      // 登录页动作：发码或提交登录，都在工作线程里执行。
      int loginAction = 0;
      {
        std::lock_guard<std::mutex> lock(mLoginMutex);
        loginAction = mLoginAction;
        mLoginAction = 0;
      }
      if (loginAction != 0 && mKugou != nullptr) {
        const std::string phone = mUi.loginPhone();
        const std::string code = mUi.loginCode();
        __android_log_print(ANDROID_LOG_INFO, kTag,
                            "登录动作=%d 手机号=%zu位 验证码=%zu位", loginAction,
                            phone.size(), code.size());
        if (phone.size() != 11 || phone[0] != '1') {
          mUi.setSmsHint("手机号需 11 位");
          mUi.notify("手机号格式不对", 3000);
        } else if (loginAction == 1) {
          mUi.setSmsSendState(1, "发送中");
          const bool ok = mKugou->loginSmsSend(phone);
          if (ok) {
            mUi.setSmsSendState(2, "验证码已发送");
            mUi.notify("验证码已发送", 3000);
          } else {
            mUi.setSmsSendState(3, "发送失败: " + mKugou->lastError());
            mUi.notify("发送失败: " + mKugou->lastError(), 4000);
          }
        } else if (code.empty()) {
          mUi.setSmsHint("验证码未填写");
          mUi.notify("验证码未填写", 3000);
        } else {
          const bool ok = mKugou->loginSmsSubmit(phone, code);
          if (ok) {
            mUi.setSmsHint("登录成功");
            mUi.notify("登录成功", 3000);
            mUi.setAccountState(true, mKugou->userId());
            mUi.closeLogin();
          } else {
            // 失败原因直接贴在登录页标题旁，不用等提示条消失。
            mUi.setSmsHint("登录失败: " + mKugou->lastError());
            mUi.notify("登录失败: " + mKugou->lastError(), 4000);
          }
        }
      }
      // 退出登录请求。
      if (mUi.takeLogoutRequest() != 0) {
        if (mKugou != nullptr) {
          mKugou->logout();
        }
        mUi.setAccountState(false, std::string());
        mUi.clearAvatar();
        mUi.closeLogin();
        mUi.notify("已退出登录", 3000);
      }
      // 每次启动模组检查一次当日会员有没有领。
      // 只在已登录时做，且每个进程只尝试一次，避免反复打接口。
      // 接口本身幂等：当天已领过会返回「今日已签到」，不会重复发放。
      static bool vipAutoChecked = false;
      if (!vipAutoChecked && mKugou != nullptr && mKugou->loggedIn()) {
        vipAutoChecked = true;
        std::string message;
        const bool ok = mKugou->claimVip(message);
        __android_log_print(ANDROID_LOG_INFO, kTag,
                            "启动自检会员: ok=%d %s", ok ? 1 : 0,
                            message.c_str());
        // 只有确实领到了才提示，已领过不打扰玩家。
        if (ok && message.find("签到成功") != std::string::npos) {
          mUi.notify("今日会员已自动领取", 3000);
        }
      }
      // 领取一天会员。
      if (mUi.takeVipRequest() != 0) {
        if (mKugou != nullptr) {
          std::string message;
          const bool ok = mKugou->claimVip(message);
          mUi.notify(ok ? "领取成功: " + message : "领取失败: " + message,
                     4000);
          __android_log_print(ANDROID_LOG_INFO, kTag, "领取会员: ok=%d %s",
                              ok ? 1 : 0, message.c_str());
        }
      }
      mUi.processPending(*mEngine);
      // 扫码登录：界面点「登录」或「刷新二维码」时开始，之后每两秒查一次状态。
      const int qrReq = mUi.takeQrRequest();
      if (qrReq != 0) {
        // 2 = 用户手动点刷新；1 = 过期自动刷新。
        startQrLogin(qrReq == 2);
      }
      pollQrLogin(std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
      const auto refreshStart = std::chrono::steady_clock::now();
      mUi.refresh(syncEngine);
      const auto refreshCost =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - refreshStart).count();
      if (refreshCost > 100) {
        __android_log_print(ANDROID_LOG_WARN, kTag, "刷新耗时 %lld 毫秒",
                            static_cast<long long>(refreshCost));
      }
      // 每 5 秒打一次心跳。卡住时心跳停在哪个区间，就能判断是工作线程停了，
      // 还是已经进入循环但卡在刷新里。
      ++beatTicks;
      const auto beatNow = std::chrono::steady_clock::now();
      if (beatNow - lastBeat >= std::chrono::seconds(5)) {
        // 这里的第二个数字是「单次 refresh() 耗时毫秒数」，不是次数。
        // 显示 0 表示刷新很快（<1ms），属正常。原标签「刷新」容易误读成次数。
        __android_log_print(ANDROID_LOG_INFO, kTag,
                            "心跳 ticks=%d 刷新耗时=%lldms", beatTicks,
                            static_cast<long long>(refreshCost));
        beatTicks = 0;
        lastBeat = beatNow;
      }
    }

    if (mEngine) {
      mEngine->stop();
    }
    __android_log_print(ANDROID_LOG_INFO, kTag, "播放已停止");
  }

// 二维码图片存成本地文件，再走系统解码，和封面同一条路径。
  static bool writeQrImage(const std::string &image, const std::string &path) {
    std::vector<unsigned char> bytes;
    const std::string marker = "base64,";
    const auto at = image.find(marker);
    if (at != std::string::npos) {
      bytes = base64Decode(image.substr(at + marker.size()));
    } else if (image.rfind("http", 0) == 0) {
      __android_log_print(ANDROID_LOG_WARN, kTag, "二维码返回的是链接: %.80s",
                          image.c_str());
      return false;
    } else {
      bytes = base64Decode(image);
    }
    if (bytes.empty()) {
      return false;
    }
    std::FILE *file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
      return false;
    }
    std::fwrite(bytes.data(), 1, bytes.size(), file);
    std::fclose(file);
    __android_log_print(ANDROID_LOG_INFO, kTag, "二维码图片已写入 %zu 字节",
                        bytes.size());
    return true;
  }

  // 二维码图片降到 200 像素以内再交给宿主，避免大图占用过多显存。
  static void shrinkRgba(std::vector<unsigned char> &rgba, int &width, int &height,
                         int limit) {
    if (width <= limit && height <= limit) {
      return;
    }
    const int dw = width < limit ? width : limit;
    const int dh = height < limit ? height : limit;
    std::vector<unsigned char> out(static_cast<std::size_t>(dw) * dh * 4);
    for (int y = 0; y < dh; ++y) {
      const int sy = y * height / dh;
      for (int x = 0; x < dw; ++x) {
        const int sx = x * width / dw;
        const std::size_t si =
            (static_cast<std::size_t>(sy) * width + sx) * 4;
        const std::size_t di = (static_cast<std::size_t>(y) * dw + x) * 4;
        out[di] = rgba[si];
        out[di + 1] = rgba[si + 1];
        out[di + 2] = rgba[si + 2];
        out[di + 3] = rgba[si + 3];
      }
    }
    rgba.swap(out);
    width = dw;
    height = dh;
  }

  // 把「端口 + 当前 key + 状态」落盘。日志会滚动，端口一丢就没法独立验证，
  // 落一份到数据目录后，任何时刻都能读到当前登录卡在哪一步。
  void writeQrState(const char *stage, int status) {
    if (mDataDir.empty()) {
      return;
    }
    std::ofstream out(mDataDir + "/qr_state.json");
    if (!out) {
      return;
    }
    const int port = (mKugou != nullptr) ? mKugou->port() : 0;
    const long long ts = static_cast<long long>(std::time(nullptr));
    out << "{\n";
    out << "  \"port\": " << port << ",\n";
    out << "  \"key\": \"" << mQrKey << "\",\n";
    out << "  \"url\": \"https://h5.kugou.com/apps/loginQRCode/html/index.html"
           "?appid=3116&qrcode="
        << mQrKey << "\",\n";
    out << "  \"status\": " << status << ",\n";
    out << "  \"stage\": \"" << stage << "\",\n";
    out << "  \"fail\": " << mQrFailCount << ",\n";
    out << "  \"ts\": " << ts << "\n";
    out << "}\n";
  }

  // manual=true 表示用户主动点了「刷新二维码」，此时不受防抖限制，
  // 否则会出现「点了刷新但界面没反应」的假死。
  void startQrLogin(bool manual = false) {
    if (!mKugou) {
      mUi.setQrStatus("音源未就绪");
      return;
    }
    const long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count();
    // 自动刷新（过期重试）才做四秒防抖：避免刚生成就被换掉，
    // 扫到旧 key 导致页面空白。用户手动点击一律放行。
    if (!manual && !mQrKey.empty() && nowMs - mQrLastStartMs < 4000) {
      __android_log_print(ANDROID_LOG_INFO, kTag,
                          "忽略重复刷新 距上次 %lld 毫秒",
                          static_cast<long long>(nowMs - mQrLastStartMs));
      return;
    }
    mQrLastStartMs = nowMs;
    mQrKey.clear();
    mQrNextMs = 0;
    mQrStartMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now().time_since_epoch())
                     .count();
    mUi.clearQr();
    mUi.setQrStatus("正在获取官方二维码");
    const std::string path = mDataDir + "/qr.png";
    for (int attempt = 0; attempt < 2; ++attempt) {
      std::string key;
      std::string image;
      if (!mKugou->loginQrStart(key, image)) {
        __android_log_print(ANDROID_LOG_WARN, kTag, "官方二维码第 %d 次获取失败",
                            attempt + 1);
        continue;
      }
      // 原图落盘只为了排查；服务只给 key 时就不落盘。
      std::vector<unsigned char> rgba;
      int width = 0;
      int height = 0;
      bool shown = false;
      if (!image.empty() && writeQrImage(image, path) &&
          decodeCover(mJavaVm, path, rgba, width, height)) {
        shown = mUi.uploadQr(rgba, width, height);
      }
      if (!shown) {
        // 服务没给图，或者图解不开时，用 key 自己拼标准登录链接生成。
        const std::string loginUrl =
            "https://h5.kugou.com/apps/loginQRCode/html/index.html?appid=3116"
            "&qrcode=" +
            key;
        shown = mUi.uploadQrUrl(loginUrl);
      }
      if (!shown) {
        continue;
      }
      mQrKey = key;
      mQrNextMs = 0;
      writeQrState("created", 0);
      static int qrSeq = 0;
      ++qrSeq;
      mUi.setQrStatus("官方二维码已生成 第 " + std::to_string(qrSeq) + " 次");
      return;
    }
    mUi.setQrStatus("二维码获取失败, 点刷新重试");
  }

  void pollQrLogin(long long now) {
    if (mQrKey.empty() || !mKugou || now < mQrNextMs) {
      return;
    }
    // 轮询最多三分钟，避免登录页长时间停在后台反复请求。
    if (now - mQrStartMs > 180000) {
      mQrKey.clear();
      mUi.setQrStatus("二维码已超时, 点刷新");
      return;
    }
    mQrNextMs = now + 2000;
    std::string token;
    std::string userId;
    const int status = mKugou->loginQrCheck(mQrKey, token, userId);
    // -1 表示请求或解析失败，之前会被当成“等待扫码”一直挂着，
    // 看起来像登录没反应。连续失败就明确提示，不再装作在等。
    if (status < 0) {
      ++mQrFailCount;
      writeQrState("poll_fail", -1);
      if (mQrFailCount >= 3) {
        mUi.setQrStatus("登录服务无响应, 点刷新重试");
      }
      return;
    }
    mQrFailCount = 0;
    writeQrState("status", status);
    // 酷狗扫码状态：1 等待扫描，2 已扫描待确认，4 确认成功，0 过期。
    if (status == 4) {
      mQrKey.clear();
      mQrRefreshCount = 0;
      mUi.setQrStatus("登录成功");
      mUi.notify("登录成功", 3000);
      // 登录成功后自动收起登录页，回到音乐库。
      mUi.setAccountState(true, mKugou->userId());
      mUi.closeLogin();
    } else if (status == 0) {
      mQrKey.clear();
      // 过期自动换一张，但最多换三次，避免无限循环。
      if (mQrRefreshCount < 3) {
        ++mQrRefreshCount;
        mUi.setQrStatus("二维码已过期, 自动刷新");
        mUi.requestQrRefresh();
      } else {
        mUi.setQrStatus("二维码已过期, 点刷新");
      }
    } else if (status == 2) {
      mUi.setQrStatus("已扫码, 请在手机上确认");
    } else if (status == 1) {
      mUi.setQrStatus("等待扫码");
    }
  }

  // 主线程侧：只做两件事——按需唤起后台线程、取出后台准备好的结果。
  // 这里绝不能有网络调用，一次 12 秒阻塞就能让歌词、世界检测全部失真。
  void refreshAvatar() {
    if (mKugou == nullptr || !mKugou->loggedIn()) {
      return;
    }
    // 还没有结果时唤起后台；已有结果就直接消费。
    if (!mAvatarBusy.load()) {
      bool hasResult = false;
      {
        std::lock_guard<std::mutex> lock(mAvatarMutex);
        hasResult = mAvatarHasNick || mAvatarHasFile;
      }
      if (hasResult) {
        std::string nickname;
        std::string path;
        {
          std::lock_guard<std::mutex> lock(mAvatarMutex);
          if (mAvatarHasNick) {
            nickname = mAvatarReadyNick;
            mAvatarHasNick = false;
          }
          if (mAvatarHasFile) {
            path = mAvatarReadyPath;
            mAvatarHasFile = false;
          }
        }
        if (!nickname.empty()) {
          mUi.setAccountState(true, nickname);
        }
        if (!path.empty()) {
          // 解码在本机做，不涉及网络，耗时约几毫秒。
          std::vector<unsigned char> rgba;
          int width = 0;
          int height = 0;
          if (decodeCover(mJavaVm, path, rgba, width, height)) {
            // 比例 0.5 = 正圆。输出 192 再缩到 48 显示，边缘足够干净。
            auto frames =
                makeRoundedCoverFrames(rgba, width, height, 192, 1, 0.5f);
            if (!frames.empty()) {
              mUi.uploadAvatar(frames.front(), 192, 192);
            }
          } else {
            __android_log_print(ANDROID_LOG_WARN, kTag, "头像解码失败: %s",
                                path.c_str());
          }
        }
        return;
      }
      // 没结果就排一次后台任务，不等待。
      mAvatarWanted.store(true);
    }
  }

  // 后台线程：缓慢重试，绝不占用主循环。
  void avatarLoop() {
    long long nextTryMs = 0;
    while (!mAvatarStop.load()) {
      // 每 200ms 醒一次，够快也够省。
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      if (mAvatarStop.load()) {
        break;
      }
      if (!mAvatarWanted.exchange(false)) {
        continue;
      }
      const long long now =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now().time_since_epoch())
              .count();
      if (nextTryMs != 0 && now < nextTryMs) {
        // 还没到时间，放回去下次再试。
        mAvatarWanted.store(true);
        continue;
      }
      if (mKugou == nullptr || !mKugou->loggedIn()) {
        nextTryMs = now + 5000;
        mAvatarWanted.store(true);
        continue;
      }
      mAvatarBusy.store(true);
      std::string nickname;
      std::string avatarUrl;
      const bool got = mKugou->fetchUserInfo(nickname, avatarUrl);
      std::string path;
      if (got && !avatarUrl.empty() && avatarUrl != mAvatarAppliedUrl) {
        const std::string candidate = mDataDir + "/avatar.jpg";
        if (mKugou->fetchAvatar(avatarUrl, candidate)) {
          mAvatarAppliedUrl = avatarUrl;
          path = candidate;
        }
      }
      mAvatarBusy.store(false);
      if (!got) {
        // 失败退避 30 秒，避免频繁打接口。
        nextTryMs = now + 30000;
        mAvatarWanted.store(true);
        __android_log_print(ANDROID_LOG_WARN, kTag,
                            "账号资料未取到, 30s 后重试");
        continue;
      }
      // 成功：10 分钟后再查一次，昵称改动能跟上。
      nextTryMs = now + 600000;
      {
        std::lock_guard<std::mutex> lock(mAvatarMutex);
        if (!nickname.empty()) {
          mAvatarReadyNick = nickname;
          mAvatarHasNick = true;
        }
        if (!path.empty()) {
          mAvatarReadyPath = path;
          mAvatarHasFile = true;
        }
      }
    }
  }

  // 歌词线程：慢一次只影响它自己，主循环始终稳定。
  // 失败按 3s / 8s / 20s 退避，最多 3 次；拿到非空歌词就不再试。
  // 换歌时目标 id 变了，计数清零重新开始。
  void lyricLoop() {
    long long nextTryMs = 0;
    int attempts = 0;
    std::string curId;
    while (!mLyricStop.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(150));
      if (mLyricStop.load()) {
        break;
      }
      musicmod::Track target;
      bool hasTarget = false;
      {
        std::lock_guard<std::mutex> lock(mLyricMutex);
        if (!mLyricReqId.empty()) {
          target = mLyricReqTrack;
          hasTarget = true;
        }
      }
      if (!hasTarget) {
        attempts = 0;
        curId.clear();
        nextTryMs = 0;
        continue;
      }
      if (target.id != curId) {
        curId = target.id;
        attempts = 0;
        nextTryMs = 0;
      }
      if (attempts >= 3) {
        continue;
      }
      const long long now =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now().time_since_epoch())
              .count();
      if (nextTryMs != 0 && now < nextTryMs) {
        continue;
      }
      if (mKugou == nullptr) {
        continue;
      }
      const int attemptNo = ++attempts;
      std::string text;
      {
        // 与封面线程共用一把网络锁：KugouProvider 的成员不是线程安全的。
        std::lock_guard<std::mutex> netLock(mNetMutex);
        text = mKugou->lyric(target);
      }
      if (!text.empty()) {
        {
          std::lock_guard<std::mutex> lock(mLyricMutex);
          // 目标已经被换掉就直接扔，不交付。
          if (mLyricReqId == target.id) {
            mLyricReadyId = target.id;
            mLyricReadyText = text;
            mLyricHasResult = true;
          }
        }
        attempts = 3;
        __android_log_print(ANDROID_LOG_INFO, kTag,
                            "歌词已获取 第%d次 %.16s", attemptNo,
                            target.id.c_str());
      } else {
        const long long backoffMs =
            (attemptNo == 1) ? 3000 : (attemptNo == 2 ? 8000 : 20000);
        nextTryMs = now + backoffMs;
        __android_log_print(ANDROID_LOG_WARN, kTag,
                            "歌词为空 第%d次 %.16s 下次重试 %lldms后", attemptNo,
                            target.id.c_str(), backoffMs);
      }
    }
  }

  // 缓存文件能不能直接复用。只看大小不够：HTML 错误页也可能上千字节。
  // 必须验文件头，否则坏缓存会被永久复用，封面再也不换。
  static bool coverFileUsable(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
      return false;
    }
    unsigned char head[12] = {0};
    in.read(reinterpret_cast<char *>(head), static_cast<std::streamsize>(sizeof(head)));
    if (in.gcount() < static_cast<std::streamsize>(sizeof(head))) {
      return false;
    }
    if (head[0] == 0xFF && head[1] == 0xD8 && head[2] == 0xFF) {
      return true;
    }
    if (head[0] == 0x89 && head[1] == 'P' && head[2] == 'N' && head[3] == 'G') {
      return true;
    }
    if (head[0] == 'G' && head[1] == 'I' && head[2] == 'F') {
      return true;
    }
    if (head[0] == 'R' && head[1] == 'I' && head[2] == 'F' && head[3] == 'F' &&
        head[8] == 'W' && head[9] == 'E' && head[10] == 'B' && head[11] == 'P') {
      return true;
    }
    return false;
  }

  // 封面线程：只负责下载文件，不碰 UI。
  // 失败按 2s / 5s / 12s 退避，最多 3 次；成功即止。
  // 换歌时目标变了，计数清零重新开始。
  void coverLoop() {
    std::string curId;
    int attempts = 0;
    long long nextTryMs = 0;
    while (!mCoverStop.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(150));
      if (mCoverStop.load()) {
        break;
      }
      musicmod::Track target;
      std::string path;
      bool hasTarget = false;
      {
        std::lock_guard<std::mutex> lock(mCoverMutex);
        if (!mCoverReqId.empty()) {
          target = mCoverReqTrack;
          path = mCoverReqPath;
          hasTarget = true;
        }
      }
      if (!hasTarget) {
        curId.clear();
        attempts = 0;
        nextTryMs = 0;
        continue;
      }
      if (target.id != curId) {
        curId = target.id;
        attempts = 0;
        nextTryMs = 0;
      }
      if (attempts >= 3) {
        continue;
      }
      // 本地已经有【有效图片】才复用。只判断大小会把 HTML 错误页当成封面。
      {
        if (coverFileUsable(path)) {
          {
            std::lock_guard<std::mutex> lock(mCoverMutex);
            if (mCoverReqId == target.id) {
              mCoverDoneId = target.id;
              mCoverDonePath = path;
            }
          }
          attempts = 3;
          mUi.coverLog("线程: 复用本地缓存 %.16s", target.id.c_str());
          __android_log_print(ANDROID_LOG_INFO, kTag, "封面复用本地缓存 %.16s",
                              target.id.c_str());
          continue;
        }
      }
      const long long now =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now().time_since_epoch())
              .count();
      if (nextTryMs != 0 && now < nextTryMs) {
        continue;
      }
      if (mKugou == nullptr) {
        continue;
      }
      const int attemptNo = ++attempts;
      bool ok = false;
      {
        std::lock_guard<std::mutex> netLock(mNetMutex);
        ok = mKugou->fetchCover(target, path);
      }
      if (ok) {
        {
          std::lock_guard<std::mutex> lock(mCoverMutex);
          // 目标已被换掉就丢弃，不交付。
          if (mCoverReqId == target.id) {
            mCoverDoneId = target.id;
            mCoverDonePath = path;
          }
        }
        attempts = 3;
        mUi.coverLog("线程: 下载成功 第%d次 %.16s", attemptNo,
                     target.id.c_str());
        __android_log_print(ANDROID_LOG_INFO, kTag, "封面已下载 第%d次 %.16s",
                            attemptNo, target.id.c_str());
      } else {
        const long long backoffMs =
            (attemptNo == 1) ? 2000 : (attemptNo == 2 ? 5000 : 12000);
        nextTryMs = now + backoffMs;
        mUi.coverLog("线程: 下载失败 第%d次 %.16s 退避%lldms", attemptNo,
                     target.id.c_str(), backoffMs);
        __android_log_print(ANDROID_LOG_WARN, kTag,
                            "封面下载失败 第%d次 %.16s 下次重试 %lldms后",
                            attemptNo, target.id.c_str(), backoffMs);
      }
    }
  }

  void refreshCover() {
    if (!mEngine) {
      return;
    }
    // 第一步：把封面线程下载好的图贴上去。
    // decode 和上传都要碰 JNI / 游戏线程，只能在这里做。
    std::string doneId;
    std::string donePath;
    {
      std::lock_guard<std::mutex> lock(mCoverMutex);
      if (!mCoverDoneId.empty()) {
        doneId = mCoverDoneId;
        donePath = mCoverDonePath;
        mCoverDoneId.clear();
        mCoverDonePath.clear();
      }
    }
    if (!doneId.empty()) {
      const musicmod::Track *nowTrack = mEngine->currentTrack();
      mUi.coverLog("落地: doneId=%.16s 当前=%.16s %s", doneId.c_str(),
                   nowTrack != nullptr ? nowTrack->id.c_str() : "(空)",
                   (nowTrack != nullptr && nowTrack->id == doneId) ? "匹配"
                                                                   : "不匹配");
      if (nowTrack != nullptr && nowTrack->id == doneId) {
        std::vector<unsigned char> rgba;
        int width = 0;
        int height = 0;
        if (decodeCover(mJavaVm, donePath, rgba, width, height)) {
          auto frames = makeRoundedCoverFrames(rgba, width, height, 256, 1);
          __android_log_print(ANDROID_LOG_INFO, kTag, "封面上传 帧=%zu 源=%dx%d",
                              frames.size(), width, height);
          if (!mUi.uploadCoverFrames(frames, 256, 256)) {
            mUi.coverLog("落地: 上传返回失败 %.16s", doneId.c_str());
          }
        } else {
          mUi.coverLog("落地: 解码失败, 删缓存 %.16s", doneId.c_str());
          __android_log_print(ANDROID_LOG_WARN, kTag,
                              "封面解码失败, 保留旧图: %s", donePath.c_str());
          // 文件坏了立刻删掉。留着的话下次切回来又会被当成可用缓存，
          // 于是这首歌的封面永远不换 —— 就是这个 bug 最隐蔽的一环。
          std::remove(donePath.c_str());
        }
      } else {
        __android_log_print(ANDROID_LOG_INFO, kTag,
                            "封面丢弃(期间切歌): %.16s -> %.16s", doneId.c_str(),
                            nowTrack != nullptr
                                ? nowTrack->id.substr(0, 16).c_str()
                                : "(空)");
      }
    }

    // 第二步：检测换歌，给封面线程下发下载任务。
    const musicmod::Track *track = mEngine->currentTrack();
    if (track == nullptr) {
      if (!mLastCoverKey.empty()) {
        mLastCoverKey.clear();
        mUi.clearCover();
      }
      return;
    }
    if (track->id == mLastCoverKey) {
      return;
    }
    mLastCoverKey = track->id;
    mUi.coverLog("换歌: %.16s 本地图=%d", track->id.c_str(),
                 track->coverPath.empty() ? 0 : 1);
    __android_log_print(ANDROID_LOG_INFO, kTag, "封面切换 id=%.40s 有本地图=%d",
                        track->id.c_str(), track->coverPath.empty() ? 0 : 1);
    if (!track->coverPath.empty()) {
      // 曲目自带本地图：直接解码上传，不联网。
      std::vector<unsigned char> rgba;
      int width = 0;
      int height = 0;
      if (decodeCover(mJavaVm, track->coverPath, rgba, width, height)) {
        auto frames = makeRoundedCoverFrames(rgba, width, height, 256, 1);
        mUi.uploadCoverFrames(frames, 256, 256);
      } else {
        __android_log_print(ANDROID_LOG_WARN, kTag,
                            "封面解码失败, 保留旧图: %s",
                            track->coverPath.c_str());
      }
      return;
    }
    // 交给封面线程下载。主循环绝不能再碰这个网络请求。
    {
      std::lock_guard<std::mutex> lock(mCoverMutex);
      mCoverReqPath = mDataDir + "/cover_" +
                      (track->id.empty() ? "unknown" : track->id) + ".jpg";
      mCoverReqTrack = *track;
      mCoverReqId = track->id;
    }
  }

  static bool isGameLibraryMapped() {
    // dlopen 成功会持有引用计数，必须成对 dlclose。
    void *handle = dlopen("libminecraftpe.so", RTLD_NOW | RTLD_NOLOAD);
    if (handle == nullptr) {
      return false;
    }
    dlclose(handle);
    return true;
  }

  JavaVM *mJavaVm{};
  std::string mDataDir;
  std::string mModDirName;
  // 模组目录之外的镜像目录，用于防更新覆盖。
  std::string mBackupDir;
  // 上一次同步给引擎的音量，避免每拍重复设置。
  float mLastAppliedVolume{-1.0f};
  // 保存后同步镜像，防止更新覆盖丢数据。
  std::function<void()> mMirrorOut;
  std::string mLastCoverKey;
  // 封面线程状态。下载在后台做，主循环只负责解码和上传。
  std::thread mCoverWorker;
  std::atomic<bool> mCoverStop{false};
  std::mutex mCoverMutex;
  musicmod::Track mCoverReqTrack;  // 要下封面的曲目副本
  std::string mCoverReqPath;       // 下载到哪个本地文件
  std::string mCoverReqId;         // 对应曲目 id
  std::string mCoverDoneId;        // 下载完成的曲目 id
  std::string mCoverDonePath;      // 完成后的本地文件
  // 歌词线程和封面线程都要打服务库，串行化避免并发写 KugouProvider。
  std::mutex mNetMutex;
  // 头像拉取全部在后台线程做。
  // 原因：httpGet 是阻塞 socket，超时 12 秒。v39 把它放在主循环里同步调用，
  // 一次失败就让主循环停摆十几秒——歌词行号不推进、世界检测失准、
  // 退出世界不暂停，全都由此而来。主线程碰网络是根因，必须搬走。
  std::thread mAvatarWorker;
  std::atomic<bool> mAvatarStop{false};
  std::atomic<bool> mAvatarBusy{false};
  std::atomic<bool> mAvatarWanted{false};
  // 后台线程写、主线程读，用互斥锁保护。
  std::mutex mAvatarMutex;
  std::string mAvatarReadyNick;
  std::string mAvatarReadyPath;
  bool mAvatarHasNick{false};
  bool mAvatarHasFile{false};
  // 上次已经处理过的地址，避免同一张图反复解码上传。
  std::string mAvatarAppliedUrl;
  std::string mLastLyricId;
  // 歌词重试状态。
  // 原来只有「id 变了才拉一次，拉失败也记成已拉过」，
  // 于是网络抖一下、或者接口偶发返回空，那首歌就永久显示「暂无歌词」，
  // 而实际上它有歌词 —— 这就是「绝对是歌词问题」的那个现象。
  // 现在：失败后按 3s / 8s / 20s 退避重试，最多 3 次；
  // 成功（拿到非空歌词）后不再重试。
  // 歌词线程状态。歌词的网络请求一律在这个线程里做，主循环只交换数据。
  // 复用头像线程的模式：stop 标志 + 互斥保护的交接区。
  std::thread mLyricWorker;
  std::atomic<bool> mLyricStop{false};
  std::mutex mLyricMutex;
  musicmod::Track mLyricReqTrack;   // 当前要拉的曲目副本
  std::string mLyricReqId;          // 对应曲目 id，换歌就改它
  std::string mLyricReadyId;        // 线程拉回来的曲目 id
  std::string mLyricReadyText;      // 线程拉回来的歌词正文
  bool mLyricHasResult{false};
  // 扫码登录状态：当前二维码 key 与下次轮询时间。
  std::string mQrKey;
  long long mQrNextMs{0};
  // 二维码会话开始时间与自动刷新次数，用来限制轮询时长，避免长时间占用。
  long long mQrStartMs{0};
  int mQrRefreshCount{0};
  int mQrFailCount{0};
  // 上一次真正生成二维码的时间，用来挡住重复的刷新请求。
  long long mQrLastStartMs{0};
  std::unique_ptr<musicmod::PlaylistProvider> mProvider;
  std::unique_ptr<musicmod::KugouProvider> mKugou;
  std::unique_ptr<musicmod::AudioEngine> mEngine;
  musicmod::HudUi mUi;
  musicmod::RenderHook mRenderHook;

  std::atomic<bool> mRunning{false};
  std::mutex mExitMutex;
  std::mutex mLoginMutex;
  int mLoginAction{0};
  std::condition_variable mExitCv;
  std::thread mWorker;
  std::mutex mSearchMutex;
  std::string mSearchKeyword;
  std::string mLastKeyword;
  int mSearchTarget{0};
  std::vector<musicmod::Track> mSearchList;
  std::vector<musicmod::Track> mRecommendList;
  std::vector<musicmod::Track> mFavoriteList;
  // 收藏内容变过，下次进收藏页要硬刷。
  bool mFavListDirty{false};
  // 推荐三个子页各自缓存，避免切页时互相覆盖。
  // 索引：0 红心，1 探索，2 小众。
  std::vector<musicmod::Track> mRecCache[3];
  std::string mReportedId;
  musicmod::Track mReportedTrack;
  long long mReportStartedMs{0};
  bool mSearchPending{false};
  std::atomic<bool> mFavoritesPending{false};
};

MusicMod &sharedMod() {
  static MusicMod mod;
  return mod;
}

} // namespace

PL_REGISTER_MOD(MusicMod, sharedMod())