#include "KugouProvider.hpp"
#include <android/log.h>
#include <cstdio>
#include <cctype>
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

bool KugouProvider::loadPlaylists() {
  mPlaylists.clear();
  if (mPort <= 0 || mToken.empty()) {
    mLastError = "请先登录";
    return false;
  }
  const std::string cookieText =
      "token=" + mToken + "; userid=" + mUserId;
  const std::string cookieArg = "&cookie=" + urlEncode(cookieText);
  const HttpResult result =
      httpGet(mPort, "/user/playlist?page=1&pagesize=100" + cookieArg, 12000);
  if (!result.ok || result.body.empty()) {
    mLastError = "歌单列表请求失败";
    __android_log_print(ANDROID_LOG_WARN, kTag, "歌单列表请求失败: status=%d",
                        result.status);
    return false;
  }
  try {
    const auto root = nlohmann::json::parse(result.body);
    const auto &data = root["data"];
    if (!data.is_object() || !data.contains("info") ||
        !data["info"].is_array()) {
      mLastError = "歌单结构不识别";
      __android_log_print(ANDROID_LOG_WARN, kTag,
                          "歌单结构不识别: %.240s", result.body.c_str());
      return false;
    }
    for (const auto &item : data["info"]) {
      PlaylistInfo info;
      info.id = jsonString(item, "list_create_listid");
      if (info.id.empty()) {
        continue;
      }
      info.gid = jsonString(item, "list_create_gid");
      info.name = jsonString(item, "name");
      if (info.name.empty()) {
        info.name = "未命名歌单";
      }
      // 封面字段名不统一，按常见候选依次试。
      for (const char *field : {"pic", "cover", "img", "list_pic"}) {
        const std::string pic = jsonString(item, field);
        if (!pic.empty() && pic.rfind("http", 0) == 0) {
          info.cover = pic;
          break;
        }
      }
      if (item.contains("m_count") && item["m_count"].is_number()) {
        info.count = item["m_count"].get<int>();
      }
      if (item.contains("type") && item["type"].is_number()) {
        info.type = item["type"].get<int>();
      }
      // 「默认收藏」判据：实测字段是 is_def，值为 1。
      // 用户账号实测：listid=1 name=默认收藏 is_def=1（这才是默认那张）,
      //               listid=2 name=我喜欢   is_def=2。
      // 旧实现用 is_pri（恒 0）和 name=="我喜欢"，把「我喜欢」错标成默认。
      if (item.contains("is_def") && item["is_def"].is_number() &&
          item["is_def"].get<int>() == 1) {
        info.isDefault = true;
      }
      if (info.name.find("默认") != std::string::npos) {
        info.isDefault = true;
      }
      mPlaylists.push_back(std::move(info));
    }
    __android_log_print(ANDROID_LOG_INFO, kTag, "歌单列表加载完成 %zu 个",
                        mPlaylists.size());
    // 默认选中「我喜欢」。没标出来的话退回曲目最多的那个。
    if (mFavListId.empty() && !mPlaylists.empty()) {
      std::string best;
      int bestCount = -1;
      for (const auto &pl : mPlaylists) {
        // isDefault 已经在上面按 is_def==1 标好了，这里直接优先取它。
        if (pl.isDefault && !pl.id.empty()) {
          best = pl.id;
          bestCount = pl.count;
          break;
        }
        if (pl.count > bestCount) {
          bestCount = pl.count;
          best = pl.id;
        }
      }
      if (!best.empty()) {
        mFavListId = best;
        __android_log_print(ANDROID_LOG_INFO, kTag, "默认选中歌单 listid=%s",
                            best.c_str());
      }
    }
    return !mPlaylists.empty();
  } catch (const std::exception &error) {
    mLastError = "歌单列表解析失败";
    __android_log_print(ANDROID_LOG_WARN, kTag, "歌单列表解析失败: %s",
                        error.what());
    return false;
  }
}

bool KugouProvider::loadPlaylistTracks(const std::string &listId) {
  // pagesize 必须小一点：实测 500 会让服务库回 HTTP 502，
  // 表现为「点歌单打开失败」。100 稳定。
  if (mPort <= 0 || mToken.empty()) {
    mLastError = "请先登录";
    return false;
  }
  if (listId.empty()) {
    mLastError = "歌单 id 为空";
    return false;
  }
  const std::string cookieText =
      "token=" + mToken + "; userid=" + mUserId;
  const std::string cookieArg = "&cookie=" + urlEncode(cookieText);
  const std::string path = "/playlist/track/all/new?listid=" +
                           urlEncode(listId) + "&page=1&pagesize=100" +
                           cookieArg;
  const HttpResult result = httpGet(mPort, path, 12000);
  if (!result.ok || result.body.empty()) {
    mLastError = "歌单歌曲请求失败";
    return false;
  }
  try {
    const auto root = nlohmann::json::parse(result.body);
    const auto &data = root["data"];
    // 空歌单的返回里没有 info 数组。这是正常状态，不是解析失败：
    // 清空当前列表并返回 true，让 UI 进到第二层显示「这个歌单还是空的」。
    if (!data.is_object() || !data.contains("info") ||
        !data["info"].is_array()) {
      mTracks.clear();
      __android_log_print(ANDROID_LOG_INFO, kTag,
                          "歌单 %s 为空(无 info 数组)", listId.c_str());
      return true;
    }
    std::vector<Track> parsed;
    parsed.reserve(data["info"].size());
    for (const auto &item : data["info"]) {
      Track track;
      track.id = jsonString(item, "hash");
      track.fileId = jsonString(item, "fileid");
      track.albumId = jsonString(item, "album_id");
      // album_audio_id 必须用 mixsongid，不是 audio_id。
      // 实测歌单接口两个字段都在，但值完全不同：
      //   林俊杰《由你选择》audio_id=717628, mixsongid=32068124
      // 播放接口和歌词接口要的是 mixsongid。传成 audio_id 的后果是
      //   取播放地址 -> 服务端找不到 -> UI 报「没有版权」
      //   搜歌词     -> 匹配不上   -> UI 显示「暂无歌词」
      // 搜索页一直是对的（那边本来就用 MixSongID 优先），所以搜索能播、
      // 歌单和收藏不能播。这里对齐搜索页的顺序。
      track.audioId = jsonString(item, "mixsongid");
      if (track.audioId.empty()) {
        track.audioId = jsonString(item, "audio_id");
      }
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
      track.title = stripAudioName(cleanText(jsonString(item, "name")));
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
    mTracks = std::move(parsed);
    __android_log_print(ANDROID_LOG_INFO, kTag, "歌单 %s 加载 %zu 首",
                        listId.c_str(), mTracks.size());
    // 解析出 0 首也返回 true：空歌单是可进入的正常状态。
    return true;
  } catch (const std::exception &error) {
    mLastError = "歌单歌曲解析失败";
    __android_log_print(ANDROID_LOG_WARN, kTag, "歌单歌曲解析失败: %s",
                        error.what());
    return false;
  }
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
  // 「我喜欢」统一走 favoritesListId()：它优先取 is_def==1 的歌单，
  // 与 addToFavorites / removeFromFavorites 的写入目标是同一张。
  // 旧实现这里另按「曲目最多」挑，读的写的不是同一张歌单，
  // 表现就是「收藏了却显示没收藏」或反过来。
  const std::string listId = favoritesListId();
  if (listId.empty()) {
    mLastError = "找不到收藏歌单";
    __android_log_print(ANDROID_LOG_WARN, kTag, "找不到收藏歌单");
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
      track.fileId = jsonString(item, "fileid");
      track.albumId = jsonString(item, "album_id");
      // 同 loadPlaylistTracks：album_audio_id 用 mixsongid，audio_id 兜底。
      // 这两个字段在红心接口里都有，但值不同，选错就播不了、也没歌词。
      track.audioId = jsonString(item, "mixsongid");
      if (track.audioId.empty()) {
        track.audioId = jsonString(item, "audio_id");
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

// 只看文件头判断是不是图片。够用，而且不需要解码库。
// 必须做这个判断：防盗链/CDN 出错时返回的 HTML 也是非空响应体，
// 直接落盘会被后续当成封面文件，解码永远失败。
static bool looksLikeImage(const std::string &data) {
  if (data.size() < 12) {
    return false;
  }
  const auto *b = reinterpret_cast<const unsigned char *>(data.data());
  if (b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) {
    return true;  // JPEG
  }
  if (b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G') {
    return true;  // PNG
  }
  if (b[0] == 'G' && b[1] == 'I' && b[2] == 'F') {
    return true;  // GIF
  }
  if (b[0] == 'R' && b[1] == 'I' && b[2] == 'F' && b[3] == 'F' && b[8] == 'W' &&
      b[9] == 'E' && b[10] == 'B' && b[11] == 'P') {
    return true;  // WEBP
  }
  return false;
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
  // 带 Referer：酷狗 CDN 对部分封面做了防盗链，不带会被挡回错误页。
  const HttpResult image = httpGetUrl(
      cover,
      {{"User-Agent",
        "Mozilla/5.0 (Linux; Android 13) AppleWebKit/537.36 Chrome/120.0 Mobile Safari/537.36"},
       {"Referer", "https://www.kugou.com/"},
       {"Accept", "image/avif,image/webp,image/apng,image/*,*/*;q=0.8"}},
      10000);
  if (!image.ok || image.body.empty()) {
    return false;
  }
  // 关键校验：内容必须真的是图片。见文件顶部 looksLikeImage 的说明。
  if (!looksLikeImage(image.body)) {
    __android_log_print(ANDROID_LOG_WARN, kTag,
                        "封面不是图片, 拒收: 大小=%zu 前4字节=%02X%02X%02X%02X",
                        image.body.size(),
                        static_cast<unsigned char>(image.body[0]),
                        static_cast<unsigned char>(image.body[1]),
                        static_cast<unsigned char>(image.body[2]),
                        static_cast<unsigned char>(image.body[3]));
    return false;
  }
  // 先写临时文件再改名：中途中断不会留下半截文件被当成有效缓存。
  const std::string tmpPath = path + ".part";
  {
    std::ofstream out(tmpPath, std::ios::binary);
    if (!out) {
      return false;
    }
    out.write(image.body.data(), static_cast<std::streamsize>(image.body.size()));
    out.close();
  }
  std::remove(path.c_str());
  if (std::rename(tmpPath.c_str(), path.c_str()) != 0) {
    std::remove(tmpPath.c_str());
    return false;
  }
  __android_log_print(ANDROID_LOG_INFO, kTag, "封面已下载 %zu 字节 %s",
                      image.body.size(), cover.c_str());
  return true;
}

bool KugouProvider::fetchFavoriteHashes(std::set<std::string> &out) {
  out.clear();
  if (mPort <= 0 || mToken.empty()) {
    return false;
  }
  const std::string cookieText = "token=" + mToken + "; userid=" + mUserId;
  const std::string cookieArg = "&cookie=" + urlEncode(cookieText);
  // 歌单 id 必须与「写入收藏」同源：favoritesListId() 优先取 is_def==1
  // 的「默认收藏」。旧实现这里按「曲目最多」另挑一张，读写不一致，
  // 会出现「明明收藏了却显示没收藏」。
  const std::string listId = favoritesListId();
  if (listId.empty()) {
    return false;
  }
  // 分页拉全。pagesize 用 MD3 的默认值 30，给太大服务端会截断或拒绝。
  constexpr int kPageSize = 30;
  bool anyPageOk = false;
  for (int page = 1; page <= 6; ++page) {
    const std::string path = "/playlist/track/all/new?listid=" +
                             urlEncode(listId) + "&page=" +
                             std::to_string(page) + "&pagesize=" +
                             std::to_string(kPageSize) + cookieArg;
    const HttpResult result = httpGet(mPort, path, 12000);
    if (!result.ok || result.body.empty()) {
      break;
    }
    std::size_t got = 0;
    try {
      const auto root = nlohmann::json::parse(result.body);
      const auto &data = root["data"];
      if (!data.is_object() || !data.contains("info") ||
          !data["info"].is_array()) {
        break;
      }
      for (const auto &item : data["info"]) {
        std::string hash = jsonString(item, "hash");
        if (hash.empty()) {
          continue;
        }
        // 统一小写：搜索结果用的是 FileHash，收藏接口用的是 hash，
        // 两者大小写不一定一致，不归一就会「收藏了却显示没收藏」。
        for (auto &ch : hash) {
          ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        out.insert(hash);
        ++got;
      }
    } catch (const std::exception &) {
      break;
    }
    anyPageOk = true;
    if (got < static_cast<std::size_t>(kPageSize)) {
      break;  // 最后一页
    }
  }
  return anyPageOk && !out.empty();
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

bool KugouProvider::fetchUserInfo(std::string &nickname,
                                  std::string &avatarUrl) {
  nickname.clear();
  avatarUrl.clear();
  if (mPort <= 0) {
    return false;
  }
  if (mToken.empty()) {
    return false;
  }
  const std::string cookieText =
      "token=" + mToken + "; userid=" + mUserId;
  const std::string path = "/user/detail?cookie=" + urlEncode(cookieText);
  const HttpResult result = httpGet(mPort, path, 12000);
  __android_log_print(ANDROID_LOG_INFO, kTag, "用户资料响应 长度=%zu %.400s",
                      result.body.size(), result.body.c_str());
  if (result.body.empty()) {
    return false;
  }
  try {
    const auto root = nlohmann::json::parse(result.body);
    // 昵称：概念版的资料结构没有公开文档，按常见字段依次试探。
    for (const char *field : {"nickname", "user_name", "username", "nick"}) {
      const std::string text = nodeText(findJsonNode(root, field));
      if (!text.empty()) {
        nickname = text;
        break;
      }
    }
    // 头像：找到第一个像 URL 的字段就用。
    for (const char *field : {"pic", "user_pic", "img", "pic_url",
                              "avatar", "head_img"}) {
      const std::string text = nodeText(findJsonNode(root, field));
      if (!text.empty() && text.rfind("http", 0) == 0) {
        avatarUrl = text;
        break;
      }
    }
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "用户资料解析: 昵称=%s 头像=%s",
                        nickname.empty() ? "(空)" : nickname.c_str(),
                        avatarUrl.empty() ? "(空)" : avatarUrl.c_str());
  } catch (const std::exception &error) {
    __android_log_print(ANDROID_LOG_WARN, kTag, "用户资料解析失败: %s",
                        error.what());
    return false;
  }
  return !nickname.empty() || !avatarUrl.empty();
}

bool KugouProvider::fetchAvatar(const std::string &url,
                                const std::string &path) {
  if (url.empty() || url.rfind("http", 0) != 0) {
    return false;
  }
  const HttpResult image =
      httpGetUrl(url, {{"User-Agent", "Mozilla/5.0"}}, 10000);
  if (!image.ok || image.body.empty()) {
    __android_log_print(ANDROID_LOG_WARN, kTag, "头像下载失败 %s", url.c_str());
    return false;
  }
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    return false;
  }
  out.write(image.body.data(), static_cast<std::streamsize>(image.body.size()));
  out.close();
  __android_log_print(ANDROID_LOG_INFO, kTag, "头像已下载 %zu 字节",
                      image.body.size());
  return true;
}

std::string KugouProvider::favoritesListId() {
  if (!mFavListId.empty()) {
    return mFavListId;
  }
  if (mPort <= 0 || mToken.empty()) {
    return {};
  }
  const std::string cookieText =
      "token=" + mToken + "; userid=" + mUserId;
  const std::string cookieArg = "&cookie=" + urlEncode(cookieText);
  const HttpResult listRes =
      httpGet(mPort, "/user/playlist?page=1&pagesize=100" + cookieArg, 12000);
  if (!listRes.ok || listRes.body.empty()) {
    __android_log_print(ANDROID_LOG_WARN, kTag, "取歌单列表失败, 无法确定我以为");
    return {};
  }
  std::string listId;
  std::string listName;
  int bestCount = -1;
  try {
    const auto root = nlohmann::json::parse(listRes.body);
    const auto &data = root["data"];
    if (data.is_object() && data.contains("info") && data["info"].is_array()) {
      for (const auto &item : data["info"]) {
        const std::string id = jsonString(item, "list_create_listid");
        if (id.empty()) {
          continue;
        }
        int count = 0;
        if (item.contains("m_count") && item["m_count"].is_number()) {
          count = item["m_count"].get<int>();
        }
        const std::string nm = jsonString(item, "name");
        // 「默认收藏」判定：实测字段是 is_def，值为 1。
        // 旧实现用 is_pri，该字段恒为 0，从来没命中过。
        // 名字里带「默认」也认，避免字段名以后又变。
        const bool isDef =
            (item.contains("is_def") && item["is_def"].is_number() &&
             item["is_def"].get<int>() == 1) ||
            (nm.find("默认") != std::string::npos);
        if (isDef) {
          listId = id;
          listName = nm;
          // 默认收藏优先，直接定下来，不再比曲目数。
          break;
        }
        if (count > bestCount) {
          bestCount = count;
          listId = id;
          listName = nm;
        }
      }
    }
  } catch (const std::exception &) {
  }
  if (listId.empty()) {
    // 兜底：2 是多数账号的「我喜欢」。取不到就试它，失败会打日志。
    listId = "2";
  }
  mFavListId = listId;
  mFavListName = listName;
  __android_log_print(ANDROID_LOG_INFO, kTag,
                      "收藏目标歌单 listid=%s (%s) 曲目数=%d",
                      mFavListId.c_str(), mFavListName.c_str(), bestCount);
  return mFavListId;
}

bool KugouProvider::addToFavorites(const Track &track) {
  mLastError.clear();
  if (mPort <= 0) {
    mLastError = "服务未就绪";
    return false;
  }
  if (mToken.empty()) {
    mLastError = "请先登录";
    return false;
  }
  const std::string listId = favoritesListId();
  if (listId.empty()) {
    mLastError = "找不到我以为歌单";
    return false;
  }
  const long long clienttime = nowSeconds();
  // data 格式：name|hash|album_id|mixsongid，多项用逗号分隔。
  const std::string data =
      track.title + "|" + track.id + "|" + track.albumId + "|" + track.audioId;
  // 全部参数走 query。
  // 对照 MD3 handle_tracks_add：它在服务库内部用 q_str / q.get /
  // param_or_cookie_str 从 query 取所有字段，不读 body。
  // 之前用 POST + form body，服务库读不到 listid，松手就拒。
  const std::string path =
      "/playlist/tracks/add?last_time=" + std::to_string(clienttime) +
      "&last_area=gztx" +
      "&userid=" + urlEncode(mUserId) +
      "&token=" + urlEncode(mToken) +
      "&listid=" + urlEncode(listId) +
      "&list_ver=0&type=0&slow_upload=1" +
      "&scene=" + urlEncode("false;null") +
      "&data=" + urlEncode(data) +
      "&cookie=" + urlEncode("token=" + mToken + "; userid=" + mUserId);
  __android_log_print(ANDROID_LOG_INFO, kTag,
                      "收藏请求 listid=%s hash=%.20s album=%s audio=%s",
                      listId.c_str(), track.id.c_str(), track.albumId.c_str(),
                      track.audioId.c_str());
  const HttpResult result = httpGet(mPort, path, 15000);
  __android_log_print(ANDROID_LOG_INFO, kTag, "收藏响应 status=%d body=%.400s",
                      result.status, result.body.c_str());
  if (result.body.empty()) {
    mLastError = "网络无响应";
    return false;
  }
  try {
    const auto root = nlohmann::json::parse(result.body);
    // 成功判据必须只看 data.status == 1。
    // 实测响应：{"data":{"status":1,"listid":1,...}} —— status 在 data 里面。
    // 旧实现读顶层 status / error_code：
    //   顶层没有这两个字段 → value() 返回默认 0
    //   → errorCode == 0 恒成立 → 永远返回 true
    // 于是「提示已添加、歌单里却没有」，取消时又报「歌单里没有这首歌」。
    const auto *dataNode = findJsonNode(root, "data");
    int status = 0;
    if (dataNode != nullptr && dataNode->is_object()) {
      if (dataNode->contains("status") && (*dataNode)["status"].is_number()) {
        status = (*dataNode)["status"].get<int>();
      }
    }
    // 有些接口把 status 放顶层，两种都兜一下（但不许再用 error_code==0 判成功）。
    if (status == 0 && root.contains("status") && root["status"].is_number()) {
      status = root["status"].get<int>();
    }
    __android_log_print(ANDROID_LOG_INFO, kTag, "收藏结果 status=%d", status);
    if (status == 1) {
      __android_log_print(ANDROID_LOG_INFO, kTag, "收藏成功 %.40s",
                          track.title.c_str());
      return true;
    }
    std::string reason;
    for (const char *field : {"error_msg", "msg", "message", "error"}) {
      reason = nodeText(pickNode(root, field));
      if (!reason.empty()) {
        break;
      }
    }
    if (reason.empty()) {
      reason = "服务端拒绝 status=" + std::to_string(status);
    }
    mLastError = reason;
    __android_log_print(ANDROID_LOG_WARN, kTag, "收藏失败: %s", reason.c_str());
  } catch (const std::exception &error) {
    mLastError = error.what();
    return false;
  }
  return false;
}

bool KugouProvider::removeFromFavorites(const Track &track) {
  mLastError.clear();
  if (mPort <= 0 || mToken.empty()) {
    mLastError = "请先登录";
    return false;
  }
  const std::string listId = favoritesListId();
  if (listId.empty()) {
    mLastError = "找不到我以为歌单";
    return false;
  }
  const long long clienttime = nowSeconds();
  // 参数名必须是 fileids（不是 data），且值必须是数字 fileid。
  // 实测：传 hash 时接口回 200 但曲目仍然在列表里，等于没删。
  std::string fileId = track.fileId;
  if (fileId.empty()) {
    // 搜索结果里的曲目没有 fileid，先去歌单里按 hash 找一次。
    const std::string cookieText =
        "token=" + mToken + "; userid=" + mUserId;
    const std::string cookieArg = "&cookie=" + urlEncode(cookieText);
    const std::string listPath = "/playlist/track/all/new?listid=" +
                                 urlEncode(listId) +
                                 "&page=1&pagesize=200" + cookieArg;
    const HttpResult listRes = httpGet(mPort, listPath, 15000);
    if (listRes.ok && !listRes.body.empty()) {
      try {
        const auto root = nlohmann::json::parse(listRes.body);
        const auto &data = root["data"];
        if (data.is_object() && data.contains("info") &&
            data["info"].is_array()) {
          for (const auto &item : data["info"]) {
            if (jsonString(item, "hash") == track.id) {
              fileId = jsonString(item, "fileid");
              break;
            }
          }
        }
      } catch (const std::exception &) {
      }
    }
  }
  if (fileId.empty()) {
    mLastError = "歌单里找不到这首歌";
    __android_log_print(ANDROID_LOG_WARN, kTag,
                        "取消收藏失败: 拿不到 fileid hash=%.20s",
                        track.id.c_str());
    return false;
  }
  const std::string path =
      "/playlist/tracks/del?last_time=" + std::to_string(clienttime) +
      "&last_area=gztx" +
      "&userid=" + urlEncode(mUserId) +
      "&token=" + urlEncode(mToken) +
      "&listid=" + urlEncode(listId) +
      "&list_ver=0&type=0" +
      "&fileids=" + urlEncode(fileId) +
      "&cookie=" + urlEncode("token=" + mToken + "; userid=" + mUserId);
  __android_log_print(ANDROID_LOG_INFO, kTag,
                      "取消收藏 listid=%s fileid=%s hash=%.16s",
                      listId.c_str(), fileId.c_str(), track.id.c_str());
  const HttpResult result = httpGet(mPort, path, 15000);
  __android_log_print(ANDROID_LOG_INFO, kTag,
                      "取消收藏响应 status=%d body=%.400s", result.status,
                      result.body.c_str());
  if (result.body.empty()) {
    mLastError = "网络无响应";
    return false;
  }
  try {
    const auto root = nlohmann::json::parse(result.body);
    // 同 addToFavorites：只看 data.status == 1。
    // 旧写法用 errorCode == 0 兜底，而顶层没有这个字段、value() 返回默认 0，
    // 于是永远判成功 —— 取消收藏也会「报了成功其实没删」。
    const auto *dataNode2 = findJsonNode(root, "data");
    int status2 = 0;
    if (dataNode2 != nullptr && dataNode2->is_object() &&
        dataNode2->contains("status") && (*dataNode2)["status"].is_number()) {
      status2 = (*dataNode2)["status"].get<int>();
    }
    if (status2 == 0 && root.contains("status") && root["status"].is_number()) {
      status2 = root["status"].get<int>();
    }
    __android_log_print(ANDROID_LOG_INFO, kTag, "取消收藏结果 status=%d",
                        status2);
    if (status2 == 1) {
      return true;
    }
    mLastError = "服务端拒绝 status=" + std::to_string(status2);
  } catch (const std::exception &error) {
    mLastError = error.what();
    return false;
  }
  return false;
}

bool KugouProvider::isFavorite(const Track &track) {
  if (mPort <= 0 || mToken.empty() || track.id.empty()) {
    return false;
  }
  // 直接查「我喜欢」里有没有这个 hash。歌单最多取 500 首，
  // 超出时可能误判为未收藏，这种情况宁可显示空心也不误报已收藏。
  const std::string listId = favoritesListId();
  if (listId.empty()) {
    return false;
  }
  const std::string cookieText =
      "token=" + mToken + "; userid=" + mUserId;
  const std::string cookieArg = "&cookie=" + urlEncode(cookieText);
  const std::string path = "/playlist/track/all/new?listid=" +
                           urlEncode(listId) + "&page=1&pagesize=100" +
                           cookieArg;
  const HttpResult result = httpGet(mPort, path, 12000);
  if (!result.ok || result.body.empty()) {
    return false;
  }
  try {
    const auto root = nlohmann::json::parse(result.body);
    const auto &data = root["data"];
    if (data.is_object() && data.contains("info") && data["info"].is_array()) {
      for (const auto &item : data["info"]) {
        if (jsonString(item, "hash") == track.id) {
          return true;
        }
      }
    }
  } catch (const std::exception &) {
  }
  return false;
}

bool KugouProvider::findFavoriteFileId(const std::string &hash,
                                       std::string &fileId) {
  fileId.clear();
  if (mPort <= 0 || mToken.empty() || hash.empty()) {
    return false;
  }
  const std::string listId = favoritesListId();
  if (listId.empty()) {
    return false;
  }
  const std::string cookieText =
      "token=" + mToken + "; userid=" + mUserId;
  const std::string cookieArg = "&cookie=" + urlEncode(cookieText);
  const std::string path = "/playlist/track/all/new?listid=" +
                           urlEncode(listId) + "&page=1&pagesize=200" +
                           cookieArg;
  const HttpResult result = httpGet(mPort, path, 15000);
  if (!result.ok || result.body.empty()) {
    return false;
  }
  try {
    const auto root = nlohmann::json::parse(result.body);
    const auto &data = root["data"];
    if (data.is_object() && data.contains("info") && data["info"].is_array()) {
      for (const auto &item : data["info"]) {
        if (jsonString(item, "hash") == hash) {
          fileId = jsonString(item, "fileid");
          return !fileId.empty();
        }
      }
    }
  } catch (const std::exception &) {
  }
  return false;
}

bool KugouProvider::searchByKind(SearchKind kind,
                                  const std::string &keywords) {
  if (mPort <= 0 || keywords.empty()) {
    return false;
  }
  if (kind == SearchKind::Song) {
    return search(keywords);
  }
  const char *route =
      (kind == SearchKind::Album) ? "/search/album" : "/search/artist";
  const std::string path = std::string(route) + "?keyword=" +
                           urlEncode(keywords) + "&page=1&pagesize=30";
  const HttpResult result = httpGet(mPort, path, 12000);
  if (!result.ok || result.body.empty()) {
    mLastError = "搜索请求失败";
    __android_log_print(ANDROID_LOG_WARN, kTag, "搜索失败 %s status=%d",
                        route, result.status);
    return false;
  }
  // 先打一段原始响应，字段名对不上时靠它定位。只在长度较短时打。
  __android_log_print(ANDROID_LOG_INFO, kTag, "搜索 %s 响应 %.300s", route,
                      result.body.c_str());
  try {
    const auto root = nlohmann::json::parse(result.body);
    const nlohmann::json *list = pickTrackArray(root);
    if (list == nullptr) {
      mLastError = "搜索结果结构不识别";
      __android_log_print(ANDROID_LOG_WARN, kTag, "搜索 %s 结构不识别",
                          route);
      return false;
    }
    if (kind == SearchKind::Album) {
      mAlbums.clear();
      for (const auto &item : *list) {
        AlbumInfo album;
        // 专辑 id：常见 alblum_id / album_id / albumid，全试一遍。
        for (const char *k : {"album_id", "albumid", "AlbumID", "alblum_id"}) {
          album.id = jsonString(item, k);
          if (!album.id.empty()) {
            break;
          }
        }
        for (const char *k : {"album_name", "albumname", "AlbumName", "name"}) {
          album.name = cleanText(jsonString(item, k));
          if (!album.name.empty()) {
            break;
          }
        }
        for (const char *k : {"singer_name", "singername", "author_name",
                              "SingerName"}) {
          album.artist = cleanText(jsonString(item, k));
          if (!album.artist.empty()) {
            break;
          }
        }
        for (const char *k : {"imgurl", "img", "cover", "pic", "album_pic"}) {
          const std::string pic = jsonString(item, k);
          if (!pic.empty() && pic.rfind("http", 0) == 0) {
            album.cover = pic;
            break;
          }
        }
        for (const char *k : {"songcount", "song_count", "songcounts"}) {
          if (item.contains(k) && item[k].is_number()) {
            album.songCount = item[k].get<int>();
            break;
          }
        }
        album.artistId = jsonString(item, "author_id");
        if (album.artistId.empty()) {
          album.artistId = jsonString(item, "singerid");
        }
        if (album.id.empty() || album.name.empty()) {
          continue;
        }
        mAlbums.push_back(std::move(album));
      }
      __android_log_print(ANDROID_LOG_INFO, kTag, "专辑搜索 %s 命中 %zu 张",
                          keywords.c_str(), mAlbums.size());
      return !mAlbums.empty();
    }
    // 歌手
    mArtists.clear();
    for (const auto &item : *list) {
      ArtistInfo info;
      for (const char *k : {"author_id", "singerid", "ArtistId", "id"}) {
        info.id = jsonString(item, k);
        if (!info.id.empty()) {
          break;
        }
      }
      for (const char *k : {"author_name", "singername", "SingerName", "name"}) {
        info.name = cleanText(jsonString(item, k));
        if (!info.name.empty()) {
          break;
        }
      }
      for (const char *k : {"avatar", "imgurl", "image", "pic"}) {
        const std::string pic = jsonString(item, k);
        if (!pic.empty() && pic.rfind("http", 0) == 0) {
          info.avatar = pic;
          break;
        }
      }
      if (item.contains("songcount") && item["songcount"].is_number()) {
        info.songCount = item["songcount"].get<int>();
      }
      if (item.contains("albumcount") && item["albumcount"].is_number()) {
        info.albumCount = item["albumcount"].get<int>();
      }
      if (info.id.empty() || info.name.empty()) {
        continue;
      }
      mArtists.push_back(std::move(info));
    }
    __android_log_print(ANDROID_LOG_INFO, kTag, "歌手搜索 %s 命中 %zu 位",
                        keywords.c_str(), mArtists.size());
    return !mArtists.empty();
  } catch (const std::exception &error) {
    mLastError = "搜索结果解析失败";
    __android_log_print(ANDROID_LOG_WARN, kTag, "搜索解析失败: %s",
                        error.what());
    return false;
  }
}

bool KugouProvider::loadArtistDetail(const std::string &artistId) {
  mCurrentArtist = ArtistInfo();
  mCurrentArtist.id = artistId;
  if (mPort <= 0 || artistId.empty()) {
    return false;
  }
  const std::string path = "/artist/detail?id=" + urlEncode(artistId);
  const HttpResult result = httpGet(mPort, path, 12000);
  if (!result.ok || result.body.empty()) {
    __android_log_print(ANDROID_LOG_WARN, kTag, "歌手资料请求失败 id=%s",
                        artistId.c_str());
    return false;
  }
  __android_log_print(ANDROID_LOG_INFO, kTag, "歌手资料响应 %.300s",
                      result.body.c_str());
  try {
    const auto root = nlohmann::json::parse(result.body);
    for (const char *k : {"author_name", "singername", "name"}) {
      const std::string name = cleanText(nodeText(pickNode(root, k)));
      if (!name.empty()) {
        mCurrentArtist.name = name;
        break;
      }
    }
    for (const char *k : {"avatar", "imgurl", "image", "pic", "sizable_avatar"}) {
      const std::string pic = nodeText(pickNode(root, k));
      if (!pic.empty() && pic.rfind("http", 0) == 0) {
        mCurrentArtist.avatar = pic;
        break;
      }
    }
    __android_log_print(ANDROID_LOG_INFO, kTag, "歌手资料 名字=%s 头像=%s",
                        mCurrentArtist.name.c_str(),
                        mCurrentArtist.avatar.empty()
                            ? "(空)"
                            : mCurrentArtist.avatar.c_str());
  } catch (const std::exception &error) {
    __android_log_print(ANDROID_LOG_WARN, kTag, "歌手资料解析失败: %s",
                        error.what());
    return false;
  }
  return !mCurrentArtist.name.empty();
}

bool KugouProvider::loadArtistSongs(const std::string &artistId,
                                    const std::string &sort) {
  if (mPort <= 0 || artistId.empty()) {
    return false;
  }
  // sort 参数：MD3 里 hot 传 1，其余传 2。
  const std::string sortArg = (sort == "hot") ? "1" : "2";
  // area_code=all 不能省。对照 MD3 的 handle_audios，它的 data_map 里
  // 除了 author_id/page/pagesize/sort 还带 area_code；漏掉时上游返回
  // {"total":0,"data":[]}——接口 200 但没有内容。
  const std::string path = "/artist/audios?id=" + urlEncode(artistId) +
                           "&page=1&pagesize=100&sort=" + sortArg +
                           "&area_code=all";
  const HttpResult result = httpGet(mPort, path, 15000);
  if (!result.ok || result.body.empty()) {
    mLastError = "歌手歌曲请求失败";
    __android_log_print(ANDROID_LOG_WARN, kTag, "歌手歌曲请求失败 id=%s",
                        artistId.c_str());
    return false;
  }
  __android_log_print(ANDROID_LOG_INFO, kTag, "歌手歌曲响应 %.300s",
                      result.body.c_str());
  try {
    const auto root = nlohmann::json::parse(result.body);
    const nlohmann::json *list = pickTrackArray(root);
    if (list == nullptr) {
      mLastError = "歌手歌曲结构不识别";
      return false;
    }
    std::vector<Track> parsed;
    parsed.reserve(list->size());
    for (const auto &item : *list) {
      Track track;
      // 歌手接口字段名与搜索不同：hash / audio_name / album_id / mixsongid
      for (const char *k : {"hash", "FileHash", "audio_hash"}) {
        track.id = jsonString(item, k);
        if (!track.id.empty()) {
          break;
        }
      }
      for (const char *k : {"audio_name", "songname", "SongName", "name"}) {
        const std::string raw = cleanText(jsonString(item, k));
        if (!raw.empty()) {
          track.title = stripAudioName(raw);
          break;
        }
      }
      if (track.title.empty()) {
        continue;
      }
      for (const char *k : {"author_name", "singername", "SingerName"}) {
        const std::string a = cleanText(jsonString(item, k));
        if (!a.empty()) {
          track.artist = a;
          break;
        }
      }
      track.albumId = jsonString(item, "album_id");
      if (track.albumId.empty()) {
        track.albumId = jsonString(item, "AlbumID");
      }
      track.audioId = jsonString(item, "mixsongid");
      if (track.audioId.empty()) {
        track.audioId = jsonString(item, "audio_id");
      }
      if (item.contains("timelen") && item["timelen"].is_number()) {
        track.durationMs = item["timelen"].get<long long>();
      }
      if (track.id.empty()) {
        continue;
      }
      parsed.push_back(std::move(track));
    }
    mTracks = std::move(parsed);
    __android_log_print(ANDROID_LOG_INFO, kTag, "歌手 %s 歌曲 %zu 首",
                        artistId.c_str(), mTracks.size());
    return !mTracks.empty();
  } catch (const std::exception &error) {
    mLastError = "歌手歌曲解析失败";
    __android_log_print(ANDROID_LOG_WARN, kTag, "歌手歌曲解析失败: %s",
                        error.what());
    return false;
  }
}

bool KugouProvider::loadArtistAlbums(const std::string &artistId,
                                     const std::string &sort) {
  if (mPort <= 0 || artistId.empty()) {
    return false;
  }
  // MD3 里 albums 的 hot 传 3，其余传 1。
  const std::string sortArg = (sort == "hot") ? "3" : "1";
  // 同样对照 MD3 handle_albums：category=1、area_code=all 都必须带。
  const std::string path = "/artist/albums?id=" + urlEncode(artistId) +
                           "&page=1&pagesize=50&sort=" + sortArg +
                           "&category=1&area_code=all";
  const HttpResult result = httpGet(mPort, path, 15000);
  if (!result.ok || result.body.empty()) {
    mLastError = "歌手专辑请求失败";
    return false;
  }
  __android_log_print(ANDROID_LOG_INFO, kTag, "歌手专辑响应 %.300s",
                      result.body.c_str());
  try {
    const auto root = nlohmann::json::parse(result.body);
    const nlohmann::json *list = pickTrackArray(root);
    if (list == nullptr) {
      mLastError = "歌手专辑结构不识别";
      return false;
    }
    mAlbums.clear();
    for (const auto &item : *list) {
      AlbumInfo album;
      for (const char *k : {"album_id", "albumid", "AlbumID"}) {
        album.id = jsonString(item, k);
        if (!album.id.empty()) {
          break;
        }
      }
      for (const char *k : {"album_name", "albumname", "AlbumName", "name"}) {
        album.name = cleanText(jsonString(item, k));
        if (!album.name.empty()) {
          break;
        }
      }
      for (const char *k : {"imgurl", "img", "cover", "pic"}) {
        const std::string pic = jsonString(item, k);
        if (!pic.empty() && pic.rfind("http", 0) == 0) {
          album.cover = pic;
          break;
        }
      }
      album.artistId = artistId;
      if (album.id.empty() || album.name.empty()) {
        continue;
      }
      mAlbums.push_back(std::move(album));
    }
    __android_log_print(ANDROID_LOG_INFO, kTag, "歌手 %s 专辑 %zu 张",
                        artistId.c_str(), mAlbums.size());
    return !mAlbums.empty();
  } catch (const std::exception &error) {
    mLastError = "歌手专辑解析失败";
    __android_log_print(ANDROID_LOG_WARN, kTag, "歌手专辑解析失败: %s",
                        error.what());
    return false;
  }
}

bool KugouProvider::loadAlbumSongs(const std::string &albumId) {
  if (mPort <= 0 || albumId.empty()) {
    return false;
  }
  // 与 MD3 的实现对齐：必须带 cookie，pagesize 用 30。
  // 原来给的是 pagesize=200 且没带 cookie —— 服务端直接拒绝，
  // 界面上就是「专辑加载失败: 专辑歌曲请求失败」。
  // 歌单接口给 500 时回 502，同一个坑，值不能贪大。专辑通常十几首，30 够。
  const std::string cookieArg =
      "&cookie=" + urlEncode("token=" + mToken + "; userid=" + mUserId);
  const std::string path = "/album/songs?album_id=" + urlEncode(albumId) +
                           "&page=1&pagesize=30" + cookieArg;
  const HttpResult result = httpGet(mPort, path, 15000);
  if (!result.ok || result.body.empty()) {
    mLastError = "专辑歌曲请求失败";
    return false;
  }
  __android_log_print(ANDROID_LOG_INFO, kTag, "专辑歌曲响应 %.300s",
                      result.body.c_str());
  try {
    const auto root = nlohmann::json::parse(result.body);
    const nlohmann::json *list = pickTrackArray(root);
    if (list == nullptr) {
      mLastError = "专辑歌曲结构不识别";
      return false;
    }
    std::vector<Track> parsed;
    parsed.reserve(list->size());
    for (const auto &item : *list) {
      Track track;
      for (const char *k : {"hash", "FileHash", "audio_hash"}) {
        track.id = jsonString(item, k);
        if (!track.id.empty()) {
          break;
        }
      }
      for (const char *k : {"audio_name", "songname", "SongName", "name"}) {
        const std::string raw = cleanText(jsonString(item, k));
        if (!raw.empty()) {
          track.title = stripAudioName(raw);
          break;
        }
      }
      if (track.title.empty()) {
        continue;
      }
      for (const char *k : {"author_name", "singername", "SingerName"}) {
        const std::string a = cleanText(jsonString(item, k));
        if (!a.empty()) {
          track.artist = a;
          break;
        }
      }
      track.albumId = albumId;
      track.audioId = jsonString(item, "mixsongid");
      if (track.audioId.empty()) {
        track.audioId = jsonString(item, "audio_id");
      }
      if (item.contains("timelen") && item["timelen"].is_number()) {
        track.durationMs = item["timelen"].get<long long>();
      }
      if (track.id.empty()) {
        continue;
      }
      parsed.push_back(std::move(track));
    }
    mTracks = std::move(parsed);
    __android_log_print(ANDROID_LOG_INFO, kTag, "专辑 %s 歌曲 %zu 首",
                        albumId.c_str(), mTracks.size());
    return !mTracks.empty();
  } catch (const std::exception &error) {
    mLastError = "专辑歌曲解析失败";
    __android_log_print(ANDROID_LOG_WARN, kTag, "专辑歌曲解析失败: %s",
                        error.what());
    return false;
  }
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