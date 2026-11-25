#pragma once

#include <jni.h>

#include <string>
#include <utility>
#include <vector>

namespace musicmod {

struct HttpResult {
  bool ok{false};
  int status{0};
  std::string body;
};

// 只访问本机回环上的服务，够用就行，不引入额外依赖。
HttpResult httpGet(int port, const std::string &path, int timeoutMs = 8000);

// 表单 POST，body 形如 a=1&b=2。
HttpResult httpPost(int port, const std::string &path, const std::string &body,
                    int timeoutMs = 8000);

// 对关键词做百分号编码，中文直接按 UTF-8 字节处理。
std::string urlEncode(const std::string &value);

// 外网 HTTPS GET，走 Android 自带的 HttpsURLConnection。
// 需要在模组初始化时把 JavaVM 传进来。
void setHttpJavaVm(JavaVM *vm);
HttpResult httpGetUrl(const std::string &url,
                      const std::vector<std::pair<std::string, std::string>> &headers,
                      int timeoutMs = 10000);

// 小写十六进制 MD5。
std::string md5Hex(const std::string &text);
} // namespace musicmod
