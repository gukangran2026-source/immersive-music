#include "HttpClient.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <cstdint>
#include <vector>

namespace musicmod {
namespace {

std::string decodeChunked(const std::string &body) {
  std::string out;
  std::size_t pos = 0;
  while (pos < body.size()) {
    const std::size_t lineEnd = body.find("\r\n", pos);
    if (lineEnd == std::string::npos) {
      break;
    }
    const std::string sizeLine = body.substr(pos, lineEnd - pos);
    const std::size_t chunkSize =
        std::strtoul(sizeLine.c_str(), nullptr, 16);
    pos = lineEnd + 2;
    if (chunkSize == 0) {
      break;
    }
    if (pos + chunkSize > body.size()) {
      out.append(body, pos, body.size() - pos);
      break;
    }
    out.append(body, pos, chunkSize);
    pos += chunkSize + 2;
  }
  return out;
}

} // namespace

std::string urlEncode(const std::string &value) {
  static const char *kHex = "0123456789ABCDEF";
  std::string out;
  out.reserve(value.size() * 3);
  for (unsigned char c : value) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
        c == '~') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 0x0F]);
    }
  }
  return out;
}

HttpResult httpGet(int port, const std::string &path, int timeoutMs) {
  HttpResult result;

  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return result;
  }

  timeval tv{};
  tv.tv_sec = timeoutMs / 1000;
  tv.tv_usec = (timeoutMs % 1000) * 1000;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<std::uint16_t>(port));
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return result;
  }

  std::string request = "GET " + path + " HTTP/1.1\r\n";
  request += "Host: 127.0.0.1\r\n";
  request += "Accept: application/json\r\n";
  request += "Connection: close\r\n\r\n";

  if (::send(fd, request.data(), request.size(), 0) < 0) {
    ::close(fd);
    return result;
  }

  std::string raw;
  char buffer[8192];
  for (;;) {
    const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
    if (n <= 0) {
      break;
    }
    raw.append(buffer, static_cast<std::size_t>(n));
    if (raw.size() > 8u * 1024u * 1024u) {
      break;
    }
  }
  ::close(fd);

  const std::size_t headerEnd = raw.find("\r\n\r\n");
  if (headerEnd == std::string::npos) {
    return result;
  }

  const std::string head = raw.substr(0, headerEnd);
  const std::size_t firstSpace = head.find(' ');
  if (firstSpace != std::string::npos) {
    result.status = std::atoi(head.c_str() + firstSpace + 1);
  }

  std::string body = raw.substr(headerEnd + 4);
  if (head.find("chunked") != std::string::npos) {
    body = decodeChunked(body);
  }
  result.body = std::move(body);
  result.ok = (result.status == 200);
  return result;
}

HttpResult httpPost(int port, const std::string &path, const std::string &body,
                    int timeoutMs) {
  HttpResult result;

  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return result;
  }

  timeval tv{};
  tv.tv_sec = timeoutMs / 1000;
  tv.tv_usec = (timeoutMs % 1000) * 1000;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<std::uint16_t>(port));
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return result;
  }

  std::string request = "POST " + path + " HTTP/1.1\r\n";
  request += "Host: 127.0.0.1\r\n";
  request += "Accept: application/json\r\n";
  request += "Content-Type: application/x-www-form-urlencoded\r\n";
  request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  request += "Connection: close\r\n\r\n";
  request += body;

  if (::send(fd, request.data(), request.size(), 0) < 0) {
    ::close(fd);
    return result;
  }

  std::string raw;
  char buffer[8192];
  for (;;) {
    const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
    if (n <= 0) {
      break;
    }
    raw.append(buffer, static_cast<std::size_t>(n));
    if (raw.size() > 8u * 1024u * 1024u) {
      break;
    }
  }
  ::close(fd);

  const std::size_t headerEnd = raw.find("\r\n\r\n");
  if (headerEnd == std::string::npos) {
    return result;
  }
  const std::string head = raw.substr(0, headerEnd);
  const std::size_t firstSpace = head.find(' ');
  if (firstSpace != std::string::npos) {
    result.status = std::atoi(head.c_str() + firstSpace + 1);
  }
  std::string payload = raw.substr(headerEnd + 4);
  if (head.find("chunked") != std::string::npos) {
    payload = decodeChunked(payload);
  }
  result.body = std::move(payload);
  result.ok = (result.status == 200);
  return result;
}

namespace {

// ---- MD5（RFC 1321，够用于接口签名） ----
struct Md5Ctx {
  std::uint32_t a{0x67452301};
  std::uint32_t b{0xefcdab89};
  std::uint32_t c{0x98badcfe};
  std::uint32_t d{0x10325476};
  std::uint64_t length{0};
  unsigned char buffer[64]{};
  std::size_t used{0};
};

std::uint32_t rotl(std::uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }

void md5Block(Md5Ctx &ctx, const unsigned char *block) {
  static const std::uint32_t K[64] = {
      0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a,
      0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
      0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340,
      0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
      0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
      0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
      0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
      0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
      0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92,
      0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
      0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
  static const int S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7,
                            12, 17, 22, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,
                            14, 20, 5, 9, 14, 20, 4, 11, 16, 23, 4, 11, 16,
                            23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21,
                            6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
  std::uint32_t M[16];
  for (int i = 0; i < 16; ++i) {
    M[i] = static_cast<std::uint32_t>(block[i * 4]) |
           (static_cast<std::uint32_t>(block[i * 4 + 1]) << 8) |
           (static_cast<std::uint32_t>(block[i * 4 + 2]) << 16) |
           (static_cast<std::uint32_t>(block[i * 4 + 3]) << 24);
  }
  std::uint32_t A = ctx.a, B = ctx.b, C = ctx.c, D = ctx.d;
  for (int i = 0; i < 64; ++i) {
    std::uint32_t F = 0;
    int g = 0;
    if (i < 16) {
      F = (B & C) | (~B & D);
      g = i;
    } else if (i < 32) {
      F = (D & B) | (~D & C);
      g = (5 * i + 1) % 16;
    } else if (i < 48) {
      F = B ^ C ^ D;
      g = (3 * i + 5) % 16;
    } else {
      F = C ^ (B | ~D);
      g = (7 * i) % 16;
    }
    const std::uint32_t tmp = D;
    D = C;
    C = B;
    B = B + rotl(A + F + K[i] + M[g], S[i]);
    A = tmp;
  }
  ctx.a += A;
  ctx.b += B;
  ctx.c += C;
  ctx.d += D;
}

void md5Update(Md5Ctx &ctx, const unsigned char *data, std::size_t size) {
  ctx.length += static_cast<std::uint64_t>(size) * 8;
  while (size > 0) {
    const std::size_t take = std::min<std::size_t>(64 - ctx.used, size);
    std::memcpy(ctx.buffer + ctx.used, data, take);
    ctx.used += take;
    data += take;
    size -= take;
    if (ctx.used == 64) {
      md5Block(ctx, ctx.buffer);
      ctx.used = 0;
    }
  }
}

std::string md5Finish(Md5Ctx &ctx) {
  const std::uint64_t bits = ctx.length;
  unsigned char pad[72]{};
  pad[0] = 0x80;
  std::size_t padLen = (ctx.used < 56) ? (56 - ctx.used) : (120 - ctx.used);
  md5Update(ctx, pad, padLen);
  unsigned char len[8];
  for (int i = 0; i < 8; ++i) {
    len[i] = static_cast<unsigned char>((bits >> (8 * i)) & 0xFF);
  }
  md5Update(ctx, len, 8);
  const std::uint32_t words[4] = {ctx.a, ctx.b, ctx.c, ctx.d};
  static const char *hex = "0123456789abcdef";
  std::string out;
  out.reserve(32);
  for (std::uint32_t word : words) {
    for (int i = 0; i < 4; ++i) {
      const unsigned char byte =
          static_cast<unsigned char>((word >> (8 * i)) & 0xFF);
      out.push_back(hex[byte >> 4]);
      out.push_back(hex[byte & 0x0F]);
    }
  }
  return out;
}

JavaVM *gJavaVm = nullptr;

} // namespace

void setHttpJavaVm(JavaVM *vm) { gJavaVm = vm; }

std::string md5Hex(const std::string &text) {
  Md5Ctx ctx;
  md5Update(ctx, reinterpret_cast<const unsigned char *>(text.data()),
            text.size());
  return md5Finish(ctx);
}

HttpResult httpGetUrl(
    const std::string &url,
    const std::vector<std::pair<std::string, std::string>> &headers,
    int timeoutMs) {
  HttpResult result;
  if (gJavaVm == nullptr || url.empty()) {
    return result;
  }

  JNIEnv *env = nullptr;
  bool attached = false;
  if (gJavaVm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) !=
      JNI_OK) {
    if (gJavaVm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
      return result;
    }
    attached = true;
  }

  jobject connection = nullptr;
  jobject stream = nullptr;
  jbyteArray buffer = nullptr;
  jobject output = nullptr;

  do {
    jclass urlClass = env->FindClass("java/net/URL");
    if (urlClass == nullptr) {
      break;
    }
    jmethodID urlCtor =
        env->GetMethodID(urlClass, "<init>", "(Ljava/lang/String;)V");
    jmethodID openConnection = env->GetMethodID(
        urlClass, "openConnection", "()Ljava/net/URLConnection;");
    if (urlCtor == nullptr || openConnection == nullptr) {
      break;
    }
    jstring jUrl = env->NewStringUTF(url.c_str());
    jobject urlObject = env->NewObject(urlClass, urlCtor, jUrl);
    env->DeleteLocalRef(jUrl);
    if (urlObject == nullptr) {
      break;
    }
    connection = env->CallObjectMethod(urlObject, openConnection);
    env->DeleteLocalRef(urlObject);
    if (env->ExceptionCheck() || connection == nullptr) {
      env->ExceptionClear();
      break;
    }

    jclass httpClass = env->FindClass("java/net/HttpURLConnection");
    if (httpClass == nullptr) {
      break;
    }
    jmethodID setMethod = env->GetMethodID(httpClass, "setRequestMethod",
                                           "(Ljava/lang/String;)V");
    jmethodID setConnectTimeout =
        env->GetMethodID(httpClass, "setConnectTimeout", "(I)V");
    jmethodID setReadTimeout =
        env->GetMethodID(httpClass, "setReadTimeout", "(I)V");
    jmethodID setRedirect = env->GetMethodID(
        httpClass, "setInstanceFollowRedirects", "(Z)V");
    jmethodID setProperty = env->GetMethodID(
        httpClass, "setRequestProperty",
        "(Ljava/lang/String;Ljava/lang/String;)V");
    jmethodID getCode = env->GetMethodID(httpClass, "getResponseCode", "()I");
    jmethodID getInput = env->GetMethodID(
        httpClass, "getInputStream", "()Ljava/io/InputStream;");
    if (setMethod == nullptr || getCode == nullptr || getInput == nullptr) {
      break;
    }
    jstring get = env->NewStringUTF("GET");
    env->CallVoidMethod(connection, setMethod, get);
    env->DeleteLocalRef(get);
    if (setConnectTimeout != nullptr) {
      env->CallVoidMethod(connection, setConnectTimeout, timeoutMs);
    }
    if (setReadTimeout != nullptr) {
      env->CallVoidMethod(connection, setReadTimeout, timeoutMs);
    }
    if (setRedirect != nullptr) {
      env->CallVoidMethod(connection, setRedirect, JNI_TRUE);
    }
    for (const auto &header : headers) {
      if (setProperty == nullptr) {
        break;
      }
      jstring key = env->NewStringUTF(header.first.c_str());
      jstring value = env->NewStringUTF(header.second.c_str());
      env->CallVoidMethod(connection, setProperty, key, value);
      env->DeleteLocalRef(key);
      env->DeleteLocalRef(value);
    }
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
      break;
    }

    const jint code = env->CallIntMethod(connection, getCode);
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
      break;
    }
    result.status = static_cast<int>(code);

    stream = env->CallObjectMethod(connection, getInput);
    if (env->ExceptionCheck() || stream == nullptr) {
      env->ExceptionClear();
      stream = nullptr;
      result.ok = (result.status == 200);
      break;
    }

    jclass outputClass = env->FindClass("java/io/ByteArrayOutputStream");
    jmethodID outputCtor = env->GetMethodID(outputClass, "<init>", "()V");
    jmethodID outputWrite =
        env->GetMethodID(outputClass, "write", "([BII)V");
    jmethodID outputBytes = env->GetMethodID(outputClass, "toByteArray",
                                             "()[B");
    jclass streamClass = env->FindClass("java/io/InputStream");
    jmethodID read = env->GetMethodID(streamClass, "read", "([B)I");
    if (outputCtor == nullptr || outputWrite == nullptr ||
        outputBytes == nullptr || read == nullptr) {
      break;
    }
    output = env->NewObject(outputClass, outputCtor);
    buffer = env->NewByteArray(4096);
    if (output == nullptr || buffer == nullptr) {
      break;
    }
    for (;;) {
      const jint n = env->CallIntMethod(stream, read, buffer);
      if (env->ExceptionCheck()) {
        env->ExceptionClear();
        break;
      }
      if (n <= 0) {
        break;
      }
      env->CallVoidMethod(output, outputWrite, buffer, 0, n);
      if (env->ExceptionCheck()) {
        env->ExceptionClear();
        break;
      }
    }
    auto bytes = static_cast<jbyteArray>(
        env->CallObjectMethod(output, outputBytes));
    if (bytes != nullptr && !env->ExceptionCheck()) {
      const jsize size = env->GetArrayLength(bytes);
      result.body.resize(static_cast<std::size_t>(size));
      if (size > 0) {
        env->GetByteArrayRegion(bytes, 0, size,
                                reinterpret_cast<jbyte *>(result.body.data()));
      }
    }
    result.ok = (result.status == 200);
  } while (false);

  if (env->ExceptionCheck()) {
    env->ExceptionClear();
  }
  if (buffer != nullptr) {
    env->DeleteLocalRef(buffer);
  }
  if (output != nullptr) {
    env->DeleteLocalRef(output);
  }
  if (stream != nullptr) {
    env->DeleteLocalRef(stream);
  }
  if (connection != nullptr) {
    jclass httpClass = env->FindClass("java/net/HttpURLConnection");
    if (httpClass != nullptr) {
      jmethodID disconnect = env->GetMethodID(httpClass, "disconnect", "()V");
      if (disconnect != nullptr) {
        env->CallVoidMethod(connection, disconnect);
      }
    }
    env->DeleteLocalRef(connection);
  }
  if (attached) {
    gJavaVm->DetachCurrentThread();
  }
  return result;
}

} // namespace musicmod
