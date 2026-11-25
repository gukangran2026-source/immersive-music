#include "KugouProvider.hpp"
#include <android/log.h>
#include <cstdio>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <vector>
#include <sys/stat.h>
#include <dlfcn.h>

#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>

#include "HttpClient.hpp"

namespace musicmod {
namespace {

constexpr const char *kTag = "MusicMod";

// 官方登录接口的网页签名盐。
constexpr const char *kWebRoute = "NVPh5oo715z5DIWAeQlhMDsWXXQV4hwt";

long long nowSeconds() {
  return static_cast<long long>(std::time(nullptr));
}

// 按官方 web 规则签名：md5(盐 + 排序后的 k=v 拼接 + 盐)。
std::string signedWebUrl(const std::string &host, const std::string &path,
                         std::map<std::string, std::string> &params) {
  std::string joined;
  for (const auto &item : params) {
    joined += item.first + "=" + item.second;
  }
  params["signature"] = md5Hex(std::string(kWebRoute) + joined + kWebRoute);
  std::string url = "https://" + host + path + "?";
  bool first = true;
  for (const auto &item : params) {
    if (!first) {
      url += "&";
    }
    first = false;
    url += urlEncode(item.first) + "=" + urlEncode(item.second);
  }
  return url;
}

using StartServerFn = int (*)(int port, const char *dataDir);
using StopServerFn = void (*)();
using IsRunningFn = int (*)();
using GetPortFn = int (*)();

// 在任意 JSON 里找第一个 http 链接，兼容不同版本返回结构。
const nlohmann::json *findFirstUrl(const nlohmann::json &node,
                                   std::string &out) {
  if (node.is_string()) {
    const std::string &value = node.get_ref<const std::string &>();
    if (value.rfind("http", 0) == 0 && value.size() > 12) {
      out = value;
      return &node;
    }
    return nullptr;
  }
  if (node.is_array()) {
    for (const auto &item : node) {
      if (findFirstUrl(item, out) != nullptr) {
        return &node;
      }
    }
    return nullptr;
  }
  if (node.is_object()) {
    for (auto it = node.begin(); it != node.end(); ++it) {
      if (findFirstUrl(it.value(), out) != nullptr) {
        return &node;
      }
    }
  }
  return nullptr;
}

// 搜索结果的曲目数组字段在不同版本里叫 lists / info / data。
const nlohmann::json *pickTrackArray(const nlohmann::json &root) {
  if (!root.is_object()) {
    return nullptr;
  }
  const nlohmann::json *data = nullptr;
  if (root.contains("data")) {
    data = &root["data"];
  } else {
    data = &root;
  }

  if (data->is_array()) {
    return data;
  }
  if (!data->is_object()) {
    return nullptr;
  }
  for (const char *key : {"lists", "info", "list", "songs"}) {
    if (data->contains(key) && (*data)[key].is_array()) {
      return &(*data)[key];
    }
  }
  return nullptr;
}

// 酷狗返回的文本带 <em> 高亮标签和 HTML 实体，直接丢给渲染会变成方块。
// 红心歌单（/playlist/track/all/new）返回的 name 字段其实是【文件名】，
// 形如 "旧北乐队 - 灿.mp3"、"林俊杰、农夫 - 由你选择.mp3"。
// 直接拿它当歌名有三个后果：
//   1. 歌名末尾带着 ".mp3" 显示出来，看着像格式标识；
//   2. 拿 "旧北乐队 - 灿.mp3" 去搜歌词，关键词根本不匹配，全部搜不到；
//   3. 封面接口按这个名字也对不上。
// 所以这里统一剥掉扩展名和 "歌手 - " 前缀。
std::string stripAudioName(const std::string &input) {
  std::string text = input;
  // 去掉常见音频扩展名（大小写都处理）。
  static const char *const kExts[] = {".mp3", ".flac", ".ape", ".m4a",
                                      ".wav", ".ogg", ".aac", ".wma"};
  for (const char *ext : kExts) {
    const std::size_t len = std::strlen(ext);
    if (text.size() <= len) {
      continue;
    }
    const std::size_t at = text.size() - len;
    bool same = true;
    for (std::size_t i = 0; i < len; ++i) {
      if (std::tolower(static_cast<unsigned char>(text[at + i])) !=
          std::tolower(static_cast<unsigned char>(ext[i]))) {
        same = false;
        break;
      }
    }
    if (same) {
      text.erase(at);
      break;
    }
  }
  // 去掉 "歌手 - 歌名" 里的前缀。分隔符兼容 "-"、"–"、"—_ "。
  // 只在左侧不长（<48 字节）且右侧非空时才切，避免误伤歌名自带的横杠。
  const std::size_t sep = text.find(" - ");
  if (sep != std::string::npos && sep > 0 && sep < 48 &&
      sep + 3 < text.size()) {
    text = text.substr(sep + 3);
  }
  // 去首尾空白。
  const std::size_t first = text.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return {};
  }
  const std::size_t last = text.find_last_not_of(" \t\r\n");
  return text.substr(first, last - first + 1);
}

std::string cleanText(const std::string &input) {
  std::string out;
  out.reserve(input.size());
  for (std::size_t i = 0; i < input.size();) {
    if (input.compare(i, 4, "<em>") == 0) {
      i += 4;
      continue;
    }
    if (input.compare(i, 5, "</em>") == 0) {
      i += 5;
      continue;
    }
    if (input.compare(i, 4, "&amp;") == 0) {
      out.push_back('&');
      i += 5;
      continue;
    }
    if (input.compare(i, 6, "&#39;") == 0) {
      out.push_back('\'');
      i += 6;
      continue;
    }
    if (input.compare(i, 4, "&lt;") == 0) {
      out.push_back('<');
      i += 4;
      continue;
    }
    if (input.compare(i, 4, "&gt;") == 0) {
      out.push_back('>');
      i += 4;
      continue;
    }
    if (input.compare(i, 6, "&nbsp;") == 0) {
      out.push_back(' ');
      i += 6;
      continue;
    }
    const unsigned char byte = static_cast<unsigned char>(input[i]);
    // 丢掉控制字符，避免渲染出异常字形。
    if (byte < 0x20 && byte != '\t') {
      ++i;
      continue;
    }
    out.push_back(input[i]);
    ++i;
  }
  return out;
}

std::string jsonString(const nlohmann::json &item, const char *key) {
  if (!item.is_object() || !item.contains(key)) {
    return {};
  }
  const auto &value = item[key];
  if (value.is_string()) {
    return value.get<std::string>();
  }
  if (value.is_number_integer()) {
    return std::to_string(value.get<long long>());
  }
  return {};
}

// 本 so 所在目录。Android 只允许在应用自己的库目录里 dlopen，
// 所以要把额外组件复制到这里再加载。
// 生成 32 位十六进制设备标识。
std::string randomGuid() {
  static const char *kHex = "0123456789ABCDEF";
  std::string out;
  out.reserve(32);
  for (int i = 0; i < 32; ++i) {
    out.push_back(kHex[rand() % 16]);
  }
  return out;
}

// 自身 .so 的完整路径，用 dladdr 反查；失败返回空串。
std::string selfPath() {
  Dl_info info{};
  if (dladdr(reinterpret_cast<void *>(&selfPath), &info) == 0 ||
      info.dli_fname == nullptr) {
    return {};
  }
  return info.dli_fname;
}

std::string selfDir() {
  const std::string path = selfPath();
  if (path.empty()) {
    return {};
  }
  const std::size_t slash = path.find_last_of('/');
  if (slash == std::string::npos) {
    return {};
  }
  return path.substr(0, slash);
}

// 在任意层级查找同名字段，供封面接口使用。
const nlohmann::json *findJsonNode(const nlohmann::json &node,
                                   const char *key) {
  if (node.is_object()) {
    const auto it = node.find(key);
    if (it != node.end()) {
      return &(*it);
    }
    for (auto iter = node.begin(); iter != node.end(); ++iter) {
      if (const auto *found = findJsonNode(iter.value(), key)) {
        return found;
      }
    }
  } else if (node.is_array()) {
    for (const auto &item : node) {
      if (const auto *found = findJsonNode(item, key)) {
        return found;
      }
    }
  }
  return nullptr;
}

} // namespace

// ── 运行时库（酷狗服务）的加载方式 ──────────────────────────────
// 核心约束：Android 14 起**不允许从可写位置 dlopen 库**。
// 模组目录在外部存储（Android/media 或 Android/data），属可写位置，
// 直接从那里 dlopen 会被 linker 以「namespace 不可访问」拒绝，
// 所以服务库必须先落到应用私有目录（/data/data/<pkg>/cache/...）。
//
// 两种来源，按优先级：
//  ①内嵌（默认）：build 时 objcopy 把 libkugou_server.so 转成
//    embed/libkugou_blob.o 一起链接进来，运行时分块写出。
//    单文件自包含，导包不会因为缺附加文件而失效——首选。
//  ②runtime.zip 兜底：mod 目录里若放了 runtime.zip（STORED 格式），
//    就优先用它，方便不改主 so 直接换服务库。
//
// 绝不要用「追加到主 so 尾部」的方式：实测那种形态会被拒绝加载，
// 表现为面板完全不出现（但日志里音乐逻辑还在跑，极易误判）。
//
// 落盘目录：优先 selfDir()（主 so 所在目录）。LeviLauncher 会把 mod
// 复制到 /data/data/<pkg>/cache/native_mods/... 再加载，那正是私有目录。

constexpr const char *kRuntimeZipName = "runtime.zip";

namespace {
// 去掉 runtime/ 前缀：旧打包器会给内层条目加这层前缀。
std::string stripRuntimePrefix(const std::string &name) {
  static const std::string kPre = "runtime/";
  if (name.rfind(kPre, 0) == 0) {
    return name.substr(kPre.size());
  }
  return name;
}

// 逐条解析 STORED 的 zip 并落盘。构建时用 ZIP_STORED 打包，
// 所以这里不需要 inflate，避免为此把 zlib 拉进工程。
// 返回写出的文件数，负数表示格式错误。
int extractStoredZip(const unsigned char *z, std::size_t n,
                     const std::string &dest) {
  std::size_t p = 0;
  int count = 0;
  while (p + 30 <= n) {
    if (!(z[p] == 'P' && z[p + 1] == 'K' && z[p + 2] == 3 && z[p + 3] == 4)) {
      break;
    }
    std::uint16_t method = 0;
    std::uint16_t nlen = 0;
    std::uint16_t elen = 0;
    std::uint32_t csize = 0;
    std::memcpy(&method, z + p + 8, 2);
    std::memcpy(&csize, z + p + 18, 4);
    std::memcpy(&nlen, z + p + 26, 2);
    std::memcpy(&elen, z + p + 28, 2);
    if (method != 0) {
      __android_log_print(ANDROID_LOG_WARN, kTag,
                          "runtime.zip 含压缩条目(method=%u)，只支持 store",
                          method);
      return -1;
    }
    const std::string rawName(reinterpret_cast<const char *>(z + p + 30), nlen);
    std::string name = stripRuntimePrefix(rawName);
    const std::size_t dataOff = p + 30 + nlen + elen;
    if (dataOff + csize > n) {
      return -1;
    }
    if (!name.empty() && name.back() != '/') {
      const std::string path = dest + "/" + name;
      std::error_code ec;
      const std::size_t slash = path.find_last_of('/');
      if (slash != std::string::npos) {
        std::filesystem::create_directories(path.substr(0, slash), ec);
      }
      std::ofstream f(path, std::ios::binary | std::ios::trunc);
      if (f) {
        f.write(reinterpret_cast<const char *>(z + dataOff),
                static_cast<std::streamsize>(csize));
        f.close();
        if (f) {
          ++count;
          if (name.size() > 3 && name.compare(name.size() - 3, 3, ".so") == 0) {
            // Android 14：可写位置不能 dlopen，必须收紧权限。
            ::chmod(path.c_str(), 0500);
          }
        }
      }
    }
    p = dataOff + csize;
  }
  return count;
}

// 内嵌服务库：build 时由 objcopy 生成 embed/libkugou_blob.o 链接进来。
// 符号名固定为 _binary_libkugou_server_so_start/_end。
extern "C" {
extern const unsigned char _binary_libkugou_server_so_start[];
extern const unsigned char _binary_libkugou_server_so_end[];
}

// 把内嵌服务库分块写到 target 并 chmod 0500；已存在且大小一致则复用。
// 返回 true 表示 target 可用。
bool materializeEmbedded(const std::string &target) {
  const auto *begin = _binary_libkugou_server_so_start;
  const auto *end = _binary_libkugou_server_so_end;
  const std::size_t size = static_cast<std::size_t>(end - begin);
  if (size < 1024) {
    __android_log_print(ANDROID_LOG_WARN, kTag, "内嵌服务库为空(%zu)", size);
    return false;
  }
  {
    std::error_code sizeEc;
    const auto existing = std::filesystem::file_size(target, sizeEc);
    if (!sizeEc && existing == static_cast<std::uintmax_t>(size)) {
      ::chmod(target.c_str(), 0500);  // 已有文件可能权限不对，补一次
      return true;
    }
  }
  std::error_code ec;
  const std::size_t slash = target.find_last_of('/');
  if (slash != std::string::npos) {
    std::filesystem::create_directories(target.substr(0, slash), ec);
  }
  // 旧文件若是 0500，直接覆盖写会失败，先删。
  std::filesystem::remove(target, ec);
  std::ofstream f(target, std::ios::binary | std::ios::trunc);
  if (!f) {
    __android_log_print(ANDROID_LOG_WARN, kTag, "内嵌服务库写不出: %s",
                        target.c_str());
    return false;
  }
  constexpr std::size_t kChunk = 1 << 16;
  std::size_t offset = 0;
  while (offset < size) {
    const std::size_t n = (size - offset) < kChunk ? (size - offset) : kChunk;
    f.write(reinterpret_cast<const char *>(begin + offset),
            static_cast<std::streamsize>(n));
    if (!f) {
      f.close();
      std::filesystem::remove(target, ec);
      return false;
    }
    offset += n;
  }
  f.close();
  if (!f) {
    std::filesystem::remove(target, ec);
    return false;
  }
  ::chmod(target.c_str(), 0500);  // Android 14：可写位置不能 dlopen
  __android_log_print(ANDROID_LOG_INFO, kTag, "内嵌服务库已写出: %zu 字节 -> %s",
                      size, target.c_str());
  return true;
}

// 在候选目录里找 runtime.zip，找到就解压到 stage 并返回服务库路径。
std::string extractRuntimeZip(const std::vector<std::string> &roots,
                              const std::string &stage) {
  for (const auto &root : roots) {
    const std::string zipPath = root + "/" + kRuntimeZipName;
    std::ifstream probe(zipPath, std::ios::binary | std::ios::ate);
    if (!probe) {
      continue;
    }
    const auto size = static_cast<std::size_t>(probe.tellg());
    probe.seekg(0);
    std::vector<unsigned char> blob(size);
    if (!probe.read(reinterpret_cast<char *>(blob.data()),
                    static_cast<std::streamsize>(size))) {
      continue;
    }
    probe.close();

    std::error_code ec;
    // stage 里的旧文件是 0500，覆盖写会失败，先整目录清掉。
    std::filesystem::remove_all(stage, ec);
    std::filesystem::create_directories(stage, ec);
    const int n = extractStoredZip(blob.data(), blob.size(), stage);
    if (n <= 0) {
      __android_log_print(ANDROID_LOG_WARN, kTag,
                          "runtime.zip 解压失败(%d): %s", n, zipPath.c_str());
      continue;
    }
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "runtime.zip 已解压: %d 个文件 <- %s", n,
                        zipPath.c_str());
    return stage + "/libkugou_server.so";
  }
  return {};
}

// 目录可写性探测：能创建临时文件就算可写。
bool dirWritable(const std::string &dir) {
  std::error_code ec;
  const std::string probe = dir + "/.wprobe";
  {
    std::ofstream f(probe, std::ios::binary | std::ios::trunc);
    if (!f) {
      return false;
    }
  }
  std::filesystem::remove(probe, ec);
  return true;
}
} // namespace

KugouProvider::~KugouProvider() { shutdown(); }

bool KugouProvider::init(const std::string &libPath,
                         const std::string &dataDir) {
  if (mPort > 0) {
    return true;
  }
  mDataDir = dataDir;
  loadSession();

  // ── 服务库定位顺序 ───────────────────────────────────────
  // 1) mod 目录里的 runtime.zip，解压到私有 cache 再加载（主路径）。
  // 2) 外部 libPath（调用方传入的 data/libkugou_server.so），兜底。
  //
  // 选择私有 cache 的原因：Android 14 起不允许从可写位置 dlopen 库，
  // 而 mod 目录本身（外部存储或 media 目录）不算私有，会被 linker 拒绝。
  // selfDir() 拿到的正是主 so 所在目录，LeviLauncher 下它就是
  // /data/data/<pkg>/cache/native_mods/... —— 私有、可写、允许 dlopen。
  std::string resolved;

  // ── 第一步：内嵌释放（首选，单文件自包含）─────────────────
  // 目标目录必须是私有 cache：selfDir() 在 LeviLauncher 下就是
  // /data/data/<pkg>/cache/native_mods/...，可写且允许 dlopen。
  {
    const std::string self = selfDir();
    std::string target;
    if (!self.empty()) {
      target = self + "/libkugou_impl.so";
    } else if (!dataDir.empty()) {
      target = dataDir + "/libkugou_impl.so";
    }
    if (!target.empty() && materializeEmbedded(target)) {
      resolved = target;
      __android_log_print(ANDROID_LOG_INFO, kTag, "服务库来自内嵌: %s",
                          resolved.c_str());
    }
  }

  // ── 第二步：runtime.zip 兜底（可热换服务库，不必重编主 so）───
  if (resolved.empty()) {
    const std::string self = selfDir();
    std::vector<std::string> roots;
    if (!self.empty()) {
      // self 是主 so 所在目录。LeviLauncher 下它是私有 cache 里的副本目录。
      roots.push_back(self);
      roots.push_back(self + "/..");
    }
    if (!dataDir.empty()) {
      // dataDir 实际是 <modRoot>/data，zip 在它的上一层 <modRoot>，
      // 所以父目录必须排在前面，否则在 mod 目录里永远找不到 runtime.zip。
      roots.push_back(dataDir + "/..");
      roots.push_back(dataDir);
    }
    // stage 目录：私有 cache 里单独开一层，避免和主 so 混在一起。
    std::string stage;
    if (!self.empty()) {
      stage = self + "/musicmod_runtime";
    } else if (!dataDir.empty()) {
      stage = dataDir + "/musicmod_runtime";
    }
    if (!stage.empty()) {
      resolved = extractRuntimeZip(roots, stage);
      if (!resolved.empty()) {
        __android_log_print(ANDROID_LOG_INFO, kTag, "服务库来自 runtime.zip: %s",
                            resolved.c_str());
      }
    }
  }

  if (resolved.empty()) {
    // 没有 runtime.zip（或解压失败）：按以下顺序兜底
    //   a) dataDir/libkugou_server.so —— 历史版本释放过的副本
    //   b) 调用方传入的 libPath
    // 注意 dataDir 是外部可写目录，非私有，dlopen 很可能被 linker 拒绝；
    // 但先试一次总比直接放弃好，后面还有复制到 selfDir 的兜底。
    const std::string legacy = dataDir + "/libkugou_server.so";
    std::error_code lec;
    if (std::filesystem::exists(legacy, lec)) {
      resolved = legacy;
      __android_log_print(ANDROID_LOG_WARN, kTag,
                          "无 runtime.zip，改用历史副本: %s", legacy.c_str());
    } else {
      resolved = libPath;
      __android_log_print(ANDROID_LOG_WARN, kTag,
                          "无 runtime.zip，退回外部文件: %s", libPath.c_str());
    }
  }

  mLib = dlopen(resolved.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (mLib == nullptr) {
    // 数据目录会被 linker 命名空间拒绝，复制到本 so 所在目录再试一次。
    // 源文件优先用已解析出来的 resolved，而不是可能不存在的 libPath。
    const std::string dir = selfDir();
    if (!dir.empty()) {
      const std::string target = dir + "/libkugou_impl.so";
      std::error_code ec;
      std::filesystem::copy_file(
          resolved, target, std::filesystem::copy_options::overwrite_existing,
          ec);
      if (!ec) {
        mLib = dlopen(target.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (mLib != nullptr) {
          __android_log_print(ANDROID_LOG_INFO, kTag, "酷狗组件改从 %s 加载",
                              target.c_str());
        } else {
          __android_log_print(ANDROID_LOG_WARN, kTag,
                              "复制后仍加载失败: %s",
                              dlerror() != nullptr ? dlerror() : "未知");
        }
      } else {
        __android_log_print(ANDROID_LOG_WARN, kTag, "复制组件失败: %s",
                            ec.message().c_str());
      }
    }
  }
  if (mLib == nullptr) {
    __android_log_print(ANDROID_LOG_WARN, kTag, "酷狗组件加载失败");
    return false;
  }

  auto startFn = reinterpret_cast<StartServerFn>(dlsym(mLib, "start_server"));
  auto portFn = reinterpret_cast<GetPortFn>(dlsym(mLib, "get_server_port"));
  if (startFn == nullptr || portFn == nullptr) {
    __android_log_print(ANDROID_LOG_WARN, kTag, "酷狗组件缺少入口符号");
    dlclose(mLib);
    mLib = nullptr;
    return false;
  }

  int port = startFn(0, dataDir.c_str());
  if (port <= 0) {
    port = portFn();
  }
  if (port <= 0) {
    __android_log_print(ANDROID_LOG_WARN, kTag, "酷狗本地服务启动失败");
    dlclose(mLib);
    mLib = nullptr;
    return false;
  }

  mPort = port;
  __android_log_print(ANDROID_LOG_INFO, kTag, "酷狗本地服务已启动, 端口=%d",
                      mPort);

  // 首次运行要先注册设备拿 dfid，否则取播放地址会被上游拒绝（illegal key）。
  const std::string devicePath = dataDir + "/device_info.json";
  bool needRegister = true;
  {
    std::ifstream in(devicePath);
    if (in) {
      const std::string text((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
      needRegister = text.find("\"dfid\"") == std::string::npos;
    }
  }

  if (needRegister) {
    const HttpResult reg = httpGet(mPort, "/register/dev", 15000);
    std::string dfid;
    try {
      const auto root = nlohmann::json::parse(reg.body);
      if (root.contains("data") && root["data"].contains("dfid")) {
        dfid = root["data"]["dfid"].get<std::string>();
      }
    } catch (const std::exception &) {
    }
    __android_log_print(ANDROID_LOG_INFO, kTag, "设备注册: status=%d dfid=%s",
                        reg.status, dfid.c_str());
    if (!dfid.empty()) {
      std::ofstream out(devicePath);
      out << "{\n  \"dfid\": \"" << dfid << "\",\n  \"guid\": \""
          << randomGuid() << "\"\n}\n";
      out.close();
      // 重新启动服务，让它加载刚写入的设备信息。
      auto stopFn = reinterpret_cast<StopServerFn>(dlsym(mLib, "stop_server"));
      if (stopFn != nullptr) {
        stopFn();
      }
      const int again = startFn(0, dataDir.c_str());
      mPort = again > 0 ? again : 0;
      __android_log_print(ANDROID_LOG_INFO, kTag, "设备注册后重启服务, 端口=%d",
                          mPort);
    }
  }

  return mPort > 0;
}

void KugouProvider::shutdown() {
  if (mLib == nullptr) {
    mPort = 0;
    return;
  }
  auto stopFn = reinterpret_cast<StopServerFn>(dlsym(mLib, "stop_server"));
  if (stopFn != nullptr) {
    stopFn();
  }
  dlclose(mLib);
  mLib = nullptr;
  mPort = 0;
}

bool KugouProvider::available() { return mPort > 0; }

std::vector<Track> KugouProvider::tracks() { return mTracks; }

bool KugouProvider::search(const std::string &keywords) {
  if (mPort <= 0 || keywords.empty()) {
    return false;
  }

  const std::string path = "/search?keywords=" + urlEncode(keywords) +
                           "&page=1&pagesize=30";
  const HttpResult result = httpGet(mPort, path, 12000);
  if (!result.ok || result.body.empty()) {
    __android_log_print(ANDROID_LOG_WARN, kTag, "酷狗搜索失败: status=%d",
                        result.status);
    return false;
  }

  nlohmann::json root;
  try {
    root = nlohmann::json::parse(result.body);
  } catch (const std::exception &error) {
    __android_log_print(ANDROID_LOG_WARN, kTag, "酷狗搜索返回解析失败: %s",
                        error.what());
    return false;
  }

  const nlohmann::json *list = pickTrackArray(root);
  if (list == nullptr) {
    __android_log_print(ANDROID_LOG_WARN, kTag, "酷狗搜索结果结构不识别");
    return false;
  }

  std::vector<Track> parsed;
  parsed.reserve(list->size());
  for (const auto &item : *list) {
    Track track;
    track.id = jsonString(item, "FileHash");
    track.title = cleanText(jsonString(item, "SongName"));
    if (track.title.empty()) {
      track.title = cleanText(jsonString(item, "FileName"));
    }
    track.artist = cleanText(jsonString(item, "SingerName"));
    if (!track.artist.empty()) {
      const std::string prefix = track.artist + " - ";
      if (track.title.rfind(prefix, 0) == 0) {
        track.title = track.title.substr(prefix.size());
      }
    }
    track.albumId = jsonString(item, "AlbumID");
    track.audioId = jsonString(item, "MixSongID");
    if (track.audioId.empty()) {
      track.audioId = jsonString(item, "Audioid");
    }
    const std::string seconds = jsonString(item, "Duration");
    if (!seconds.empty()) {
      track.durationMs = std::atoll(seconds.c_str()) * 1000;
    }
    if (track.id.empty() || track.title.empty()) {
      continue;
    }
    parsed.push_back(std::move(track));
  }

  if (parsed.empty()) {
    __android_log_print(ANDROID_LOG_WARN, kTag, "酷狗搜索没有可播放结果");
    return false;
  }

  mTracks = std::move(parsed);
  // 原唱优先：标题里带翻唱、伴奏、DJ、cover 的排到后面，其余标为原唱。
  const auto isCover = [](const Track &track) {
    const std::string &title = track.title;
    return title.find("翻唱") != std::string::npos ||
           title.find("伴奏") != std::string::npos ||
           title.find("cover") != std::string::npos ||
           title.find("COVER") != std::string::npos ||
           title.find("DJ") != std::string::npos;
  };
  std::stable_sort(mTracks.begin(), mTracks.end(),
                   [&](const Track &a, const Track &b) {
                     return !isCover(a) && isCover(b);
                   });
  __android_log_print(ANDROID_LOG_INFO, kTag, "酷狗搜索完成: %s 命中 %zu 首",
                      keywords.c_str(), mTracks.size());
  return true;
}

bool KugouProvider::loadFavorites() {
  if (mPort <= 0 || mToken.empty()) {
    mLastError = "请先登录";
    __android_log_print(ANDROID_LOG_WARN, kTag, "红心歌曲需要先登录");
    return false;
  }
  const std::string cookieText =
      "token=" + mToken + "; userid=" + mUserId;
  const std::string cookieArg = "&cookie=" + urlEncode(cookieText);
  // 先列云歌单，再取曲目最多的那个当作「我喜欢」。
  // 旧实现写死 listid=8，账号里根本没有这个歌单，接口直接回 30203。
  const HttpResult listRes =
      httpGet(mPort, "/user/playlist?page=1&pagesize=100" + cookieArg, 12000);
  std::string listId;
  int bestCount = -1;
  if (listRes.ok && !listRes.body.empty()) {
    try {
      const auto root = nlohmann::json::parse(listRes.body);
      const auto &data = root["data"];
      if (data.is_object() && data.contains("info") &&
          data["info"].is_array()) {
        for (const auto &item : data["info"]) {
          const std::string id = jsonString(item, "list_create_listid");
          if (id.empty()) {
            continue;
          }
          int count = 0;
          if (item.contains("m_count") && item["m_count"].is_number()) {
            count = item["m_count"].get<int>();
          }
          if (count > bestCount) {
            bestCount = count;
            listId = id;
          }
        }
      }
    } catch (const std::exception &) {
    }
  }
  if (listId.empty()) {
    listId = "2";
  }
  if (bestCount == 0) {
    mLastError = "账号里还没有收藏歌曲";
    __android_log_print(ANDROID_LOG_WARN, kTag, "红心歌单为空");
    return false;
  }
  const std::string path =
      "/playlist/track/all/new?listid=" + urlEncode(listId) +
      "&page=1&pagesize=100" + cookieArg;
  const HttpResult result = httpGet(mPort, path, 12000);
  if (!result.ok || result.body.empty()) {
    mLastError = "红心歌曲请求失败";
    __android_log_print(ANDROID_LOG_WARN, kTag,
                        "红心歌曲请求失败: status=%d %.200s", result.status,
                        result.body.c_str());
    return false;
  }
  try {
    const auto root = nlohmann::json::parse(result.body);
    const auto &data = root["data"];
    if (!data.is_object() || !data.contains("info") ||
        !data["info"].is_array()) {
      mLastError = "红心歌曲结构不识别";
      __android_log_print(ANDROID_LOG_WARN, kTag, "红心歌曲结构不识别: %.240s",
                          result.body.c_str());
      return false;
    }
    std::vector<Track> parsed;
    parsed.reserve(data["info"].size());
    for (const auto &item : data["info"]) {
      Track track;
      track.id = jsonString(item, "hash");
      track.albumId = jsonString(item, "album_id");
      // audio_id 在红心接口里可能叫 audio_id，也可能叫 mixsongid，
      // 取不到就拿 hash 兜底（歌词/封面接口都接受 hash）。
      track.audioId = jsonString(item, "audio_id");
      if (track.audioId.empty()) {
        track.audioId = jsonString(item, "mixsongid");
      }
      // 歌手：把所有 singerinfo 用「、」连起来，和酷狗显示一致。
      if (item.contains("singerinfo") && item["singerinfo"].is_array() &&
          !item["singerinfo"].empty()) {
        std::string joined;
        for (const auto &singer : item["singerinfo"]) {
          const std::string one = cleanText(jsonString(singer, "name"));
          if (one.empty()) {
            continue;
          }
          if (!joined.empty()) {
            joined += "、";
          }
          joined += one;
        }
        track.artist = joined;
      }
      // 歌名：name 是文件名（带 .mp3 和 "歌手 - " 前缀），必须剥离。
      track.title = stripAudioName(cleanText(jsonString(item, "name")));
      // 剥离后如果为空（极端情况），退回原始 name，至少有东西显示。
      if (track.title.empty()) {
        track.title = cleanText(jsonString(item, "name"));
      }
      if (item.contains("timelen") && item["timelen"].is_number()) {
        track.durationMs = item["timelen"].get<long long>();
      }
      if (track.id.empty() || track.title.empty()) {
        continue;
      }
      parsed.push_back(std::move(track));
    }
    if (parsed.empty()) {
      mLastError = "红心歌单里没有歌曲";
      __android_log_print(ANDROID_LOG_WARN, kTag, "红心歌曲为空");
      return false;
    }
    mTracks = std::move(parsed);
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "红心歌曲加载完成 listid=%s %zu 首", listId.c_str(),
                        mTracks.size());
    return true;
  } catch (const std::exception &error) {
    mLastError = "红心歌曲解析失败";
    __android_log_print(ANDROID_LOG_WARN, kTag, "红心歌曲解析失败: %s",
                        error.what());
    return false;
  }
}

bool KugouProvider::fetchCover(const Track &track, const std::string &path) {
  if (mPort <= 0 || track.id.empty()) {
    return false;
  }
  const std::string infoPath =
      "/images?hash=" + urlEncode(track.id) +
      "&album_id=" + urlEncode(track.albumId) +
      "&album_audio_id=" + urlEncode(track.audioId);
  const HttpResult info = httpGet(mPort, infoPath, 8000);
  if (!info.ok || info.body.empty()) {
    return false;
  }
  std::string cover;
  try {
    const auto root = nlohmann::json::parse(info.body);
    const auto *coverNode = findJsonNode(root, "sizable_cover");
    if (coverNode == nullptr) {
      coverNode = findJsonNode(root, "cover");
    }
    if (coverNode != nullptr && coverNode->is_string()) {
      cover = coverNode->get<std::string>();
    }
  } catch (const std::exception &) {
    return false;
  }
  if (cover.empty()) {
    return false;
  }
  const std::string placeholder = "{size}";
  const auto at = cover.find(placeholder);
  if (at != std::string::npos) {
    cover.replace(at, placeholder.size(), "400");
  }
  if (cover.rfind("http", 0) != 0) {
    return false;
  }
  const HttpResult image = httpGetUrl(cover, {{"User-Agent", "Mozilla/5.0"}}, 10000);
  if (!image.ok || image.body.empty()) {
    return false;
  }
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    return false;
  }
  out.write(image.body.data(), static_cast<std::streamsize>(image.body.size()));
  out.close();
  __android_log_print(ANDROID_LOG_INFO, kTag, "封面已下载 %zu 字节 %s",
                      image.body.size(), cover.c_str());
  return true;
}

bool KugouProvider::loadDailyRecommend(int variant) {
  if (mPort <= 0) {
    return false;
  }
  // 探索(variant 0) 与 小众(variant 1) 之前都落到 /everyday_song_recommend，
  // 上游同一个接口，歌单当然一模一样。小众改走风格推荐
  // /everyday/style/recommend（服务端 → /everydayrec.service/everyday_style_recommend）。
  std::string api = (variant == 0) ? "/everyday/recommend" : "/everyday/style/recommend";
  std::string path = api + "?cookie=" +
                     urlEncode("token=" + mToken + "; userid=" + mUserId);
  if (variant != 0) {
    // 风格推荐需要 tagids，空值表示由服务端给默认风格。
    path += "&tagids=";
  }
  HttpResult result = httpGet(mPort, path, 15000);
  if ((!result.ok || result.body.empty()) && variant != 0) {
    // 风格推荐不可用时退回每日推荐，至少不出现空列表。
    __android_log_print(ANDROID_LOG_WARN, kTag,
                        "风格推荐失败, 退回每日推荐: %.200s", result.body.c_str());
    api = "/recommend/songs";
    path = api + "?cookie=" +
           urlEncode("token=" + mToken + "; userid=" + mUserId);
    result = httpGet(mPort, path, 15000);
  }
  if (!result.ok || result.body.empty()) {
    __android_log_print(ANDROID_LOG_WARN, kTag, "每日推荐请求失败: %s %.200s",
                        api.c_str(), result.body.c_str());
    return false;
  }
  try {
    const auto root = nlohmann::json::parse(result.body);
    const auto *data = findJsonNode(root, "data");
    const nlohmann::json *list = nullptr;
    if (data != nullptr && data->is_object()) {
      const auto it = data->find("song_list");
      if (it != data->end() && it->is_array()) {
        list = &(*it);
      }
    }
    if (list == nullptr) {
      __android_log_print(ANDROID_LOG_WARN, kTag, "每日推荐结构不识别: %.200s",
                          result.body.c_str());
      return false;
    }
    std::vector<Track> parsed;
    parsed.reserve(list->size());
    for (const auto &item : *list) {
      Track track;
      track.id = jsonString(item, "hash");
      track.title = cleanText(jsonString(item, "songname"));
      track.artist = cleanText(jsonString(item, "author_name"));
      track.albumId = jsonString(item, "album_id");
      track.audioId = jsonString(item, "album_audio_id");
      const std::string seconds = jsonString(item, "time_length");
      if (!seconds.empty()) {
        track.durationMs = std::atoll(seconds.c_str()) * 1000;
      }
      if (track.id.empty() || track.title.empty()) {
        continue;
      }
      parsed.push_back(std::move(track));
    }
    if (parsed.empty()) {
      return false;
    }
    mTracks = std::move(parsed);
    __android_log_print(ANDROID_LOG_INFO, kTag, "每日推荐加载完成 %s %zu 首",
                        variant == 0 ? "探索" : "小众", mTracks.size());
    return true;
  } catch (const std::exception &error) {
    __android_log_print(ANDROID_LOG_WARN, kTag, "每日推荐解析失败: %s",
                        error.what());
    return false;
  }
}

bool KugouProvider::loadSimilar(const Track &seed) {
  if (mPort <= 0 || seed.audioId.empty()) {
    return false;
  }
  const std::string path =
      "/ai/recommend?album_audio_id=" + urlEncode(seed.audioId) +
      "&cookie=" + urlEncode("token=" + mToken + "; userid=" + mUserId) +
      "&userid=" + urlEncode(mUserId);
  const HttpResult result = httpGet(mPort, path, 15000);
  if (!result.ok || result.body.empty()) {
    return false;
  }
  try {
    const auto root = nlohmann::json::parse(result.body);
    const auto *data = findJsonNode(root, "data");
    const nlohmann::json *list = nullptr;
    if (data != nullptr && data->is_object()) {
      const auto it = data->find("song_list");
      if (it != data->end() && it->is_array()) {
        list = &(*it);
      }
    }
    if (list == nullptr) {
      return false;
    }
    std::vector<Track> parsed;
    for (const auto &item : *list) {
      Track track;
      track.id = jsonString(item, "hash");
      track.title = cleanText(jsonString(item, "songname"));
      track.artist = cleanText(jsonString(item, "author_name"));
      track.albumId = jsonString(item, "album_id");
      track.audioId = jsonString(item, "mixsongid");
      if (track.audioId.empty()) {
        const auto *relate = findJsonNode(item, "album_audio_id");
        if (relate != nullptr && relate->is_number_integer()) {
          track.audioId = std::to_string(relate->get<long long>());
        }
      }
      const std::string seconds = jsonString(item, "time_length");
      if (!seconds.empty()) {
        track.durationMs = std::atoll(seconds.c_str()) * 1000;
      }
      if (!track.id.empty() && !track.title.empty()) {
        parsed.push_back(std::move(track));
      }
    }
    if (parsed.empty()) {
      return false;
    }
    mTracks = std::move(parsed);
    __android_log_print(ANDROID_LOG_INFO, kTag, "相似续接 %zu 首",
                        mTracks.size());
    return true;
  } catch (const std::exception &error) {
    __android_log_print(ANDROID_LOG_WARN, kTag, "相似续接解析失败: %s",
                        error.what());
    return false;
  }
}

std::string KugouProvider::streamUrl(const Track &track) {
  if (mPort <= 0 || track.id.empty()) {
    return {};
  }

  // 两条上游路径都试一遍，某些歌在一条上会被拒。
  std::string cookieText = "token=" + mToken + "; userid=" + mUserId;
  if (!mVipToken.empty()) {
    cookieText += "; vip_token=" + mVipToken;
  }
  if (!mVipType.empty()) {
    cookieText += "; vip_type=" + mVipType;
  }
  for (const char *api : {"/song/url/new", "/song/url"}) {
    std::string path = std::string(api) + "?hash=" +
                       urlEncode(track.id) + "&quality=128&cookie=" +
                       urlEncode(cookieText);
    if (!track.audioId.empty()) {
      path += "&album_audio_id=" + urlEncode(track.audioId);
    }
    const HttpResult result = httpGet(mPort, path, 12000);
    if (!result.ok || result.body.empty()) {
      __android_log_print(ANDROID_LOG_WARN, kTag, "取地址失败 %s: status=%d %s",
                          api, result.status,
                          result.body.substr(0, 200).c_str());
      continue;
    }

    nlohmann::json root;
    try {
      root = nlohmann::json::parse(result.body);
    } catch (const std::exception &error) {
      __android_log_print(ANDROID_LOG_WARN, kTag, "播放地址解析失败: %s",
                          error.what());
      continue;
    }

    std::string url;
    auto pickUrl = [](const nlohmann::json &node) -> std::string {
      for (const char *key : {"url", "backupUrl", "backup_url", "backurl"}) {
        const auto it = node.find(key);
        if (it == node.end()) {
          continue;
        }
        if (it->is_string()) {
          const std::string value = it->get<std::string>();
          if (!value.empty()) {
            return value;
          }
        } else if (it->is_array()) {
          for (const auto &entry : *it) {
            if (entry.is_string()) {
              const std::string value = entry.get<std::string>();
              if (!value.empty()) {
                return value;
              }
            }
          }
        }
      }
      return {};
    };
    const auto *dataNode = findJsonNode(root, "data");
    if (dataNode != nullptr && dataNode->is_array() && !dataNode->empty()) {
      const auto &first = (*dataNode)[0];
      url = pickUrl(first);
      if (url.empty()) {
        const int privilege = first.value("privilege", -1);
        __android_log_print(ANDROID_LOG_WARN, kTag,
                            "无播放地址 %s: privilege=%d vip_token=%s", api,
                            privilege, mVipToken.empty() ? "无" : "有");
      }
    } else {
      url = pickUrl(root);
    }
    if (url.empty()) {
      __android_log_print(ANDROID_LOG_WARN, kTag, "无播放地址 %s: %s", api,
                          result.body.substr(0, 220).c_str());
      continue;
    }
    __android_log_print(ANDROID_LOG_INFO, kTag, "播放地址(%s): %s", api,
                        url.substr(0, 90).c_str());
    return url;
  }
  return {};
}

std::string KugouProvider::lyric(const Track &track) {
  if (mPort <= 0 || track.id.empty()) {
    return {};
  }
  const long long durationSec =
      track.durationMs > 0 ? track.durationMs / 1000 : 0;
  // keywords 必须用【干净歌名】。红心歌单返回的 name 是
  // "旧北乐队 - 灿.mp3" 这种文件名，直接拿去搜歌词必然搜不到 ——
  // 这就是"那些歌全部没有歌词"的直接原因。
  // 这里再兜一次底：即使上游没剥离，也在此处剥掉扩展名与前缀。
  const std::string cleanTitle = stripAudioName(track.title);
  const std::string keyword = cleanTitle.empty() ? track.title : cleanTitle;
  const std::string basePath =
      "/search/lyric?hash=" + urlEncode(track.id) +
      "&album_audio_id=" + urlEncode(track.audioId) +
      "&duration=" + std::to_string(durationSec);
  // 第一次带 keywords，失败就去掉 keywords 只按 hash 再试一次
  // （hash 是曲目唯一标识，服务端能直接定位歌词）。
  HttpResult search = httpGet(mPort, basePath + "&keywords=" + urlEncode(keyword),
                              12000);
  if (!search.ok || search.body.empty()) {
    __android_log_print(ANDROID_LOG_WARN, kTag,
                        "歌词搜索失败(带关键词) %.16s 关键字=%s", track.id.c_str(),
                        keyword.c_str());
    return {};
  }
  {
    // 如果带关键词返回的候选为空，退一步按 hash 单独搜。
    // 注意：必须用 findJsonNode 递归找，不能 root.find("candidates") 只查顶层。
    bool hasCandidates = false;
    try {
      const auto probe = nlohmann::json::parse(search.body);
      const auto *found = findJsonNode(probe, "candidates");
      hasCandidates = (found != nullptr && found->is_array() && !found->empty());
    } catch (const std::exception &) {
    }
    if (!hasCandidates) {
      __android_log_print(ANDROID_LOG_INFO, kTag,
                          "歌词候选为空，改用纯 hash 重试 %.16s",
                          track.id.c_str());
      search = httpGet(mPort, basePath, 12000);
    }
  }
  // 原来这里要求 id 和 accesskey 都非空才继续，而酷狗对部分曲目
  // （尤其是列表里带格式标记的那些）不返回 accesskey，
  // 于是整首歌直接判成"暂无歌词"。实际 accesskey 只是可选参数，
  // 不传也能取到公开歌词。改成：只要拿到 id 就试，accesskey 有就带上。
  std::vector<std::pair<std::string, std::string>> candidates;
  try {
    const auto root = nlohmann::json::parse(search.body);
    // 递归查找：候选实际在 data.candidates 下面。
    const auto *found = findJsonNode(root, "candidates");
    if (found == nullptr || !found->is_array() || found->empty()) {
      __android_log_print(ANDROID_LOG_WARN, kTag,
                          "歌词候选为空(已递归查找) %.16s 原始=%.160s",
                          track.id.c_str(), search.body.c_str());
      return {};
    }
    const auto &it = *found;
    // 取前 3 个候选逐个尝试，第一个没歌词就换下一个。
    for (std::size_t i = 0; i < it.size() && i < 3; ++i) {
      const auto &cand = it[i];
      const std::string cid = jsonString(cand, "id");
      if (cid.empty()) {
        continue;
      }
      candidates.emplace_back(cid, jsonString(cand, "accesskey"));
    }
  } catch (const std::exception &) {
    return {};
  }
  if (candidates.empty()) {
    return {};
  }
  // 优先取 krc：只有它带逐字时间轴。拿不到再退回 lrc。
  for (const auto &cand : candidates) {
  const std::string &lyricId = cand.first;
  const std::string &accessKey = cand.second;
  for (const char *fmt : {"krc", "lrc"}) {
    std::string lyricPath =
        "/lyric?id=" + urlEncode(lyricId) + "&decode=1&fmt=" + fmt;
    if (!accessKey.empty()) {
      lyricPath += "&accesskey=" + urlEncode(accessKey);
    }
    // 超时从 8s 放宽到 12s：歌词接口在弱网下经常 8s 才回，
    // 一超时就直接判"无歌词"，观感像随机丢歌词。
    const HttpResult result = httpGet(mPort, lyricPath, 12000);
    if (!result.ok || result.body.empty()) {
      continue;
    }
    std::string text;
    try {
      const auto root = nlohmann::json::parse(result.body);
      auto take = [&text](const nlohmann::json &node) {
        for (const char *key : {"decodeContent", "content", "lyrics", "lyric",
                                "lrc"}) {
          if (node.contains(key) && node[key].is_string()) {
            const std::string value = node[key].get<std::string>();
            if (!value.empty()) {
              text = value;
              return true;
            }
          }
        }
        return false;
      };
      if (!take(root) && root.contains("data")) {
        take(root["data"]);
      }
    } catch (const std::exception &) {
      continue;
    }
    if (text.empty()) {
      continue;
    }
    const bool wordTimed =
        text.find("]<") != std::string::npos;
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "歌词格式=%s 逐字=%d 长度=%zu", fmt,
                        wordTimed ? 1 : 0, text.size());
    if (wordTimed || std::string(fmt) == "lrc") {
      return text;
    }
  }
  }  // for (candidates)
  return {};
}

// ---------------- 扫码登录 ----------------

namespace {

// 在任意层级里找第一个同名节点，兼容服务把数据放在 data.xxx 的情况。
const nlohmann::json *pickNode(const nlohmann::json &node, const char *key) {
  if (node.is_object()) {
    const auto it = node.find(key);
    if (it != node.end()) {
      return &(*it);
    }
    for (auto iter = node.begin(); iter != node.end(); ++iter) {
      if (const auto *found = pickNode(*iter, key); found != nullptr) {
        return found;
      }
    }
  } else if (node.is_array()) {
    for (const auto &item : node) {
      if (const auto *found = pickNode(item, key); found != nullptr) {
        return found;
      }
    }
  }
  return nullptr;
}

std::string nodeText(const nlohmann::json *node) {
  if (node == nullptr) {
    return {};
  }
  if (node->is_string()) {
    return node->get<std::string>();
  }
  if (node->is_number_integer()) {
    return std::to_string(node->get<long long>());
  }
  if (node->is_number_unsigned()) {
    return std::to_string(node->get<unsigned long long>());
  }
  return {};
}

} // namespace

bool KugouProvider::loginQrStart(std::string &key, std::string &image) {
  key.clear();
  image.clear();
  // 首选本地服务的建码接口。MD3Music 用的就是这条路，
  // 请求参数和签名全由库自己处理，发出来的码和客户端一致。
  // 实测 /login/qr/key 才会真正发码；/login/qr/create 只返回空模板。
  if (mPort > 0) {
    const char *paths[] = {"/login/qr/key?appid=3116", "/login/qr/key"};
    for (const char *p : paths) {
      const HttpResult local = httpGet(mPort, p, 12000);
      if (!local.ok || local.body.empty()) {
        __android_log_print(ANDROID_LOG_WARN, kTag, "本地建码失败: %s status=%d",
                            p, local.status);
        continue;
      }
      __android_log_print(ANDROID_LOG_INFO, kTag, "本地建码返回: %.200s",
                          local.body.c_str());
      try {
        const auto root = nlohmann::json::parse(local.body);
        const auto &data = root.contains("data") ? root["data"] : root;
        for (const char *name : {"qrcode", "uuid", "key", "qrkey"}) {
          key = nodeText(pickNode(data, name));
          if (!key.empty()) {
            break;
          }
        }
        for (const char *name : {"qrcode_img", "qrcodebase64", "img", "qr",
                                 "qrcode_image"}) {
          image = nodeText(pickNode(data, name));
          if (!image.empty()) {
            break;
          }
        }
        __android_log_print(ANDROID_LOG_INFO, kTag, "本地建码 key=%s 图=%zu",
                            key.c_str(), image.size());
      } catch (const std::exception &error) {
        __android_log_print(ANDROID_LOG_WARN, kTag, "本地建码解析失败: %s",
                            error.what());
      }
      if (!key.empty()) {
        return true;
      }
    }
  }
  std::map<std::string, std::string> params;
  params["dfid"] = "-";
  params["mid"] = "-";
  params["uuid"] = "-";
  // 建码、链接、查询三处 appid 必须一致。
  // 用 3116 建码时 key 后缀是 3116，链接里的 appid 也是 3116，
  // 页面才能认出这是概念版的登录码。
  params["appid"] = "3116";
  params["clientver"] = "11440";
  params["clienttime"] = std::to_string(nowSeconds());
  params["type"] = "1";
  params["plat"] = "4";
  params["srcappid"] = "2919";
  params["qrcode_txt"] =
      "https://h5.kugou.com/apps/loginQRCode/html/index.html?appid=3116&";
  const std::string url =
      signedWebUrl("login-user.kugou.com", "/v2/qrcode", params);
  const HttpResult result = httpGetUrl(
      url,
      {{"User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                       "AppleWebKit/537.36 (KHTML, like Gecko) "
                       "Chrome/120.0.0.0 Safari/537.36"},
       {"dfid", "-"},
       {"mid", "-"},
       {"clienttime", params["clienttime"]}},
      12000);
  if (!result.ok || result.body.empty()) {
    __android_log_print(ANDROID_LOG_WARN, kTag,
                        "扫码 key 请求失败: status=%d body=%.160s",
                        result.status, result.body.c_str());
    return false;
  }
  try {
    const auto root = nlohmann::json::parse(result.body);
    const auto &data = root.contains("data") ? root["data"] : root;
    key = nodeText(pickNode(data, "qrcode"));
    image = nodeText(pickNode(data, "qrcode_img"));
    if (key.empty()) {
      key = nodeText(pickNode(data, "key"));
    }
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "官方扫码 key=%s 长度=%zu 图=%zu", key.c_str(),
                        key.size(), image.size());
  } catch (const std::exception &error) {
    __android_log_print(ANDROID_LOG_WARN, kTag, "扫码 key 解析失败: %s",
                        error.what());
    return false;
  }
  return !key.empty() && !image.empty();
}

int KugouProvider::loginQrCheck(const std::string &key, std::string &token,
                                std::string &userId) {
  if (key.empty() || mPort <= 0) {
    return -1;
  }
  // 直接用本地服务的轮询接口。旧实现走官方 /v2/get_userinfo_qrcode，
  // 该路由已经不存在，永远拿不到结果。
  const std::string path = "/login/qr/check?key=" + urlEncode(key);
  const HttpResult result = httpGet(mPort, path, 8000);
  if (!result.ok || result.body.empty()) {
    return -1;
  }
  int status = -1;
  try {
    static std::string lastBody;
    if (result.body != lastBody) {
      lastBody = result.body;
      __android_log_print(ANDROID_LOG_INFO, kTag, "扫码响应变化: %.240s",
                          result.body.c_str());
    }
    const auto root = nlohmann::json::parse(result.body);
    const auto &data = root.contains("data") ? root["data"] : root;
    const auto statusIt = data.find("status");
    if (statusIt != data.end() && statusIt->is_number()) {
      status = statusIt->get<int>();
    } else {
      const auto rootStatus = root.find("status");
      if (rootStatus != root.end() && rootStatus->is_number()) {
        status = rootStatus->get<int>();
      }
    }
    token = nodeText(pickNode(data, "token"));
    userId = nodeText(pickNode(data, "userid"));
    if (userId.empty()) {
      userId = nodeText(pickNode(data, "user_id"));
    }
    mVipToken = nodeText(pickNode(data, "vip_token"));
    mVipType = nodeText(pickNode(data, "vip_type"));
    // 4 表示对方已在手机上确认，返回里带 token 才算登录成功。
    if (status == 4 && !token.empty()) {
      mToken = token;
      mUserId = userId;
      saveSession();
      __android_log_print(ANDROID_LOG_INFO, kTag, "扫码登录成功, 用户=%s",
                          userId.c_str());
    } else {
      __android_log_print(ANDROID_LOG_INFO, kTag, "扫码状态=%d", status);
    }
  } catch (const std::exception &error) {
    __android_log_print(ANDROID_LOG_WARN, kTag, "扫码结果解析失败: %s",
                        error.what());
    return -1;
  }
  return status;
}
void KugouProvider::reportListen(const Track &track, const std::string &event,
                                 long long durationMs) {
  if (mPort <= 0 || mToken.empty() || track.audioId.empty()) {
    return;
  }
  const std::string cookieText =
      "token=" + mToken + "; userid=" + mUserId;
  std::string path = "/user/listen/report?event=" + urlEncode(event) +
                     "&mixsongid=" + urlEncode(track.audioId) +
                     "&cookie=" + urlEncode(cookieText);
  if (event == "end") {
    path += "&duration=" + std::to_string(durationMs) +
            "&state=" + urlEncode("有效播放");
  }
  const HttpResult result = httpGet(mPort, path, 8000);
  __android_log_print(ANDROID_LOG_INFO, kTag,
                      "听歌上报 %s 曲目=%s 时长=%lld 结果=%.120s",
                      event.c_str(), track.audioId.c_str(), durationMs,
                      result.body.c_str());
}

bool KugouProvider::claimVip(std::string &message) {
  message.clear();
  if (mPort <= 0) {
    message = "本地服务未就绪";
    return false;
  }
  if (mToken.empty()) {
    message = "请先登录";
    return false;
  }
  const std::string cookieText =
      "token=" + mToken + "; userid=" + mUserId;
  char day[16]{};
  const std::time_t now = std::time(nullptr);
  if (std::tm *tmNow = std::localtime(&now)) {
    std::strftime(day, sizeof(day), "%Y-%m-%d", tmNow);
  }
  if (day[0] == '\0') {
    message = "系统日期不可用";
    return false;
  }
  const std::string path =
      "/youth/day/vip?receive_day=" + std::string(day) +
      "&cookie=" + urlEncode(cookieText);
  const HttpResult result = httpGet(mPort, path, 12000);
  __android_log_print(ANDROID_LOG_INFO, kTag, "领取会员响应: %.240s",
                      result.body.c_str());
  if (result.body.empty()) {
    message = "网络无响应";
    return false;
  }
  try {
    const auto root = nlohmann::json::parse(result.body);
    const int errorCode = root.value("error_code", 0);
    const int status = root.value("status", 0);
    if (errorCode == 0 && status == 1) {
      message = "签到成功，24 小时会员已到账";
      return true;
    }
    std::string reason;
    for (const char *field : {"error_msg", "msg", "message", "error"}) {
      reason = nodeText(pickNode(root, field));
      if (!reason.empty()) {
        break;
      }
    }
    if (errorCode == 131001) {
      // 当天已签到，24 小时会员已经到账。
      message = "今日已签到，24 小时会员已到账";
      return true;
    } else if (errorCode == 297002) {
      reason = "今日会员奖励已领取";
    }
    if (reason.empty()) {
      reason = "服务端拒绝 error_code=" + std::to_string(errorCode);
    }
    message = reason;
  } catch (const std::exception &error) {
    message = error.what();
    return false;
  }
  return false;
}

bool KugouProvider::loginSmsSend(const std::string &mobile) {
  mLastError.clear();
  if (mPort <= 0 || mobile.empty()) {
    mLastError = "本地服务未就绪";
    return false;
  }
  // 只走已确认可用的发码接口，避免后面两个错误路径覆盖真实原因。
  const std::string path = "/captcha/sent?mobile=" + urlEncode(mobile);
  const HttpResult result = httpGet(mPort, path, 12000);
  __android_log_print(ANDROID_LOG_INFO, kTag, "发码 status=%d %.200s",
                      result.status, result.body.c_str());
  if (result.body.empty()) {
    mLastError = "本地服务无响应";
    return false;
  }
  try {
    const auto root = nlohmann::json::parse(result.body);
    const auto *statusNode = pickNode(root, "status");
    const int status = (statusNode != nullptr && statusNode->is_number())
                           ? statusNode->get<int>()
                           : 0;
    const std::string msg = nodeText(pickNode(root, "data"));
    if (status == 1) {
      __android_log_print(ANDROID_LOG_INFO, kTag, "发码成功: %s",
                          msg.c_str());
      return true;
    }
    // 服务端原话直接显示，例如“该号码发送的短信数已经超过上限”。
    mLastError = msg.empty() ? "服务端拒绝发码" : msg;
    return false;
  } catch (const std::exception &error) {
    __android_log_print(ANDROID_LOG_WARN, kTag, "发码结果解析失败: %s",
                        error.what());
    mLastError = "发码响应无法解析";
    return false;
  }
}

bool KugouProvider::loginSmsSubmit(const std::string &mobile,
                                   const std::string &code) {
  mLastError.clear();
  if (mPort <= 0 || mobile.empty() || code.empty()) {
    mLastError = "手机号或验证码为空";
    return false;
  }
  // 只发一次规范请求，不穷举参数名：反复试会消耗验证码并触发限流。
  // 若本地服务要求设备身份，必须从原版配置读取，不能随机生成。
  const std::string path = std::string("/login/cellphone?mobile=") +
                           urlEncode(mobile) + "&code=" + urlEncode(code) +
                           "&area_code=86";
  HttpResult result = httpGet(mPort, path, 12000);
  __android_log_print(ANDROID_LOG_INFO, kTag,
                      "验证码登录请求 status=%d %.240s", result.status,
                      result.body.c_str());
  __android_log_print(ANDROID_LOG_INFO, kTag, "验证码登录: status=%d %.240s",
                      result.status, result.body.c_str());
  if (result.body.empty()) {
    mLastError = "网络错误";
    return false;
  }
  try {
    const auto root = nlohmann::json::parse(result.body);
    const std::string token = nodeText(pickNode(root, "token"));
    std::string user = nodeText(pickNode(root, "userid"));
    if (user.empty()) {
      user = nodeText(pickNode(root, "user_id"));
    }
    if (token.empty()) {
      mLastError = nodeText(pickNode(root, "data"));
      if (mLastError.empty()) {
        mLastError = "登录失败";
      }
      return false;
    }
    mToken = token;
    mUserId = user;
    saveSession();
    __android_log_print(ANDROID_LOG_INFO, kTag, "手机号登录成功, 用户=%s",
                        user.c_str());
    return true;
  } catch (const std::exception &error) {
    __android_log_print(ANDROID_LOG_WARN, kTag, "登录结果解析失败: %s",
                        error.what());
    mLastError = "返回内容无法解析";
    return false;
  }
}

void KugouProvider::logout() {
  mToken.clear();
  mUserId.clear();
  mLastError.clear();
  const std::string path = mDataDir + "/session.json";
  std::remove(path.c_str());
  __android_log_print(ANDROID_LOG_INFO, kTag, "已退出登录");
}

bool KugouProvider::loadSession() {
  if (mDataDir.empty()) {
    return false;
  }
  std::ifstream input(mDataDir + "/session.json");
  if (!input.is_open()) {
    return false;
  }
  try {
    nlohmann::json root;
    input >> root;
    mToken = nodeText(pickNode(root, "token"));
    mUserId = nodeText(pickNode(root, "userid"));
    mVipToken = nodeText(pickNode(root, "vip_token"));
    mVipType = nodeText(pickNode(root, "vip_type"));
  } catch (const std::exception &) {
    return false;
  }
  __android_log_print(ANDROID_LOG_INFO, kTag, "登录态载入: 用户=%s",
                      mUserId.c_str());
  return !mToken.empty();
}

bool KugouProvider::saveSession() {
  if (mDataDir.empty()) {
    return false;
  }
  nlohmann::json root;
  root["token"] = mToken;
  root["userid"] = mUserId;
  root["vip_token"] = mVipToken;
  root["vip_type"] = mVipType;
  // 原子写 + 保留一份 session.json.bak。
  // 直接覆盖写：写到一半崩溃会把 token 一起抹掉，玩家要重新扫码。
  // 这里先写 .tmp，成功后再 rename，同时留一份 .bak 兜底。
  const std::string path = mDataDir + "/session.json";
  const std::string tmp = path + ".tmp";
  const std::string bak = path + ".bak";
  {
    std::ofstream output(tmp, std::ios::trunc);
    if (!output.is_open()) {
      return false;
    }
    output << root.dump(2);
    output.flush();
    if (!output.good()) {
      output.close();
      std::remove(tmp.c_str());
      return false;
    }
    output.close();
  }
  std::error_code ec;
  if (std::filesystem::exists(path, ec)) {
    std::filesystem::copy_file(path, bak,
        std::filesystem::copy_options::overwrite_existing, ec);
  }
  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    std::filesystem::copy_file(tmp, path,
        std::filesystem::copy_options::overwrite_existing, ec);
    std::remove(tmp.c_str());
  }
  return true;
}

} // namespace musicmod