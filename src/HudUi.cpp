#include "HudUi.hpp"
#include "qrcodegen.hpp"

#include <android/log.h>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <dlfcn.h>

#include <chrono>
#include <ctime>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <sys/stat.h>
#include <fstream>
#include <span>
#include <thread>
#include <string_view>
#include <vector>

#include "AudioEngine.hpp"
#include "pl/Input.hpp"
#include "pl/ModMenu.hpp"

// 宿主提供的暂停菜单状态；弱符号，宿主未导出时自动为空。
namespace pl {
namespace runtime {
bool IsPauseMenuOpen() __attribute__((weak));
}
}

namespace musicmod {
namespace {

constexpr const char *kTag = "MusicMod";
constexpr const char *kModuleId = "musicmod.hud";
constexpr const char *kCoverId = "musicmod.cover";
constexpr const char *kQrId = "musicmod.qr";
// 右上角圆形头像的图片 id 前缀，必须与封面分开，否则会互相覆盖。
constexpr const char *kAvatarId = "musicmod.avatar";

using RegisterModuleFn = bool (*)(const pl::modmenu::ModuleInfo &);
using SubmitDrawFn = void (*)(std::string_view,
                              std::span<const pl::modmenu::DrawCommand>);
using UnregisterModuleFn = void (*)(std::string_view);
using RegisterTouchFn = void (*)(pl::input::TouchCallback);
using RegisterImageFn = bool (*)(std::string_view,
                                 std::span<const unsigned char>, int, int);
using GetSurfaceFn = pl::modmenu::HudSurfaceSize (*)();
using RegisterTextFn = void (*)(pl::input::TextInputCallback);
using RegisterKeyFn = void (*)(pl::input::KeyCallback);
using ShowKeyboardFn = void (*)();

// 配色：取自 LeviMemEditor 的深灰绿设计语言。
// 特征：几乎不透明的中性深灰、4px 小圆角、1px 细边框、无阴影、
// 强调色只用在一个很窄的青绿上，其余全靠灰阶分层。
std::uint32_t kBg = 0xFF0A0C0E;          // 最底层背景（黑色调）
std::uint32_t kBgSoft = 0x1AFFFFFF;
std::uint32_t kTextMain = 0xFFF2F5F7;    // 主文字
std::uint32_t kTextDim = 0xFFA9ABB4;     // 次要文字
std::uint32_t kAccent = 0xFF9EDCE7;      // 薄荷青强调
std::uint32_t kAccentDeep = 0xFF4E7C86;
std::uint32_t kSecondary = 0xFF92A0A3;
std::uint32_t kTrack = 0xFF2A2D34;       // 进度槽 / 未选按钮
std::uint32_t kGlassTop = 0x1AFFFFFF;
std::uint32_t kGlassEdge = 0xFF41484B;   // 外边框
std::uint32_t kGlassBottom = 0xFF2B3134;
std::uint32_t kRowActive = 0x2E8DBDA2;   // 选中行（alpha 版本，宿主可能忽略）
// 选中行实色底：不依赖 alpha，日间浅绿、夜间深绿，保证与文字对比。
std::uint32_t kRowActiveSolid = 0xFF2E4550;
// 面板分层：top 是标题栏，raised 是浮起的卡片。
std::uint32_t kPanelBase = 0xFF1D2026;
std::uint32_t kSheet = 0xFF1D2026;
std::uint32_t kSheetSoft = 0xFF252930;   // 列表区底
std::uint32_t kFieldBg = 0xFF2E323A;     // 输入框
std::uint32_t kFieldActive = 0xFF383D46;
std::uint32_t kTopBar = 0xFF292D34;      // 标题栏
std::uint32_t kRaised = 0xFF2A2E35;      // 浮起卡片
std::uint32_t kLine = 0xFF353A42;        // 分割线
// 当前是否处于夜间模式，供日志与调试确认。
bool gNightMode = true;

// 按小时切换：18:00–6:00 为夜间，其余为日间。
void applyTheme(bool night) {
  gNightMode = night;
  if (night) {
    // 夜间：黑灰色调（中性灰，不带蓝也不带绿，不是纯黑）。
    // 层次：#1A1A1C 底 → #232326 面板 → #2C2C30 浮起 → #36363B 输入
    // 配色对齐 LeviMusic（同宿主的成熟模组）：
    // 近黑的中性底、比底稍亮的冷灰面板、单一薄荷青强调色，其余全靠灰阶分层。
    kBg = 0xFF111214;
    kBgSoft = 0x1AFFFFFF;
    // 主文字不用纯白：宿主给文字加固定阴影，纯白时阴影特别显眼。
    kTextMain = 0xFFF2F5F7;
    kTextDim = 0xFFA9ABB4;
    kAccent = 0xFF9EDCE7;      // 薄荷青，用作强调与选中态
    kAccentDeep = 0xFF4E7C86;
    kSecondary = 0xFF8E939C;
    kTrack = 0xFF2A2D34;
    kGlassTop = 0x14FFFFFF;
    kGlassEdge = 0xFF353A42;
    kGlassBottom = 0xFF23262C;
    kRowActive = 0x2E9EDCE7;
    kRowActiveSolid = 0xFF2E4550;
    kPanelBase = 0xFF1D2026;
    kSheet = 0xFF1D2026;
    kSheetSoft = 0xFF252930;
    kFieldBg = 0xFF2E323A;
    kFieldActive = 0xFF383D46;
    kTopBar = 0xFF292D34;
    kRaised = 0xFF2A2E35;
    kLine = 0xFF353A42;
  } else {
    // 日间：同一套中性灰阶翻到浅色，强调色加深保证对比度。
    kBg = 0xFFF4F6F5;
    kBgSoft = 0x1A0B1512;
    kTextMain = 0xFF0D1214;
    // 次要文字压到 0x2A3336：白底上必须够黑，否则读不清。
    kTextDim = 0xFF2A3336;
    kAccent = 0xFF2E6350;
    kAccentDeep = 0xFF2C5943;
    kSecondary = 0xFF5C6A6E;
    kTrack = 0xFFDFE4E3;
    kGlassTop = 0x40FFFFFF;
    kGlassEdge = 0xFFB9C2C1;
    kGlassBottom = 0xFFE6EAE9;
    kRowActive = 0x263E7A5E;
    // 日间用浅绿实色，深色文字压上去才看得清。
    kRowActiveSolid = 0xFFC6DCD2;
    kPanelBase = 0xFFF7F9F8;
    kSheet = 0xFFF7F9F8;
    kSheetSoft = 0xFFEFF3F2;
    kFieldBg = 0xFFEFF3F2;
    kFieldActive = 0xFFFFFFFF;
    kTopBar = 0xFFECF0EF;
    kRaised = 0xFFFFFFFF;
    kLine = 0xFFD5DCDB;
  }
}

// 取当前小时（本地时间）。宿主无游戏内时间接口，只能先按系统时间。
int currentHour() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  return tm.tm_hour;
}

bool isNightHour(int hour) { return hour >= 18 || hour < 6; }
// LumaUI 的动效统一是 0.2s ease。
constexpr long long kAnimMs = 200;
// 圆角半径：按钮 8、卡片 12、输入框 8。
constexpr float kRadiusBtn = 10.0f;
constexpr float kRadiusCard = 14.0f;
// 宿主文字命令的 y 是基线而不是顶部，按字号比例下移补偿。
constexpr float kTextBaselineRatio = 0.7f;
std::size_t utf8CharCount(const std::string &text) {
  std::size_t count = 0;
  for (std::size_t i = 0; i < text.size();) {
    const unsigned char lead = static_cast<unsigned char>(text[i]);
    std::size_t len = 1;
    if ((lead & 0xE0) == 0xC0) {
      len = 2;
    } else if ((lead & 0xF0) == 0xE0) {
      len = 3;
    } else if ((lead & 0xF8) == 0xF0) {
      len = 4;
    }
    i += len;
    ++count;
  }
  return count;
}

std::uint32_t blendColor(std::uint32_t from, std::uint32_t to, float t) {
  t = std::min(1.0f, std::max(0.0f, t));
  const auto channel = [t](std::uint32_t a, std::uint32_t b) {
    return static_cast<std::uint32_t>(a + (b - a) * t);
  };
  const std::uint32_t a = channel((from >> 24) & 0xFF, (to >> 24) & 0xFF);
  const std::uint32_t r = channel((from >> 16) & 0xFF, (to >> 16) & 0xFF);
  const std::uint32_t g = channel((from >> 8) & 0xFF, (to >> 8) & 0xFF);
  const std::uint32_t b = channel(from & 0xFF, to & 0xFF);
  return (a << 24) | (r << 16) | (g << 8) | b;
}
// 注册成功中文字体后，所有文字都指定该字体，避免生僻字变方块。
bool gUseCjkFont = false;

void *openPreloader() {
  void *handle = dlopen("libpreloader.so", RTLD_NOW | RTLD_NOLOAD);
  if (handle != nullptr) {
    return handle;
  }
  return dlopen("libpreloader.so", RTLD_NOW);
}

long long nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::string fmtTime(long long ms) {
  if (ms < 0) {
    ms = 0;
  }
  const long long total = ms / 1000;
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "%02lld:%02lld", total / 60, total % 60);
  return std::string(buffer);
}

// 估算一串文字的像素宽度：ASCII 按半个字宽，其余按一个字宽。
float estimateTextWidth(const std::string &text, float size) {
  float width = 0.0f;
  for (std::size_t i = 0; i < text.size();) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    std::size_t step = 1;
    if ((c & 0x80) == 0) {
      step = 1;
    } else if ((c & 0xE0) == 0xC0) {
      step = 2;
    } else if ((c & 0xF0) == 0xE0) {
      step = 3;
    } else {
      step = 4;
    }
    width += (c < 0x80) ? size * 0.62f : size;
    i += step;
  }
  return width;
}

// 跑马灯：放得下就原样返回；放不下就按 260 毫秒一步左移一个字，
// 滚到末尾停顿一轮再从头开始。
std::string marqueeText(const std::string &text, float width, float size) {
  if (text.empty() || width <= 8.0f) {
    return text;
  }
  // 逐个字符累计宽度，先找出能完整显示的字符数。
  std::vector<std::size_t> starts;
  float used = 0.0f;
  std::size_t cursor = 0;
  while (cursor < text.size()) {
    const unsigned char c = static_cast<unsigned char>(text[cursor]);
    std::size_t step = 1;
    if ((c & 0x80) == 0) {
      step = 1;
    } else if ((c & 0xE0) == 0xC0) {
      step = 2;
    } else if ((c & 0xF0) == 0xE0) {
      step = 3;
    } else {
      step = 4;
    }
    const float cw = (c < 0x80) ? size * 0.62f : size;
    if (used + cw > width) {
      break;
    }
    used += cw;
    starts.push_back(cursor);
    cursor += step;
  }
  // 整串都放得下，原样返回。
  if (cursor >= text.size()) {
    return text;
  }
  if (starts.empty()) {
    return text;
  }
  const std::size_t visible = starts.size();
  const std::size_t total = starts.size() + 1;
  const long long cycle = static_cast<long long>(total) + 3;
  const std::size_t start = static_cast<std::size_t>((nowMs() / 260) % cycle);
  if (start >= visible) {
    return text.substr(0, starts[visible - 1] + 1) + "…";
  }
  const std::size_t from = starts[start];
  const std::size_t endChar = start + visible;
  const std::size_t to = (endChar < starts.size()) ? starts[endChar] : text.size();
  return text.substr(from, to - from) + "…";
}


using Cmd = pl::modmenu::DrawCommand;
using Out = std::vector<Cmd>;

// ---- 临时探针：定位一个位置固定、来源不明的方形框 ----
// 只统计面积很大的矩形，最多打 240 行就自动停，不会长期刷日志。
// 定位完成后整段删除。
const char *gDbgSec = "pre";
int gDbgLeft = 240;

void rect(Out &out, float x, float y, float w, float h, std::uint32_t color) {
  if (gDbgLeft > 0 && w > 280.0f && h > 280.0f) {
    --gDbgLeft;
    __android_log_print(ANDROID_LOG_INFO, "MusicMod",
                        "大矩形 [%s] %.0f,%.0f %.0fx%.0f", gDbgSec, x, y, w,
                        h);
  }
  Cmd c;
  c.type = pl::modmenu::DrawCommandType::RectFilled;
  c.x = x;
  c.y = y;
  c.w = w;
  c.h = h;
  c.color = color;
  out.push_back(std::move(c));
}
// 圆角矩形：中间一块整矩形，左右两条竖矩形，四角各一个实心圆。
// 宿主没有圆角与裁剪能力，只能用图元拼。
// 前置声明：roundRect 用 circleFill 画圆角，但它在文件后面才定义。
void circleFill(Out &out, float cx, float cy, float d, std::uint32_t color);

void circleFill(Out &out, float cx, float cy, float d, std::uint32_t color);

void roundRect(Out &out, float x, float y, float w, float h, float r,
               std::uint32_t color) {
  // 宿主 RectFilled 原生支持圆角：把半径填进第三个浮点字段 x3 即可。
  // preloader 的 bridge 每条指令打包 x/y/w/h/x3/y3 六个浮点，
  // 由 Java 侧渲染成带抗锯齿的圆角矩形。
  //
  // 之前一直不知道这个字段，用「三块矩形 + 四个实心圆」硬拼圆角，
  // 拼接处必然出现接缝、深浅不一、角上台阶——这就是长期看到的「色差」。
  // 现在一条指令搞定，没有接缝。
  if (w <= 0.0f || h <= 0.0f) {
    return;
  }
  const float maxR = std::min(w, h) * 0.5f;
  if (r < 0.0f) {
    r = 0.0f;
  }
  if (r > maxR) {
    r = maxR;
  }
  Cmd c;
  c.type = pl::modmenu::DrawCommandType::RectFilled;
  c.x = x;
  c.y = y;
  c.w = w;
  c.h = h;
  c.x3 = r;
  c.color = color;
  out.push_back(std::move(c));
}
// 面板：LeviMemEditor 风格的实色卡片。
// 不用阴影和玻璃高光，改用「实色底 + 1px 外边框 + 4px 小圆角」分层。
// 保留 alpha 参数：宿主支持透明时仍可透出游戏画面。
void glassPanel(Out &out, float x, float y, float w, float h,
                float alpha = 0.85f, float overrideAlpha = -1.0f) {
  // 面板 = 一条圆角填充 + 顶部 1px 高光，不画描边。
  //
  // 为什么不做描边：宿主图元只有实心矩形和实心圆，画不出「环」。
  // 半透明面板想要一圈闭合边框，只能用四条细矩形去拼，
  // 而矩形拼不出圆角处的弧 —— 四个角要么留空（看起来边框断开），
  // 要么出现台阶和深浅不一（看起来色差）。
  // 同宿主的 LeviMusic 模组也是这个做法：填充 + 顶部高光，没有边框。
  const float r = (h <= 72.0f) ? 12.0f : (h <= 240.0f ? 18.0f : 22.0f);
  // 大面板也跟着「面板透明度」走，但设下限：宿主加不了文字描边，
  // 太透时字会糊在亮色场景里看不清。
  // 三形态各自独立：单行条、中面板的透明度由调用方通过 overrideAlpha 传入，
  // 大面板直接用 alpha 并保留可读性下限。传 -1 表示未单独设置、回落默认。
  float effAlpha = (overrideAlpha >= 0.0f) ? overrideAlpha : alpha;
  constexpr float kBigPanelMinAlpha = 0.35f;
  if (h > 240.0f && effAlpha < kBigPanelMinAlpha) {
    effAlpha = kBigPanelMinAlpha;
  }
  const std::uint32_t a = static_cast<std::uint32_t>(
      std::min(1.0f, std::max(0.05f, effAlpha)) * 255.0f);
  roundRect(out, x, y, w, h, r, (a << 24) | (kPanelBase & 0x00FFFFFFu));
  // 顶部高光：一条比面板略亮的细线，给出「浮在画面上」的边界感。
  // 只画直边段，圆角处不出头（圆角本身由圆角填充体现）。
  const std::uint32_t hlA =
      static_cast<std::uint32_t>(static_cast<float>(a) * 0.30f);
  if (hlA > 0) {
    rect(out, x + r, y + 1.0f, std::max(0.0f, w - r * 2.0f), 1.0f,
         (hlA << 24) | 0x00FFFFFFu);
  }
}
void glassBall(Out &out, float x, float y, float w, float h) {
  const auto circle = [&out](float cx, float cy, float d, std::uint32_t color) {
    Cmd c;
    c.type = pl::modmenu::DrawCommandType::CircleFilled;
    c.x = cx - d * 0.5f;
    c.y = cy - d * 0.5f;
    c.w = d;
    c.h = d;
    c.color = color;
    out.push_back(std::move(c));
  };
  const float cx = x + w * 0.5f;
  const float cy = y + h * 0.5f;
  const float d = std::min(w, h);
  // 五层：暗边、玻璃体、内部渐亮、主高光、次高光。
  circle(cx, cy, d, 0xFF080C11);
  circle(cx, cy, d - 4.0f, 0xFF1B2A33);
  circle(cx, cy - d * 0.06f, d * 0.72f, 0xFF24343F);
  circle(cx - d * 0.15f, cy - d * 0.18f, d * 0.36f, 0x22FFFFFF);
  circle(cx + d * 0.16f, cy + d * 0.2f, d * 0.22f, 0x12000000);
}
void text(Out &out, float x, float y, float size, std::uint32_t color,
          const std::string &value) {
  if (value.empty()) {
    return;
  }
  Cmd c;
  c.type = pl::modmenu::DrawCommandType::Text;
  c.x = x;
  c.y = y + size * kTextBaselineRatio;
  c.size = size;
  c.color = color;
  c.text = value;
  if (gUseCjkFont) {
    c.fontId = "musicmod.cjk";
  }
  out.push_back(std::move(c));
}

// 像素级平滑滚动：逐字判断是否落在可视区内，区外字符不画，
// 这样既能按像素连续位移，也不会溢出窗口。
void smoothMarquee(Out &out, float x, float y, float size, float width,
                   const std::string &value, std::uint32_t color,
                   std::uint32_t, long long nowMs) {
  if (value.empty() || width <= 8.0f) {
    return;
  }
  const float textW = estimateTextWidth(value, size);
  if (textW <= width) {
    text(out, x, y, size, color, value);
    return;
  }
  const float gap = width * 0.35f;
  const float span = textW + gap;
  const float offset = std::fmod((nowMs % 100000) * 0.045f, span);
  for (int copy = 0; copy < 2; ++copy) {
    float cursor = (copy == 0) ? -offset : (span - offset);
    std::size_t i = 0;
    while (i < value.size()) {
      const unsigned char c = static_cast<unsigned char>(value[i]);
      std::size_t step = 1;
      if ((c & 0x80) == 0) {
        step = 1;
      } else if ((c & 0xE0) == 0xC0) {
        step = 2;
      } else if ((c & 0xF0) == 0xE0) {
        step = 3;
      } else {
        step = 4;
      }
      const float cw = (c < 0x80) ? size * 0.62f : size;
      if (cursor >= 0.0f && cursor + cw <= width) {
        text(out, x + cursor, y, size, color, value.substr(i, step));
      }
      cursor += cw;
      i += step;
    }
  }
}

// 单命令跑马灯：整串文字只压一条绘制命令，超宽时按字符窗口滚动。
// 旧实现把每个字拆成独立命令，列表一屏就产生两三百条，渲染线程扛不住。
void marqueeFit(Out &out, float x, float y, float size, float width,
                const std::string &value, std::uint32_t color,
                long long phaseMs) {
  if (value.empty() || width <= 8.0f) {
    return;
  }
  const float textW = estimateTextWidth(value, size);
  // 估算字宽与真实字体宽度仍有一点点误差，留 3% 安全边距，
  // 宁可多滚一点，也不要出现「实际已经溢出、但判定成放得下所以不滚」。
  if (textW * 1.03f <= width) {
    text(out, x, y, size, color, value);
    return;
  }
  // 记录每个字符的字节起点与像素起点，供窗口滚动使用。
  std::vector<std::size_t> offsets;
  std::vector<float> pixels;
  float cursor = 0.0f;
  for (std::size_t i = 0; i < value.size();) {
    const unsigned char c = static_cast<unsigned char>(value[i]);
    std::size_t step = 1;
    if ((c & 0xE0) == 0xC0) {
      step = 2;
    } else if ((c & 0xF0) == 0xE0) {
      step = 3;
    } else if ((c & 0xF8) == 0xF0) {
      step = 4;
    }
    offsets.push_back(i);
    pixels.push_back(cursor);
    cursor += (c < 0x80) ? size * 0.62f : size;
    i += step;
  }
  const float gap = size * 2.5f;
  // 标准跑马灯：先在原位停留（让人看清开头），再匀速左移，
  // 末尾对齐后再停留，然后瞬回开头。
  //
  // 旧实现是「整串从左侧界外滑入、穿过窗口、从右侧滑出」，
  // 一个周期要 (textW + width + gap) / 65 像素每秒，长标题能到十几秒，
  // 其中大半时间窗口里几乎没有字，看起来就像「半天不出来一个字」。
  const float overflow = textW + gap - width;
  const float holdMs = 1800.0f;  // 两端各停 1.8 秒
  // 移动时长按 28 像素/秒估算，再钳到 [1.5 秒, 10 秒]。
  // 玩家反馈 60 像素/秒偏快、字跟不上眼睛，这里整体放慢一档；
  // 溢出很多的长标题也不能拖太久，所以给个上限。
  float moveMs = overflow / 0.028f;
  if (moveMs < 1500.0f) {
    moveMs = 1500.0f;
  }
  if (moveMs > 10000.0f) {
    moveMs = 10000.0f;
  }
  const float cycle = holdMs * 2.0f + moveMs;
  const float t = std::fmod(static_cast<float>(phaseMs % 10000000LL), cycle);
  float shift = 0.0f;
  if (t < holdMs) {
    shift = 0.0f;
  } else if (t < holdMs + moveMs) {
    // 匀速：总位移按时间比例分配，不受速度常量影响。
    shift = -overflow * ((t - holdMs) / moveMs);
  } else {
    shift = -overflow;
  }
  // 宿主没有裁剪能力，只能提交「完整落在窗口内」的连续字符段，
  // 出界的部分一个都不发，避免文字溢出行外。
  const float startPx = x + shift;
  std::size_t first = 0;
  // 跳过整体还在左界外的字符。
  while (first + 1 < pixels.size() &&
         startPx + pixels[first] + size * 0.6f < x) {
    ++first;
  }
  std::size_t last = first;
  // 只收完整落在右界内的字符。
  while (last < pixels.size() &&
         startPx + pixels[last] + size * 0.6f <= x + width) {
    ++last;
  }
  if (last <= first) {
    return;
  }
  const float drawX = startPx + pixels[first];
  const std::size_t beginByte = offsets[first];
  const std::size_t endByte =
      (last < offsets.size()) ? offsets[last] : value.size();
  text(out, drawX, y, size, color, value.substr(beginByte, endByte - beginByte));
}

void image(Out &out, float x, float y, float w, float h,
           const std::string &id) {
  Cmd c;
  c.type = pl::modmenu::DrawCommandType::Image;
  c.x = x;
  c.y = y;
  c.w = w;
  c.h = h;
  // 颜色当透明度用：不填 0 的话宿主按全透明处理，图片画不出来。
  c.color = 0xFFFFFFFF;
  c.imageId = id;
  out.push_back(std::move(c));
}

// 以下图标全部用矩形堆叠绘制。宿主对 Line 图元不渲染，实测暂停图标可见、
// 播放图标消失，所以一律改成实心矩形与扫描线三角形。
void triFill(Out &out, float ax, float ay, float bx, float by, float cx,
             float cy, std::uint32_t color) {
  constexpr int kRows = 9;
  const float minY = std::min(std::min(ay, by), cy);
  const float maxY = std::max(std::max(ay, by), cy);
  const float span = maxY - minY;
  if (span < 1.0f) {
    return;
  }
  const float xs[3] = {ax, bx, cx};
  const float ys[3] = {ay, by, cy};
  const float step = span / kRows;
  for (int i = 0; i < kRows; ++i) {
    const float y = minY + i * step;
    const float mid = y + step * 0.5f;
    float lo = 1.0e9f;
    float hi = -1.0e9f;
    for (int e = 0; e < 3; ++e) {
      const float x0 = xs[e];
      const float y0 = ys[e];
      const float x1 = xs[(e + 1) % 3];
      const float y1 = ys[(e + 1) % 3];
      const bool crosses = (y0 <= mid && mid <= y1) || (y1 <= mid && mid <= y0);
      if (!crosses) {
        continue;
      }
      float sample = x0;
      if (std::fabs(y1 - y0) > 0.001f) {
        sample = x0 + (mid - y0) * (x1 - x0) / (y1 - y0);
      }
      lo = std::min(lo, sample);
      hi = std::max(hi, sample);
    }
    if (hi > lo) {
      rect(out, lo, y, hi - lo, step + 0.8f, color);
    }
  }
}

// 用一串小方块拼出斜线，宿主的 Line 图元不渲染，只能这样画叉号。
void diag(Out &out, float x1, float y1, float x2, float y2, float thick,
          std::uint32_t color) {
  constexpr int kSteps = 16;
  for (int i = 0; i < kSteps; ++i) {
    const float t = (static_cast<float>(i) + 0.5f) / kSteps;
    const float px = x1 + (x2 - x1) * t;
    const float py = y1 + (y2 - y1) * t;
    rect(out, px - thick * 0.5f, py - thick * 0.5f, thick, thick, color);
  }
}

void glyphClose(Out &out, float cx, float cy, float s, std::uint32_t color) {
  const float h = s * 0.5f;
  diag(out, cx - h, cy - h, cx + h, cy + h, s * 0.14f, color);
  diag(out, cx + h, cy - h, cx - h, cy + h, s * 0.14f, color);
}

void glyphPlay(Out &out, float cx, float cy, float s,
               std::uint32_t color) {
  // 三角形的视觉质心在左侧，整体右移一点看起来才居中。
  triFill(out, cx - s * 0.30f, cy - s * 0.52f, cx + s * 0.52f, cy,
          cx - s * 0.30f, cy + s * 0.52f, color);
}

void glyphPause(Out &out, float cx, float cy, float s,
                std::uint32_t color) {
  // 两条竖杠：宽 0.24s，间距 0.20s，整体高 1.04s。
  // 原来宽只有 0.18s，小尺寸下左右两条细得几乎看不见。
  const float barW = s * 0.24f;
  const float gap = s * 0.20f;
  rect(out, cx - gap - barW, cy - s * 0.52f, barW, s * 1.04f, color);
  rect(out, cx + gap, cy - s * 0.52f, barW, s * 1.04f, color);
}

void glyphPrev(Out &out, float cx, float cy, float s,
               std::uint32_t color) {
  // 左侧竖杠 + 左向三角。
  const float barW = s * 0.22f;
  rect(out, cx - s * 0.50f, cy - s * 0.52f, barW, s * 1.04f, color);
  triFill(out, cx + s * 0.44f, cy - s * 0.52f, cx - s * 0.26f, cy,
          cx + s * 0.44f, cy + s * 0.52f, color);
}

void circleFill(Out &out, float cx, float cy, float d,
                std::uint32_t color) {
  // 宿主 CircleFilled 的语义是【x/y 为圆心，size 为半径】。
  // 依据：同宿主的 LeviMusic 模组就是这么用的（disc() 里 c.size = radius）。
  // 以前我们按「左上角 + 宽高」传，导致整圈偏移半个直径。
  // w/h 一并填上，兼容把 w/h 当直径的旧渲染路径。
  Cmd c;
  c.type = pl::modmenu::DrawCommandType::CircleFilled;
  c.x = cx;
  c.y = cy;
  c.w = d;
  c.h = d;
  c.size = d * 0.5f;
  c.color = color;
  out.push_back(c);
}

// 玻璃圆按钮：落影、上亮下暗的球面、左上高光。
// 五个副本足以看出厚度，命令数也可控，不会拖垮帧率。
void glassCircle(Out &out, float cx, float cy, float d, bool primary) {
  const std::uint32_t bodyTop = primary ? 0xFF2E5A54 : 0xFF1E2B33;
  const std::uint32_t bodyLow = primary ? 0xFF22443F : 0xFF161F26;
  circleFill(out, cx, cy + d * 0.05f, d + 6.0f, 0x33000000);
  circleFill(out, cx, cy, d, bodyTop);
  circleFill(out, cx, cy + d * 0.12f, d * 0.86f, bodyLow);
  circleFill(out, cx - d * 0.17f, cy - d * 0.2f, d * 0.34f, 0x24FFFFFF);
  if (primary) {
    circleFill(out, cx, cy - d * 0.34f, d * 0.5f, 0x14FFFFFF);
  }
}

void glyphNext(Out &out, float cx, float cy, float s,
               std::uint32_t color) {
  // 右向三角 + 右侧竖杠。
  const float barW = s * 0.22f;
  rect(out, cx + s * 0.50f - barW, cy - s * 0.52f, barW, s * 1.04f, color);
  triFill(out, cx - s * 0.44f, cy - s * 0.52f, cx + s * 0.26f, cy,
          cx - s * 0.44f, cy + s * 0.52f, color);
}

void glyphExpand(Out &out, float cx, float cy, float s, std::uint32_t color) {
  const float t = std::max(2.0f, s * 0.12f);
  const float h = s * 0.5f;
  const float arm = s * 0.36f;
  // 左上
  rect(out, cx - h, cy - h, arm, t, color);
  rect(out, cx - h, cy - h, t, arm, color);
  // 右上
  rect(out, cx + h - arm, cy - h, arm, t, color);
  rect(out, cx + h - t, cy - h, t, arm, color);
  // 左下
  rect(out, cx - h, cy + h - t, arm, t, color);
  rect(out, cx - h, cy + h - arm, t, arm, color);
  // 右下
  rect(out, cx + h - arm, cy + h - t, arm, t, color);
  rect(out, cx + h - t, cy + h - arm, t, arm, color);
}

void glyphCollapse(Out &out, float cx, float cy, float s, std::uint32_t color) {
  const float t = std::max(2.0f, s * 0.12f);
  const float h = s * 0.5f;
  const float arm = s * 0.36f;
  rect(out, cx - h + arm, cy - h, t, arm, color);
  rect(out, cx - h + arm, cy - h + arm - t, arm - t, t, color);
  rect(out, cx + h - arm - t, cy - h, t, arm, color);
  rect(out, cx + h - arm, cy - h + arm - t, arm - t, t, color);
  rect(out, cx - h + arm, cy + h - arm, t, arm, color);
  rect(out, cx - h + arm, cy + h - arm, arm - t, t, color);
  rect(out, cx + h - arm - t, cy + h - arm, t, arm, color);
  rect(out, cx + h - arm, cy + h - arm, arm - t, t, color);
}

// ═══════════════════ 图标贴图 ═══════════════════
// 宿主的图元只有实心矩形、实心圆、实心三角形，拼斜线和弧必定出锯齿：
// 三角是逐行扫描矩形堆的，边上有台阶；两条竖线之间的缝也压不干净。
// 所以三大按键改成「CPU 生成抗锯齿位图 → registerImage 注册 → Image 整块贴」。
// 路由不依赖外部 png 文件，图在内存里画，随模组一起走。
enum IconKind { kIconPrev = 0, kIconPlay = 1, kIconPause = 2, kIconNext = 3 };

const char *const kIconNames[4] = {"prev", "play", "pause", "next"};
std::string gIconIds[4];
bool gIconsReady = false;

// 归一化坐标下的形状判定：u/v 都在 0..1。
bool iconHit(int kind, float u, float v) {
  const auto inRect = [](float px, float py, float x0, float y0, float x1,
                         float y1) {
    return px >= x0 && px <= x1 && py >= y0 && py <= y1;
  };
  // 点在三角形内：三条边的叉积同号。
  const auto inTri = [](float px, float py, float ax, float ay, float bx,
                        float by, float cx, float cy) {
    const float d1 = (px - bx) * (ay - by) - (ax - bx) * (py - by);
    const float d2 = (px - cx) * (by - cy) - (bx - cx) * (py - cy);
    const float d3 = (px - ax) * (cy - ay) - (cx - ax) * (py - ay);
    const bool neg = (d1 < 0) || (d2 < 0) || (d3 < 0);
    const bool pos = (d1 > 0) || (d2 > 0) || (d3 > 0);
    return !(neg && pos);
  };
  switch (kind) {
  case kIconPrev:
    // 竖条 + 左向三角
    return inRect(u, v, 0.22f, 0.18f, 0.34f, 0.82f) ||
           inTri(u, v, 0.80f, 0.16f, 0.38f, 0.50f, 0.80f, 0.84f);
  case kIconPlay:
    // 右向三角。右侧顶点略收，视觉重心才在正中。
    return inTri(u, v, 0.30f, 0.16f, 0.80f, 0.50f, 0.30f, 0.84f);
  case kIconPause:
    // 两条圆头竖条（用矩形近似，边缘靠超采样磨平）
    return inRect(u, v, 0.32f, 0.18f, 0.44f, 0.82f) ||
           inRect(u, v, 0.56f, 0.18f, 0.68f, 0.82f);
  case kIconNext:
    return inTri(u, v, 0.20f, 0.16f, 0.62f, 0.50f, 0.20f, 0.84f) ||
           inRect(u, v, 0.66f, 0.18f, 0.78f, 0.82f);
  default:
    return false;
  }
}

// 4x4 超采样生成 RGBA 图标，alpha 就是覆盖率。
// tint 是烘焙进去的颜色：播放/暂停用薄荷青，上/下一首用白色，
// 这样切歌键保持中性、播放键有强调色，和按钮底的分层一致。
std::vector<unsigned char> makeIconRgba(int kind, int size,
                                        std::uint32_t tint) {
  std::vector<unsigned char> px(
      static_cast<std::size_t>(size) * size * 4, 0);
  constexpr int kSample = 4;
  constexpr int kTotal = kSample * kSample;
  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x) {
      int hits = 0;
      for (int sy = 0; sy < kSample; ++sy) {
        for (int sx = 0; sx < kSample; ++sx) {
          const float u = (static_cast<float>(x) +
                           (static_cast<float>(sx) + 0.5f) / kSample) /
                          static_cast<float>(size);
          const float v = (static_cast<float>(y) +
                           (static_cast<float>(sy) + 0.5f) / kSample) /
                          static_cast<float>(size);
          if (iconHit(kind, u, v)) {
            ++hits;
          }
        }
      }
      const std::size_t o =
          (static_cast<std::size_t>(y) * size + x) * 4;
      px[o + 0] = static_cast<unsigned char>((tint >> 16) & 0xFF);
      px[o + 1] = static_cast<unsigned char>((tint >> 8) & 0xFF);
      px[o + 2] = static_cast<unsigned char>(tint & 0xFF);
      px[o + 3] = static_cast<unsigned char>(255 * hits / kTotal);
    }
  }
  return px;
}

// 贴图版图标：贴图没准备好时退回原来的图元画法，保证不会整块消失。
void iconPrev(Out &out, float cx, float cy, float s,
                std::uint32_t color) {
  // 贴图图标已停用（见下方 glyph* 的说明），统一走矢量图元。
  glyphPrev(out, cx, cy, s, color);
}
void iconPlay(Out &out, float cx, float cy, float s,
                std::uint32_t color) {
  // 贴图图标已停用（见下方 glyph* 的说明），统一走矢量图元。
  glyphPlay(out, cx, cy, s, color);
}
void iconPause(Out &out, float cx, float cy, float s,
                std::uint32_t color) {
  // 贴图图标已停用（见下方 glyph* 的说明），统一走矢量图元。
  glyphPause(out, cx, cy, s, color);
}
void iconNext(Out &out, float cx, float cy, float s,
                std::uint32_t color) {
  // 贴图图标已停用（见下方 glyph* 的说明），统一走矢量图元。
  glyphNext(out, cx, cy, s, color);
}

bool insideRect(float px, float py, float x, float y, float w, float h) {
  // 统一给按钮留 10 像素触摸容差，手指略偏也能点中。
  constexpr float kSlop = 10.0f;
  return px >= x - kSlop && px <= x + w + kSlop && py >= y - kSlop &&
         py <= y + h + kSlop;
}

// 按钮命中区按贴图矩形再向外扩一圈，纵向扩得比横向多。
bool insideButton(float px, float py, float x, float y, float w, float h,
                  float padX = 8.0f, float padY = 12.0f) {
  // 旧版 insideRect 的 5px 容差必须保留，否则换成 insideButton 后反而更小。
  constexpr float kBase = 5.0f;
  const float dx = padX + kBase;
  const float dy = padY + kBase;
  return px >= x - dx && px <= x + w + dx && py >= y - dy &&
         py <= y + h + dy;
}
// 面板数字键盘的 12 个键：1-9、删、0、确。
const char *digitLabel(int index) {
  static const char *kLabels[12] = {"1", "2", "3", "4", "5", "6",
                                    "7", "8", "9", "删", "0", "确"};
  return (index >= 0 && index < 12) ? kLabels[index] : kLabels[0];
}

// 取 UTF-8 首字符，用作头像里的一个字。
// 右上角头像改成真实图片后已无人调用，保留是因为短账号名场景可能再用；
// 加 maybe_unused 避免 -Wall 下报未使用警告。
[[maybe_unused]] std::string firstChar(const std::string &text) {
  if (text.empty()) {
    return "我";
  }
  const unsigned char c = static_cast<unsigned char>(text[0]);
  std::size_t len = 1;
  if ((c & 0xE0) == 0xC0) {
    len = 2;
  } else if ((c & 0xF0) == 0xE0) {
    len = 3;
  } else if ((c & 0xF8) == 0xF0) {
    len = 4;
  }
  if (len > text.size()) {
    len = text.size();
  }
  return text.substr(0, len);
}

// FNV-1a 哈希：内容没变就不重复把图片交给宿主，减少纹理替换。
std::size_t hashBytes(const std::vector<unsigned char> &data) {
  std::size_t h = 1469598103934665603ull;
  for (unsigned char b : data) {
    h ^= b;
    h *= 1099511628211ull;
  }
  return h;
}

// 手机号与验证码只用数字。输入法有两种提交方式：逐字追加、整段重发，
// 这里都兼容，避免出现 1381381 这种叠字。

// 播放模式图标：顺序（三条横线 + 向下箭头）、随机（交叉箭头）、
// 单曲循环（圆弧 + 数字 1）。
// 全部用手绘图元堆出来——宿主对 Line 图元不渲染（实测暂停图标可见、
// 播放图标消失），所以一律用实心矩形、三角和多边形。
void glyphModeSequential(Out &out, float cx, float cy, float s,
                         std::uint32_t color) {
  const float w = s * 0.62f;
  const float gap = s * 0.24f;
  // 三条横线，自上而下依次缩短，末端各带一个三角指示向下进行。
  for (int i = 0; i < 3; ++i) {
    const float ly = cy - gap + i * gap;
    const float lw = w * (1.0f - 0.18f * static_cast<float>(i));
    rect(out, cx - w * 0.5f, ly - s * 0.045f, lw, s * 0.09f, color);
  }
  triFill(out, cx + w * 0.5f - s * 0.06f, cy - s * 0.02f,
          cx + w * 0.5f + s * 0.26f, cy + gap * 0.5f,
          cx + w * 0.5f - s * 0.06f, cy + gap + s * 0.04f, color);
}

void glyphModeShuffle(Out &out, float cx, float cy, float s,
                      std::uint32_t color) {
  // 两条交叉的斜线，用一串小方块拼出来。
  // 直接用 diag 会画出很厚的块，缩小后成一坨，所以按固定步长点阵化。
  const int steps = 9;
  const float half = s * 0.42f;
  for (int i = 0; i < steps; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(steps - 1);
    const float px = cx - half + t * half * 2.0f;
    const float py1 = cy - half + t * half * 2.0f;
    const float py2 = cy + half - t * half * 2.0f;
    rect(out, px - s * 0.05f, py1 - s * 0.05f, s * 0.1f, s * 0.1f, color);
    rect(out, px - s * 0.05f, py2 - s * 0.05f, s * 0.1f, s * 0.1f, color);
  }
  // 右端两个箭头尖，表示两条线各自往右上/右下走。
  triFill(out, cx + half - s * 0.02f, cy - s * 0.02f,
          cx + half + s * 0.3f, cy - s * 0.3f,
          cx + half + s * 0.02f, cy - s * 0.34f, color);
}

void glyphModeRepeatOne(Out &out, float cx, float cy, float s,
                        std::uint32_t color) {
  // 圆弧用点阵拼：半径 r，绕 300 度留一个缺口，缺口处画箭头。
  const float r = s * 0.34f;
  const int steps = 26;
  for (int i = 0; i < steps; ++i) {
    const float a = 0.5f + (6.2831853f * 0.86f) *
                               (static_cast<float>(i) /
                                static_cast<float>(steps - 1));
    const float px = cx + std::cos(a) * r;
    const float py = cy + std::sin(a) * r;
    rect(out, px - s * 0.05f, py - s * 0.05f, s * 0.1f, s * 0.1f, color);
  }
  // 中间一个「1」：竖条 + 顶部斜钩 + 底部横线。
  rect(out, cx - s * 0.035f, cy - s * 0.16f, s * 0.07f, s * 0.34f, color);
  rect(out, cx - s * 0.15f, cy - s * 0.05f, s * 0.11f, s * 0.065f, color);
}

// 爱心：两个圆 + 一个倒三角，标准画法，缩到 18px 仍然认得出。
// hash 统一小写：搜索结果取 FileHash，收藏接口取 hash，
// 两者值可能只差大小写。不归一就会出现「收藏了却没显示收藏」。
std::string lowerHash(const std::string &in) {
  std::string out = in;
  for (auto &ch : out) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return out;
}

void glyphHeart(Out &out, float cx, float cy, float s, bool filled,
                std::uint32_t color) {
  const float r = s * 0.27f;
  if (filled) {
    circleFill(out, cx - r * 0.82f, cy - r * 0.30f, r * 2.0f, color);
    circleFill(out, cx + r * 0.82f, cy - r * 0.30f, r * 2.0f, color);
    triFill(out, cx - r * 1.70f, cy + r * 0.10f,
            cx + r * 1.70f, cy + r * 0.10f, cx, cy + r * 2.1f, color);
  } else {
    // 空心：用更小的圆挖掉中间，再用底色压一层。
    // 这里不做真正的挖除（没有混合模式），改用描边式点阵。
    const int steps = 24;
    for (int i = 0; i < steps; ++i) {
      const float t = static_cast<float>(i) / static_cast<float>(steps);
      const float a = 3.1415926f * t * 2.0f;
      const float px = cx + std::cos(a) * r * 1.5f;
      const float py = cy - r * 0.2f + std::sin(a) * r * 1.3f;
      if (sinf(a) > 0.35f) {
        continue;   // 下半圆属于尖角区，交给三角
      }
      rect(out, px - s * 0.045f, py - s * 0.045f, s * 0.09f, s * 0.09f, color);
    }
    triFill(out, cx - r * 1.35f, cy + r * 0.05f,
            cx + r * 1.35f, cy + r * 0.05f, cx, cy + r * 1.75f, color);
  }
}

void appendDigits(std::string &target, const std::string &text,
                  std::size_t limit) {
  std::string digits;
  digits.reserve(text.size());
  for (char c : text) {
    if (c >= '0' && c <= '9') {
      digits.push_back(c);
    }
  }
  if (digits.empty()) {
    return;
  }
  // 整段重发：新串以已有内容开头，直接替换。
  if (digits.size() > target.size() &&
      digits.compare(0, target.size(), target) == 0) {
    target = digits;
  } else if (digits.size() > 1 && target.size() >= digits.size() &&
             target.compare(target.size() - digits.size(), digits.size(),
                            digits) == 0) {
    // 只有多字符整段重复才去重。
    // 单个数字必须允许连续相同，例如 11、10086、1122。
    return;
  } else {
    target += digits;
  }
  if (target.size() > limit) {
    target.resize(limit);
  }
}

} // namespace

void HudUi::setRealScreen(float w, float h) {
  std::lock_guard<std::mutex> lock(mMutex);
  mRealW = w;
  mRealH = h;
  __android_log_print(ANDROID_LOG_INFO, kTag, "真实屏幕 %.0fx%.0f 绘制面 %.0fx%.0f",
                      mRealW, mRealH, mScreenW, mScreenH);
}
void HudUi::setEngine(AudioEngine *engine) {
  std::lock_guard<std::mutex> lock(mMutex);
  mEngine = engine;
  if (mEngine != nullptr) {
    mEngine->setVolume(mVolume);
  }
}

void HudUi::setSearchHandler(std::function<void(const std::string &)> handler) {
  std::lock_guard<std::mutex> lock(mMutex);
  mSearchHandler = std::move(handler);
}

void HudUi::setTabHandler(std::function<void(int)> handler) {
  std::lock_guard<std::mutex> lock(mMutex);
  mTabHandler = std::move(handler);
}

void HudUi::setNotice(const std::string &text, long long durationMs) {
  std::lock_guard<std::mutex> lock(mMutex);
  setNoticeLocked(text, durationMs);
}

void HudUi::setNoticeLocked(const std::string &text, long long durationMs) {
  mNotice = text;
  mNoticeUntil = nowMs() + durationMs;
  mDirty = true;
}

bool HudUi::handleTextInput(const std::string &text) {
  std::function<void(const std::string &)> handler;
  std::string keyword;
  __android_log_print(ANDROID_LOG_INFO, kTag, "文本输入: 长度=%zu",
                      text.size());
  {
    std::lock_guard<std::mutex> lock(mMutex);
    if (mLoginField != 0) {
      // 登录页输入：落到手机号或验证码，不污染搜索框。
      if (mLoginField == 1) {
        appendDigits(mLoginPhone, text, 11);
      } else {
        appendDigits(mLoginCode, text, 6);
      }
      mDirty = true;
      return true;
    }
    if (mInputActive) {
      // 输入框模式：先累积到输入框，等用户点确定再提交。
      mInputText += text;
      if (mInputText.size() > 40) {
        mInputText.resize(40);
      }
      mDirty = true;
      return true;
    }
    if (!mSearchActive) {
      return false;
    }
    mSearchActive = false;
    keyword = text;
    // 去掉首尾空白，空串直接忽略。
    const std::size_t begin = keyword.find_first_not_of(" \t\r\n");
    const std::size_t end = keyword.find_last_not_of(" \t\r\n");
    if (begin == std::string::npos) {
      keyword.clear();
    } else {
      keyword = keyword.substr(begin, end - begin + 1);
    }
    handler = mSearchHandler;
    if (!keyword.empty()) {
      setNoticeLocked("正在搜索 " + keyword, 4000);
    } else {
      setNoticeLocked("没有输入关键词", 2000);
    }
  }
  if (!keyword.empty() && handler) {
    __android_log_print(ANDROID_LOG_INFO, kTag, "搜索请求: %s",
                        keyword.c_str());
    handler(keyword);
  }
  return true;
}

bool HudUi::handleKey(int keyCode, unsigned int unicodeChar, bool isDown) {
  if (!isDown) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mMutex);
  __android_log_print(ANDROID_LOG_INFO, kTag, "按键: %d 输入框=%d 登录位=%d",
                      keyCode, mInputActive ? 1 : 0, mLoginField);
  if (mLoginField != 0) {
    // 退格按 UTF-8 字符边界删，避免把汉字截半个。
    if (keyCode == 67) {
      std::string &target = (mLoginField == 1) ? mLoginPhone : mLoginCode;
      while (!target.empty()) {
        const unsigned char byte = static_cast<unsigned char>(target.back());
        target.pop_back();
        if ((byte & 0xC0) != 0x80) {
          break;
        }
      }
      mDirty = true;
      return true;
    }
    return false;
  }
  if (!mInputActive) {
    return false;
  }
  if (keyCode == 67) { // KEYCODE_DEL
    // 按 UTF-8 字符边界删，避免把汉字截成半个。
    while (!mInputText.empty()) {
      const unsigned char byte = static_cast<unsigned char>(mInputText.back());
      mInputText.pop_back();
      if ((byte & 0xC0) != 0x80) {
        break;
      }
    }
    mDirty = true;
    return true;
  }
  // 可见字符：把 unicodeChar 转成 UTF-8 追加到输入框。
  // 中文输入法在拼音阶段只发 KeyEvent，不触发 TextInputEvent，
  // 所以必须在这里接住，否则字会全部留在输入法浮条上。
  if (unicodeChar >= 0x20 && unicodeChar != 0x7F) {
    if (unicodeChar < 0x80) {
      mInputText.push_back(static_cast<char>(unicodeChar));
    } else if (unicodeChar < 0x800) {
      mInputText.push_back(static_cast<char>(0xC0 | (unicodeChar >> 6)));
      mInputText.push_back(static_cast<char>(0x80 | (unicodeChar & 0x3F)));
    } else if (unicodeChar < 0x10000) {
      mInputText.push_back(static_cast<char>(0xE0 | (unicodeChar >> 12)));
      mInputText.push_back(static_cast<char>(0x80 | ((unicodeChar >> 6) & 0x3F)));
      mInputText.push_back(static_cast<char>(0x80 | (unicodeChar & 0x3F)));
    } else {
      mInputText.push_back(static_cast<char>(0xF0 | (unicodeChar >> 18)));
      mInputText.push_back(static_cast<char>(0x80 | ((unicodeChar >> 12) & 0x3F)));
      mInputText.push_back(static_cast<char>(0x80 | ((unicodeChar >> 6) & 0x3F)));
      mInputText.push_back(static_cast<char>(0x80 | (unicodeChar & 0x3F)));
    }
    if (mInputText.size() > 40) {
      mInputText.resize(40);
    }
    mDirty = true;
    __android_log_print(ANDROID_LOG_INFO, kTag, "按键输入: %s",
                        mInputText.c_str());
    return true;
  }
  return false;
}

bool HudUi::resolveHostApi() {
  void *preloader = openPreloader();
  if (preloader == nullptr) {
    return false;
  }

  mRegisterModule = dlsym(
      preloader, "_ZN2pl7modmenu14registerModuleERKNS0_10ModuleInfoE");
  mSubmitDraw = dlsym(
      preloader,
      "_ZN2pl7modmenu18submitDrawCommandsENSt6__ndk117basic_string_viewIcNS1_11char_"
      "traitsIcEEEENS1_4spanIKNS0_11DrawCommandELm18446744073709551615EEE");
  mUnregisterModule = dlsym(
      preloader,
      "_ZN2pl7modmenu16unregisterModuleENSt6__ndk117basic_string_viewIcNS1_11char_"
      "traitsIcEEEE");
  mRegisterTouch = dlsym(
      preloader,
      "_ZN2pl5input21registerTouchCallbackENSt6__ndk18functionIFbRKNS0_10TouchEventE"
      "EEE");
  mRegisterImage = dlsym(
      preloader,
      "_ZN2pl7modmenu13registerImageENSt6__ndk117basic_string_viewIcNS1_11char_tra"
      "itsIcEEEENS1_4spanIKhLm18446744073709551615EEEii");
  mGetSurfaceSize =
      dlsym(preloader, "_ZN2pl7modmenu17getHudSurfaceSizeEv");
  mRegisterText = dlsym(
      preloader,
      "_ZN2pl5input25registerTextInputCallbackENSt6__ndk18functionIFbRKNS0_"
      "14TextInputEventEEEE");
  mShowKeyboard = dlsym(preloader, "_ZN2pl5input12showKeyboardEv");
  mRegisterFont = dlsym(
      preloader,
      "_ZN2pl7modmenu12registerFontENSt6__ndk117basic_string_viewIcNS1_11char_"
      "traitsIcEEEENS1_4spanIKhLm18446744073709551615EEE");
  mRegisterKey = dlsym(
      preloader,
      "_ZN2pl5input19registerKeyCallbackENSt6__ndk18functionIFbRKNS0_8KeyEvent"
      "EEEE");

  __android_log_print(ANDROID_LOG_INFO, kTag,
                      "界面: module=%d draw=%d touch=%d image=%d surface=%d "
                      "text=%d key=%d kbd=%d",
                      mRegisterModule != nullptr, mSubmitDraw != nullptr,
                      mRegisterTouch != nullptr, mRegisterImage != nullptr,
                      mGetSurfaceSize != nullptr, mRegisterText != nullptr,
                      mRegisterKey != nullptr, mShowKeyboard != nullptr);
  return mRegisterModule != nullptr && mSubmitDraw != nullptr;
}

void HudUi::resolveSurface() {
  if (mGetSurfaceSize == nullptr) {
    return;
  }
  auto fn = reinterpret_cast<GetSurfaceFn>(mGetSurfaceSize);
  const auto size = fn();
  if (size.width > 100.0f && size.height > 100.0f) {
    std::lock_guard<std::mutex> lock(mMutex);
    if (std::fabs(size.width - mScreenW) > 0.5f ||
        std::fabs(size.height - mScreenH) > 0.5f) {
      __android_log_print(ANDROID_LOG_INFO, kTag, "界面: 绘制面变化 %.0fx%.0f",
                          size.width, size.height);
      mScreenW = size.width;
      mScreenH = size.height;
      mDirty = true;
      // 绘制面尺寸变了要重新摆放，否则面板会按旧尺寸偏在一侧。
      placePanel();
    }
  }
}

void HudUi::placePanel() {
  // 调用方已持锁。先算出目标矩形，再从当前值过渡过去。
  //
  // 起点必须是「当前真实显示的位置」。
  // 如果上一次动画还没走完就又切了形态，mPanel* 是插值中间值，
  // 直接拿来当起点是对的；但如果上一次动画已结束，
  // mPanel* 应该就等于 mTarget*（updateAnim 里会归位）。
  // 原来的问题在于：动画被打断时没有把 mPanel* 钳住，
  // 而 updateAnim 的 !mAnimating 分支又会把 mPanel* 直接赋成 mTarget*，
  // 两者交替，导致下一次动画从一个「不该在的位置」起步，
  // 看起来就是反着弹。
  // 这里统一：以 mPanel* 为唯一真实位置，并在打断时立刻定住。
  const float fromX = mPanelX;
  const float fromY = mPanelY;
  const float fromW = mPanelW;
  const float fromH = mPanelH;

  if (mMode == Mode::Mini) {
    mTargetW = (mShowCover ? 660.0f : 560.0f);
    mTargetH = 192.0f;
    if (mMiniX + mTargetW > mScreenW) {
      mMiniX = mScreenW - mTargetW - 12.0f;
    }
    if (mMiniY + mTargetH > mScreenH) {
      mMiniY = mScreenH - mTargetH - 12.0f;
    }
    mMiniX = std::max(0.0f, mMiniX);
    mMiniY = std::max(0.0f, mMiniY);
    mTargetX = mMiniX;
    mTargetY = mMiniY;
  } else if (mMode == Mode::Bar) {
    // 单行迷你条：高度收到 64，宽度固定 620。
    // 位置沿用 ballX / ballY，玩家拖过的位置换形态后依然生效。
    // 玩家反馈最小窗口「还是有一点遮挡」。
    // 原来是 620x64，横跨小半个屏幕、压住底部物品栏上沿。
    // 收到 520x56：按钮和歌名都还放得下，横向占屏明显减小。
    mTargetW = 520.0f;
    mTargetH = 56.0f;
    if (!mHasBallPos) {
      // 首次进迷你条：落在中窗口中心，之后以拖动位置为准。
      const float miniW = mShowCover ? 660.0f : 560.0f;
      constexpr float kMiniH = 192.0f;
      mBallX = mMiniX + (miniW - mTargetW) * 0.5f;
      mBallY = mMiniY + (kMiniH - mTargetH) * 0.5f;
      mHasBallPos = true;
    }
    mBallX = std::min(std::max(mBallX, 0.0f), std::max(0.0f, mScreenW - mTargetW));
    mBallY = std::min(std::max(mBallY, 0.0f), std::max(0.0f, mScreenH - mTargetH));
    mTargetX = mBallX;
    mTargetY = mBallY;
  } else {
    // 大面板：完全固定。始终屏幕居中，不接受拖动，也不记忆位置。
    // 以前这里按记忆坐标摆放，配合 MOVE 阶段的拖动分支，玩家在
    // 列表以外的空白处一划就把整块面板拽偏。拖动分支已删除，
    // 位置也不再读写 ui.json。
    mTargetW = std::min(mScreenW * 0.82f, 1560.0f);
    mTargetH = std::min(mScreenH * 0.7f, 760.0f);
    mTargetX = std::max(0.0f, (mScreenW - mTargetW) * 0.5f);
    mTargetY = std::max(0.0f, (mScreenH - mTargetH) * 0.5f);
  }

  mAnimFromX = fromX;
  mAnimFromY = fromY;
  mAnimFromW = fromW;
  mAnimFromH = fromH;
  mAnimStartMs = nowMs();
  mAnimating = true;
  saveState();
  mDirty = true;

  __android_log_print(ANDROID_LOG_INFO, kTag,
                      "界面: 目标 %.0f,%.0f %.0fx%.0f 面 %.0fx%.0f mini %.0f,%.0f",
                      mTargetX, mTargetY, mTargetW, mTargetH, mScreenW, mScreenH,
                      mMiniX, mMiniY);
}

void HudUi::updateAnim(long long now) {
  // 调用方已持锁。
  if (!mAnimating) {
    mPanelX = mTargetX;
    mPanelY = mTargetY;
    mPanelW = mTargetW;
    mPanelH = mTargetH;
    return;
  }
  // 动画时长 200ms 偏短，配合 16ms 帧间隔只有 12 帧。
  // 中面板<->大面板跨度很大（660→1560），200ms 太快，观感像「闪一下」。
  // 拉到 280ms，约 17 帧，位移看得清。
  constexpr float kDurationMs = 280.0f;
  float t = static_cast<float>(now - mAnimStartMs) / kDurationMs;
  if (t < 0.0f) {
    t = 0.0f;
  }
  if (t >= 1.0f) {
    t = 1.0f;
    mAnimating = false;
  }
  // 缓动：原来是纯 easeOutCubic（1-inv³），起步速度最陡、末尾急停，
  // 观感像「弹出去然后撞住」，配合反向位移就像倒着走。
  // 改成 easeInOutCubic：起步柔和、中段快、末尾收得稳，
  // 拉大和收起两个方向看起来都自然。
  const float eased =
      (t < 0.5f) ? (4.0f * t * t * t)
                 : (1.0f - std::pow(-2.0f * t + 2.0f, 3.0f) * 0.5f);
  mPanelX = mAnimFromX + (mTargetX - mAnimFromX) * eased;
  mPanelY = mAnimFromY + (mTargetY - mAnimFromY) * eased;
  mPanelW = mAnimFromW + (mTargetW - mAnimFromW) * eased;
  mPanelH = mAnimFromH + (mTargetH - mAnimFromH) * eased;
  mDirty = true;
}

void HudUi::registerTouch() {
  if (mRegisterTouch == nullptr) {
    return;
  }
  auto registerFn = reinterpret_cast<RegisterTouchFn>(mRegisterTouch);
  registerFn([this](const pl::input::TouchEvent &event) {
    return handleTouch(event.action, event.x, event.y, nowMs());
  });
}

bool HudUi::installCjkFont() {
  if (mRegisterFont == nullptr) {
    __android_log_print(ANDROID_LOG_WARN, kTag, "字体接口不可用");
    return false;
  }

  std::vector<unsigned char> data;
  for (const char *path : {"/system/fonts/NotoSansCJK-Regular.ttc",
                           "/system/fonts/NotoSansSC-Regular.otf"}) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
      continue;
    }
    data.assign(std::istreambuf_iterator<char>(in),
                std::istreambuf_iterator<char>());
    if (!data.empty()) {
      break;
    }
  }
  if (data.empty()) {
    __android_log_print(ANDROID_LOG_WARN, kTag, "没有可用的中文字体文件");
    return false;
  }

  using RegisterFontFn = bool (*)(std::string_view,
                                  std::span<const unsigned char>);
  auto registerFontFn = reinterpret_cast<RegisterFontFn>(mRegisterFont);
  const bool ok = registerFontFn(
      "musicmod.cjk", std::span<const unsigned char>(data.data(), data.size()));
  gUseCjkFont = ok;
  __android_log_print(ANDROID_LOG_INFO, kTag, "中文字体注册=%d 字节=%zu", ok,
                      data.size());
  return ok;
}

bool HudUi::install() {
  // 偏好权重默认「适中」，避免首次进入全部显示为少推。
  //
  // 但这里绝对不能无条件覆盖：loadState() 在 setStatePath() 时就已执行过，
  // mPrefWeights 里装的是玩家上次存的真实值。
  // 原来这里直接 store(1)，等于每次启动都把磁盘读出来的偏好清成「适中」，
  // 所以「粤语设成多推，重启就变回适中」。
  // 正确做法：只有在确实没读到配置（全新的清档）时才写默认值。
  if (!mConfigLoadedFromDisk.load()) {
    for (int i = 0; i < 12; ++i) {
      mPrefWeights[i].store(1);
    }
  }

  if (mInstalled) {
    return true;
  }
  const bool hostOk = resolveHostApi();
  __android_log_print(ANDROID_LOG_INFO, kTag, "界面: 宿主接口=%d", hostOk ? 1 : 0);
  if (!hostOk) {
    return false;
  }
  resolveSurface();
  __android_log_print(ANDROID_LOG_INFO, kTag, "界面: 绘制面=%dx%d",
                      static_cast<int>(mScreenW), static_cast<int>(mScreenH));
  // 图标贴图：一次性生成并注册，之后每帧只是贴图，不再堆图元。
  if (mRegisterImage != nullptr && !gIconsReady) {
    auto regFn = reinterpret_cast<RegisterImageFn>(mRegisterImage);
    constexpr int kIconPx = 96;
    bool allOk = true;
    for (int k = 0; k < 4; ++k) {
      // 播放/暂停用薄荷青（和 kAccent 同色），上/下一首用近白。
      const std::uint32_t tint =
          (k == kIconPlay || k == kIconPause) ? 0xFF9EDCE7u : 0xFFF2F5F7u;
      std::vector<unsigned char> px = makeIconRgba(k, kIconPx, tint);
      const std::string id = std::string("musicmod.icon.") + kIconNames[k];
      if (regFn(id.c_str(),
                std::span<const unsigned char>(px.data(), px.size()),
                kIconPx, kIconPx)) {
        gIconIds[k] = id;
      } else {
        allOk = false;
      }
    }
    gIconsReady = allOk;
    __android_log_print(ANDROID_LOG_INFO, kTag, "图标贴图注册=%d",
                        gIconsReady ? 1 : 0);
  }
  // 中文字体异步注册：19MB 的 ttc 同步读入会阻塞模块注册，
  // 当初因此被注释掉，结果所有文字回落到系统默认衬线体。
  // 改成后台线程注册，不阻塞主流程；成功后下一帧自动生效。
  std::thread([this] {
    if (installCjkFont()) {
      mDirty = true;
    }
  }).detach();

  auto registerFn = reinterpret_cast<RegisterModuleFn>(mRegisterModule);
  __android_log_print(ANDROID_LOG_INFO, kTag, "界面: 开始注册模块");

  pl::modmenu::ModuleInfo info;
  info.moduleId = kModuleId;
  info.displayName = "沉浸播放";
  info.description = "游戏内音乐播放与歌词显示";
  // modId 必须与宿主认定的「owning lifecycle mod」完全一致，
  // 否则 Preloader 直接拒绝注册：
  //   "declared mod_id X does not match owning lifecycle mod Y"
  // 一旦不匹配，游戏内 Mod Menu 里连这个模组都看不到，
  // 但音乐、歌词、日志照常工作，极易误判成"模组没加载"。
  //
  // 关键：这个「lifecycle mod 名」是 mods/ 下的**目录名**，
  // 不是 manifest.json 里的 name。而目录名各实例并不统一
  // （26.62.1 是 MusicMod、1.26.60.4 是沉浸播放），
  // 所以绝不能写死，必须由 setModId() 注入真实目录名。
  info.modId = mModId;
  info.defaultEnabled = true;
  info.hideInHudEditor = false;
  info.configs = {
      pl::modmenu::ConfigEntry{.key = "showCover",
                               .displayName = "显示歌曲封面",
                               .type = pl::modmenu::ConfigType::Toggle,
                               .defaultValue = "false"},
      pl::modmenu::ConfigEntry{.key = "showProgress",
                               .displayName = "显示进度条",
                               .type = pl::modmenu::ConfigType::Toggle,
                               .defaultValue = "true"},
      pl::modmenu::ConfigEntry{.key = "autoPlay",
                               .displayName = "进入世界后自动播放",
                               .type = pl::modmenu::ConfigType::Toggle,
                               .defaultValue = "true"},
      pl::modmenu::ConfigEntry{.key = "uploadListen",
                               .displayName = "上传听歌数据",
                               .type = pl::modmenu::ConfigType::Toggle,
                               .defaultValue = "true"},
      pl::modmenu::ConfigEntry{.key = "sourceKugou",
                               .displayName = "音乐源：酷狗概念版",
                               .type = pl::modmenu::ConfigType::Button,
                               .defaultValue = "当前"},
      pl::modmenu::ConfigEntry{.key = "sourceYoutube",
                               .displayName = "音乐源：YouTube Music",
                               .type = pl::modmenu::ConfigType::Button,
                               .defaultValue = "敬请期待"},
      pl::modmenu::ConfigEntry{.key = "language",
                               .displayName = "界面语言",
                               .type = pl::modmenu::ConfigType::Radio,
                               .defaultValue = "中文",
                               .maxValue = "中文,English"},
      pl::modmenu::ConfigEntry{.key = "lyricDelay",
                               .displayName = "歌词延迟毫秒",
                               .type = pl::modmenu::ConfigType::SliderInt,
                               .defaultValue = "0",
                               .minValue = "-5000",
                               .maxValue = "5000"},
      pl::modmenu::ConfigEntry{.key = "fontScale",
                               .displayName = "文字大小",
                               .type = pl::modmenu::ConfigType::SliderFloat,
                               .defaultValue = "1.0",
                               .minValue = "0.7",
                               .maxValue = "1.6"},
      pl::modmenu::ConfigEntry{.key = "volume",
                               .displayName = "音乐音量",
                               .type = pl::modmenu::ConfigType::SliderInt,
                               .defaultValue = "70",
                               .minValue = "0",
                               .maxValue = "100"},
  };
  info.onConfigChanged = [this](std::string_view, std::string_view key,
                                std::string_view value) {
    applyConfig(std::string(key), std::string(value));
  };

  if (!registerFn(info)) {
    __android_log_print(ANDROID_LOG_ERROR, kTag, "界面: 模块注册失败");
    return false;
  }

  registerTouch();
  if (mRegisterText != nullptr) {
    auto registerTextFn = reinterpret_cast<RegisterTextFn>(mRegisterText);
    registerTextFn([this](const pl::input::TextInputEvent &event) {
      return handleTextInput(event.text);
    });
  }
  if (mRegisterKey != nullptr) {
    auto registerKeyFn = reinterpret_cast<RegisterKeyFn>(mRegisterKey);
    registerKeyFn([this](const pl::input::KeyEvent &event) {
      return handleKey(event.keyCode, event.unicodeChar, event.isKeyDown);
    });
  }
  {
    std::lock_guard<std::mutex> lock(mMutex);
    mInstalled = true;
    placePanel();
  }
  __android_log_print(ANDROID_LOG_INFO, kTag,
                      "界面: 已注册, 屏幕=%.0fx%.0f, modId=%s",
                      mScreenW, mScreenH, mModId.c_str());
  // 提交线程暂不启用：宿主只认主循环线程提交的命令。
  mSubmitWanted.store(true);
  return true;
}

void HudUi::applyConfig(const std::string &key, const std::string &value,
                        bool immediate) {
  std::lock_guard<std::mutex> lock(mMutex);
  const bool truthy = (value == "true" || value == "1");
  const bool fromDisk = mConfigLoadedFromDisk.load();
  const bool isSwitch = (key == "showCover" || key == "showProgress" ||
                         key == "autoPlay" || key == "uploadListen");
  // 宿主在注册模块、打开菜单时，会把所有配置项按 defaultValue 连续回放一遍。
  // 它和「玩家点了一下开关」走的是同一个回调，值也可能完全相同，单条无法区分。
  //
  // 所以开关类一律先入队，等这一批回调停下来再判定：
  //   一批 3 项以上 → 宿主回放，整批丢弃，保住磁盘里的值
  //   只有 1 项      → 玩家操作，照常应用
  //
  // 旧实现按「该 key 是不是第一次出现」判断，第二次打开菜单时不再拦截，
  // 宿主回放出来的默认值就把玩家设置冲掉了，表现就是「开关不记忆」。
  if (!immediate && isSwitch && fromDisk) {
    mCfgPending[key] = value;
    mCfgPendingMs = nowMs();
    return;
  }
  try {
    if (key == "showCover") {
      mShowCover = truthy;
      saveState();  // 立刻落盘，否则重启后回默认值
    } else if (key == "showProgress") {
      mShowProgress = truthy;
      saveState();
    } else if (key == "autoPlay") {
      mAutoPlayOnWorld = truthy;
      saveState();
    } else if (key == "musicVolume") {
      // 菜单里的「音乐音量」滑块（0..100）。
      // 原来只注册了菜单项，applyConfig 里没有这个分支，
      // 所以拖滑块完全没反应。
      const int pct = std::min(100, std::max(0, std::stoi(value)));
      mVolume = static_cast<float>(pct) / 100.0f;
      saveState();
    } else if (key == "uploadListen") {
      mUploadListen.store(truthy);
      saveState();
    } else if (key == "sourceYoutube") {
      setNoticeLocked("YouTube Music 敬请期待", 3000);
    } else if (key == "sourceKugou") {
      setNoticeLocked("当前音乐源：酷狗概念版", 2500);
    } else if (key == "language") {
      mEnglish.store(value == "English");
      mDirty = true;
    } else if (key == "lyricDelay") {
      mLyricDelay.store(std::stoi(value));
      mDirty = true;
    } else if (key == "fontScale") {
      mFontScale = std::stof(value);
      mFontScale = std::min(1.6f, std::max(0.7f, mFontScale));
    } else if (key == "volume") {
      mVolume = std::min(1.0f, std::max(0.0f, std::stof(value) / 100.0f));
      if (mEngine != nullptr) {
        mEngine->setVolume(mVolume);
      }
    }
  } catch (...) {
    __android_log_print(ANDROID_LOG_WARN, kTag, "界面配置解析失败: %s=%s",
                        key.c_str(), value.c_str());
    return;
  }
  placePanel();
  __android_log_print(ANDROID_LOG_INFO, kTag, "界面配置: %s=%s", key.c_str(),
                      value.c_str());
}

bool HudUi::autoPlayEnabled() {
  std::lock_guard<std::mutex> lock(mMutex);
  return mAutoPlayOnWorld;
}

bool HudUi::uploadCoverFrames(
    const std::vector<std::vector<unsigned char>> &frames, int width,
    int height) {
  if (mRegisterImage == nullptr || frames.empty() || width <= 0 ||
      height <= 0) {
    return false;
  }
  const std::size_t hash = hashBytes(frames.front());
  {
    std::lock_guard<std::mutex> lock(mMutex);
    if (mCoverReady && hash == mCoverHash && width == mCoverW &&
        height == mCoverH) {
      // 内容和当前显示的一模一样，跳过。打日志是为了区分两种情况：
      // 一是这张图和上一首真的是同一张（例如服务端给的默认封面），
      // 二是别的地方出了问题导致拿错图。没有日志就只能猜。
      __android_log_print(ANDROID_LOG_INFO, kTag,
                          "封面内容与当前相同, 跳过上传 hash=%zu", hash);
      coverLog("跳过上传: 内容与当前相同 hash=%zu", hash);
      return true;
    }
  }
  auto registerFn = reinterpret_cast<RegisterImageFn>(mRegisterImage);
  std::vector<std::string> ids;
  ids.reserve(frames.size());
  bool reusedExisting = false;
  for (std::size_t i = 0; i < frames.size(); ++i) {
    const std::string imageId = std::string(kCoverId) + "." +
                                std::to_string(hash) + "." +
                                std::to_string(i);
    const bool registered = registerFn(
        imageId.c_str(),
        std::span<const unsigned char>(frames[i].data(), frames[i].size()),
        width, height);
    bool known = false;
    {
      std::lock_guard<std::mutex> lock(mMutex);
      known = mRegisteredImages.count(imageId) > 0;
    }
    if (!registered) {
      // 实测：同一个 id 第二次注册必然返回 false（游戏层不接受重复注册）。
      // 但那张图在第一次成功时就已经在里面了，id 一直有效。
      // 只有我们自己成功注册过的 id 才允许这样复用；
      // 从没成功过的 id 失败就是真失败，不能假装成功。
      if (!known) {
        __android_log_print(
            ANDROID_LOG_WARN, kTag,
            "封面上传失败(registerImage 返回 false) 帧=%zu/%zu id=%s", i,
            frames.size(), imageId.c_str());
        coverLog("上传失败: registerImage=false 帧=%zu/%zu id=%s", i,
                 frames.size(), imageId.c_str());
        return false;
      }
      reusedExisting = true;
    } else if (!known) {
      std::lock_guard<std::mutex> lock(mMutex);
      mRegisteredImages.insert(imageId);
    }
    ids.push_back(imageId);
  }
  if (reusedExisting) {
    coverLog("上传: id 已存在, 直接复用(不重复注册) hash=%zu", hash);
  }
  {
    std::lock_guard<std::mutex> lock(mMutex);
    mCoverReady = true;
    mCoverIds = std::move(ids);
    mCoverId = mCoverIds.empty() ? std::string(kCoverId) : mCoverIds.front();
    mCoverRotationMs = 0;
    mLastCoverTickMs = nowMs();
    mCoverW = width;
    mCoverH = height;
    mCoverHash = hash;
    mDirty = true;
  }
  // 注意：ids 已经被 move 到 mCoverIds，此时 ids.size() 是 0，
  // 直接拿它打日志会写出「帧=0」这种误导信息。用 frames.size()。
  coverLog("上传成功: 帧=%zu 尺寸=%dx%d hash=%zu%s", frames.size(), width,
           height, hash, reusedExisting ? " (复用已存在id)" : "");
  return true;
}

bool HudUi::uploadAvatar(const std::vector<unsigned char> &rgba, int width,
                         int height) {
  if (mRegisterImage == nullptr || rgba.empty() || width <= 0 || height <= 0 ||
      rgba.size() < static_cast<std::size_t>(width) * height * 4) {
    return false;
  }
  const std::size_t hash = hashBytes(rgba);
  {
    std::lock_guard<std::mutex> lock(mMutex);
    if (mAvatarReady && hash == mAvatarHash && width == mAvatarW &&
        height == mAvatarH) {
      return true;
    }
  }
  auto registerFn = reinterpret_cast<RegisterImageFn>(mRegisterImage);
  const std::string imageId =
      std::string(kAvatarId) + "." + std::to_string(hash);
  const bool avatarRegistered =
      registerFn(imageId.c_str(),
                 std::span<const unsigned char>(rgba.data(), rgba.size()),
                 width, height);
  if (!avatarRegistered) {
    // 同封面：重复注册会被拒，但图已在游戏层里，同一个 id 可以直接复用。
    bool known = false;
    {
      std::lock_guard<std::mutex> lock(mMutex);
      known = mRegisteredImages.count(imageId) > 0;
    }
    if (!known) {
      return false;
    }
  } else {
    std::lock_guard<std::mutex> lock(mMutex);
    mRegisteredImages.insert(imageId);
  }
  {
    std::lock_guard<std::mutex> lock(mMutex);
    mAvatarReady = true;
    mAvatarImg = imageId;
    mAvatarW = width;
    mAvatarH = height;
    mAvatarHash = hash;
    mDirty = true;
  }
  __android_log_print(ANDROID_LOG_INFO, kTag, "头像已上传 %dx%d id=%s",
                      width, height, imageId.c_str());
  return true;
}

const std::string &HudUi::avatarId() const { return mAvatarImg; }

void HudUi::clearAvatar() {
  std::lock_guard<std::mutex> lock(mMutex);
  mAvatarReady = false;
  mAvatarImg = std::string(kAvatarId);
  mAvatarHash = 0;
  mDirty = true;
}

const std::string &HudUi::coverId() const {
  if (mCoverIds.empty()) {
    return mCoverId;
  }
  const std::size_t index = (mCoverRotationMs / 60) % mCoverIds.size();
  return mCoverIds[index];
}

bool HudUi::uploadCover(const std::vector<unsigned char> &rgba, int width,
                        int height) {
  return uploadCoverFrames({rgba}, width, height);
}

void HudUi::setLyric(const std::string &trackId, const std::string &text) {
  auto finish = [&](std::vector<LyricLine> lines) {
    // 过滤制作人名单：KRC/LRC 开头常带「作词: xxx」「编曲: xxx」，
    // 不过滤会被当成歌词滚出来。按关键词匹配，不区分大小写。
    static const char *kMetaKeys[] = {
        "作词", "作曲", "编曲", "制作人", "监制", "混音", "母带",
        "录音", "和声", "吉他", "贝斯", "鼓", "键盘", "弦乐",
        "出品", "发行", "统筹", "企划", "策划", "OP", "SP",
        "词：", "曲：", "词:", "曲:", "Lyricist", "Composer",
        "Arranger", "Producer", "Mixed", "Mastered"};
    lines.erase(
        std::remove_if(lines.begin(), lines.end(),
                       [](const LyricLine &line) {
                         if (line.text.empty()) {
                           return true;
                         }
                         for (const char *key : kMetaKeys) {
                           if (line.text.find(key) != std::string::npos) {
                             return true;
                           }
                         }
                         return false;
                       }),
        lines.end());
    std::sort(lines.begin(), lines.end(),
              [](const LyricLine &a, const LyricLine &b) {
                return a.timeMs < b.timeMs;
              });
    // 去掉相邻重复行。KRC 里同一句常被标注多次（分行或补时间戳），
    // 两行文字完全一样，视觉上就是「同一句出现两条」。
    // 保留第一条，把后面重复的起止时间并进去（延长首条的时长）。
    for (std::size_t i = 1; i < lines.size();) {
      if (lines[i].text == lines[i - 1].text) {
        const long long prevEnd =
            lines[i - 1].timeMs + std::max(0LL, lines[i - 1].durationMs);
        const long long curEnd =
            lines[i].timeMs + std::max(0LL, lines[i].durationMs);
        if (curEnd > prevEnd) {
          lines[i - 1].durationMs = curEnd - lines[i - 1].timeMs;
        }
        lines.erase(lines.begin() + static_cast<long>(i));
      } else {
        ++i;
      }
    }
    // 逐字宽度只在这里算一次；绘制时直接查表，不再每帧遍历字符。
    const float size = 20.0f * mFontScale;
    for (auto &line : lines) {
      line.charWidths.clear();
      line.charEndsPx.clear();
      line.totalWidth = 0.0f;
      if (line.chars.empty()) {
        line.totalWidth = estimateTextWidth(line.text, size);
        continue;
      }
      line.charWidths.reserve(line.chars.size());
      line.charEndsPx.reserve(line.chars.size());
      for (const auto &glyph : line.chars) {
        const float w = estimateTextWidth(glyph, size);
        line.charWidths.push_back(w);
        line.totalWidth += w;
        line.charEndsPx.push_back(line.totalWidth);
      }
    }
    // 诊断：打印首句的 glyph 粒度与时间戳，判断推色异常的原因。
    if (!lines.empty()) {
      const LyricLine &first = lines.front();
      std::string sample;
      for (std::size_t g = 0; g < first.chars.size() && g < 6; ++g) {
        sample += first.chars[g];
      }
      __android_log_print(ANDROID_LOG_INFO, kTag,
                          "歌词诊断 行数=%zu 首句glyph=%zu 前6字=[%s] "
                          "宽=%.0f timed=%d",
                          lines.size(), first.chars.size(), sample.c_str(),
                          first.totalWidth,
                          first.charStarts.size() == first.chars.size() ? 1 : 0);
      if (first.charStarts.size() >= 3) {
        __android_log_print(ANDROID_LOG_INFO, kTag,
                            "歌词诊断 时间戳 s0=%lld e0=%lld s1=%lld e1=%lld "
                            "s2=%lld e2=%lld",
                            first.charStarts[0], first.charEnds[0],
                            first.charStarts[1], first.charEnds[1],
                            first.charStarts[2], first.charEnds[2]);
      }
    }
    std::lock_guard<std::mutex> lock(mMutex);
    // 歌词是异步拉回来的，回来时可能已经切歌。
    // 不校验就会「歌和歌词对不上」：晚到的那份把新歌的歌词盖掉。
    //
    // 必须问引擎要当前曲目，不能用 mDisplayedId：
    // mDisplayedId 是在 refresh() 的 syncEngine 分支里更新的（约 160ms 一次），
    // 而 setLyric 是 main 线程拿到歌词后立刻调用，两者不同步，会漏判。
    const Track *curTrack =
        mEngine != nullptr ? mEngine->currentTrack() : nullptr;
    const std::string curId = curTrack != nullptr ? curTrack->id : std::string();
    if (!trackId.empty() && !curId.empty() && trackId != curId) {
      __android_log_print(ANDROID_LOG_INFO, kTag,
                          "歌词丢弃(期间切歌): %.16s 当前=%.16s",
                          trackId.c_str(), curId.c_str());
      return;
    }
    mLyricTrackId = trackId;
    mLyrics = std::move(lines);
    mCurrentLyric = 0;
    mDirty = true;
  };

  bool krc = false;
  for (std::size_t i = 0; i + 3 < text.size(); ++i) {
    if (text[i] == ']' && text[i + 1] == '<') {
      krc = true;
      break;
    }
  }
  if (krc) {
    std::vector<LyricLine> lines;
    std::size_t begin = 0;
    while (begin <= text.size()) {
      std::size_t end = text.find('\n', begin);
      if (end == std::string::npos) {
        end = text.size();
      }
      std::string line = text.substr(begin, end - begin);
      begin = end + 1;
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
        line.pop_back();
      }
      if (line.size() < 5 || line[0] != '[') {
        continue;
      }
      const std::size_t headerEnd = line.find(']');
      if (headerEnd == std::string::npos) {
        continue;
      }
      const std::string header = line.substr(1, headerEnd - 1);
      const std::size_t comma = header.find(',');
      if (comma == std::string::npos) {
        continue;
      }
      long long lineStart = 0;
      long long lineDuration = 0;
      try {
        lineStart = std::stoll(header.substr(0, comma));
        lineDuration = std::stoll(header.substr(comma + 1));
      } catch (const std::exception &) {
        continue;
      }
      LyricLine parsed;
      parsed.timeMs = lineStart;
      parsed.durationMs = lineDuration;
      std::size_t pos = headerEnd + 1;
      while (pos < line.size()) {
        const std::size_t open = line.find('<', pos);
        if (open == std::string::npos) {
          break;
        }
        const std::size_t close = line.find('>', open + 1);
        if (close == std::string::npos) {
          break;
        }
        const std::string tag = line.substr(open + 1, close - open - 1);
        const std::size_t c1 = tag.find(',');
        const std::size_t c2 =
            c1 == std::string::npos ? std::string::npos : tag.find(',', c1 + 1);
        if (c1 == std::string::npos || c2 == std::string::npos) {
          break;
        }
        long long charOffset = 0;
        long long charDuration = 0;
        try {
          charOffset = std::stoll(tag.substr(0, c1));
          charDuration = std::stoll(tag.substr(c1 + 1, c2 - c1 - 1));
        } catch (const std::exception &) {
          break;
        }
        const std::size_t nextTag = line.find('<', close + 1);
        const std::size_t textEnd =
            nextTag == std::string::npos ? line.size() : nextTag;
        std::string glyph = line.substr(close + 1, textEnd - close - 1);
        pos = textEnd;
        if (glyph.empty()) {
          continue;
        }
        parsed.chars.push_back(glyph);
        parsed.text += glyph;
        parsed.charStarts.push_back(lineStart + charOffset);
        parsed.charEnds.push_back(lineStart + charOffset + charDuration);
      }
      if (!parsed.chars.empty()) {
        lines.push_back(std::move(parsed));
      }
    }
    finish(std::move(lines));
    return;
  }

  std::vector<LyricLine> lines;
  std::size_t begin = 0;
  bool timed = false;
  while (begin <= text.size()) {
    std::size_t end = text.find('\n', begin);
    if (end == std::string::npos) {
      end = text.size();
    }
    std::string line = text.substr(begin, end - begin);
    begin = end + 1;
    std::vector<long long> stamps;
    std::size_t pos = 0;
    while (pos < line.size() && line[pos] == '[') {
      const std::size_t close = line.find(']', pos + 1);
      if (close == std::string::npos) {
        break;
      }
      const std::string tag = line.substr(pos + 1, close - pos - 1);
      const std::size_t colon = tag.find(':');
      if (colon != std::string::npos && colon > 0) {
        try {
          const int minutes = std::stoi(tag.substr(0, colon));
          const std::string rest = tag.substr(colon + 1);
          const std::size_t dot = rest.find('.');
          const std::string secText =
              dot == std::string::npos ? rest : rest.substr(0, dot);
          const int seconds = std::stoi(secText);
          int fraction = 0;
          if (dot != std::string::npos) {
            std::string frac = rest.substr(dot + 1);
            if (frac.size() > 3) {
              frac.resize(3);
            }
            while (frac.size() < 3) {
              frac.push_back('0');
            }
            fraction = std::stoi(frac);
          }
          stamps.push_back(static_cast<long long>(minutes) * 60000 +
                           seconds * 1000 + fraction);
          timed = true;
        } catch (const std::exception &) {
        }
      }
      pos = close + 1;
    }
    std::string content = line.substr(pos);
    const std::size_t first = content.find_first_not_of(" \t\r");
    if (first == std::string::npos) {
      continue;
    }
    content = content.substr(first);
    for (long long stamp : stamps) {
      lines.push_back({stamp, 0, content, {}, {}, {}});
    }
    if (stamps.empty() && !timed) {
      lines.push_back({0, 0, content, {}, {}, {}});
    }
  }
  finish(std::move(lines));
}

void HudUi::clearCover() {
  std::lock_guard<std::mutex> lock(mMutex);
  mCoverReady = false;
}

void HudUi::setCoverLogPath(const std::string &path) {
  std::lock_guard<std::mutex> lock(mCoverLogMutex);
  mCoverLogPath = path;
}

void HudUi::coverLog(const char *fmt, ...) {
  char buf[256];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  std::lock_guard<std::mutex> lock(mCoverLogMutex);
  // 写两份：一份在数据目录（模组自己用），一份在启动器的 minecraft 镜像目录。
  // 后者是外部可读的固定路径，出问题时能直接把文件捞出来看。
  // 数据目录属于应用私有，命令行没有权限读，只写那里等于白写。
  auto appendTo = [&](const std::string &path) {
    if (path.empty()) {
      return;
    }
    struct stat st {};
    if (::stat(path.c_str(), &st) == 0 && st.st_size > 64 * 1024) {
      ::remove(path.c_str());
    }
    std::ofstream out(path, std::ios::app);
    if (out) {
      out << nowMs() << " " << buf << "\n";
    }
  };
  appendTo(mCoverLogPath);
  static const char *kExternalLog =
      "/storage/emulated/0/Android/media/org.levimc.launcher/minecraft/"
      "MusicModData/cover_debug.log";
  if (mCoverLogPath != kExternalLog) {
    appendTo(kExternalLog);
  }
}

void HudUi::clearLyric() {
  std::lock_guard<std::mutex> lock(mMutex);
  mLyrics.clear();
  mCurrentLyric = 0;
  mLyricTrackId.clear();
  mDirty = true;
}

void HudUi::clearQr() {
  std::lock_guard<std::mutex> lock(mMutex);
  mQrReady = false;
  mQrZoom = false;
  mQrModules = 0;
  mQrPixels.clear();
  mDirty = true;
}

bool HudUi::uploadQr(const std::vector<unsigned char> &rgba, int width,
                     int height) {
  if (rgba.empty() || width <= 0 || height <= 0 ||
      rgba.size() < static_cast<std::size_t>(width) * height * 4) {
    return false;
  }
  const auto isDark = [&](int x, int y) {
    const std::size_t i =
        (static_cast<std::size_t>(y) * width + x) * 4;
    if (rgba[i + 3] < 128) {
      return false;
    }
    return rgba[i] + rgba[i + 1] + rgba[i + 2] < 384;
  };
  int minX = width, minY = height, maxX = -1, maxY = -1;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      if (isDark(x, y)) {
        minX = std::min(minX, x);
        minY = std::min(minY, y);
        maxX = std::max(maxX, x);
        maxY = std::max(maxY, y);
      }
    }
  }
  if (maxX < minX || maxY < minY) {
    return false;
  }
  // 左上角定位图案顶边是连续 7 个黑模块，用它推算单模块像素宽。
  int run = 0;
  for (int x = minX; x <= maxX && isDark(x, minY); ++x) {
    ++run;
  }
  if (run < 7) {
    return false;
  }
  const float pitch = static_cast<float>(run) / 7.0f;
  const int side = std::max(maxX - minX + 1, maxY - minY + 1);
  const int modules = static_cast<int>(std::lround(side / pitch));
  if (modules < 21 || modules > 57) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mMutex);
  mQrCells.clear();
  mQrPixels = rgba;
  mQrW = width;
  mQrH = height;
  mQrMinX = minX;
  mQrMinY = minY;
  mQrPitch = pitch;
  mQrModules = modules;
  mQrReady = true;
  mDirty = true;
  return true;
}

bool HudUi::uploadQrUrl(const std::string &url) {
  if (url.empty()) {
    return false;
  }
  try {
    const qrcodegen::QrCode code = qrcodegen::QrCode::encodeText(
        url.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
    const int modules = code.getSize();
    if (modules < 21 || modules > 177) {
      return false;
    }
    std::vector<unsigned char> cells(
        static_cast<std::size_t>(modules) * modules, 0);
    for (int y = 0; y < modules; ++y) {
      for (int x = 0; x < modules; ++x) {
        cells[static_cast<std::size_t>(y) * modules + x] =
            code.getModule(x, y) ? 1 : 0;
      }
    }
    std::lock_guard<std::mutex> lock(mMutex);
    mQrCells.swap(cells);
    mQrPixels.clear();
    mQrModules = modules;
    mQrPitch = 0.0f;
    mQrReady = true;
    mDirty = true;
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "二维码按链接生成 模块=%d 链接长度=%zu", modules,
                        url.size());
    return true;
  } catch (const std::exception &error) {
    __android_log_print(ANDROID_LOG_WARN, kTag, "二维码生成失败: %s",
                        error.what());
    return false;
  }
}

bool HudUi::qrDarkAt(int row, int col) const {
  if (row < 0 || col < 0 || row >= mQrModules || col >= mQrModules) {
    return false;
  }
  if (!mQrCells.empty()) {
    return mQrCells[static_cast<std::size_t>(row) * mQrModules + col] != 0;
  }
  if (mQrPixels.empty() || mQrPitch <= 0.0f) {
    return false;
  }
  const int px = mQrMinX + static_cast<int>((col + 0.5f) * mQrPitch);
  const int py = mQrMinY + static_cast<int>((row + 0.5f) * mQrPitch);
  if (px < 0 || py < 0 || px >= mQrW || py >= mQrH) {
    return false;
  }
  const std::size_t pi = (static_cast<std::size_t>(py) * mQrW + px) * 4;
  return mQrPixels[pi + 3] >= 128 &&
         mQrPixels[pi] + mQrPixels[pi + 1] + mQrPixels[pi + 2] < 384;
}

void HudUi::setQrStatus(const std::string &text) {
  std::lock_guard<std::mutex> lock(mMutex);
  if (mQrMsg == text) {
    return;
  }
  mQrMsg = text;
  mDirty = true;
  __android_log_print(ANDROID_LOG_INFO, kTag, "登录状态: %s", text.c_str());
  // 顺手落一份到数据目录，日志被系统清掉也能复盘登录卡在哪一步。
  if (!mStatePath.empty()) {
    const std::size_t slash = mStatePath.find_last_of('/');
    if (slash != std::string::npos) {
      std::ofstream logFile(mStatePath.substr(0, slash) + "/qr_status.txt",
                            std::ios::app);
      if (logFile.is_open()) {
        std::time_t now = std::time(nullptr);
        char stamp[32] = {0};
        std::strftime(stamp, sizeof(stamp), "%m-%d %H:%M:%S",
                      std::localtime(&now));
        logFile << stamp << " " << text << "\n";
      }
    }
  }
}

int HudUi::takeQrRequest() { return mPendingQr.exchange(0); }

void HudUi::closeLogin() {
  std::lock_guard<std::mutex> lock(mMutex);
  mLoginPage = false;
  mSmsMode = false;
  mLoginField = 0;
  // 收起登录页时重置发码冷却，下次进来是干净的。
  mSmsSendState = 0;
  mSmsSendUntil = 0;
  mDirty = true;
}

void HudUi::requestQrRefresh() { mPendingQr.store(1); }

void HudUi::setSmsHint(const std::string &text) {
  std::lock_guard<std::mutex> lock(mMutex);
  if (mSmsHint == text) {
    return;
  }
  mSmsHint = text;
  mDirty = true;
}

void HudUi::setAccountState(bool loggedIn, const std::string &label) {
  std::lock_guard<std::mutex> lock(mMutex);
  if (mLoggedIn == loggedIn && mUserLabel == label) {
    return;
  }
  mLoggedIn = loggedIn;
  mUserLabel = label;
  mDirty = true;
  __android_log_print(ANDROID_LOG_INFO, kTag, "账号状态: 已登录=%d 账号=%s",
                      loggedIn ? 1 : 0, label.c_str());
}

int HudUi::takeLogoutRequest() { return mPendingLogout.exchange(0); }

void HudUi::requestLogout() {
  std::lock_guard<std::mutex> lock(mMutex);
  mLogoutConfirm = true;
  mDirty = true;
}

void HudUi::confirmLogout() {
  std::lock_guard<std::mutex> lock(mMutex);
  mLogoutConfirm = false;
  mPendingLogout.store(1);
  mDirty = true;
}

void HudUi::cancelLogout() {
  std::lock_guard<std::mutex> lock(mMutex);
  mLogoutConfirm = false;
  mDirty = true;
}

int HudUi::takeVipRequest() { return mPendingVip.exchange(0); }

void HudUi::setListTracks(const std::vector<Track> &tracks) {
  std::lock_guard<std::mutex> lock(mMutex);
  mShownTracks = tracks;
  mTrackTitles.clear();
  mTrackArtists.clear();
  mTrackIds.clear();
  mTrackTitles.reserve(tracks.size());
  mTrackArtists.reserve(tracks.size());
  mTrackIds.reserve(tracks.size());
  for (const auto &track : tracks) {
    mTrackTitles.push_back(track.title);
    mTrackArtists.push_back(track.artist);
    mTrackIds.push_back(track.id);
  }
  mTrackCount = tracks.size();
  mManualList = true;
  // 诊断：把列表首曲 id 打出来，与「收藏集合样本」对比即可判断
  // 心形显示不准是「id 不匹配」还是「集合不对」。
  __android_log_print(ANDROID_LOG_INFO, kTag, "列表更新: %zu 首 首曲id=%s",
                      mTrackIds.size(),
                      mTrackIds.empty() ? "-" : mTrackIds.front().c_str());
  // 数据真的变了就重放滑入动画。
  // 原来动画只在点击时触发，但子分类数据是异步拉取的：
  // 第一次点「探索」时缓存为空，列表瞬间清空，动画在空列表上播，看不见。
  // 改到这里触发，等数据到达再播，视觉上就对了。
  if (!tracks.empty()) {
    mRecTabAnimStartMs = nowMs();
    mDirty = true;
  }
  mListScroll = 0.0f;
  mDirty = true;
}

int HudUi::currentTab() const { return static_cast<int>(mTab); }

void HudUi::setTrackSelectHandler(std::function<void(int, int)> handler) {
  std::lock_guard<std::mutex> lock(mMutex);
  mTrackSelectHandler = std::move(handler);
}

void HudUi::setTabChangeHandler(std::function<void(int)> handler) {
  std::lock_guard<std::mutex> lock(mMutex);
  mTabChangeHandler = std::move(handler);
}

void HudUi::setSmsSendState(int state, const std::string &hint) {
  std::lock_guard<std::mutex> lock(mMutex);
  mSmsSendState = state;
  if (state == 2 && nowMs() >= mSmsSendUntil) {
    // 成功但还没进冷却时补一个 60 秒。
    mSmsSendUntil = nowMs() + 60000;
  }
  if (!hint.empty()) {
    mSmsHint = hint;
  }
  mDirty = true;
}

void HudUi::setLoginSendHandler(std::function<void()> handler) {
  std::lock_guard<std::mutex> lock(mMutex);
  mLoginSendHandler = std::move(handler);
}

void HudUi::setLoginSubmitHandler(std::function<void()> handler) {
  std::lock_guard<std::mutex> lock(mMutex);
  mLoginSubmitHandler = std::move(handler);
}

void HudUi::updateArtistInfo(const ArtistInfo &info) {
  std::lock_guard<std::mutex> lock(mMutex);
  if (!info.name.empty()) {
    mArtistInfo.name = info.name;
  }
  if (!info.avatar.empty()) {
    mArtistInfo.avatar = info.avatar;
  }
  if (info.songCount > 0) {
    mArtistInfo.songCount = info.songCount;
  }
  if (info.albumCount > 0) {
    mArtistInfo.albumCount = info.albumCount;
  }
  mDirty = true;
  __android_log_print(ANDROID_LOG_INFO, kTag, "歌手资料回填 名字=%s",
                      mArtistInfo.name.c_str());
}

void HudUi::setSearchAlbums(const std::vector<AlbumInfo> &list) {
  std::lock_guard<std::mutex> lock(mMutex);
  mSearchAlbums = list;
  mListScroll = 0.0f;
  mDirty = true;
  __android_log_print(ANDROID_LOG_INFO, kTag, "搜索专辑 %zu 张",
                      mSearchAlbums.size());
}

void HudUi::setSearchArtists(const std::vector<ArtistInfo> &list) {
  std::lock_guard<std::mutex> lock(mMutex);
  mSearchArtists = list;
  mListScroll = 0.0f;
  mDirty = true;
  __android_log_print(ANDROID_LOG_INFO, kTag, "搜索歌手 %zu 位",
                      mSearchArtists.size());
}

void HudUi::enterArtist(const ArtistInfo &artist) {
  std::lock_guard<std::mutex> lock(mMutex);
  mArtistInfo = artist;
  mArtistView = 1;
  mArtistTab = 0;
  mArtistAlbums.clear();
  mListScroll = 0.0f;
  mDirty = true;
  __android_log_print(ANDROID_LOG_INFO, kTag, "进入歌手页 %s id=%s",
                      artist.name.c_str(), artist.id.c_str());
}

// 已持锁版本：只做状态变更，不再加锁。
// 触摸路径（handleTouchLocked）必须调这个，否则同一线程二次加锁会死锁。
void HudUi::enterAlbumView() {
  std::lock_guard<std::mutex> lock(mMutex);
  mAlbumView = 1;
  mAlbumOpenName = mPendingAlbumName;
  mListScroll = 0.0f;
  // 这一层没有输入框，别让键盘继续往看不见的地方打字。
  mInputActive = false;
  mDirty = true;
}

void HudUi::exitAlbumView() {
  std::lock_guard<std::mutex> lock(mMutex);
  mAlbumView = 0;
  mAlbumOpenName.clear();
  mListScroll = 0.0f;
  mDirty = true;
}

int HudUi::takePendingPlayNext() { return mPendingPlayNext.exchange(-1); }

void HudUi::setFavoriteIds(const std::set<std::string> &ids) {
  std::lock_guard<std::mutex> lock(mMutex);
  mFavTrackIds.clear();
  for (const auto &raw : ids) {
    if (raw.empty()) {
      continue;
    }
    mFavTrackIds.insert(lowerHash(raw));
  }
  mDirty = true;
  __android_log_print(ANDROID_LOG_INFO, kTag, "收藏集合已更新: %zu 首 样本=%s",
                      mFavTrackIds.size(),
                      mFavTrackIds.empty() ? "-" : mFavTrackIds.begin()->c_str());
}

void HudUi::setTrackFavoritedById(const std::string &id, bool favorited) {
  if (id.empty()) {
    return;
  }
  std::lock_guard<std::mutex> lock(mMutex);
  const std::string key = lowerHash(id);
  if (favorited) {
    mFavTrackIds.insert(key);
  } else {
    mFavTrackIds.erase(key);
  }
  mDirty = true;
}

bool HudUi::isTrackFavorited(const std::string &id) {
  if (id.empty()) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mMutex);
  return mFavTrackIds.count(lowerHash(id)) > 0;
}

bool HudUi::rowFavoritedLocked(std::size_t index) const {
  // 调用方已持界面锁。队列视图查队列，其余查浏览列表。
  std::string id;
  if (mQueueView == 1) {
    if (index < mQueueTracks.size()) {
      id = mQueueTracks[index].id;
    }
  } else if (index < mTrackIds.size()) {
    id = mTrackIds[index];
  }
  return !id.empty() && mFavTrackIds.count(lowerHash(id)) > 0;
}

void HudUi::toggleQueueView() {
  std::lock_guard<std::mutex> lock(mMutex);
  mQueueView = (mQueueView == 1) ? 0 : 1;
  // 切换时要拿一次最新的引擎队列。
  mQueueSyncRequest.store(1);
  mListScroll = 0.0f;
  mDirty = true;
  __android_log_print(ANDROID_LOG_INFO, kTag, "播放队列侧栏: %s",
                      mQueueView == 1 ? "打开" : "关闭");
}

void HudUi::toggleQueueViewLocked() {
  mQueueView = (mQueueView == 1) ? 0 : 1;
  mQueueSyncRequest.store(1);
  mListScroll = 0.0f;
  mDirty = true;
  __android_log_print(ANDROID_LOG_INFO, kTag, "播放队列侧栏: %s",
                      mQueueView == 1 ? "打开" : "关闭");
}

void HudUi::setQueueTracks(const std::vector<Track> &tracks) {
  std::lock_guard<std::mutex> lock(mMutex);
  mQueueTracks = tracks;
  mDirty = true;
}

int HudUi::takeQueueSyncRequest() { return mQueueSyncRequest.exchange(0); }

void HudUi::showPlayQueue() {
  std::lock_guard<std::mutex> lock(mMutex);
  mAlbumView = 0;
  mAlbumOpenName.clear();
  // 退出歌手页与搜索的卡片分类，统一回到「歌曲行」。
  mArtistView = 0;
  mArtistAlbums.clear();
  mSearchKind = 0;
  mListScroll = 0.0f;
  mDirty = true;
}

bool HudUi::listShowsTracksLocked() const {
  if (mQueueView == 1) {
    // 队列视图在「我的」页也要显示播放列表：那页的设置项此时整体让路，
    // 左半边只剩队列。原来这里 return false，导致在我的页开队列是空白。
    return true;
  }
  if (mTab == Tab::Mine) {
    return false;  // 「我的」页是设置项，没有歌曲列表
  }
  if (mTab == Tab::Favorites && mFavView == 0) {
    return false;  // 收藏页第一层是歌单卡片
  }
  if (mTab == Tab::Search && mAlbumView == 1) {
    return true;  // 专辑歌曲层
  }
  if (mArtistView == 1 && mArtistTab == 1) {
    return false;  // 歌手页的专辑子标签
  }
  if (mTab == Tab::Search && mArtistView == 0 && mSearchKind != 0) {
    return false;  // 搜索页的专辑/歌手分类
  }
  return true;
}

void HudUi::exitArtistLocked() {
  mArtistView = 0;
  mArtistAlbums.clear();
  // 退出歌手页时必须一并退出专辑层，否则下次进来还停在上次的专辑里。
  mAlbumView = 0;
  mAlbumOpenName.clear();
  mListScroll = 0.0f;
  mDirty = true;
}

void HudUi::exitArtist() {
  std::lock_guard<std::mutex> lock(mMutex);
  exitArtistLocked();
}

void HudUi::setArtistAlbums(const std::vector<AlbumInfo> &list) {
  std::lock_guard<std::mutex> lock(mMutex);
  mArtistAlbums = list;
  mArtistTab = 1;
  mListScroll = 0.0f;
  mDirty = true;
  __android_log_print(ANDROID_LOG_INFO, kTag, "歌手专辑 %zu 张",
                      mArtistAlbums.size());
}

std::string HudUi::takeOpenAlbumRequest() {
  std::lock_guard<std::mutex> lock(mMutex);
  const std::string id = mPendingAlbumId;
  mPendingAlbumId.clear();
  return id;
}

void HudUi::setFavoritePlaylists(const std::vector<PlaylistInfo> &list) {
  std::lock_guard<std::mutex> lock(mMutex);
  mFavPlaylists = list;
  // 重新拉到歌单列表时回到第一层，避免停留在已失效的歌单里。
  mFavView = 0;
  mListScroll = 0.0f;
  mDirty = true;
  __android_log_print(ANDROID_LOG_INFO, kTag, "收藏页歌单数=%zu",
                      mFavPlaylists.size());
}

std::string HudUi::takeOpenPlaylistRequest() {
  std::lock_guard<std::mutex> lock(mMutex);
  const std::string id = mPendingOpenPlaylist;
  mPendingOpenPlaylist.clear();
  return id;
}

void HudUi::enterFavoritePlaylist(const std::string &id,
                                  const std::string &name) {
  std::lock_guard<std::mutex> lock(mMutex);
  mFavOpenId = id;
  mFavOpenName = name;
  mFavView = 1;
  mListScroll = 0.0f;
  mDirty = true;
  __android_log_print(ANDROID_LOG_INFO, kTag, "进入歌单 %s listid=%s",
                      name.c_str(), id.c_str());
}

// 已持锁版本，理由同 exitArtistLocked。
void HudUi::exitFavoritePlaylistLocked() {
  mFavView = 0;
  mFavOpenId.clear();
  mFavOpenName.clear();
  mListScroll = 0.0f;
  mDirty = true;
}

void HudUi::exitFavoritePlaylist() {
  std::lock_guard<std::mutex> lock(mMutex);
  exitFavoritePlaylistLocked();
}

void HudUi::setFavoriteToggleHandler(
    std::function<void(const std::string &)> handler) {
  std::lock_guard<std::mutex> lock(mMutex);
  mFavoriteToggleHandler = std::move(handler);
}

void HudUi::setFavoriteState(bool favorited, const std::string &trackId) {
  std::lock_guard<std::mutex> lock(mMutex);
  // 只接受当前曲目的结果。异步查询晚到时曲目可能已经换了，
  // 直接覆盖会让新歌被标成上一首的收藏状态。
  if (!trackId.empty() && !mFavoriteTrackId.empty() &&
      trackId != mFavoriteTrackId) {
    return;
  }
  if (mTrackFavorited != favorited) {
    mTrackFavorited = favorited;
    mDirty = true;
  }
}

std::string HudUi::takeFavoriteQuery() {
  if (mFavoriteQueryPending.exchange(0) == 0) {
    return {};
  }
  std::lock_guard<std::mutex> lock(mMutex);
  return mFavoriteTrackId;
}

void HudUi::toggleFavoriteOnCurrent() {
  std::string trackId;
  {
    std::lock_guard<std::mutex> lock(mMutex);
    trackId = mFavoriteTrackId;
  }
  if (trackId.empty()) {
    setNotice("还没有播放中的歌曲", 2000);
    return;
  }
  std::function<void(const std::string &)> handler;
  {
    std::lock_guard<std::mutex> lock(mMutex);
    handler = mFavoriteToggleHandler;
  }
  if (!handler) {
    setNotice("收藏功能未就绪", 2000);
    return;
  }
  // 必须锁外调用：它会联网，持锁会卡住绘制线程。
  handler(trackId);
}

void HudUi::setMenuChecker(std::function<bool()> checker) {
  std::lock_guard<std::mutex> lock(mMutex);
  mMenuChecker = std::move(checker);
}
void HudUi::setPauseChecker(std::function<bool()> checker) {
  std::lock_guard<std::mutex> lock(mMutex);
  mPauseChecker = std::move(checker);
}

std::string HudUi::loginPhone() {
  std::lock_guard<std::mutex> lock(mMutex);
  return mLoginPhone;
}

std::string HudUi::loginCode() {
  std::lock_guard<std::mutex> lock(mMutex);
  return mLoginCode;
}

void HudUi::setStatePath(const std::string &path) {
  mStatePath = path;
  loadState();
}

void HudUi::loadState() {
  if (mStatePath.empty()) {
    return;
  }
  std::ifstream input(mStatePath);
  if (!input.is_open()) {
    return;
  }
  std::string line;
  while (std::getline(input, line)) {
    const std::size_t eq = line.find('=');
    if (eq == std::string::npos) {
      continue;
    }
    const std::string key = line.substr(0, eq);
    const std::string value = line.substr(eq + 1);
    try {
      if (key == "miniX") {
        mMiniX = std::stof(value);
      } else if (key == "miniY") {
        mMiniY = std::stof(value);
      } else if (key == "ballX") {
        mBallX = std::stof(value);
      } else if (key == "ballY") {
        mBallY = std::stof(value);
      } else if (key == "hasBall") {
        mHasBallPos = (value == "1");
      } else if (key == "mode") {
        int mode = std::stoi(value);
        // 3 = Bar（单行迷你条，新增）。
        // 2 = Ball（圆球）已废弃，旧存档读到就迁移成 Bar，否则玩家
        // 更新后会落进一个不再绘制的形态，面板直接消失。
        if (mode == 2) {
          mode = 3;
        }
        if (mode >= 0 && mode <= 3) {
          mMode = static_cast<Mode>(mode);
        }
      } else if (key == "panelAlpha") {
        mPanelAlpha = std::min(1.0f, std::max(0.1f, std::stof(value)));
      } else if (key == "showCover") {
        mShowCover = (value == "1");
      } else if (key == "showProgress") {
        mShowProgress = (value == "1");
      } else if (key == "autoPlay") {
        mAutoPlayOnWorld = (value == "1");
      } else if (key == "english") {
        mEnglish.store(value == "1");
      } else if (key == "volume") {
        mVolume = std::min(1.0f, std::max(0.0f, std::stof(value)));
      } else if (key == "themeMode") {
        const int mode = std::stoi(value);
        if (mode >= 0 && mode <= 2) {
          mThemeMode = mode;
          mAppliedNight = -1;  // 强制下一帧重套配色
        }
      } else if (key == "alphaMini") {
        mAlphaMini = std::stof(value);
      } else if (key == "alphaBar") {
        mAlphaBar = std::stof(value);
      } else if (key == "disableMini") {
        mDisableMini = (value == "1");
      } else if (key == "uploadListen") {
        mUploadListen.store(value == "1");
      } else if (key == "prefLevel") {
        // 推送强度：原来只写不读，每次升级都回到默认值。
        const int level = std::stoi(value);
        if (level >= 0 && level <= 2) {
          mPrefLevel.store(level);
        }
      } else if (key.rfind("prefW", 0) == 0) {
        const int index = std::stoi(key.substr(5));
        const int weight = std::stoi(value);
        if (index >= 0 && index < 12 && weight >= 0 && weight <= 2) {
          mPrefWeights[index].store(weight);
        }
      }
    } catch (const std::exception &) {
    }
  }
  // 展开面板会占掉大半屏幕，启动时一律退回中面板，避免挡住游戏自己的按钮。
  if (mMode == Mode::Expanded) {
    mMode = Mode::Mini;
    placePanel();
  }
  __android_log_print(ANDROID_LOG_INFO, kTag,
                      "界面位置已恢复: mini=%.0f,%.0f ball=%.0f,%.0f 形态=%d",
                      mMiniX, mMiniY, mBallX, mBallY,
                      static_cast<int>(mMode));
  mConfigLoadedFromDisk.store(true);
}

void HudUi::saveState() {
  if (mStatePath.empty()) {
    return;
  }
  // 原子写：先写 .tmp，再 rename 覆盖正式文件。
  // 直接 ofstream 覆盖原文件，写到一半被杀进程会把旧数据也毁掉，
  // 一次崩溃就丢了玩家全部设置。
  const std::string tmpPath = mStatePath + ".tmp";
  const std::string bakPath = mStatePath + ".bak";
  {
    std::ofstream output(tmpPath, std::ios::trunc);
    if (!output.is_open()) {
      __android_log_print(ANDROID_LOG_WARN, kTag, "保存失败: 打不开临时文件");
      return;
    }
    output << "miniX=" << mMiniX << "\n";
    output << "miniY=" << mMiniY << "\n";
    output << "ballX=" << mBallX << "\n";
    output << "ballY=" << mBallY << "\n";
    output << "hasBall=" << (mHasBallPos ? 1 : 0) << "\n";
    output << "mode=" << static_cast<int>(mMode) << "\n";
    output << "uploadListen=" << (mUploadListen.load() ? 1 : 0) << "\n";
    output << "disableMini=" << (mDisableMini ? 1 : 0) << "\n";
    output << "panelAlpha=" << mPanelAlpha << "\n";
    output << "alphaMini=" << mAlphaMini << "\n";
    output << "alphaBar=" << mAlphaBar << "\n";
    output << "themeMode=" << mThemeMode << "\n";
    // 这些开关原来只存在内存里，重启就回默认值。
    // 玩家在模组设置里打开「显示封面」，进游戏却是关的，就是这里没落盘。
    output << "showCover=" << (mShowCover ? 1 : 0) << "\n";
    output << "showProgress=" << (mShowProgress ? 1 : 0) << "\n";
    output << "autoPlay=" << (mAutoPlayOnWorld ? 1 : 0) << "\n";
    output << "english=" << (mEnglish.load() ? 1 : 0) << "\n";
    // 音乐音量（不是系统音量）：单独存，和面板透明度一样属于玩家设置。
    output << "volume=" << mVolume << "\n";
    // 推送强度：原来只写进内存、从不落盘，每次升级都回默认值。
    output << "prefLevel=" << mPrefLevel.load() << "\n";
    for (int i = 0; i < 12; ++i) {
      output << "prefW" << i << "=" << mPrefWeights[i].load() << "\n";
    }
    output.flush();
    if (!output.good()) {
      __android_log_print(ANDROID_LOG_WARN, kTag, "保存失败: 写入异常");
      output.close();
      std::remove(tmpPath.c_str());
      return;
    }
    output.close();
  }
  // 先把旧文件复制一份 .bak，再原子替换。
  std::error_code ec;
  if (std::filesystem::exists(mStatePath, ec)) {
    std::filesystem::copy_file(mStatePath, bakPath,
                              std::filesystem::copy_options::overwrite_existing,
                              ec);
  }
  std::filesystem::rename(tmpPath, mStatePath, ec);
  if (ec) {
    // 某些文件系统不支持 rename 覆盖，退回直接 copy。
    std::filesystem::copy_file(
        tmpPath, mStatePath,
        std::filesystem::copy_options::overwrite_existing, ec);
    std::remove(tmpPath.c_str());
  }
  __android_log_print(ANDROID_LOG_INFO, kTag,
                      "保存 ui.json alpha=%.2f theme=%d level=%d",
                      mPanelAlpha, mThemeMode, mPrefLevel.load());
}
bool HudUi::hitControl(float x, float y) const {
  // 只有展开面板需要细分命中：面板面积大，若整块吞掉触摸，
  // 游戏自己的按钮就点不到，表现为“点不动、进不了世界”。
  if (mMode != Mode::Expanded) {
    return true;
  }
  const float tx = mTargetX;
  const float ty = mTargetY;
  const float tw = mTargetW;
  const float th = mTargetH;
  // 标题栏的叉号、减号、搜索、登录
  if (insideRect(x, y, tx + tw - 64.0f, ty + 12.0f, 48.0f, 52.0f)) {
    return true;
  }
  if (insideRect(x, y, tx + tw - 72.0f, ty + th - 40.0f, 56.0f, 36.0f)) {
    return true;
  }
  if (insideRect(x, y, tx + tw - 138.0f, ty + 6.0f, 74.0f, 54.0f)) {
    return true;
  }
  // 登录入口：与绘制位置一致，否则点击会被当成空白丢弃。
  if (insideRect(x, y, tx + tw - 206.0f, ty + 6.0f, 92.0f, 54.0f)) {
    return true;
  }
  // 登录页已换成二维码与数字键盘，整块登录面板都要接收触摸。
  // 这里不能再使用旧版手机号输入框的坐标，否则按钮在执行动作前就被忽略。
  if (mLoginPage) {
    return true;
  }
  // 列表、输入框、确定键、分页栏
  const float listLeft = tx + 24.0f;
  const float inputY = ty + 52.0f;
  // 推荐页子分类（红心/探索/小众）：画在 y+46 起、高 56，
  // 之前白名单里没有这一块，按下就被当成空白吞掉，表现为
  // 「只有子标签最下面几像素能点」。这里必须与绘制同尺寸。
  if (mTab == Tab::Recommend &&
      insideRect(x, y, listLeft, ty + 46.0f, 540.0f, 56.0f)) {
    return true;
  }
  if (mTab == Tab::Search &&
      insideButton(x, y, listLeft, inputY, 540.0f - 74.0f, 40.0f, 4.0f, 12.0f)) {
    return true;
  }
  if (mTab == Tab::Search &&
      insideButton(x, y, listLeft + 540.0f - 66.0f, inputY, 66.0f, 40.0f, 4.0f, 12.0f)) {
    return true;
  }
  if (insideRect(x, y, listLeft, ty + 84.0f, 540.0f, th - 108.0f)) {
    return true;
  }
  if (insideRect(x, y, listLeft, ty + th - 54.0f, 540.0f, 40.0f)) {
    return true;
  }
  // 右侧播放区：进度条与三个控制键
  const float coverShift = (mShowCover && mCoverReady) ? 250.0f : 0.0f;
  const float px = tx + 620.0f + coverShift;
  if (insideRect(x, y, px, ty + 212.0f, tw - 660.0f - coverShift, 34.0f)) {
    return true;
  }
  if (insideRect(x, y, px, ty + 272.0f, 230.0f, 70.0f)) {
    return true;
  }
  // 其余空白一律交给游戏。
  return false;
}
namespace {
// 把系统送进来的触摸坐标换算到绘制面坐标系。
// 系统给的是屏幕物理像素，绘制面可能是另一套尺寸，不换算就会出现「看得见、点不中」。
void mapTouchToSurface(float &x, float &y, float realW, float realH,
                       float surfW, float surfH) {
  if (realW < 100.0f || realH < 100.0f || surfW < 100.0f ||
      surfH < 100.0f) {
    return;
  }
  // 触摸事件来自宿主的绘制面（与 HUD 绘制命令同一坐标系），
  // 实测 wm size = 2400x1080 与 getHudSurfaceSize 一致。
  // 之前用 DisplayMetrics 的 2320 做换算，等于把每次点击向右推 3.4%，
  // 表现就是「所有按钮都要点偏左/偏下才中」。
  // 只有两者差异超过 8% 才认为确实是两套坐标系，才做换算。
  const float dx = std::fabs(surfW - realW) / surfW;
  const float dy = std::fabs(surfH - realH) / surfH;
  if (dx < 0.08f && dy < 0.08f) {
    return;
  }
  float sx = 1.0f;
  float sy = 1.0f;
  const bool realLandscape = realW >= realH;
  const bool surfLandscape = surfW >= surfH;
  if (realLandscape == surfLandscape) {
    sx = surfW / realW;
    sy = surfH / realH;
  } else {
    sx = surfW / realH;
    sy = surfH / realW;
  }
  if (sx < 0.2f || sx > 5.0f || sy < 0.2f || sy > 5.0f) {
    return;
  }
  x *= sx;
  y *= sy;
}
} // namespace

bool HudUi::handleTouch(int action, float x, float y, long long timeMs) {
  // 暂停菜单或模组设置打开时，HUD 不接收触摸，避免点到后面的面板。
  static bool pauseSymbolLogged = false;
  if (!pauseSymbolLogged) {
    pauseSymbolLogged = true;
    __android_log_print(ANDROID_LOG_INFO, kTag, "暂停菜单符号=%d 回调=%d",
                        pl::runtime::IsPauseMenuOpen != nullptr ? 1 : 0,
                        mPauseChecker ? 1 : 0);
  }
  if (mMenuChecker && mMenuChecker()) {
    __android_log_print(ANDROID_LOG_INFO, kTag, "触摸被忽略: 菜单打开 action=%d",
                        action);
    return false;
  }
  if (mPauseChecker && mPauseChecker()) {
    __android_log_print(ANDROID_LOG_INFO, kTag, "触摸被忽略: 暂停检查 action=%d",
                        action);
    return false;
  }
  if (pl::runtime::IsPauseMenuOpen != nullptr &&
      pl::runtime::IsPauseMenuOpen()) {
    __android_log_print(ANDROID_LOG_INFO, kTag, "触摸被忽略: 暂停菜单 action=%d",
                        action);
    return false;
  }
  bool ret = false;
  std::function<void()> deferred;
  {
    std::unique_lock<std::mutex> lock(mMutex);
    // 初次进游戏、还没播过歌时，迷你条虽然存在但不该吃触摸。
    // 否则它占着屏幕右上角，会把游戏主界面的「开始」按钮吞掉。
    //
    // 但门禁不能只看「有没有歌」：播完一首、队列清空之后 mHasTrack 会变假，
    // 而面板此时仍然是可见的。旧实现一律 return false，结果是面板看得见、
    // 却整页点不动——玩家点「退出登录」什么都不发生，看起来就像卡死。
    // 「我的」页的透明度、主题、退出登录、领取会员与有没有歌毫无关系。
    //
    // 所以加上 mHudArmed 兜底：只要玩家主动打开过面板（播过歌或手动展开），
    // 就正常派发触摸，不再整页锁死。
    if (!mHasTrack && !mHudArmed) {
      return false;
    }
    float tx = x;
    float ty = y;
    mapTouchToSurface(tx, ty, mRealW, mRealH, mScreenW, mScreenH);
    // 换算前后都打进日志，便于定位「按钮要按下半截」这类系统性错位。
    static int touchLogCount = 0;
    if (action == 0 && touchLogCount < 40) {
      ++touchLogCount;
      __android_log_print(ANDROID_LOG_INFO, kTag,
                          "坐标换算 原始=%.0f,%.0f 结果=%.0f,%.0f 真实=%.0fx%.0f 绘制=%.0fx%.0f",
                          x, y, tx, ty, mRealW, mRealH, mScreenW, mScreenH);
    }
    ret = handleTouchLocked(action, tx, ty, timeMs, deferred);
    // 标定点：无论是否命中都记录，用来量测「点上半格无效」的偏移量。
    if (action == 0) {
      mCalibX = tx;
      mCalibY = ty;
      mCalibMs = timeMs;
      mDirty = true;
    }
    // 记录按下点，供水波纹使用；抬起不覆盖，波纹从按下点继续扩散。
    if (action == 0 && ret) {
      mRippleX = tx;
      mRippleY = ty;
      mRippleStartMs = timeMs;
    }
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "触摸返回 action=%d 消费=%d 动画=%d 跟踪=%d 形态=%d",
                        action, ret ? 1 : 0, mAnimating ? 1 : 0,
                        mTracking ? 1 : 0, static_cast<int>(mMode));
  }
  // 任何被消费的点击都可能改了界面状态，统一置脏，避免「点了不刷新」。
  if (ret) {
    std::lock_guard<std::mutex> lock(mMutex);
    mDirty = true;
  }
  // 弹键盘、发验证码、登录请求都可能等网络或等宿主，一律在锁外执行。
  if (deferred) {
    const auto start = std::chrono::steady_clock::now();
    deferred();
    const auto cost = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - start).count();
    if (cost > 80) {
      __android_log_print(ANDROID_LOG_WARN, kTag, "锁外动作耗时 %lld 毫秒",
                          static_cast<long long>(cost));
    }
  }
  return ret;
}

bool HudUi::handleTouchLocked(int action, float x, float y, long long timeMs,
                              std::function<void()> &deferred) {
  // 退出登录确认框：显示期间独占所有触摸事件（按下/移动/抬起都吃掉），
  // 既避免误点别的控件，也避免事件漏给游戏点到它自己的按钮。
  if (mLogoutConfirm) {
    const float cw = std::min(mScreenW * 0.62f, 760.0f);
    const float ch = 250.0f;
    const float cx0 = (mScreenW - cw) * 0.5f;
    const float cy0 = (mScreenH - ch) * 0.5f;
    const float btnW = (cw - 132.0f) * 0.5f;
    const float btnY = cy0 + ch - 96.0f;
    if (action == 0) {
      if (insideButton(x, y, cx0 + 44.0f, btnY, btnW, 58.0f, 6.0f, 6.0f)) {
        deferred = [this] { cancelLogout(); };
        return true;
      }
      if (insideButton(x, y, cx0 + 88.0f + btnW, btnY, btnW, 58.0f, 6.0f,
                       6.0f)) {
        deferred = [this] { confirmLogout(); };
        return true;
      }
      // 卡片外点一下等于取消。必须留这条逃逸路径：
      // 这一段是无条件 return true 独占所有触摸的，一旦按钮命中因为
      // 缩放或坐标换算出错，玩家就再也关不掉这个框，只能重启游戏。
      const bool insideCard =
          x >= cx0 && x <= cx0 + cw && y >= cy0 && y <= cy0 + ch;
      if (!insideCard) {
        __android_log_print(ANDROID_LOG_INFO, kTag, "退出确认框: 点卡片外取消");
        deferred = [this] { cancelLogout(); };
        return true;
      }
    }
    return true;
  }
  if (action == 0) {
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "触摸按下 %.0f,%.0f 相对面板=%.0f,%.0f 形态=%d "
                        "目标=%.0f,%.0f %.0fx%.0f 绘制=%.0f,%.0f %.0fx%.0f "
                        "屏幕=%.0fx%.0f 阶段=%d 登录页=%d 短信=%d",
                        x, y, x - mTargetX, y - mTargetY,
                        static_cast<int>(mMode), mTargetX, mTargetY,
                        mTargetW, mTargetH, mPanelX, mPanelY, mPanelW, mPanelH,
                        mScreenW, mScreenH, mStage.load(),
                        mLoginPage ? 1 : 0, mSmsMode ? 1 : 0);
    // 动画期间命中区域与画面不一致，这一帧不处理，但要消费掉，
    // 否则事件链会被游戏接走，抬指再也回不来。
    // 面板矩形之外：不消费，直接把事件还给游戏。
    // 原来这里一路走到底 return true，并且下面会无条件置 mTracking = true，
    // 面板外的按下被吞之后，后续的移动也一并被吞——游戏中表现就是
    // 移动摇杆没反应、划屏不转视角。「只触发过一次」是因为它取决于
    // 手指有没有正好落在面板边缘的 16 像素容差内。
    if (!inPanelArea(x, y)) {
      return false;
    }
    // 动画期间命中区域与画面不一致，这一帧不处理。
    // 已确认落在面板内，必须消费掉，否则事件链会被游戏接走。
    if (mAnimating) {
      return true;
    }
    // 每次按下先清掉上一次的阴影矩形，避免没设置矩形的按钮沿用旧位置。
    mPressRectW = 0.0f;

    // 登录页优先处理，避免被通用面板逻辑拦截。
    if (mLoginPage && mMode == Mode::Expanded) {
      const float lx = mTargetX + 24.0f;
      const float btnY = mTargetY + 466.0f;
      // 登录页也保留右上角关闭，避免铺底后无法退出。
      if (insideRect(x, y, mTargetX + mTargetW - 64.0f, mTargetY + 12.0f,
                     48.0f, 52.0f)) {
        mLoginPage = false;
        mDirty = true;
        return true;
      }
      auto press = [&](float bx, float by, float bw, float bh) {
        mPressRectX = bx;
        mPressRectY = by;
        mPressRectW = bw;
        mPressRectH = bh;
        mPressedUntil = nowMs() + 140;
        mDirty = true;
      };
      if (mQrZoom) {
        mQrZoom = false;
        mDirty = true;
        return true;
      }
      if (mLoggedIn) {
        const float ly0 = mTargetY + 100.0f;
        if (insideRect(x, y, lx, ly0 + 96.0f, 220.0f, 52.0f)) {
          press(lx, ly0 + 96.0f, 220.0f, 52.0f);
          // 退出登录不直接执行，先弹确认。
          deferred = [this] { requestLogout(); };
          return true;
        }
        if (insideRect(x, y, lx, ly0 + 172.0f, 220.0f, 40.0f)) {
          press(lx, ly0 + 172.0f, 220.0f, 40.0f);
          mLoginPage = false;
          return true;
        }
        return true;
      }
      if (!mSmsMode) {
        if (insideRect(x, y, lx, mTargetY + 122.0f, 328.0f, 328.0f)) {
          mQrZoom = true;
          mDirty = true;
          return true;
        }
        if (insideRect(x, y, lx, btnY, 150.0f, 48.0f)) {
          press(lx, btnY, 150.0f, 48.0f);
          mQrMsg = "正在刷新二维码";
          // 2 = 用户手动刷新，main 侧会跳过四秒防抖。
          deferred = [this] { mPendingQr.store(2); };
          return true;
        }
        if (insideRect(x, y, lx + 170.0f, btnY, 160.0f, 48.0f)) {
          press(lx + 170.0f, btnY, 160.0f, 48.0f);
          mSmsMode = true;
          mLoginField = 1;
          mSmsHint = "点数字键输入手机号";
          return true;
        }
        if (insideRect(x, y, lx, mTargetY + 578.0f, 220.0f, 44.0f)) {
          press(lx, mTargetY + 578.0f, 220.0f, 44.0f);
          mLoginPage = false;
          mQrReady = false;
          return true;
        }
        return true;
      }
      // 短信态：输入框、数字键、右侧按钮
      const float ly0 = mTargetY + 100.0f;
      const float boxY = ly0 + 34.0f;
      const float keyY = boxY + 64.0f;
      if (insideRect(x, y, lx, boxY, 346.0f, 52.0f)) {
        press(lx, boxY, 346.0f, 52.0f);
        mLoginField = 1;
        mSmsHint = "输入手机号";
        return true;
      }
      if (insideRect(x, y, lx + 360.0f, boxY, 220.0f, 52.0f)) {
        press(lx + 360.0f, boxY, 220.0f, 52.0f);
        mLoginField = 2;
        mSmsHint = "输入验证码";
        return true;
      }
      for (int i = 0; i < 12; ++i) {
        const int col = i % 3;
        const int row = i / 3;
        const float kx = lx + col * 118.0f;
        const float ky = keyY + row * 64.0f;
        if (!insideRect(x, y, kx, ky, 110.0f, 56.0f)) {
          continue;
        }
        press(kx, ky, 110.0f, 56.0f);
        std::string &target = (mLoginField == 2) ? mLoginCode : mLoginPhone;
        const std::size_t limit = (mLoginField == 2) ? 6 : 11;
        if (i == 9) {
          if (!target.empty()) {
            target.pop_back();
            mSmsHint = "已删除一位";
          }
        } else if (i == 11) {
          mLoginField = (mLoginField == 2) ? 1 : 2;
          mSmsHint = (mLoginField == 2) ? "输入验证码" : "输入手机号";
        } else {
          const char ch = (i == 10) ? '0' : static_cast<char>('1' + i);
          appendDigits(target, std::string(1, ch), limit);
          mSmsHint = (mLoginField == 2)
                         ? "验证码 " + std::to_string(target.size()) + "/6"
                         : "手机号 " + std::to_string(target.size()) + "/11";
        }
        return true;
      }
      const float ax = lx + 380.0f;
      if (insideRect(x, y, ax, keyY, 220.0f, 56.0f)) {
        press(ax, keyY, 220.0f, 56.0f);
        if (nowMs() < mSmsSendUntil) {
          mSmsHint = "冷却中, 稍后再发";
          return true;
        }
        if (mLoginSendHandler) {
          mSmsSendState = 1;
          mSmsSendUntil = nowMs() + 60000;
          mSmsHint = "发送中";
          deferred = [this] { mLoginSendHandler(); };
        }
        return true;
      }
      // 登录键命中区加高，覆盖按钮上下各 12 像素，避免边缘点不中。
      if (insideRect(x, y, ax, keyY + 56.0f, 220.0f, 80.0f)) {
        press(ax, keyY + 68.0f, 220.0f, 56.0f);
        __android_log_print(ANDROID_LOG_INFO, kTag,
                            "点击登录提交 手机号=%zu位 验证码=%zu位",
                            mLoginPhone.size(), mLoginCode.size());
        mSmsHint = "登录中";
        mDirty = true;
        if (mLoginSubmitHandler) {
          deferred = [this] { mLoginSubmitHandler(); };
        }
        return true;
      }
      if (insideRect(x, y, ax, keyY + 136.0f, 220.0f, 48.0f)) {
        press(ax, keyY + 136.0f, 220.0f, 48.0f);
        mLoginField = (mLoginField == 2) ? 1 : 2;
        return true;
      }
      if (insideRect(x, y, ax, keyY + 194.0f, 220.0f, 40.0f)) {
        press(ax, keyY + 194.0f, 220.0f, 40.0f);
        mSmsMode = false;
        mLoginField = 0;
        return true;
      }
      if (insideRect(x, y, ax, keyY + 228.0f, 220.0f, 40.0f)) {
        press(ax, keyY + 228.0f, 220.0f, 40.0f);
        mLoginPage = false;
        mLoginField = 0;
        return true;
      }
      return true;
    }

    // 我的：推荐偏好与退出登录。
    // 注意：这里不能加 mLoggedIn 前置条件。绘制侧（我的页）不要求登录，
    // 若点击侧要求，未登录时控件看得见却点不动，透明度与主题都调不了。
    if (mTab == Tab::Mine && mQueueView == 0) {
      // 三形态独立的透明度：改成滑块，按下即定位、可连续拖动。
      // 坐标全部来自绘制侧写入的 mAlphaRowY / mAlphaSliderX/W，
      // 不再两边各写一套硬编码（历史上因此多次「看得见点不到」）。
      for (int i = 0; i < 3; ++i) {
        const float rowY = mAlphaRowY[i];
        if (mAlphaSliderW <= 0.0f) {
          break;
        }
        // 命中区上下各放宽 4px，滑块细，手指不容易精确压中。
        if (y >= rowY - 4.0f && y <= rowY + mAlphaSliderH + 4.0f &&
            x >= mAlphaSliderX - 12.0f &&
            x <= mAlphaSliderX + mAlphaSliderW + 12.0f) {
          const float ratio =
              std::min(1.0f, std::max(0.0f, (x - mAlphaSliderX) / mAlphaSliderW));
          const float v = 0.10f + ratio * 0.90f;
          if (i == 0) {
            mPanelAlpha = v;
          } else if (i == 1) {
            mAlphaMini = v;
          } else {
            mAlphaBar = v;
          }
          mDrag = Drag::AlphaSlider;
          mAlphaDragIdx = i;
          mTracking = true;
          mMoved = true;  // 吞掉抬手，避免抬手路径再判一次别的按钮
          mDirty = true;
          static const char *kNames[3] = {"大面板", "中面板", "单行条"};
          setNoticeLocked(std::string(kNames[i]) + "透明度 " +
                              std::to_string(static_cast<int>(v * 100.0f + 0.5f)) +
                              "%",
                          1200);
          deferred = [this] { saveState(); };
          return true;
        }
      }
      // 主题切换按钮：三态循环，点一次换一个并立即保存。
      if (insideButton(x, y, mTargetX + 600.0f, mTargetY + 320.0f, 240.0f,
                       48.0f, 4.0f, 10.0f)) {
        mThemeMode = (mThemeMode + 1) % 3;
        mAppliedNight = -1;  // 下一帧按新模式重套配色
        mDirty = true;
        const char *names[3] = {"跟随时间", "浅色", "深色"};
        setNoticeLocked(std::string("主题 ") + names[mThemeMode], 1400);
        deferred = [this] { saveState(); };
        return true;
      }
      // 假偏好已移除：那 12 个 chip 与 3 档「推送强度」从未参与任何
      // 推荐逻辑（prefW0~prefW11 在 main.cpp / KugouProvider 里一次都
      // 没被读取），且酷狗概念版 App 里没有对应界面，属于纯摆设。
      // 上传开关：y 来自绘制侧（mMineUploadY），
      // 原来这里写死 mTargetY+482，删掉偏好区后布局上移，会点不到。
      if (mMineUploadY > 0.0f &&
          insideButton(x, y, mTargetX + 24.0f, mMineUploadY, 300.0f, 48.0f,
                       4.0f, 8.0f)) {
        const bool next = !mUploadListen.load();
        mUploadListen.store(next);
        setNoticeLocked(next ? "听歌数据上报已开启" : "听歌数据上报已关闭",
                        3000);
        deferred = [this] { saveState(); };
        mPressRectX = mTargetX + 24.0f;
        mPressRectY = mMineUploadY;
        mPressRectW = 300.0f;
        mPressRectH = 48.0f;
        mPressedUntil = nowMs() + 140;
        mDirty = true;
        return true;
      }
      if (mMineDisableMiniY > 0.0f &&
          insideButton(x, y, mTargetX + 24.0f, mMineDisableMiniY, 300.0f,
                       48.0f, 4.0f, 8.0f)) {
        mDisableMini = !mDisableMini;
        // 开启时若正停在中面板，直接退到单行条，否则这一档再也进不去。
        if (mDisableMini && mMode == Mode::Mini) {
          mMode = Mode::Bar;
          placePanel();
        }
        setNoticeLocked(mDisableMini ? "已关闭中面板" : "已恢复中面板", 2000);
        deferred = [this] { saveState(); };
        mPressRectX = mTargetX + 24.0f;
        mPressRectY = mMineDisableMiniY;
        mPressRectW = 300.0f;
        mPressRectH = 48.0f;
        mPressedUntil = nowMs() + 140;
        mDirty = true;
        return true;
      }
      if (mMineVipY > 0.0f &&
          insideButton(x, y, mTargetX + 240.0f, mMineVipY, 200.0f, 52.0f,
                       6.0f, 8.0f)) {
        mPressRectX = mTargetX + 240.0f;
        mPressRectY = mMineVipY;
        mPressRectW = 200.0f;
        mPressRectH = 52.0f;
        mPressedUntil = nowMs() + 140;
        deferred = [this] { mPendingVip.store(1); };
        return true;
      }
      if (mMineLogoutY > 0.0f &&
          insideButton(x, y, mTargetX + 24.0f, mMineLogoutY, 200.0f, 52.0f,
                       6.0f, 8.0f)) {
        mPressRectX = mTargetX + 24.0f;
        mPressRectY = mMineLogoutY;
        mPressRectW = 200.0f;
        mPressRectH = 52.0f;
        mPressedUntil = nowMs() + 140;
        // 退出登录不直接执行，先弹确认。
        deferred = [this] { requestLogout(); };
        return true;
      }
    }
    if (mMode == Mode::Bar) {
      // 单行迷你条：[歌名（可拖动）] [上曲] [暂停] [下曲] [放大]
      // 这里的几何必须和 drawFrame 里画单行条时用的完全一致，
      // 否则会出现「看得见点不到」。两处都是 padL=16、
      // expandW=btnW=44、playW=52、gap=4。
      if (!insideRect(x, y, mTargetX, mTargetY, mTargetW, mTargetH)) {
        return false;
      }
      // 只要按在条上，就先占住整个手势链。
      // 否则按在按钮上时 mTracking 没置位，后续 MOVE 会走到
      // 「!mTracking 就 return false」，事件漏给游戏，视角就转了。
      mTracking = true;
      mMoved = false;
      mDrag = Drag::None;
      mBarTapCandidate = false;
      mDownX = x;
      mDownY = y;
      mDownTimeMs = timeMs;
      // 尺寸随 mTargetW/H 一起从 620x64 收到 520x56 后，
      // 这里的内边距和按钮尺寸必须同比缩，否则按钮会叠到歌名上。
      const float padL = 12.0f;
      const float cyMid = mTargetY + mTargetH * 0.5f;
      const float expandW = 40.0f;
      const float btnW = 40.0f;
      const float playW = 46.0f;
      const float gap = 3.0f;
      const float expandX = mTargetX + mTargetW - padL - expandW;
      // 必须与绘制侧完全一致，否则按钮看得见点不中。
      // 单行条只剩一个收藏键（播放顺序键已移到大面板）。
      //   从右往左：放大 → 收藏 → 下曲 → 播放 → 上曲
      const float smallW = 40.0f;
      const float heartX = expandX - gap - smallW;
      const float nextX = heartX - gap - btnW;
      const float playX = nextX - gap - playW;
      const float prevX = playX - gap - btnW;
      const float titleW = std::max(60.0f, prevX - padL - 10.0f);

      if (insideButton(x, y, expandX, cyMid - expandW * 0.5f, expandW,
                       expandW, 4.0f, 8.0f)) {
        // 放大键直接在触摸回调里切形态，和中窗口放大键一致。
        // 不走 mPendingAction：ActionExpand 没有对应的 switch 分支，
        // 走 pending 会落进 default 被静默丢弃（之前就踩过这个坑）。
        mPressedAction = 9;
        mPressedUntil = nowMs() + 140;
        mDirty = true;
        mMoved = true;   // 吞掉后面的抬手，避免在新形态上误点
        // 三层阶梯：单行条 -> 中面板 -> 大面板。
        // 玩家在「我的」里关掉中面板时，直接从单行条跳到大面板。
        mMode = mDisableMini ? Mode::Expanded : Mode::Mini;
        placePanel();
        return true;
      }
      // 单行条不响应播放顺序切换：该按钮只在大面板里。
      // 收藏：已收藏则取消，未收藏则加入。
      if (insideButton(x, y, heartX, cyMid - smallW * 0.5f, smallW, smallW,
                       4.0f, 8.0f)) {
        mPressedAction = 12;
        mPressedUntil = nowMs() + 140;
        // 收藏要联网，必须在锁外执行。
        deferred = [this] { toggleFavoriteOnCurrent(); };
        mDirty = true;
        return true;
      }
      if (insideButton(x, y, prevX, cyMid - btnW * 0.5f, btnW, btnW, 4.0f,
                       8.0f)) {
        mPressedAction = 6;
        mPressedUntil = nowMs() + 140;
        mPendingAction.store(ActionPrevious);
        mDirty = true;
        return true;
      }
      if (insideButton(x, y, playX, cyMid - playW * 0.5f, playW, playW, 4.0f,
                       8.0f)) {
        mPressedAction = 7;
        mPressedUntil = nowMs() + 140;
        mPendingAction.store(ActionToggle);
        mDirty = true;
        return true;
      }
      if (insideButton(x, y, nextX, cyMid - btnW * 0.5f, btnW, btnW, 4.0f,
                       8.0f)) {
        mPressedAction = 8;
        mPressedUntil = nowMs() + 140;
        mPendingAction.store(ActionNext);
        mDirty = true;
        return true;
      }
      // 条上剩下的任何位置都当作「按住歌名」处理：
      // 按下即进入拖动候选，长按并移动就是移动整条，轻点则放大到中窗口。
      // 这里绝不能 return false —— 一旦返回 false，宿主会把后续
      // MOVE 事件交给游戏，表现就是「拖不动、视角反而转了」。
      mTracking = true;
      mMoved = false;
      mDrag = Drag::Panel;
      mBarTapCandidate = true;
      mDownX = x;
      mDownY = y;
      mGrabX = x - mBallX;
      mGrabY = y - mBallY;
      mDownTimeMs = timeMs;
      return true;
    }

    // 面板外 18 像素以内也吃掉，避免手指擦边时漏给游戏点到游戏自己的暂停键。
    // 关键：必须排除面板内部。上一版写成「面板 ± 18」，这个范围把面板里面
    // 也包住了，而它排在所有按钮判定之前，结果是把面板上的每一次点击
    // 全部吞掉——表现就是「触摸确实被拦下了，但任何按钮都不响应，
    // 连进度条也不动」。
    {
      constexpr float kGuard = 18.0f;
      const bool insidePanel =
          insideRect(x, y, mPanelX, mPanelY, mPanelW, mPanelH);
      const bool nearPanel =
          x >= mPanelX - kGuard && x <= mPanelX + mPanelW + kGuard &&
          y >= mPanelY - kGuard && y <= mPanelY + mPanelH + kGuard;
      if (!insidePanel && nearPanel) {
        mTracking = true;
        mMoved = true;
        return true;
      }
    }
    if (!insideRect(x, y, mTargetX, mTargetY, mTargetW, mTargetH)) {
      return false;
    }
    // 形态切换按钮改成「按下即触发」。
    // 原来这两个键只在抬手时判定，而抬手处理开头有
    //   if (moved) return true;
    // 手指从按下到抬起漂移超过 8 像素就直接返回，按钮永远不响应。
    // 手机上点击漂移 8 像素很常见，所以实测「照图标点也点不动」。
    // 播放键一直是按下触发，这正是它一点就中的原因。
    if (mMode == Mode::Mini) {
      const float cx = mTargetX + mTargetW - 40.0f;
      // 注意：这个分支只在 Mini 形态下进来，所以按一次就必然切 Expanded。
      // 去掉重复触发的关键是「切完立刻 mMoved = true 吞掉后续事件」，
      // 以及 Expanded 分支里同样的处理，见下面。
      // 命中范围比图标本身大一圈：实测玩家照图标点，落点会偏高 20 像素左右。
      // padX/padY 之外 insideButton 还会再加 5 像素基础容差。
      if (insideButton(x, y, cx - 30.0f, mTargetY + 4.0f, 60.0f, 56.0f,
                       10.0f, 10.0f)) {
        mTracking = true;
        mMoved = true;   // 吞掉后面的抬手，避免在新形态上误点
        __android_log_print(ANDROID_LOG_INFO, kTag,
                            "命中: 中面板放大键→大面板 %.0f,%.0f", x, y);
        // 这里是【中面板】右上角的放大键，必须进大面板。
        // 上一版脚本把这里误改成了 Mode::Mini，等于切到自己，所以点不动。
        if (mMode != Mode::Expanded) {
          mMode = Mode::Expanded;
          placePanel();
        }
        return true;
      }
      if (insideButton(x, y, mTargetX + mTargetW - 72.0f,
                       mTargetY + mTargetH - 40.0f, 56.0f, 36.0f, 4.0f,
                       20.0f)) {
        mTracking = true;
        mMoved = true;
        __android_log_print(ANDROID_LOG_INFO, kTag, "命中: 收成单行条 %.0f,%.0f",
                            x, y);
        if (mMode != Mode::Bar) {
          mMode = Mode::Bar;
          placePanel();
        }
        return true;
      }
      // 上一首 / 播放暂停 / 下一首：同样改成按下即触发。
      // 原来这三个键只在抬手判定，而抬手开头有 if (moved) return true，
      // 手指漂移超过 8 像素动作就整个丢掉，实测点暂停键歌不停。
      {
        const float rowY = mTargetY + 128.0f;
        const float rowBottom = mTargetY + mTargetH;
        // 必须与绘制侧一致：按整个面板宽度居中。
        const float miniCenter = mTargetX + mTargetW * 0.5f;
        if (y >= rowY - 12.0f && y <= rowBottom + 12.0f) {
          if (x >= miniCenter - 110.0f && x < miniCenter - 20.0f) {
            __android_log_print(ANDROID_LOG_INFO, kTag, "命中: 上一首 %.0f,%.0f",
                                x, y);
            mPendingAction.store(ActionPrevious);
            mTracking = true;
            mMoved = true;
            return true;
          }
          if (x >= miniCenter - 40.0f && x < miniCenter + 40.0f) {
            __android_log_print(ANDROID_LOG_INFO, kTag,
                                "命中: 播放暂停 %.0f,%.0f", x, y);
            mPendingAction.store(ActionToggle);
            mTracking = true;
            mMoved = true;
            return true;
          }
          if (x >= miniCenter + 20.0f && x < miniCenter + 110.0f) {
            __android_log_print(ANDROID_LOG_INFO, kTag, "命中: 下一首 %.0f,%.0f",
                                x, y);
            mPendingAction.store(ActionNext);
            mTracking = true;
            mMoved = true;
            return true;
          }
        }
      }
    }

    // 空白处不做动作，也不收起。刚展开时手指位置常常落在空白上，
// 若在这里收起，用户会看到面板一闪就没了，像打不开。
    // 关键：这里只记录「是否按在控件上」，绝不能再提前 return true。
    // 旧实现遇到白名单外的点会直接消费掉事件但不设 mTracking，
    // 抬手时 !mTracking 立刻返回，点击就静默消失了。
    // 白名单本身也容易漏项（子标签行、我的页按钮都曾漏掉），
    // 表现为「只有按钮下半截能点」。改成一律记录，交给抬手统一判定。
    const bool onControl = hitControl(x, y);
    mDownOnControl = onControl;
    // 按下反馈按动作执行，不再覆盖整块面板。持续时间放到刷新循环处理。
    if (action == 0 && onControl) {
      mPressedAction = 1;
      mPressedUntil = nowMs() + 140;
      mDirty = true;
    }
    mTracking = true;
    mMoved = false;
    mDrag = Drag::None;
    mDownX = x;
    mDownY = y;
    mGrabX = x - mMiniX;
    mGrabY = y - mMiniY;
    mDownTimeMs = timeMs;

    // 音量条优先：它在右下角，和进度条不重叠，但要先判。
    // 竖向，从下往上增长，所以比例是 (底 - y) / 高度。
    // 「我的」页不绘制音量条，这里也必须排除，
    // 否则会出现「看不见但右下角能拖」。
    if (mTab != Tab::Mine && mQueueView == 0 && mVolBarH > 0.0f &&
        insideRect(x, y, mVolBarX, mVolBarY, mVolBarW, mVolBarH)) {
      mDrag = Drag::Volume;
      const float top = mVolBarY + 8.0f;
      const float bottom = mVolBarY + mVolBarH - 8.0f;
      const float span = std::max(1.0f, bottom - top);
      mVolume = std::min(1.0f, std::max(0.0f, (bottom - y) / span));
      mDirty = true;
      return true;
    }

    // 落在进度条上就进入拖动进度模式，按下即给出预览位置。
    const float barY =
        (mMode == Mode::Mini) ? mTargetY + 94.0f : mTargetY + 228.0f;
    const float barX =
        (mMode == Mode::Mini)
            ? mTargetX + 18.0f
            : mTargetX + 620.0f +
                  ((mShowCover && mCoverReady) ? 250.0f : 0.0f);
    const float barW =
        (mMode == Mode::Mini)
            ? mTargetW - 36.0f
            : mTargetW - 660.0f -
                  ((mShowCover && mCoverReady) ? 250.0f : 0.0f);
    // 「我的」页上这块区域属于透明度滑块，不能再当作播放进度条，
    // 否则调透明度会顺带把播放进度拖走。
    const bool progressHitAllowed = !(mTab == Tab::Mine);
    if (progressHitAllowed && mShowProgress &&
        insideRect(x, y, barX, barY - 28.0f, barW, 60.0f) &&
        mDurationMs > 0) {
      mDrag = Drag::Progress;
      const float ratio = std::min(1.0f, std::max(0.0f, (x - barX) / barW));
      mDragSeekMs = static_cast<long long>(mDurationMs * ratio);
      mPositionMs = mDragSeekMs;
      mDirty = true;
    } else if (mMode == Mode::Expanded) {
      const float listH = listHeightFor();
      const float trackX = mTargetX + 24.0f + 540.0f - 10.0f;
      const int visible = std::max(1, static_cast<int>(listH / 54.0f));
      const int maxScroll = std::max(0, static_cast<int>(mTrackCount) - visible);
      if (maxScroll > 0 &&
          insideRect(x, y, trackX - 12.0f, mTargetY + 104.0f, 30.0f, listH)) {
        const float trackH = listH - 4.0f;
        float thumbH = trackH * static_cast<float>(visible) /
                       static_cast<float>(std::max(1, static_cast<int>(mTrackCount)));
        thumbH = std::max(36.0f, thumbH);
        const float travel = trackH - thumbH;
        float ratio = static_cast<float>(mListScroll) /
                      static_cast<float>(maxScroll);
        ratio = std::min(1.0f, std::max(0.0f, ratio));
        const float thumbY = mTargetY + 104.0f + 2.0f + travel * ratio;
        if (y >= thumbY && y <= thumbY + thumbH) {
          mGrabY = y - thumbY;
        } else {
          const float nextRatio =
              std::min(1.0f, std::max(0.0f, (y - mTargetY - 104.0f - 2.0f) /
                                                 std::max(1.0f, travel)));
          mListScroll = nextRatio * maxScroll;
          mGrabY = thumbH * 0.5f;
        }
        mDrag = Drag::ScrollThumb;
        mDirty = true;
      } else if (insideRect(x, y, mTargetX + 24.0f, mTargetY + 104.0f,
                            540.0f, listH)) {
        // 列表区域按下即进入滚动模式，抬手没移动才当作选中某一首。
        mDrag = Drag::List;
        mGrabY = y;
      }
    }
    return true;
  }

  if (action == 2) {
    // 透明度滑块拖动：按下已定位，移动时继续跟随，实时预览。
    if (mDrag == Drag::AlphaSlider && mAlphaDragIdx >= 0 &&
        mAlphaSliderW > 0.0f) {
      const float ratio =
          std::min(1.0f, std::max(0.0f, (x - mAlphaSliderX) / mAlphaSliderW));
      const float v = 0.10f + ratio * 0.90f;
      if (mAlphaDragIdx == 0) {
        mPanelAlpha = v;
      } else if (mAlphaDragIdx == 1) {
        mAlphaMini = v;
      } else {
        mAlphaBar = v;
      }
      mDirty = true;
      return true;
    }
    // 音量拖动：实时跟随手指，不改变其它拖动状态。
    if (mDrag == Drag::Volume) {
      const float top = mVolBarY + 8.0f;
      const float bottom = mVolBarY + mVolBarH - 8.0f;
      const float span = std::max(1.0f, bottom - top);
      mVolume = std::min(1.0f, std::max(0.0f, (bottom - y) / span));
      mDirty = true;
      return true;
    }
    if (!mTracking) {
      return false;
    }
    // 单行迷你条拖动交给下面通用的 Drag::Panel 处理，不需要虚影。
    if (std::fabs(x - mDownX) > 8.0f || std::fabs(y - mDownY) > 8.0f) {
      mMoved = true;
      if (mDrag == Drag::None) {
        mDrag = Drag::Panel;
      }
    }

    if (mDrag == Drag::List) {
      // 行高必须和绘制侧一致：绘制用 max(54, 40*字号)，
      // 这里原来写死 54，字号 >1.35 时两者不同，滚动位移与实际
      // 行位置对不上，表现就是「滑一下卡一下」。
      const float kRowH = std::max(54.0f, 40.0f * mFontScale);
      const float listH = listHeightFor();
      const int maxRows = static_cast<int>(listH / kRowH);
      float maxScroll = static_cast<float>(mTrackCount) -
                        static_cast<float>(maxRows);
      if (maxScroll < 0.0f) {
        maxScroll = 0.0f;
      }
      mListScroll -= (y - mGrabY) / kRowH;
      if (mListScroll < 0.0f) {
        mListScroll = 0.0f;
      }
      if (mListScroll > maxScroll) {
        mListScroll = maxScroll;
      }
      mGrabY = y;
      mDirty = true;
      return true;
    }

    if (mDrag == Drag::ScrollThumb) {
      const float listH = listHeightFor();
      const int visible = std::max(1, static_cast<int>(listH / 54.0f));
      const int total = std::max(1, static_cast<int>(mTrackCount));
      const float maxScroll =
          static_cast<float>(std::max(0, total - visible));
      const float trackH = listH - 4.0f;
      float thumbH = trackH * static_cast<float>(visible) /
                     static_cast<float>(total);
      thumbH = std::max(36.0f, thumbH);
      const float travel = trackH - thumbH;
      float ratio = (y - mGrabY - (mTargetY + 106.0f)) /
                    std::max(1.0f, travel);
      ratio = std::min(1.0f, std::max(0.0f, ratio));
      mListScroll = ratio * maxScroll;
      mDirty = true;
      return true;
    }

    if (mDrag == Drag::Progress) {
      const float barX =
          (mMode == Mode::Mini)
              ? mTargetX + 18.0f
              : mTargetX + 620.0f +
                    ((mShowCover && mCoverReady) ? 250.0f : 0.0f);
      const float barW =
          (mMode == Mode::Mini)
              ? mTargetW - 36.0f
              : mTargetW - 660.0f -
                    ((mShowCover && mCoverReady) ? 250.0f : 0.0f);
      const float ratio = std::min(1.0f, std::max(0.0f, (x - barX) / barW));
      if (mDurationMs > 0) {
        long long target = static_cast<long long>(mDurationMs * ratio);
        if (mPlayableMs > 0 && target > mPlayableMs) {
          target = mPlayableMs;
        }
        mDragSeekMs = target;
        mPositionMs = mDragSeekMs;
        mDirty = true;
      }
    } else if (mDrag == Drag::Panel && mMode == Mode::Bar) {
      // 单行迷你条：位置存到 ballX / ballY，和中窗口的 miniX / miniY 分开，
      // 两种形态各自记住自己的位置。
      // 旧代码这里只判了 Mode::Mini，单行条永远进不来，
      // 表现就是「按住拖不动」。
      mBallX = x - mGrabX;
      mBallY = y - mGrabY;
      mBallX = std::min(std::max(mBallX, 0.0f),
                        std::max(0.0f, mScreenW - mTargetW));
      mBallY = std::min(std::max(mBallY, 0.0f),
                        std::max(0.0f, mScreenH - mTargetH));
      mHasBallPos = true;
      mAnimating = false;
      mTargetX = mBallX;
      mTargetY = mBallY;
      mPanelX = mBallX;
      mPanelY = mBallY;
      // 一旦真的移动过，就不能再当成「轻点放大」。
      mBarTapCandidate = false;
      mDirty = true;
    } else if (mDrag == Drag::Panel && mMode == Mode::Mini) {
      mMiniX = x - mGrabX;
      mMiniY = y - mGrabY;
      if (mMiniX + mTargetW > mScreenW) {
        mMiniX = mScreenW - mTargetW - 12.0f;
      }
      if (mMiniY + mTargetH > mScreenH) {
        mMiniY = mScreenH - mTargetH - 12.0f;
      }
      mMiniX = std::max(0.0f, mMiniX);
      mMiniY = std::max(0.0f, mMiniY);
      mAnimating = false;
      mTargetX = mMiniX;
      mTargetY = mMiniY;
      mPanelX = mMiniX;
      mPanelY = mMiniY;
      mDirty = true;
    }
    return true;
  }

  if (action == 3) {
    // 宿主取消手势：不落位，形态与位置都保持原样。
    mBallGhostActive = false;
    mBallPending = false;
    mTracking = false;
    mDrag = Drag::None;
    mBarTapCandidate = false;
    return false;
  }

  if (action != 1) {
    return false;
  }
  // 抬手：清掉按下高亮，并把当前设置立刻落盘（实时保存）。
  // mPressedAction 只写不清会让按钮高亮一直留着。
  mPressedAction = 0;
  mPressedUntil = 0;
  mPressRectW = 0.0f;
  mAlphaDragIdx = -1;
  mDirty = true;
  saveState();

  if (!mTracking) {
    return false;
  }

  // 单行迷你条没有长按手势：拖动由 Drag::Panel 在移动阶段处理，
  // 抬起阶段只需让下面统一的落位逻辑收尾，不再区分短按/长按。
  mTracking = false;
  const bool moved = mMoved;
  const Drag drag = mDrag;
  mDrag = Drag::None;
  // 单行条上「轻点歌名」= 放大到中窗口。
  // 这样三层之间的路是通的：单行条 -> 中窗口 -> 大面板，
  // 反向也能从大面板 X 回到中窗口、从中窗口右下角回到单行条。
  // 只认「短按且没移动」：长按不放（准备拖动但没拖动）不算放大，
  // 否则玩家按住想挪位置，一松手就跳形态。
  const bool barTap = mBarTapCandidate && !mMoved && (mMode == Mode::Bar) &&
                      (timeMs - mDownTimeMs) < 400;
  mBarTapCandidate = false;

  if (drag == Drag::Progress) {
    if (mDragSeekMs >= 0) {
      mPendingSeek.store(mDragSeekMs);
      mDragSeekMs = -1;
    }
    return true;
  }
  if (drag == Drag::ScrollThumb) {
    return true;
  }
  if (barTap) {
    mMode = Mode::Mini;
    placePanel();
    return true;
  }
  // 允许 20 像素以内的手指漂移仍算「点击」。
  // 原来 8 像素就判成拖动直接 return，而大面板的子页签判定、列表行判定
  // 都在这句之后，于是「看着点中了却没反应」。屏幕越往下越难够到，
  // 漂移越大，表现就是「越靠下的行越点不动」。
  const bool stableTap = std::fabs(x - mDownX) < 20.0f &&
                         std::fabs(y - mDownY) < 20.0f;
  if (moved && !stableTap) {
    return true;
  }
  if (timeMs - mDownTimeMs > 700) {
    return true;
  }

  if (mMode == Mode::Mini) {
    const float cx = mTargetX + mTargetW - 40.0f;
    if (insideButton(x, y, cx - 26.0f, mTargetY + 8.0f, 52.0f, 48.0f, 10.0f, 2.0f)) {
      mMode = Mode::Expanded;
      placePanel();
      return true;
    }
    if (insideButton(x, y, mTargetX + mTargetW - 72.0f,
                    mTargetY + mTargetH - 40.0f, 56.0f, 36.0f, 4.0f, 14.0f)) {
      mMode = Mode::Bar;
      placePanel();
      return true;
    }
    // 迷你条控制键：绘制圆心在 y+162、最大圆直径 62，
// 实际覆盖 y+131 到 y+193。命中区必须与绘制对齐，
// 否则上半截 25 像素是死区，表现为「只有下半格能点」。
    const float rowY = mTargetY + 128.0f;
    const float rowBottom = mTargetY + mTargetH;
    // 与绘制侧一致：整个面板居中（不能扣掉封面宽度）。
    const float miniCenter = mTargetX + mTargetW * 0.5f;
    if (y >= rowY && y <= rowBottom) {
      if (x >= miniCenter - 100.0f && x < miniCenter - 30.0f) {
        mPressRectX = miniCenter - 100.0f;
        mPressRectY = rowY;
        mPressRectW = 70.0f;
        mPressRectH = rowBottom - rowY;
        mPressedUntil = nowMs() + 140;
        mPendingAction.store(ActionPrevious);
        return true;
      }
      if (x >= miniCenter - 30.0f && x < miniCenter + 30.0f) {
        mPressRectX = miniCenter - 30.0f;
        mPressRectY = rowY;
        mPressRectW = 60.0f;
        mPressRectH = rowBottom - rowY;
        mPressedUntil = nowMs() + 140;
        mPendingAction.store(ActionToggle);
        return true;
      }
      if (x >= miniCenter + 30.0f && x < miniCenter + 100.0f) {
        mPressRectX = miniCenter + 30.0f;
        mPressRectY = rowY;
        mPressRectW = 70.0f;
        mPressRectH = rowBottom - rowY;
        mPressedUntil = nowMs() + 140;
        mPendingAction.store(ActionNext);
        return true;
      }
      // 收藏：三大件右边一格。和单行条、大面板用同一个动作。
      // 收藏要联网，必须在锁外执行，所以走 deferred。
      if (x >= miniCenter + 100.0f && x < miniCenter + 170.0f) {
        mPressRectX = miniCenter + 100.0f;
        mPressRectY = rowY;
        mPressRectW = 70.0f;
        mPressRectH = rowBottom - rowY;
        mPressedUntil = nowMs() + 140;
        mPressedAction = 12;
        deferred = [this] { toggleFavoriteOnCurrent(); };
        mDirty = true;
        return true;
      }
    }
    return true;
  }

  // Expanded 模式
  // 未登录：右上角是「登录」按钮（绘制在 w-200 起的 86x44）。
  const float loginX = mTargetX + mTargetW - 206.0f;
  if (!mLoggedIn &&
      insideButton(x, y, loginX, mTargetY + 6.0f, 92.0f, 54.0f, 6.0f, 8.0f)) {
    __android_log_print(ANDROID_LOG_INFO, kTag, "点击登录入口");
    mLoginPage = true;
    mLoginField = 0;
    mSmsMode = false;
    mQrReady = false;
    mQrMsg = "正在获取二维码";
    mPendingQr.store(1);
    mDirty = true;
    setNoticeLocked("请选择登录方式", 2000);
    return true;
  }
  // 已登录：右上角是圆形头像，热区必须与绘制侧的 w-180 起 48x48 对齐。
  // 单独写一个分支，不再和登录按钮共用一个矩形：两者位置不同，
  // 共用会出现「看得见点不到」或者「点到旁边的关闭按钮」。
  if (mLoggedIn &&
      insideButton(x, y, mTargetX + mTargetW - 184.0f, mTargetY + 4.0f,
                   56.0f, 56.0f, 6.0f, 8.0f)) {
    __android_log_print(ANDROID_LOG_INFO, kTag, "点击头像入口");
    mLoginPage = true;
    mSmsMode = false;
    mDirty = true;
    return true;
  }

// 大面板的播放模式与收藏键。
  // 位置必须与绘制侧一致：infoX = mTargetX + 620 + coverShift。
  // 这两个键在三大件的左右各 62px 处，三大件本身没有独立分支
  // （整块被 hitControl 吞掉后按相对坐标分流），所以必须在这里先判，
  // 否则点击会被三大件抢走。
  {
    const float coverShift2 = (mShowCover && mCoverReady) ? 250.0f : 0.0f;
    const float baseX = mTargetX + 620.0f + coverShift2;
    const float btnY2 = mTargetY + 318.0f;
    // 播放方式键：infoX + 230（三大件右侧第一格），坐标与绘制侧一致。
    // 绘制侧去掉了圆底、图标放大到 30，中心与三大件同高（y+318）。
    // 热区 48x56 以该中心对齐，上下各留 28，不会有点不到的死区。
    if (insideButton(x, y, baseX + 206.0f, btnY2 - 28.0f, 48.0f, 56.0f, 2.0f,
                     4.0f)) {
      if (mEngine != nullptr) {
        mEngine->cyclePlayMode();
        setNoticeLocked(
            std::string("播放模式: ") +
                (mEngine->playMode() == AudioEngine::PlayMode::Shuffle
                     ? "随机"
                     : (mEngine->playMode() == AudioEngine::PlayMode::RepeatOne
                            ? "单曲循环"
                            : "顺序")),
            1800);
      }
      mDirty = true;
      return true;
    }
    // 三条杠：播放队列侧栏（infoX + 390）。
    if (insideButton(x, y, baseX + 366.0f, btnY2 - 28.0f, 48.0f, 56.0f, 2.0f,
                     4.0f)) {
      toggleQueueViewLocked();
      return true;
    }
    // 收藏键：infoX + 310（播放方式右边一格）。
    if (insideButton(x, y, baseX + 288.0f, btnY2 - 22.0f, 44.0f, 44.0f, 2.0f,
                     4.0f)) {
      deferred = [this] { toggleFavoriteOnCurrent(); };
      mDirty = true;
      return true;
    }
  }

  // 右上角叉号：收回中面板；关掉中面板时直接回单行条。
  if (insideButton(x, y, mTargetX + mTargetW - 64.0f, mTargetY + 12.0f, 48.0f,
                 52.0f, 8.0f, 10.0f)) {
    mMode = mDisableMini ? Mode::Bar : Mode::Mini;
    placePanel();
    return true;
  }

  // 右下角减号：缩成单行迷你条
  if (insideButton(x, y, mTargetX + mTargetW - 72.0f,
                 mTargetY + mTargetH - 40.0f, 56.0f, 36.0f, 4.0f, 14.0f)) {
    mMode = Mode::Bar;
    placePanel();
    return true;
  }

  // 输入框与确定键
  const float inputY = mTargetY + 52.0f;
  const float listLeft = mTargetX + 24.0f;
  // 专辑层的返回键必须放在输入框之前判断。
  // 两者位置几乎完全重叠（都是 x+24 起、y+52 起，高度 36 / 40），
  // 输入框在前时永远先命中 —— 表现就是「点返回弹出键盘、返回没反应」。
  if (mTab == Tab::Search && mAlbumView == 1 && mQueueView == 0 &&
      insideButton(x, y, listLeft, inputY, 96.0f, 36.0f, 2.0f, 6.0f)) {
    __android_log_print(ANDROID_LOG_INFO, kTag, "专辑页: 返回");
    mAlbumView = 0;
    mAlbumOpenName.clear();
    mListScroll = 0.0f;
    // 退出这一层必须同时收起输入态，否则键盘还挂在屏幕上。
    mInputActive = false;
    mDirty = true;
    return true;
  }
  if (mTab == Tab::Search && mAlbumView == 0 && mQueueView == 0 &&
      insideButton(x, y, listLeft, inputY, 540.0f - 74.0f, 40.0f, 4.0f, 12.0f)) {
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "点击输入框: 键盘回调=%d 文本回调=%d",
                        mShowKeyboard != nullptr ? 1 : 0,
                        mRegisterText != nullptr ? 1 : 0);
    mInputActive = true;
    if (mShowKeyboard != nullptr) {
      deferred = [this] { mWantKeyboard.store(1); };
    } else {
      __android_log_print(ANDROID_LOG_WARN, kTag, "软键盘接口未解析到");
    }
    setNoticeLocked("输入完成后点确定", 2500);
    return true;
  }
  if (mTab == Tab::Search && mQueueView == 0 &&
      insideButton(x, y, listLeft + 540.0f - 66.0f, inputY, 66.0f, 40.0f, 4.0f, 12.0f)) {
    mPendingSearchText = mInputText;
    mSearchSubmit.store(1);
    return true;
  }

  // 底部四个一级入口：搜索、推荐、收藏、我的。
  const float tabY = mTargetY + mTargetH - 54.0f;
  const float tabW = 540.0f / 4.0f;
  if (mQueueView == 0 &&
      insideButton(x, y, listLeft, tabY, 540.0f, 40.0f, 0.0f, 12.0f)) {
    const int slot = static_cast<int>((x - listLeft) / tabW);
    // 显示位次 -> Tab 枚举值，必须与绘制侧 tabEnums 一致。
    const int slotToTab[4] = {static_cast<int>(Tab::Search),
                              static_cast<int>(Tab::Recommend),
                              static_cast<int>(Tab::Favorites),
                              static_cast<int>(Tab::Mine)};
    if (slot >= 0 && slot < 4) {
      const int tab = slotToTab[slot];
      // 搜索、收藏、我的需要登录后才能使用；推荐页的「探索/小众」也要登录，
      // 但推荐页本身默认进红心，未登录时红心也是空的，所以一并拦住更清楚。
      if (!mLoggedIn && tab != static_cast<int>(Tab::Recommend)) {
        setNoticeLocked("请先登录", 2000);
        return true;
      }
      if (mTab != static_cast<Tab>(tab)) {
        mTabAnimStartMs = nowMs();
      } else {
        // 同一个一级页再点也重放动画，避免「点一次要等一会儿才有反应」的观感。
        mTabAnimStartMs = nowMs();
      }
      mTab = static_cast<Tab>(tab);
      // 进收藏页就重新拉一次：玩家刚点过爱心，切过来必须立刻看到结果。
      if (mTab == Tab::Favorites) {
        mFavoriteListRequest.store(1);
      }
      mListScroll = 0.0f;
      mDirty = true;
      const int subTab = mRecTab;
      // 回调会回头调 setListTracks，而本函数已持界面锁，
      // 锁不可重入，在锁内回调会直接把自己锁死。
      deferred = [this, tab, subTab] {
        if (mTabChangeHandler) {
          mTabChangeHandler(tab);
        }
        if (mTabHandler) {
          mTabHandler(tab == 1 ? 10 + subTab : tab);
        }
      };
    }
    return true;
  }
  // 推荐页顶部的三个子分类。
  if (mTab == Tab::Recommend && mQueueView == 0) {
    const float subY = mTargetY + 52.0f;
    const float subW = 540.0f / 3.0f;
    for (int i = 0; i < 3; ++i) {
      if (!insideButton(x, y, listLeft + i * subW, subY, subW, 40.0f, 1.0f, 12.0f)) {
        continue;
      }
      if (!mLoggedIn) {
        setNoticeLocked("请先登录", 2000);
        return true;
      }
      if (mRecTab != i) {
        mRecTabAnimStartMs = nowMs();
        mTabAnimStartMs = nowMs();
      } else {
        // 同一子页再点也重放动画，不做冷却。
        mRecTabAnimStartMs = nowMs();
        mTabAnimStartMs = nowMs();
      }
      __android_log_print(ANDROID_LOG_INFO, kTag,
                          "子页签点击 序号=%d 落点=%.0f,%.0f 相对x=%.0f", i, x,
                          y, x - listLeft);
      mRecTab = i;
      mListScroll = 0.0f;
      mDirty = true;
      const int subIndex = i;
      deferred = [this, subIndex] {
        if (mTabHandler) {
          mTabHandler(10 + subIndex);
        }
      };
      return true;
    }
  }

const float listX = mTargetX + 24.0f;
  // 与绘制侧共用 listTopFor()，杜绝「画在一处、点另一处」。
  const float listY = listTopFor();
  const float listW = 540.0f;
  // 必须与绘制侧的行高公式完全一致，否则点击会错行。
  const float rowH = std::max(54.0f, 40.0f * mFontScale);
  // 专辑层的返回键已经在「输入框」之前处理掉了（见上方说明），
  // 这里不再重复判断，避免两处坐标各自演化后再次错位。

  // ============ 歌手页（优先于搜索页与列表行）============
  if (mTab == Tab::Search && mArtistView == 1 && mAlbumView == 0 &&
      mQueueView == 0) {
    // 返回键：与绘制侧同坐标（listLeft, mTargetY+70, 96x28）。
    if (insideButton(x, y, listLeft, mTargetY + 70.0f, 96.0f, 28.0f, 2.0f,
                     6.0f)) {
      __android_log_print(ANDROID_LOG_INFO, kTag, "歌手页: 返回");
      // 本函数调用时已持 mMutex，必须用 Locked 版本，否则死锁。
      exitArtistLocked();
      return true;
    }
    // 子标签：单曲 / 专辑（listX + ai*110, y+100, 104x30）
    for (int ai = 0; ai < 2; ++ai) {
      if (insideButton(x, y, listLeft + ai * 110.0f, mTargetY + 100.0f, 104.0f,
                       30.0f, 2.0f, 4.0f)) {
        if (mArtistTab != ai) {
          mArtistTab = ai;
          mListScroll = 0.0f;
          mDirty = true;
          // 切到专辑且还没数据时，请求 main 去拉。
          if (ai == 1 && mArtistAlbums.empty()) {
            mPendingArtistId = mArtistInfo.id;
            mArtistLoadRequest.store(2);
          }
        }
        return true;
      }
    }
    // 排序胶囊：热度 / 时间 切换，切完重新拉当前子标签。
    if (insideButton(x, y, listLeft + 240.0f, mTargetY + 100.0f, 120.0f, 30.0f,
                     2.0f, 4.0f)) {
      mArtistSortByTime = !mArtistSortByTime;
      mPendingArtistId = mArtistInfo.id;
      mArtistLoadRequest.store(mArtistTab == 1 ? 2 : 1);
      setNoticeLocked(mArtistSortByTime ? "排序: 按时间" : "排序: 按热度",
                      1800);
      mDirty = true;
      return true;
    }
    // 列表行：单曲走播放，专辑进入专辑歌曲。
    // 上沿用 listY（= listTopFor()），不能再写死常量。
    if (insideRect(x, y, mTargetX + 24.0f, listY, 540.0f,
                   mTargetH - 204.0f)) {
      const float rowH2 = std::max(54.0f, 40.0f * mFontScale);
      const int startRow3 = std::max(0, static_cast<int>(mListScroll));
      const float shift3 =
          (mListScroll - static_cast<float>(startRow3)) * rowH2;
      const int row3 = static_cast<int>((y - listY + shift3) / rowH2);
      const int idx3 = startRow3 + row3;
      if (mArtistTab == 1) {
        // 专辑：记下 album id，交给 main 拉歌曲。
        if (idx3 >= 0 &&
            static_cast<std::size_t>(idx3) < mArtistAlbums.size()) {
          mPendingAlbumId = mArtistAlbums[idx3].id;
          mPendingAlbumName = mArtistAlbums[idx3].name;
          __android_log_print(ANDROID_LOG_INFO, kTag, "歌手页: 打开专辑 %s",
                              mArtistAlbums[idx3].name.c_str());
        }
        return true;
      }
      // 歌手页单曲行同样支持「下一首播放」。
      if (x >= listX + listW - 56.0f && x <= listX + listW - 6.0f) {
        if (idx3 >= 0 &&
            static_cast<std::size_t>(idx3) < mTrackIds.size()) {
          mPendingPlayNext.store(idx3);
          setNoticeLocked("已加入下一首播放", 1600);
          mDirty = true;
          __android_log_print(ANDROID_LOG_INFO, kTag,
                              "下一首播放(歌手页): 行 %d 列表=%zu", idx3,
                              mTrackCount);
        }
        return true;
      }
      if (idx3 >= 0 &&
          static_cast<std::size_t>(idx3) < mTrackIds.size()) {
        mPendingIndex.store(idx3);
      }
      return true;
    }
    return true;
  }

  // ============ 搜索页三分类标签 ============
  // 与绘制侧同条件：没有查询词时不响应，避免点到看不见的东西。
  if (mTab == Tab::Search && mArtistView == 0 && mAlbumView == 0 &&
      mQueueView == 0 && searchHasQuery()) {
    for (int ki = 0; ki < 3; ++ki) {
      if (insideButton(x, y, listLeft + ki * 180.0f, mTargetY + 96.0f, 180.0f,
                       30.0f, 1.0f, 4.0f)) {
        if (mSearchKind != ki) {
          mSearchKind = ki;
          mListScroll = 0.0f;
          mDirty = true;
          __android_log_print(ANDROID_LOG_INFO, kTag, "搜索分类切到 %d", ki);
          // 已经在搜索页且有结果时，切分类要重新搜一次。
          if (!mInputText.empty()) {
            mPendingSearchText = mInputText;
            mSearchSubmit.store(1);
          }
        }
        return true;
      }
    }
  }

  // ============ 收藏页 ============
  // 第二层的返回键：放左半边，坐标必须与绘制侧完全一致。
  if (mTab == Tab::Favorites && mFavView == 1 && mQueueView == 0 &&
      insideButton(x, y, listLeft, mTargetY + 70.0f, 96.0f, 28.0f, 2.0f,
                   6.0f)) {
    __android_log_print(ANDROID_LOG_INFO, kTag, "收藏页: 返回歌单列表");
    // 本函数调用时已持 mMutex，必须用 Locked 版本，否则死锁。
    exitFavoritePlaylistLocked();
    // 退回后列表要恢复成歌单，通知 main 重新拉一次歌单列表。
    mFavoriteListRequest.store(1);
    return true;
  }
  // 第一层：点歌单卡片进入。
  if (mTab == Tab::Favorites && mFavView == 0 && mQueueView == 0 &&
      insideRect(x, y, listX, listY, listW, listHeightFor())) {
    const float listH2 = listHeightFor();
    const int startRow2 = std::max(0, static_cast<int>(mListScroll));
    const float rowShift2 =
        (mListScroll - static_cast<float>(startRow2)) * rowH;
    const int row2 = static_cast<int>((y - listY + rowShift2) / rowH);
    const int idx = startRow2 + row2;
    if (idx >= 0 && static_cast<std::size_t>(idx) < mFavPlaylists.size()) {
      const PlaylistInfo &pl = mFavPlaylists[idx];
      mPendingOpenPlaylist = pl.id;
      // 记住选中的歌单：之后点爱心就往这个歌单里加。
      mFavOpenName = pl.name;
      __android_log_print(ANDROID_LOG_INFO, kTag,
                          "收藏页: 点歌单 %s listid=%s", pl.name.c_str(),
                          pl.id.c_str());
      mPressRectX = listX;
      mPressRectY = listY + static_cast<float>(row2) * rowH - rowShift2;
      mPressRectW = listW;
      mPressRectH = rowH;
      mPressedUntil = nowMs() + 140;
      mDirty = true;
      // 曲目由 main 拉，拉完回调 enterFavoritePlaylist。
      return true;
    }
    return true;
  }

  // 搜索页「专辑」分类：点卡片进入专辑歌曲。
  // 必须排除专辑层与歌手页：否则进了专辑歌曲再点行，
  // 会被这里当成「又点了一次专辑卡片」，行数据被当专辑处理（穿模的根源）。
  if (mTab == Tab::Search && mArtistView == 0 && mAlbumView == 0 &&
      mQueueView == 0 && mSearchKind == 1 &&
      insideRect(x, y, listX, listY, listW, listHeightFor())) {
    const float shiftA = (mListScroll -
                          static_cast<float>(std::max(0, static_cast<int>(mListScroll)))) *
                         rowH;
    const int rowA = static_cast<int>((y - listY + shiftA) / rowH);
    const int idxA = std::max(0, static_cast<int>(mListScroll)) + rowA;
    if (idxA >= 0 && static_cast<std::size_t>(idxA) < mSearchAlbums.size()) {
      mPendingAlbumId = mSearchAlbums[idxA].id;
      mPendingAlbumName = mSearchAlbums[idxA].name;
      __android_log_print(ANDROID_LOG_INFO, kTag, "搜索页: 打开专辑 %s",
                          mSearchAlbums[idxA].name.c_str());
    }
    return true;
  }
  // 搜索页「歌手」分类：点卡片进歌手页。
  if (mTab == Tab::Search && mArtistView == 0 && mAlbumView == 0 &&
      mQueueView == 0 && mSearchKind == 2 &&
      insideRect(x, y, listX, listY, listW, listHeightFor())) {
    const float shiftB = (mListScroll -
                          static_cast<float>(std::max(0, static_cast<int>(mListScroll)))) *
                         rowH;
    const int rowB = static_cast<int>((y - listY + shiftB) / rowH);
    const int idxB = std::max(0, static_cast<int>(mListScroll)) + rowB;
    if (idxB >= 0 && static_cast<std::size_t>(idxB) < mSearchArtists.size()) {
      mArtistInfo = mSearchArtists[idxB];
      mArtistView = 1;
      mArtistTab = 0;
      mListScroll = 0.0f;
      mDirty = true;
      // 请求 main 拉歌手资料与歌曲。
      mPendingArtistId = mArtistInfo.id;
      mArtistLoadRequest.store(1);
      __android_log_print(ANDROID_LOG_INFO, kTag, "搜索页: 进入歌手 %s",
                          mArtistInfo.name.c_str());
    }
    return true;
  }

  if (insideRect(x, y, listX, listY, listW, listHeightFor())) {
    // 行右端两个按钮：左边「收藏」（心形）、右边「下一首播放」（加号）。
    // 只有真·歌曲行有，专辑/歌手/歌单卡片列表直接跳过。
    // 行号换算必须与绘制同源，否则滚动之后会点错一行。
    if (listShowsTracksLocked() && x >= listX + listW - 96.0f &&
        x <= listX + listW - 6.0f) {
      const int startRowN = std::max(0, static_cast<int>(mListScroll));
      const float shiftN =
          (mListScroll - static_cast<float>(startRowN)) * rowH;
      const int rowN = static_cast<int>((y - listY + shiftN) / rowH);
      const int idxN = startRowN + rowN;
      const std::size_t listTotal =
          (mQueueView == 1) ? mQueueTracks.size() : mTrackCount;
      const bool hitFav = (x < listX + listW - 52.0f);
      if (idxN >= 0 && static_cast<std::size_t>(idxN) < listTotal) {
        if (hitFav) {
          // 收藏：只切状态，不播放、不插队。
          mPendingFavTrack.store(idxN);
          mDirty = true;
          __android_log_print(ANDROID_LOG_INFO, kTag,
                              "行内收藏(点击): 行 %d tab=%d 队列=%d 总数=%zu",
                              idxN, static_cast<int>(mTab), mQueueView,
                              listTotal);
        } else {
          mPendingPlayNext.store(idxN);
          setNoticeLocked("已加入下一首播放", 1600);
          mDirty = true;
          __android_log_print(ANDROID_LOG_INFO, kTag,
                              "下一首播放(点击): 行 %d tab=%d 子页=%d 列表=%zu",
                              idxN, static_cast<int>(mTab), mRecTab,
                              mTrackCount);
        }
      }
      return true;
    }
    // 必须和绘制用同一套小数偏移：绘制是
    //   ry = listY + k*rowH - rowShift
    // 所以反过来求行号要加回 rowShift，否则列表滚动过之后整体错一行。
    int startRow = static_cast<int>(mListScroll);
    if (startRow < 0) {
      startRow = 0;
    }
    const float rowShift = (mListScroll - static_cast<float>(startRow)) * rowH;
    int row = static_cast<int>((y - listY + rowShift) / rowH);
    if (row < 0) {
      row = 0;
    }
    const int index = startRow + row;
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "列表点击 y=%.0f 行内=%.0f 行高=%.0f 起始行=%d "
                        "偏移=%.1f 行=%d 曲目=%d 总数=%zu",
                        y, y - listY, rowH, startRow, rowShift, row, index,
                        mTrackCount);
    // 队列视图下点击的是播放队列，索引与引擎队列一致。
    const std::size_t rowTotal =
        (mQueueView == 1) ? mQueueTracks.size() : mTrackCount;
    if (index >= 0 && static_cast<std::size_t>(index) < rowTotal) {
      mPendingIndex.store(index);
    }
    return true;
  }

  const float ctrlX =
      mTargetX + 620.0f +
      ((mShowCover && mCoverReady) ? 250.0f : 0.0f);
  // 命中区必须与绘制对齐：圆钮中心在 y+318、直径 74，覆盖 y+281…y+355。
  // 旧值 270…350 让按钮上半截点不到、下半截点得到，就是「点下半部分才有用」。
  const float ctrlY = mTargetY + 281.0f;
  if (y >= ctrlY && y <= ctrlY + 74.0f) {
    if (x >= ctrlX - 5.0f && x < ctrlX + 65.0f) {
      mPendingAction.store(ActionPrevious);
      return true;
    }
    if (x >= ctrlX + 65.0f && x < ctrlX + 130.0f) {
      mPendingAction.store(ActionToggle);
      return true;
    }
    if (x >= ctrlX + 130.0f && x < ctrlX + 215.0f) {
      mPendingAction.store(ActionNext);
      return true;
    }
  }
  // 面板内的空白处仍然消费：否则点在面板上的空白会漏给游戏，视角跟着转。
  return inPanelArea(x, y);
}

HudUi::~HudUi() {
  // 对象析构时若提交线程还 joinable，std::thread 会直接 terminate。
  stopSubmitThread();
}

void HudUi::startSubmitThread() {
  if (mSubmitThread.joinable()) {
    return;
  }
  mSubmitStop.store(false);
  mSubmitThread = std::thread([this] {
    while (!mSubmitStop.load()) {
      if (mSubmitWanted.exchange(false)) {
        submit();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
  });
}

void HudUi::stopSubmitThread() {
  mSubmitStop.store(true);
  if (mSubmitThread.joinable()) {
    mSubmitThread.join();
  }
}

bool HudUi::submit() {
  if (mSubmitDraw == nullptr) {
    return false;
  }
// 拿锁也计时：若工作线程正卡在提交或网络里，这里能看出主线程在等锁。
  mStage.store(6);
  std::unique_lock<std::mutex> lock(mMutex, std::defer_lock);
  if (!lock.try_lock()) {
    const auto waitStart = std::chrono::steady_clock::now();
    lock.lock();
    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - waitStart).count();
    if (waited > 30) {
      __android_log_print(ANDROID_LOG_WARN, kTag, "提交等锁 %lld 毫秒",
                          static_cast<long long>(waited));
    }
  }
  if (!mInstalled) {
    return false;
  }
  if (mMenuChecker && mMenuChecker()) {
    auto clearFn = reinterpret_cast<SubmitDrawFn>(mSubmitDraw);
    clearFn(kModuleId, std::span<const Cmd>());
    return true;
  }

  Out out;
  out.reserve(48);

  // 主题：每帧根据当前模式算一次，只有变化时才重套配色。
  // 0=跟随时间，1=强制日间，2=强制夜间。
  {
    const int hour = currentHour();
    const bool night = (mThemeMode == 2) ? true
                      : (mThemeMode == 1) ? false
                                          : isNightHour(hour);
    const int nightFlag = night ? 1 : 0;
    if (nightFlag != mAppliedNight) {
      applyTheme(night);
      mAppliedNight = nightFlag;
      mDirty = true;
      __android_log_print(ANDROID_LOG_INFO, kTag,
                          "主题切换 模式=%d 小时=%d 夜间=%d", mThemeMode, hour,
                          nightFlag);
    }
  }

  // 没有歌、用户也没主动打开过面板时，一个图元都不提交。
  // 原因：原来不管有没有歌都画迷你条，虽然看不见但占着屏幕一块区域，
  // 玩家会感觉「有一块隐形面板要点掉才能操作游戏」。
  // 这里直接清空绘制并返回，屏幕上完全不留痕迹。
  if (!mHasTrack && !mHudArmed) {
    if (mSubmittedOnce) {
      auto clearFn = reinterpret_cast<SubmitDrawFn>(mSubmitDraw);
      clearFn(kModuleId, std::span<const Cmd>());
      mSubmittedOnce = false;
    }
    return true;
  }
  mSubmittedOnce = true;
  const float x = mPanelX;
  const float y = mPanelY;
  const float w = mPanelW;
  const float h = mPanelH;

  // 只有圆球形态不铺矩形底（否则四角会把圆切成方块）。
  // 迷你条必须铺底：否则白字直接压在天空上，白天看不清歌名。
  {
    gDbgSec = "panel";
    // 单行条 / 中面板各有独立透明度；大面板走 mPanelAlpha。
    float alphaOverride = -1.0f;
    if (mMode == Mode::Bar && mAlphaBar >= 0.0f) {
      alphaOverride = mAlphaBar;
    } else if (mMode == Mode::Mini && mAlphaMini >= 0.0f) {
      alphaOverride = mAlphaMini;
    }
    glassPanel(out, x, y, w, h, mPanelAlpha, alphaOverride);
    // 顶部分隔线交给 glassPanel 内部的高光，这里不再额外画。
    // 原来这里画一条通长 1px 线，方形时看不出问题，
    // 面板改圆角后这条线会从四个圆角处伸出去。
  }
  // 底之后的所有图元都属于「内容」，切页时只对内容做淡入与滑入，
  // 避免连底板一起淡出，看上去像闪一下。
  const std::size_t contentFrom = out.size();
  if (mMode == Mode::Bar) {
    gDbgSec = "bar";
    // 单行迷你条：[歌名（长名滚动）] [上曲] [暂停] [下曲] [放大]
    // 只有一行，没有封面、没有进度条、没有次要信息。
    // 几何必须与按下阶段的命中区完全一致。
    // 必须与按下阶段的命中区完全一致，改一处就要改两处。
    const float padL = 12.0f;
    const float cyMid = y + h * 0.5f;
    const float expandW = 40.0f;
    const float btnW = 40.0f;
    const float playW = 46.0f;
    const float gap = 3.0f;
    const float expandX = x + w - padL - expandW;
    // 新增两个小按钮，宽 34（比三大件小一号）：
    //   小模式键放在「上曲」左边，爱心放在「下曲」右边。
    // 与触摸侧完全一致：单行条只有收藏一个键。
    //   从右往左：放大 → 收藏 → 下曲 → 播放 → 上曲
    const float smallW = 40.0f;
    const float heartX = expandX - gap - smallW;
    const float nextX = heartX - gap - btnW;
    const float playX = nextX - gap - playW;
    const float prevX = playX - gap - btnW;
    const float titleW = std::max(60.0f, prevX - padL - 10.0f);
    const float titleSize = 19.0f * mFontScale;

    // 歌名：超出宽度自动滚动。
    marqueeFit(out, x + padL, cyMid - titleSize * 0.5f, titleSize, titleW,
               mTitle.empty() ? std::string("未在播放") : mTitle, kTextMain,
               nowMs());

    // 单行条不放播放顺序按钮：它只在大面板里出现。
    // 上曲
    if (mPressedAction == 6) {
      roundRect(out, prevX - 4.0f, cyMid - 26.0f, btnW + 8.0f, 52.0f, 12.0f,
                kRowActive);
    }
    iconPrev(out, prevX + btnW * 0.5f, cyMid, 23.0f, kTextMain);

    // 播放 / 暂停：实心圆底主按钮，播放中用强调色，暂停用轨道色。
    {
      const bool playing = (mPlaying && !mPaused && mHasTrack);
      const float pcx = playX + playW * 0.5f;
      circleFill(out, pcx, cyMid, playW, playing ? kAccent : kTrack);
      if (mPressedAction == 7) {
        circleFill(out, pcx, cyMid, playW + 8.0f, kRowActive);
      }
      if (playing) {
        iconPause(out, pcx, cyMid, 20.0f, 0xFF12151A);
      } else {
        iconPlay(out, pcx, cyMid, 21.0f, kTextMain);
      }
    }

    // 下曲
    if (mPressedAction == 8) {
      roundRect(out, nextX - 4.0f, cyMid - 26.0f, btnW + 8.0f, 52.0f, 12.0f,
                kRowActive);
    }
    iconNext(out, nextX + btnW * 0.5f, cyMid, 23.0f, kTextMain);

    // 收藏（爱心）：已收藏为实心强调色，未收藏为空心文字色。
    {
      if (mPressedAction == 12) {
        circleFill(out, heartX + smallW * 0.5f, cyMid, smallW - 4.0f,
                   kRowActive);
      }
      glyphHeart(out, heartX + smallW * 0.5f, cyMid, 18.0f, mTrackFavorited,
                 mTrackFavorited ? kAccent : kTextDim);
    }

    // 放大：展开为完整面板
    if (mPressedAction == 9) {
      roundRect(out, expandX - 4.0f, cyMid - 24.0f, expandW + 8.0f, 48.0f,
                12.0f, kRowActive);
    }
    // 原来用 kTextDim（很暗）画 22px，加上四条断开的外框，
    // 视觉上就像只画出半个图标。改成主文字色、24px。
    glyphExpand(out, expandX + expandW * 0.5f, cyMid, 21.0f, kTextMain);
  } else if (mMode == Mode::Mini) {
    gDbgSec = "mini";


    // 这里原来手绘了四条 2px 边当边框，和 glassPanel 的边框叠在一起，
    // 是「边边角角色差」的来源之一。现在边框统一由 glassPanel 处理，删掉。
    const float coverW = mShowCover ? 100.0f : 0.0f;

    if (mShowCover && mCoverReady) {
      const float cx0 = x + w - coverW - 10.0f;
      const float cy0 = y + 52.0f;
      image(out, cx0, cy0, 88.0f, 88.0f, coverId().c_str());
      // 封面不加任何描边／边框／高光块。
      // 原来这里画了 1px 白边 + 4 条暗边 + 2 个高光块，
      // 在半透明面板上会和封面边缘叠成脏边，看着像一圈毛。
    }

    const float miniTitleSize = 27.0f * mFontScale;
    marqueeFit(out, x + 18.0f, y + 16.0f, miniTitleSize, w - 78.0f, mTitle,
               kTextMain, nowMs());
    text(out, x + 18.0f, y + 50.0f, 18.0f * mFontScale, kTextDim, mArtist);

    if (mShowProgress) {
      const float barX = x + 18.0f;
      const float barW = w - coverW - 36.0f;
      const float barY = y + 94.0f;
      roundRect(out, barX, barY - 1.0f, barW, 8.0f, 4.0f, kTrack);
      if (mDurationMs > 0) {
        if (mPlayableMs > 0 && mPlayableMs < mDurationMs) {
          const float playRatio = static_cast<float>(mPlayableMs) /
                                  static_cast<float>(mDurationMs);
          roundRect(out, barX, barY - 1.0f, barW * playRatio, 8.0f, 4.0f,
                    0xFF2F6E66);
        }
        const float ratio = std::min(
            1.0f, std::max(0.0f, static_cast<float>(mPositionMs) /
                                    static_cast<float>(mDurationMs)));
        roundRect(out, barX, barY - 1.0f, barW * ratio, 8.0f, 4.0f,
                  kAccent);
        const float knobX = barX + barW * ratio;
        circleFill(out, knobX, barY + 3.0f, 16.0f, 0x334FE3C1);
        circleFill(out, knobX, barY + 3.0f, 9.0f, kTextMain);
      }
      text(out, barX, y + 114.0f, 13.0f * mFontScale, kTextDim,
           fmtTime(mPositionMs) + " / " + fmtTime(mDurationMs));
    }

    const float rowY = y + 162.0f;
    // 三大件必须按【整个面板】居中，不能按内容区居中。
    // 原来用 controlsX + controlsW*0.5（扣掉了封面 100 宽），
    // 开封面时面板从 560 变 660，算出来的中心却仍是 x+280，
    // 比真实中心 x+330 偏左 50 像素 —— 这就是"字数一多就偏"的原因。
    // 封面在 x+550 起，三大件最远到中心+62=392，不会撞上，安全。
    const float miniCenter = x + w * 0.5f;
    constexpr float kMiniKeyGap = 62.0f;
    iconPrev(out, miniCenter - kMiniKeyGap, rowY, 30.0f, kTextMain);
    // 播放/暂停：套一个实心圆，图标用面板底色做反差。
    // 上一首/下一首保持纯图标（你说过玻璃圆底丑，只给主键加圈）。
    {
      const float r = 27.0f;
      circleFill(out, miniCenter, rowY, r * 2.0f, kAccent);
      if (mPaused || !mPlaying || !mHasTrack) {
        glyphPlay(out, miniCenter + r * 0.06f, rowY, 30.0f, 0xFF12151A);
      } else {
        glyphPause(out, miniCenter, rowY, 26.0f, 0xFF12151A);
      }
    }
    iconNext(out, miniCenter + kMiniKeyGap, rowY, 30.0f, kTextMain);
    // 收藏爱心：中面板三大件右边一格，间距与三大件相同。
    // 单行条太窄放不下这个键，大面板本来就有，所以只在中面板补。
    // 已收藏时用强调色实心，未收藏是暗色空心，一眼能分辨状态。
    {
      const float hcx = miniCenter + kMiniKeyGap * 2.0f;
      circleFill(out, hcx, rowY, 40.0f, 0x1A000000);
      glyphHeart(out, hcx, rowY, 20.0f, mTrackFavorited,
                 mTrackFavorited ? kAccent : kTextDim);
    }
    if (nowMs() < mPressedUntil && mPressRectW > 0.0f) {
      // 按下时先向外弹出一圈淡光，再叠主色填充，形成弹一下的手感。
      const float left = static_cast<float>(mPressedUntil - nowMs()) / 140.0f;
      const float k = std::max(0.0f, std::min(1.0f, left));
      roundRect(out, mPressRectX - 3.0f * k, mPressRectY - 3.0f * k,
                mPressRectW + 6.0f * k, mPressRectH + 6.0f * k,
                kRadiusBtn + 3.0f * k, 0x228DBDA2);
      roundRect(out, mPressRectX, mPressRectY, mPressRectW, mPressRectH,
                kRadiusBtn, 0x2E8DBDA2);
      rect(out, mPressRectX + kRadiusBtn, mPressRectY,
           mPressRectW - kRadiusBtn * 2.0f, 2.0f, 0x998DBDA2);
      rect(out, mPressRectX + kRadiusBtn, mPressRectY + mPressRectH - 2.0f,
           mPressRectW - kRadiusBtn * 2.0f, 2.0f, 0x998DBDA2);
    }

    // 放大图标原来是 kTextDim（很暗），实测玩家要凑近才认得出是按钮。
    // 改成主文字色，和单行条那边的放大图标保持一致。
    glyphExpand(out, x + w - 40.0f, y + 28.0f, 26.0f, kTextMain);
    // 右下角「收成单行条」按钮。
    // 原来只是一条 30x4 的横线，实测玩家根本找不到这个入口。
    // 改成有底的圆角按钮 + 减号图标，尺寸与命中区 56x36 对齐，
    // 位置是 x+w-72 / y+h-40，界面上看到的那块就是可点区域。
    {
      const float mbX = x + w - 72.0f;
      const float mbY = y + h - 40.0f;
      const bool pressedMb = (mPressedAction == 9);
      roundRect(out, mbX, mbY, 56.0f, 36.0f, 10.0f,
                pressedMb ? 0x558DBDA2 : 0x33000000);
      rect(out, mbX + 19.0f, mbY + 17.0f, 18.0f, 3.0f,
           pressedMb ? kAccent : kTextMain);
    }
  } else {
    gDbgSec = "big";
    // 推荐页有三个子分类（红心/探索/小众），标题要跟着子分类变，
    // 否则切到探索时标题还写「推荐」，与内容不符。
    const char *recSubNames[3] = {"红心", "探索", "小众"};
    const char *pageName = (mTab == Tab::Search) ? tr("搜索", "Search")
                            : (mTab == Tab::Mine) ? tr("我的", "Mine")
                            : (mTab == Tab::Favorites) ? tr("收藏", "Favorites")
                            : recSubNames[std::min(2, std::max(0, mRecTab))];
    // 专辑歌曲层：标题换成专辑名，让玩家知道自己停在哪一层。
    // 用 marqueeFit 而不是 text，专辑名过长时会滚动，不会溢出面板。
    if (mQueueView == 1) {
      text(out, x + 24.0f, y + 14.0f, 26.0f * mFontScale, kTextMain,
           tr("播放列表", "Playlist"));
      // 队列视图下副标题单独显示首数，下面那段通用副标题会跳过。
      text(out, x + 24.0f, y + 46.0f, 14.0f * mFontScale, kTextDim,
           std::to_string(mQueueTracks.size()) + tr(" 首", " tracks"));
    } else if (mTab == Tab::Search && mAlbumView == 1 &&
               !mAlbumOpenName.empty()) {
      marqueeFit(out, x + 24.0f, y + 14.0f, 26.0f * mFontScale,
                 w - 300.0f, mAlbumOpenName, kTextMain, nowMs());
    } else {
      text(out, x + 24.0f, y + 14.0f, 26.0f * mFontScale, kTextMain, pageName);
    }
    // 搜索页的副标题会与下方输入框（y+52）重叠，这一页不画。
    if (mTab != Tab::Search && mQueueView == 0) {
      text(out, x + 24.0f, y + 46.0f, 14.0f * mFontScale, kTextDim,
           (mTab == Tab::Mine) ? std::string("账号与设置")
           : (mTab == Tab::Favorites)
               // 第一层列的是歌单，副标题必须说歌单数。
               // 之前这里用了 mTrackCount，显示成上一页遗留的「30 首收藏」。
               ? ((mFavView == 0)
                      ? (mFavPlaylists.empty()
                             ? std::string("还没有歌单")
                             : std::to_string(mFavPlaylists.size()) +
                                   " 个歌单")
                      : (mTrackCount == 0
                             ? std::string("这个歌单还是空的")
                             : std::to_string(mTrackCount) + " 首收藏"))
               : std::to_string(mTrackCount) + " 首曲目");
    }
    if (mLoggedIn) {
      // 已登录：右上角画圆形头像。
      // 位置从 w-132 再往左挪一个按钮位（48 + 12），和关闭按钮拉开，
      // 两个热区不再互相挨着，点错的概率明显下降。
      const float ax = x + w - 180.0f;
      const float ay = y + 8.0f;
      // 先画略大的强调色圆、再画内圆，形成 2px 描边。
      // 头像图本身四角是全透明的，底下必须有底色，否则会露出面板背景，
      // 看起来像「头像缺了一块」。
      circleFill(out, ax + 24.0f, ay + 24.0f, 52.0f, kAccent);
      circleFill(out, ax + 24.0f, ay + 24.0f, 48.0f, 0xFF3C5170);
      if (mAvatarReady) {
        // 圆形裁切过的真实头像，左上角与圆形底色完全对齐。
        image(out, ax, ay, 48.0f, 48.0f, avatarId().c_str());
      } else {
        // 还没拿到头像：先画中性人形图标，不显示任何字符。
        // 以前这里是昵称首字符，账号 id 是数字时就显示成一个莫名其妙的
        // 「二」，玩家以为是 bug。人形图标不会被误读成状态或数量。
        circleFill(out, ax + 24.0f, ay + 17.0f, 15.0f, 0xFFDCE6F2);
        roundRect(out, ax + 12.0f, ay + 29.0f, 24.0f, 13.0f, 6.5f,
                  0xFFDCE6F2);
      }
    } else {
      // 登录按钮必须有底色和边框，深色背景上不能只画暗字。
      rect(out, x + w - 200.0f, y + 10.0f, 86.0f, 44.0f, 0xFF2E3B4E);
      rect(out, x + w - 200.0f, y + 10.0f, 86.0f, 2.0f, kAccent);
      text(out, x + w - 182.0f, y + 22.0f, 18.0f * mFontScale, kAccent,
           "登录");
    }
    glyphClose(out, x + w - 40.0f, y + 38.0f, 22.0f, kTextDim);
    // 右下角减号：缩成一个圆球
    rect(out, x + w - 60.0f, y + h - 30.0f, 30.0f, 4.0f,
         mPressedAction == 9 ? kAccent : kTextDim);

    // 登录页：默认扫码。扫码不依赖软键盘，稳定性最好。
    // 登录页独占整块面板，要铺一层不透明底色，避免下层列表与分类透出来。
    // 关键：这一层必须只在登录页画。原来它写在 if (mLoginPage) 之前，
    // 等于每帧都给大面板盖一层不透明底，把「面板透明度」设置完全作废。
    // 圆角是必须的，否则直角矩形会把 glassPanel 的四个圆角盖平。
    if (mLoginPage) {
      roundRect(out, x, y, w, h, (h < 84.0f) ? 14.0f : 20.0f, kSheet);
      rect(out, x, y, w, 2.0f, 0x4DFFFFFF);
      glyphClose(out, x + w - 40.0f, y + 38.0f, 22.0f, kTextDim);
    }
    if (mLoginPage != mLastDrawLogin || mSmsMode != mLastDrawSms ||
        mQrReady != mLastDrawQr) {
      mLastDrawLogin = mLoginPage;
      mLastDrawSms = mSmsMode;
      mLastDrawQr = mQrReady;
      __android_log_print(ANDROID_LOG_INFO, kTag,
                          "绘制: 登录页=%d 短信=%d 二维码=%d 形态=%d",
                          mLoginPage ? 1 : 0, mSmsMode ? 1 : 0,
                          mQrReady ? 1 : 0, static_cast<int>(mMode));
    }
    if (mLoginPage) {
      const float lx = x + 24.0f;
      float ly = y + 100.0f;
      // 点击二维码后全屏放大，任意处点击关闭。
      if (mQrZoom && mQrReady && mQrModules > 0 &&
          (!mQrCells.empty() || !mQrPixels.empty())) {
        const float big = std::min(w, h) - 90.0f;
        const float ox = x + (w - big) * 0.5f;
        const float oy = y + (h - big) * 0.5f - 20.0f;
        constexpr float bigPad = 36.0f;
        roundRect(out, x, y, w, h, (h < 84.0f) ? 14.0f : 20.0f, kSheet);
        rect(out, ox - bigPad, oy - bigPad, big + bigPad * 2.0f,
             big + bigPad * 2.0f, 0xFFFFFFFF);
        const float cell = big / static_cast<float>(mQrModules);
        for (int row = 0; row < mQrModules; ++row) {
          int runStart = -1;
          for (int col = 0; col <= mQrModules; ++col) {
            const bool dark = col < mQrModules && qrDarkAt(row, col);
            if (dark && runStart < 0) {
              runStart = col;
            } else if (!dark && runStart >= 0) {
              const int len = col - runStart;
              rect(out, ox + runStart * cell, oy + row * cell, len * cell + 0.1f,
                   cell + 0.1f, 0xFF000000);
              runStart = -1;
            }
          }
        }
        text(out, x + 24.0f, y + h - 44.0f, 17.0f * mFontScale, kTextDim,
             "点击任意处关闭");
        auto zoomSubmit = reinterpret_cast<SubmitDrawFn>(mSubmitDraw);
        lock.unlock();
        zoomSubmit(kModuleId, std::span<const Cmd>(out.data(), out.size()));
        return true;
      }
      if (mLoggedIn) {
        // 已登录：点头像进来看到账号页，而不是登录页。
        text(out, lx, ly, 20.0f * mFontScale, kTextMain, "账号");
        text(out, lx, ly + 44.0f, 16.0f * mFontScale, kTextDim,
             mUserLabel.empty() ? std::string("已登录")
                                : "已登录  " + mUserLabel);
        rect(out, lx, ly + 96.0f, 220.0f, 52.0f, kTrack);
        text(out, lx + 30.0f, ly + 106.0f, 17.0f * mFontScale, kTextMain,
             "退出登录");
        text(out, lx, ly + 180.0f, 15.0f * mFontScale, kAccent, "返回音乐库");
        auto accSubmit = reinterpret_cast<SubmitDrawFn>(mSubmitDraw);
        lock.unlock();
        accSubmit(kModuleId, std::span<const Cmd>(out.data(), out.size()));
        return true;
      }
      if (!mSmsMode) {
        text(out, lx, ly, 20.0f * mFontScale, kTextMain, "扫码登录");
        ly += 22.0f;
        const float qrSize = 280.0f;
        constexpr float qrPad = 24.0f;
        rect(out, lx, ly, qrSize + qrPad * 2.0f, qrSize + qrPad * 2.0f,
             0xFFFFFFFF);
        if (mQrReady && mQrModules > 0 &&
            (!mQrCells.empty() || (!mQrPixels.empty() && mQrPitch > 0.0f))) {
          const float cell = qrSize / static_cast<float>(mQrModules);
          for (int row = 0; row < mQrModules; ++row) {
            int runStart = -1;
            for (int col = 0; col <= mQrModules; ++col) {
              const bool dark = col < mQrModules && qrDarkAt(row, col);
              if (dark && runStart < 0) {
                runStart = col;
              } else if (!dark && runStart >= 0) {
                const int len = col - runStart;
                rect(out, lx + qrPad + runStart * cell,
                     ly + qrPad + row * cell, len * cell + 0.1f, cell + 0.1f,
                     0xFF000000);
                runStart = -1;
              }
            }
          }
        } else {
          text(out, lx + 20.0f, ly + 96.0f, 15.0f * mFontScale, kTextDim,
               "二维码准备中");
        }
        text(out, lx + qrSize + qrPad * 2.0f + 40.0f, ly + 26.0f, 17.0f * mFontScale,
             kTextMain, mQrMsg);
        text(out, lx + qrSize + qrPad * 2.0f + 40.0f, ly + 62.0f, 14.0f * mFontScale,
             kTextDim, "手机打开酷狗概念版扫一扫");
        ly += qrSize + qrPad * 2.0f + 16.0f;
        rect(out, lx, ly, 150.0f, 48.0f, kTrack);
        text(out, lx + 22.0f, ly + 10.0f, 16.0f * mFontScale, kTextMain,
             "刷新二维码");
        rect(out, lx + 170.0f, ly, 160.0f, 48.0f, kTrack);
        text(out, lx + 192.0f, ly + 10.0f, 16.0f * mFontScale, kTextMain,
             "手机号登录");
        text(out, lx, ly + 72.0f, 15.0f * mFontScale, kAccent, "返回音乐库");
    // 按钮按下只覆盖按钮自身，屏幕、空白和其他按钮都不变暗。
    if (nowMs() < mPressedUntil && mPressRectW > 0.0f) {
      // 按下时先向外弹出一圈淡光，再叠主色填充，形成弹一下的手感。
      const float left = static_cast<float>(mPressedUntil - nowMs()) / 140.0f;
      const float k = std::max(0.0f, std::min(1.0f, left));
      roundRect(out, mPressRectX - 3.0f * k, mPressRectY - 3.0f * k,
                mPressRectW + 6.0f * k, mPressRectH + 6.0f * k,
                kRadiusBtn + 3.0f * k, 0x228DBDA2);
      roundRect(out, mPressRectX, mPressRectY, mPressRectW, mPressRectH,
                kRadiusBtn, 0x2E8DBDA2);
      rect(out, mPressRectX + kRadiusBtn, mPressRectY,
           mPressRectW - kRadiusBtn * 2.0f, 2.0f, 0x998DBDA2);
      rect(out, mPressRectX + kRadiusBtn, mPressRectY + mPressRectH - 2.0f,
           mPressRectW - kRadiusBtn * 2.0f, 2.0f, 0x998DBDA2);
    }
    {
      auto loginSubmit = reinterpret_cast<SubmitDrawFn>(mSubmitDraw);
      // 登录页画完必须提交，否则界面会停在上一帧的列表。
        lock.unlock();
        loginSubmit(kModuleId, std::span<const Cmd>(out.data(), out.size()));
        return true;
      }
      }
      // 短信登录：面板内自带数字键盘，不弹系统键盘，避免游戏暂停遮挡界面。
      text(out, lx, ly, 20.0f * mFontScale, kTextMain, "手机号登录");
      text(out, lx + 170.0f, ly, 15.0f * mFontScale, kAccent, mSmsHint);
      ly += 34.0f;
      rect(out, lx, ly, 346.0f, 52.0f,
           mLoginField == 1 ? 0x40304A62 : kTrack);
      text(out, lx + 12.0f, ly + 10.0f, 18.0f * mFontScale,
           mLoginField == 1 ? kAccent : kTextMain,
           mLoginPhone.empty() ? std::string("手机号") : mLoginPhone);
      rect(out, lx + 360.0f, ly, 220.0f, 52.0f,
           mLoginField == 2 ? 0x40304A62 : kTrack);
      text(out, lx + 372.0f, ly + 10.0f, 18.0f * mFontScale,
           mLoginField == 2 ? kAccent : kTextMain,
           mLoginCode.empty() ? std::string("验证码") : mLoginCode);
      ly += 64.0f;
      for (int i = 0; i < 12; ++i) {
        const int col = i % 3;
        const int row = i / 3;
        const float kx = lx + col * 118.0f;
        const float ky = ly + row * 64.0f;
        rect(out, kx, ky, 110.0f, 56.0f, kFieldActive);
        text(out, kx + 42.0f, ky + 10.0f, 18.0f * mFontScale, kTextMain,
             digitLabel(i));
      }
      const float ax = lx + 380.0f;
      std::string sendLabel = "发送验证码";
      std::uint32_t sendColor = kAccent;
      const long long sendNow = nowMs();
      if (sendNow < mSmsSendUntil) {
        const long long left = (mSmsSendUntil - sendNow + 999) / 1000;
        if (mSmsSendState == 1) {
          sendLabel = "发送中 " + std::to_string(left) + "s";
        } else if (mSmsSendState == 3) {
          sendLabel = "重发 " + std::to_string(left) + "s";
        } else {
          sendLabel = "已发送 " + std::to_string(left) + "s";
        }
        sendColor = 0xFF233238;
      }
      rect(out, ax, ly, 220.0f, 56.0f, sendColor);
      text(out, ax + 26.0f, ly + 10.0f, 17.0f * mFontScale, 0xFFFFFFFF,
           sendLabel);
      rect(out, ax, ly + 68.0f, 220.0f, 56.0f, kTrack);
      text(out, ax + 82.0f, ly + 78.0f, 17.0f * mFontScale, kTextMain, "登录");
      rect(out, ax, ly + 136.0f, 220.0f, 48.0f, kTrack);
      text(out, ax + 62.0f, ly + 146.0f, 16.0f * mFontScale, kTextMain,
           "切换输入");
      text(out, ax, ly + 200.0f, 15.0f * mFontScale, kAccent, "返回扫码");
      text(out, ax, ly + 234.0f, 15.0f * mFontScale, kAccent, "返回音乐库");
    // 按钮按下只覆盖按钮自身，屏幕、空白和其他按钮都不变暗。
    if (nowMs() < mPressedUntil && mPressRectW > 0.0f) {
      // 按下时先向外弹出一圈淡光，再叠主色填充，形成弹一下的手感。
      const float left = static_cast<float>(mPressedUntil - nowMs()) / 140.0f;
      const float k = std::max(0.0f, std::min(1.0f, left));
      roundRect(out, mPressRectX - 3.0f * k, mPressRectY - 3.0f * k,
                mPressRectW + 6.0f * k, mPressRectH + 6.0f * k,
                kRadiusBtn + 3.0f * k, 0x228DBDA2);
      roundRect(out, mPressRectX, mPressRectY, mPressRectW, mPressRectH,
                kRadiusBtn, 0x2E8DBDA2);
      rect(out, mPressRectX + kRadiusBtn, mPressRectY,
           mPressRectW - kRadiusBtn * 2.0f, 2.0f, 0x998DBDA2);
      rect(out, mPressRectX + kRadiusBtn, mPressRectY + mPressRectH - 2.0f,
           mPressRectW - kRadiusBtn * 2.0f, 2.0f, 0x998DBDA2);
    }
    {
      auto loginSubmit = reinterpret_cast<SubmitDrawFn>(mSubmitDraw);
      // 登录页画完必须提交，否则界面会停在上一帧的列表。
        lock.unlock();
        loginSubmit(kModuleId, std::span<const Cmd>(out.data(), out.size()));
        return true;
      }
    }

    // 分页切换动效：内容区在 200ms 内从旧页方向滑入。
    float pageSlide = 0.0f;
    if (mTabAnimStartMs > 0) {
      const float t01 = std::min(
          1.0f, static_cast<float>(nowMs() - mTabAnimStartMs) / 200.0f);
      if (t01 >= 1.0f) {
        mTabAnimStartMs = 0;
      } else {
        const float inv = 1.0f - t01;
        pageSlide = inv * inv * inv * 26.0f;
      }
    }
    float recSlide = 0.0f;
    if (mRecTabAnimStartMs > 0) {
      const float t01 = std::min(
          1.0f, static_cast<float>(nowMs() - mRecTabAnimStartMs) / 200.0f);
      if (t01 >= 1.0f) {
        mRecTabAnimStartMs = 0;
      } else {
        const float inv = 1.0f - t01;
        recSlide = inv * inv * inv * 34.0f;
      }
    }
    const float listShift = pageSlide + recSlide;
    // 列表：上方留给输入框，下方留给分页栏
    const float listX = x + 24.0f + listShift;
    // 列表区上沿统一由 listTopFor() 决定，绘制与触摸共用同一套坐标。
    const float listY = listTopFor();
    const float listW = 540.0f;
    // 列表只在搜索页和推荐页绘制。「我的」页没有列表，
    // 原来无条件绘制，导致切到我的页时歌单和偏好按钮叠在一起。
    const bool drawList = (mTab != Tab::Mine) || (mQueueView == 1);
    // 行高按字号缩放：原来固定 54px，字号放大到 1.35 倍时
    // 标题(24px)+歌手(18px) 会挤在一起甚至重叠。
    const float rowH = std::max(54.0f, 40.0f * mFontScale);
    const float listH = listHeightFor();
    // 行数向上取整，让最后一行能画出来，背景高度也对齐到整数行，
    // 否则最后一行下方会固定露出一条空白（原来是 588-540=48px）。
    const int maxRows = drawList ? static_cast<int>(listH / rowH) : 0;
    const float listBgH = std::min(listH, maxRows * rowH);
    if (drawList) {
    rect(out, listX, listY, listW, listBgH, 0x1E000000);
    rect(out, listX, listY, listW, 1.0f, 0x18FFFFFF);
    rect(out, listX, listY, 1.0f, listBgH, 0x0EFFFFFF);
    }
    // 平滑滚动：mListScroll 是浮点行偏移，必须用小数部分做像素级位移。
    // 旧实现直接 (int)mListScroll 截断成整数行，每滚一行整块跳一个行高，
    // 表现就是「一弹一弹」。
    int startRow = static_cast<int>(mListScroll);
    if (startRow < 0) {
      startRow = 0;
    }
    const float rowShift = (mListScroll - static_cast<float>(startRow)) * rowH;
    // 多迭代一行：小数偏移会让底部空出 rowShift 像素，需要下一行补上。
    // 上界用 drawList 兜底：maxRows 在「我的」页是 0，
    // 直接写 k <= maxRows 会多跑一轮，把列表画到不该画的页面上。
    for (int k = 0; k <= (drawList ? maxRows : -1); ++k) {
      const int i = startRow + k;
      if (i < 0) {
        continue;
      }
      // 行数按当前页取：各页的数据源不同，不能一律用 mTrackCount。
      std::size_t rowLimit = mTrackCount;
      if (mQueueView == 1) {
        rowLimit = mQueueTracks.size();  // 播放队列侧栏
      } else if (mTab == Tab::Favorites && mFavView == 0) {
        rowLimit = mFavPlaylists.size();
      } else if (mTab == Tab::Search && mArtistView == 1 &&
                 mArtistTab == 1 && mAlbumView == 0) {
        rowLimit = mArtistAlbums.size();
      } else if (mTab == Tab::Search && mArtistView == 0 && mAlbumView == 0 &&
                 mSearchKind == 1) {
        rowLimit = mSearchAlbums.size();
      } else if (mTab == Tab::Search && mArtistView == 0 && mAlbumView == 0 &&
                 mSearchKind == 2) {
        rowLimit = mSearchArtists.size();
      }
      if (static_cast<std::size_t>(i) >= rowLimit) {
        break;
      }
      const float ry = listY + static_cast<float>(k) * rowH - rowShift;
      // 宿主没有裁剪能力：整行落在列表区外就整行不画，
      // 否则会压到输入框、分页栏或右侧播放区（穿模）。
      if (ry >= listY + listBgH - 0.5f || ry + rowH <= listY + 0.5f) {
        continue;
      }
      // 歌手页「专辑」子标签：画专辑卡片。
      // 专辑歌曲层下必须让路，否则卡片会和歌曲列表叠在一起（穿模）。
      if (mTab == Tab::Search && mArtistView == 1 && mArtistTab == 1 &&
          mAlbumView == 0) {
        const int ai = startRow + k;
        if (ai < 0 ||
            static_cast<std::size_t>(ai) >= mArtistAlbums.size()) {
          continue;
        }
        const bool whole2 =
            (ry >= listY - 0.5f) && (ry + rowH <= listY + listBgH + 0.5f);
        if (!whole2) {
          continue;
        }
        const AlbumInfo &al = mArtistAlbums[ai];
        roundRect(out, listX + 6.0f, ry + 4.0f, listW - 12.0f, rowH - 8.0f,
                  kRadiusBtn, 0x22000000);
        rect(out, listX + 16.0f, ry + 10.0f, 3.0f, rowH - 20.0f, kAccent);
        marqueeFit(out, listX + 30.0f, ry + 6.0f * mFontScale,
                   17.0f * mFontScale, listW - 120.0f, al.name, kTextMain,
                   nowMs() + ai * 350);
        text(out, listX + 30.0f, ry + rowH - 16.0f * mFontScale,
             13.0f * mFontScale, kTextDim, tr("专辑", "Album"));
        triFill(out, listX + listW - 34.0f, ry + rowH * 0.5f - 7.0f,
                listX + listW - 34.0f, ry + rowH * 0.5f + 7.0f,
                listX + listW - 24.0f, ry + rowH * 0.5f, kTextDim);
        continue;
      }
      // 搜索页「专辑」分类：同样画专辑卡片，数据来自 mSearchAlbums。
      if (mTab == Tab::Search && mArtistView == 0 && mAlbumView == 0 &&
          mSearchKind == 1) {
        const int ai = startRow + k;
        if (ai < 0 ||
            static_cast<std::size_t>(ai) >= mSearchAlbums.size()) {
          continue;
        }
        const bool whole3 =
            (ry >= listY - 0.5f) && (ry + rowH <= listY + listBgH + 0.5f);
        if (!whole3) {
          continue;
        }
        const AlbumInfo &al = mSearchAlbums[ai];
        roundRect(out, listX + 6.0f, ry + 4.0f, listW - 12.0f, rowH - 8.0f,
                  kRadiusBtn, 0x22000000);
        rect(out, listX + 16.0f, ry + 10.0f, 3.0f, rowH - 20.0f, 0x668DBDA2);
        marqueeFit(out, listX + 30.0f, ry + 5.0f * mFontScale,
                   17.0f * mFontScale, listW - 120.0f, al.name, kTextMain,
                   nowMs() + ai * 350);
        if (!al.artist.empty()) {
          marqueeFit(out, listX + 30.0f, ry + rowH - 17.0f * mFontScale,
                     13.0f * mFontScale, listW - 120.0f, al.artist, kTextDim,
                     nowMs() + ai * 350);
        }
        triFill(out, listX + listW - 34.0f, ry + rowH * 0.5f - 7.0f,
                listX + listW - 34.0f, ry + rowH * 0.5f + 7.0f,
                listX + listW - 24.0f, ry + rowH * 0.5f, kTextDim);
        continue;
      }
      // 搜索页「歌手」分类：画歌手卡片。
      if (mTab == Tab::Search && mArtistView == 0 && mAlbumView == 0 &&
          mSearchKind == 2) {
        const int ai = startRow + k;
        if (ai < 0 ||
            static_cast<std::size_t>(ai) >= mSearchArtists.size()) {
          continue;
        }
        const bool whole4 =
            (ry >= listY - 0.5f) && (ry + rowH <= listY + listBgH + 0.5f);
        if (!whole4) {
          continue;
        }
        const ArtistInfo &ar = mSearchArtists[ai];
        roundRect(out, listX + 6.0f, ry + 4.0f, listW - 12.0f, rowH - 8.0f,
                  kRadiusBtn, 0x22000000);
        // 头像位：没做图片加载，用圆形色块 + 名字首字之外的纯色块，
        // 避免又出现「显示一个莫名其妙的字」那种情况。
        circleFill(out, listX + 40.0f, ry + rowH * 0.5f, rowH - 22.0f,
                   0xFF3C5170);
        marqueeFit(out, listX + 72.0f, ry + 6.0f * mFontScale,
                   17.0f * mFontScale, listW - 140.0f, ar.name, kTextMain,
                   nowMs() + ai * 350);
        if (ar.songCount > 0) {
          text(out, listX + 72.0f, ry + rowH - 16.0f * mFontScale,
               13.0f * mFontScale, kTextDim,
               std::to_string(ar.songCount) + " 首");
        }
        triFill(out, listX + listW - 34.0f, ry + rowH * 0.5f - 7.0f,
                listX + listW - 34.0f, ry + rowH * 0.5f + 7.0f,
                listX + listW - 24.0f, ry + rowH * 0.5f, kTextDim);
        continue;
      }
      // 收藏页第一层：这里画的是「歌单卡片」而不是歌曲行。
      // 数据来自 mFavPlaylists，行数与 mTrackCount 无关，所以单独成块。
      if (mTab == Tab::Favorites && mFavView == 0) {
        const int pi = startRow + k;
        if (pi < 0 || static_cast<std::size_t>(pi) >= mFavPlaylists.size()) {
          continue;
        }
        const bool whole =
            (ry >= listY - 0.5f) && (ry + rowH <= listY + listBgH + 0.5f);
        // 分隔线照旧，卡片本身有底，所以只在未整行时跳过
        if (!whole) {
          continue;
        }
        const PlaylistInfo &pl = mFavPlaylists[pi];
        // 卡片：整行玻璃底 + 左侧方形占位（歌单封面还没做，先用主色块）。
        roundRect(out, listX + 6.0f, ry + 4.0f, listW - 12.0f, rowH - 8.0f,
                  kRadiusBtn, 0x22000000);
        rect(out, listX + 16.0f, ry + 10.0f, 3.0f, rowH - 20.0f,
             pl.isDefault ? kAccent : 0x668DBDA2);
        // 歌单名
        marqueeFit(out, listX + 30.0f, ry + 6.0f * mFontScale,
                   17.0f * mFontScale, listW - 120.0f, pl.name, kTextMain,
                   nowMs() + pi * 350);
        // 曲目数 + 类型
        const std::string meta =
            std::to_string(pl.count) + " 首" +
            (pl.isDefault ? std::string("  · 默认收藏") : std::string());
        text(out, listX + 30.0f, ry + rowH - 16.0f * mFontScale,
             13.0f * mFontScale, kTextDim, meta);
        // 右侧进入箭头
        triFill(out, listX + listW - 34.0f, ry + rowH * 0.5f - 7.0f,
                listX + listW - 34.0f, ry + rowH * 0.5f + 7.0f,
                listX + listW - 24.0f, ry + rowH * 0.5f, kTextDim);
        continue;
      }
      // 文字无法裁剪，只有整行都在列表区内才画；
      // 半行在外时只画能完整落下的分隔线，避免文字溢出。
      const bool rowWhole =
          (ry >= listY - 0.5f) && (ry + rowH <= listY + listBgH + 0.5f);
      const bool active = (static_cast<std::size_t>(i) == mCurrentIndex);
      float rowAlpha = 1.0f;
      if (active) {
        if (mRowAnimIndex != mCurrentIndex) {
          mRowAnimIndex = mCurrentIndex;
          mRowAnimStartMs = nowMs();
        }
        const float t01 = std::min(
            1.0f, static_cast<float>(nowMs() - mRowAnimStartMs) / 200.0f);
        rowAlpha = 0.35f + 0.65f * t01;
      }
      // 未选中行之间用极淡分隔线区分，选中行给整块玻璃底与左侧亮条。
      if (!active) {
        const float sepY = ry + rowH - 1.0f;
        if (sepY >= listY && sepY + 1.0f <= listY + listBgH) {
          rect(out, listX + 14.0f, sepY, listW - 28.0f, 1.0f, 0x0CFFFFFF);
        }
      } else if (rowWhole) {
        // 半行在外时不画选中底：圆角矩形被截断比缺一块更难看。
        // 选中项：实色底 + 圆角，左侧一条主色竖条。
        // 原来靠 alpha 做淡底，但宿主对矩形忽略 alpha，
        // 画出来是实心深墨绿，深色文字压上去看不见。改用实色变量。
        // 淡入：底色从面板底过渡到选中色，用实色插值代替 alpha。
        const std::uint32_t rowBg =
            blendColor(kPanelBase, kRowActiveSolid, rowAlpha);
        roundRect(out, listX + 6.0f, ry + 4.0f, listW - 12.0f, rowH - 8.0f,
                  kRadiusBtn, rowBg);
        rect(out, listX + 6.0f, ry + 10.0f, 3.0f, rowH - 20.0f, kAccent);
      }
      if (!rowWhole) {
        continue;
      }
      // 队列视图的数据来自 mQueueTracks，其余视图来自 mTrackTitles。
      std::string title;
      std::string rowArtist;
      if (mQueueView == 1) {
        if (static_cast<std::size_t>(i) < mQueueTracks.size()) {
          title = mQueueTracks[i].title;
          rowArtist = mQueueTracks[i].artist;
        }
      } else {
        if (static_cast<std::size_t>(i) < mTrackTitles.size()) {
          title = mTrackTitles[i];
        }
        if (static_cast<std::size_t>(i) < mTrackArtists.size()) {
          rowArtist = mTrackArtists[i];
        }
      }
      if (title.empty()) {
        title = "曲目";
      }
      // 每行都画歌手，行高不再因选中而增减；标题上移 2 像素给副标题留位。
      // 纵向位置按字号算：字号 1.6 倍时 6/31 这两个固定偏移会挤在一起。
      const float titleY = ry + 5.0f * mFontScale;
      const float artistY = ry + rowH - 15.0f * mFontScale;
      // 右侧留出 34px 给「下一首播放」图标，文字宽度相应收窄，
      // 否则长标题会压到图标上。
      const float textW =
          listShowsTracksLocked() ? (listW - 118.0f) : (listW - 44.0f);
      marqueeFit(out, listX + 22.0f, titleY, 18.0f * mFontScale, textW, title,
                 kTextMain, nowMs() + i * 350);
      if (!rowArtist.empty()) {
        // 原来用裸 text()，长歌手名会直接溢出列表区、盖到右侧播放区。
        // 改成 marqueeFit：放得下原样显示，放不下自动滚动，绝不越界。
        marqueeFit(out, listX + 22.0f, artistY, 13.0f * mFontScale, textW,
                   rowArtist, kTextDim, nowMs() + i * 350);
      }
      // 行右端「下一首播放」：一个加号，不写文字。
      // 加号比「三角+竖线」更直观地表达「加入队列」。
      // 只在真·歌曲行画；专辑/歌手/歌单卡片列表没有这个键。
      if (listShowsTracksLocked()) {
        const float ncX = listX + listW - 30.0f;   // 下一首播放（加号）
        const float favX = listX + listW - 70.0f;  // 收藏（心形）
        const float ncY = ry + rowH * 0.5f;
        // 收藏状态用实心/空心区分：实心 = 已在「我的收藏」里。
        const bool fav = rowFavoritedLocked(static_cast<std::size_t>(i));
        glyphHeart(out, favX, ncY, 17.0f, fav, fav ? kAccent : kTextDim);
        // 十字两条：横 16x3，竖 3x16，中心对齐。
        rect(out, ncX - 8.0f, ncY - 1.5f, 16.0f, 3.0f, kTextDim);
        rect(out, ncX - 1.5f, ncY - 8.0f, 3.0f, 16.0f, kTextDim);
      }
    }

    // 搜索页刚打开时还没有结果，列表区是一片空白，
    // 看起来像「没加载出来」。给一句提示，说明需要先搜。
    if (drawList && mTab == Tab::Search && mTrackCount == 0) {
      const std::string hint = tr("输入关键词搜索歌曲", "Search songs");
      const float hintSize = 16.0f * mFontScale;
      const float hintW = estimateTextWidth(hint, hintSize);
      text(out, listX + (listW - hintW) * 0.5f,
           listY + listBgH * 0.5f - hintSize * 0.5f, hintSize, kTextDim,
           hint);
    }

    // 竖向滚动条：整条可拖动，滑块长度表示可见比例。
    {
      const float trackX = listX + listW - 10.0f;
      const float trackW = 6.0f;
      const float trackH = listH - 4.0f;
      rect(out, trackX, listY + 2.0f, trackW, trackH, 0x33000000);
      const int total = static_cast<int>(mTrackCount);
      const int visible = static_cast<int>(listH / rowH);
      if (total > visible && visible > 0) {
        const float maxScroll = static_cast<float>(total - visible);
        const float minThumb = 36.0f;
        float thumbH = trackH * static_cast<float>(visible) /
                       static_cast<float>(total);
        if (thumbH < minThumb) {
          thumbH = minThumb;
        }
        const float travel = trackH - thumbH;
        float ratio = maxScroll > 0.0f ? mListScroll / maxScroll : 0.0f;
        ratio = std::min(1.0f, std::max(0.0f, ratio));
        rect(out, trackX - 1.0f, listY + 2.0f + travel * ratio, trackW + 2.0f,
             thumbH, kAccent);
      }
    }

    // 专辑歌曲层：这一层没有搜索框，位置改画「返回 + 专辑名」。
    // 不画搜索框是刻意的 —— 留着会让人以为还能在这里搜，
    // 而实际搜索会跳出当前专辑。
    const float inputY = y + 52.0f;
    if (mTab == Tab::Search && mAlbumView == 1 && mQueueView == 0) {
      roundRect(out, listX, inputY, 96.0f, 36.0f, kRadiusBtn, 0x33000000);
      text(out, listX + 16.0f, inputY + 10.0f, 15.0f * mFontScale, kTextMain,
           tr("返回", "Back"));
      marqueeFit(out, listX + 110.0f, inputY + 10.0f, 16.0f * mFontScale,
                 listW - 122.0f, mAlbumOpenName, kTextDim, nowMs());
    }
    // 搜索输入框与确定键只在搜索页、且不在专辑层/队列侧栏时绘制。
    if (mTab == Tab::Search && mAlbumView == 0 && mQueueView == 0) {
    // 输入槽做成内凹：上面一条暗线、下面一条亮线，配上不透明的深色底。
    rect(out, listX, inputY, listW - 74.0f, 40.0f,
         mInputActive ? kFieldActive : kFieldBg);
    rect(out, listX, inputY, listW - 74.0f, 1.0f,
         mInputActive ? 0x404FE3C1 : 0x40000000);
    rect(out, listX, inputY + 39.0f, listW - 74.0f, 1.0f, 0x1AFFFFFF);
    if (mInputActive && mInputText.empty()) {
      rect(out, listX + 14.0f, inputY + 9.0f, 2.0f, 22.0f, kAccent);
    }
    text(out, listX + 14.0f, inputY + 10.0f, 16.0f * mFontScale,
         mInputText.empty() ? kTextDim : kTextMain,
         mInputText.empty() ? std::string(tr("搜索歌曲", "Search")) : mInputText);

    // 原来用深底 0xFF233238 + kTextMain，日间 kTextMain 是深色，
    // 深字压深底完全看不见，看上去就是个纯黑方块。
    // 改为强调色实底 + 与底色反差足够的文字。
    {
      const bool okPressed = (mPressedAction == 10);
      rect(out, listX + listW - 66.0f, inputY, 66.0f, 40.0f,
           okPressed ? kAccentDeep : kAccent);
      // 强调色偏深，用白字；日间强调色加深过，白字对比度够。
      const std::uint32_t okTextColor = 0xFFFFFFFF;
      const float okW = estimateTextWidth(tr("确定", "OK"), 16.0f * mFontScale);
      text(out, listX + listW - 66.0f + (66.0f - okW) * 0.5f,
           inputY + (40.0f - 16.0f * mFontScale) * 0.5f, 16.0f * mFontScale,
           okTextColor, tr("确定", "OK"));
    }
    }

    // 搜索页三分类标签：单曲 / 专辑 / 歌手。默认单曲。
    // 搜索框是空的时候不显示——什么都没搜，摆一排分类没有意义，
    // 而且会把列表区往下挤，看起来像「面板自己展开了一堆东西」。
    if (mTab == Tab::Search && mArtistView == 0 && mAlbumView == 0 &&
        mQueueView == 0 && searchHasQuery()) {
      const char *kinds[3] = {tr("单曲", "Songs"), tr("专辑", "Albums"),
                              tr("歌手", "Artists")};
      const float kindY = y + 96.0f;
      const float kindW = 540.0f / 3.0f;
      for (int ki = 0; ki < 3; ++ki) {
        const float kx = listX + ki * kindW;
        const bool active = (mSearchKind == ki);
        if (active) {
          roundRect(out, kx + 4.0f, kindY, kindW - 8.0f, 30.0f, kRadiusBtn,
                    0x4206644B);
          rect(out, kx + 12.0f, kindY + 30.0f, kindW - 24.0f, 2.0f, kAccent);
        }
        const float kw = estimateTextWidth(kinds[ki], 16.0f * mFontScale);
        text(out, kx + (kindW - kw) * 0.5f, kindY + 6.0f, 16.0f * mFontScale,
             active ? kTextMain : kTextDim, kinds[ki]);
      }
    }

    // 歌手页页头。左侧返回键用和收藏页第二层相同的位置语言。
    if (mArtistView == 1) {
      const float backX = listX;
      const float backY = y + 70.0f;
      rect(out, backX, backY, 96.0f, 28.0f, 0x33FFFFFF);
      triFill(out, backX + 12.0f, backY + 9.0f, backX + 12.0f, backY + 19.0f,
              backX + 6.0f, backY + 14.0f, kTextMain);
      text(out, backX + 22.0f, backY + 5.0f, 15.0f * mFontScale, kTextMain,
           tr("返回", "Back"));
      // 歌手名
      marqueeFit(out, backX + 108.0f, backY + 4.0f, 17.0f * mFontScale,
                 260.0f,
                 mArtistInfo.name.empty() ? std::string("歌手")
                                          : mArtistInfo.name,
                 kAccent, nowMs());
      // 子标签：单曲 / 专辑
      const char *tabs2[2] = {tr("单曲", "Songs"), tr("专辑", "Albums")};
      const float atY = y + 100.0f;
      for (int ai = 0; ai < 2; ++ai) {
        const float ax = listX + ai * 110.0f;
        const bool act = (mArtistTab == ai);
        if (act) {
          roundRect(out, ax, atY, 104.0f, 30.0f, kRadiusBtn, 0x4206644B);
          rect(out, ax + 12.0f, atY + 30.0f, 80.0f, 2.0f, kAccent);
        }
        text(out, ax + 16.0f, atY + 6.0f, 16.0f * mFontScale,
             act ? kTextMain : kTextDim, tabs2[ai]);
      }
      // 排序切换：右侧一个胶囊，点一下在「热度/时间」之间切
      {
        const float sortX = listX + 240.0f;
        roundRect(out, sortX, atY, 120.0f, 30.0f, 15.0f, 0x2A000000);
        text(out, sortX + 14.0f, atY + 6.0f, 15.0f * mFontScale, kTextMain,
             mArtistSortByTime ? tr("按时间", "By time")
                               : tr("按热度", "By hot"));
      }
    }

    // 收藏页第二层的返回按钮。放左半边：
    // 位置在副标题下方、列表区上沿之上，不会和标题、头像、关闭键冲突。
    // 只在第二层（歌曲列表）出现；第一层是根层级，没有上级可返回。
    if (mTab == Tab::Favorites && mFavView == 1 && mQueueView == 0) {
      const float backX = listX;
      const float backY = y + 70.0f;
      rect(out, backX, backY, 96.0f, 28.0f, 0x33FFFFFF);
      triFill(out, backX + 12.0f, backY + 9.0f, backX + 12.0f, backY + 19.0f,
              backX + 6.0f, backY + 14.0f, kTextMain);
      text(out, backX + 22.0f, backY + 5.0f, 15.0f * mFontScale, kTextMain,
           tr("返回", "Back"));
      // 当前歌单名，跟在返回键右边，让玩家知道自己在哪一层。
      marqueeFit(out, backX + 108.0f, backY + 5.0f, 15.0f * mFontScale,
                 360.0f, mFavOpenName, kAccent, nowMs());
    }

    // 分页栏：4 项。显示顺序 搜索、推荐、收藏、我的。
    const char *tabNames[4] = {tr("搜索", "Search"), tr("推荐", "Recommend"),
                               tr("收藏", "Favorites"), tr("我的", "Mine")};
    // 显示位次 -> Tab 枚举值。收藏在枚举里是 3，但显示排第三位。
    const int tabEnums[4] = {static_cast<int>(Tab::Search),
                             static_cast<int>(Tab::Recommend),
                             static_cast<int>(Tab::Favorites),
                             static_cast<int>(Tab::Mine)};
    const float tabY = y + h - 54.0f;
    if (mQueueView == 0) {  // 队列视图独占左半边，这排入口整体不画
    const float tabW = listW / 4.0f;
    rect(out, listX, tabY - 4.0f, listW, 52.0f, 0x1C000000);
    rect(out, listX, tabY - 4.0f, listW, 1.0f, 0x16FFFFFF);
    for (int ti = 0; ti < 4; ++ti) {
      const float tx = listX + ti * tabW;
      const bool activeTab = (static_cast<int>(mTab) == tabEnums[ti]);
      if (activeTab) {
        roundRect(out, tx + 4.0f, tabY, tabW - 8.0f, 38.0f, kRadiusBtn,
                  0x4206644B);
        rect(out, tx + 10.0f, tabY + 40.0f, tabW - 20.0f, 3.0f, kAccent);
      }
      // 4 列比 3 列窄，文字改成居中，靠 26px 固定左边距会被截断。
      const float labelW =
          estimateTextWidth(tabNames[ti], 17.0f * mFontScale);
      text(out, tx + (tabW - labelW) * 0.5f, tabY + 10.0f,
           17.0f * mFontScale, activeTab ? kTextMain : kTextDim, tabNames[ti]);
    }
    }

    if (drawList) {  // 播放区与歌词只在搜索/推荐页绘制
    // 右侧播放区
    const float px = x + 620.0f;
    const float infoX = px + ((mShowCover && mCoverReady) ? 250.0f : 0.0f);
    if (mShowCover && mCoverReady) {
      float coverOff = 0.0f;
      if (mTrackAnimStartMs > 0) {
        const float p01 = std::min(
            1.0f, static_cast<float>(nowMs() - mTrackAnimStartMs) / 220.0f);
        if (p01 >= 1.0f) {
          mTrackAnimStartMs = 0;
        } else {
          const float inv = 1.0f - p01;
          coverOff = inv * inv * inv * 16.0f;
        }
      }
      image(out, px, y + 90.0f + coverOff, 220.0f, 220.0f,
            coverId().c_str());
      // 封面不加任何装饰块。
      // 原来在四个角各画了两条 12x2 的白色半透明块，
      // 在半透明面板上会叠成明显的脏边，玩家反馈"封面有描边"。
      const float expTitleSize = 30.0f * mFontScale;
      marqueeFit(out, infoX, y + 100.0f, expTitleSize,
                 x + w - infoX - 10.0f, mTitle, kTextMain, nowMs());
      text(out, infoX, y + 144.0f, 19.0f * mFontScale, kTextDim, mArtist);
    } else {
      const float expTitleSize2 = 32.0f * mFontScale;
      marqueeFit(out, infoX, y + 96.0f, expTitleSize2,
                 x + w - infoX - 10.0f, mTitle, kTextMain, nowMs());
      text(out, infoX, y + 142.0f, 20.0f * mFontScale, kTextDim, mArtist);
    }

    if (mShowProgress) {
      const float barX = infoX;
      const float barW = x + w - infoX - 30.0f;
      const float barY = y + 228.0f;
      rect(out, barX, barY, barW, 8.0f, kTrack);
      if (mDurationMs > 0) {
        if (mPlayableMs > 0 && mPlayableMs < mDurationMs) {
          const float playRatio = static_cast<float>(mPlayableMs) /
                                  static_cast<float>(mDurationMs);
          rect(out, barX, barY, barW * playRatio, 8.0f, 0xFF2F6E66);
        }
        const float ratio = std::min(
            1.0f, std::max(0.0f, static_cast<float>(mPositionMs) /
                                    static_cast<float>(mDurationMs)));
        rect(out, barX, barY, barW * ratio, 8.0f, kAccent);
        const float knobX = barX + barW * ratio;
        circleFill(out, knobX, barY + 4.0f, 20.0f, 0x334FE3C1);
        circleFill(out, knobX, barY + 4.0f, 12.0f, kTextMain);
      }
      text(out, barX, y + 256.0f, 14.0f * mFontScale, kTextDim,
           fmtTime(mPositionMs) + " / " + fmtTime(mDurationMs));
    }

    // 播放模式键：去掉圆底、图标放大到 30。
    // 纵向中心保持 y+318，和三大件在同一条水平线上 —— 上一版上移到 y+306
    // 看起来是歪的，已按反馈调回。
    {
      const int mode =
          mEngine != nullptr ? static_cast<int>(mEngine->playMode()) : 0;
      // 三大键右边依次是：播放方式 → 收藏（用户指定的顺序）。
      const float mcx = infoX + 230.0f;
      const float mcy = y + 318.0f;
      // 顺序播放是「默认态」，用主文字色；随机和单曲循环用强调色，
      // 这样一眼能看出「现在的模式和默认不一样」。
      const std::uint32_t modeColor = (mode == 0) ? kTextMain : kAccent;
      if (mode == 1) {
        glyphModeShuffle(out, mcx, mcy, 30.0f, modeColor);
      } else if (mode == 2) {
        glyphModeRepeatOne(out, mcx, mcy, 30.0f, modeColor);
      } else {
        glyphModeSequential(out, mcx, mcy, 30.0f, modeColor);
      }
    }
    // 播放队列按钮：三条杠，位于爱心右侧一格（infoX+390）。
    // 打开时整块变成强调色，一眼能看出侧栏正在显示队列。
    {
      const float qx = infoX + 390.0f;
      const float qy = y + 318.0f;
      const std::uint32_t qc = (mQueueView == 1) ? kAccent : kTextMain;
      for (int li = -1; li <= 1; ++li) {
        rect(out, qx - 13.0f, qy + static_cast<float>(li) * 8.0f - 1.5f,
             26.0f, 3.0f, qc);
      }
    }
    iconPrev(out, infoX + 30.0f, y + 318.0f, 34.0f, kTextMain);
    // 播放/暂停：实心强调色圆 + 深色图标，和中面板保持同一套语言。
    // 上一首/下一首仍是纯图标，不加底。
    {
      const float pcy = y + 318.0f;
      const float pcx = infoX + 95.0f;
      const float r = 34.0f;
      circleFill(out, pcx, pcy, r * 2.0f, kAccent);
      if (mPaused || !mPlaying || !mHasTrack) {
        glyphPlay(out, pcx + r * 0.06f, pcy, 38.0f, 0xFF12151A);
      } else {
        glyphPause(out, pcx, pcy, 33.0f, 0xFF12151A);
      }
    }
    iconNext(out, infoX + 160.0f, y + 318.0f, 34.0f, kTextMain);

    // 收藏爱心：三大件右侧 62px，与左侧模式键对称。
    {
      // 收藏在播放方式右边一格。
      const float hcx = infoX + 310.0f;
      const float hcy = y + 318.0f;
      circleFill(out, hcx, hcy, 44.0f, 0x1A000000);
      glyphHeart(out, hcx, hcy, 22.0f, mTrackFavorited,
                 mTrackFavorited ? kAccent : kTextDim);
    }

    if (!mLyrics.empty()) {
      const std::size_t index =
          std::min(mCurrentLyric, mLyrics.size() - 1);
      // 滚动歌词：当前行固定在中间基准线，整体随当前行平滑上移。
      const float lineH = 34.0f;
      const float baseY = y + 366.0f;
      const float midY = baseY + lineH * 2.0f;
      const LyricLine &curLine = mLyrics[index];
      const long long lineStart = curLine.timeMs;
      const long long nextStart =
          (index + 1 < mLyrics.size()) ? mLyrics[index + 1].timeMs : 0;
      long long lineEnd = 0;
      if (curLine.durationMs > 0) {
        lineEnd = lineStart + curLine.durationMs;
      } else {
        const long long chars =
            static_cast<long long>(utf8CharCount(curLine.text));
        lineEnd = lineStart + chars * 160 + 700;
        lineEnd = std::min(5000LL, std::max(1200LL, lineEnd - lineStart)) +
                  lineStart;
      }
      if (nextStart > lineStart &&
          (curLine.durationMs <= 0 || nextStart < lineEnd)) {
        lineEnd = nextStart;
      }
      const long long lyricPos = mPositionMs - mLyricDelay.load();
      // 切句补间已移除：原实现把整块歌词上推最多一个行高（lineH），
      // 当前行会被推到上一行的位置。当相邻两句文本相同时，视觉上就是
      // 「同一句出现两条」。改为不动位置，只让当前行做极短的亮度过渡。
      // 平滑滚动：歌词位置用「当前已经走到的行号」表示，
      // 切句时从已走到的位置缓动到新行号，不会先瞬移再回弹。
      const float lineTarget = static_cast<float>(index);
      if (mSmoothLyricIndex < 0) {
        // 首次：直接对齐，不做动画。
        mSmoothLyricIndex = static_cast<int>(index);
        mSmoothLyricPos = lineTarget;
      } else if (mSmoothLyricIndex != static_cast<int>(index)) {
        // 切句：起点是「上一帧真实画到的位置」，不是上一句的行号。
        mSmoothLyricFrom = mSmoothLyricPos;
        mSmoothLyricIndex = static_cast<int>(index);
        mSmoothLyricStartMs = nowMs();
      }
      // 420ms 缓入缓出（ease-in-out），比之前的三次缓出更柔。
      // 260ms 太快、起步太猛，看起来像「弹一下」。
      float scrollRows = 0.0f;
      {
        const long long dt = nowMs() - mSmoothLyricStartMs;
        float t01 = 1.0f;
        if (mSmoothLyricStartMs > 0 && dt >= 0) {
          t01 = std::min(1.0f, static_cast<float>(dt) / 620.0f);
        }
        // ease-in-out（smoothstep）：两头慢、中间快。
        // 原来用 ease-out（1-inv^3），起步瞬间就把大部分位移跑完，
        // 看起来像先把字甩上去再停下，所以方向感是反的。
        const float eased = t01 * t01 * (3.0f - 2.0f * t01);
        mSmoothLyricPos =
            mSmoothLyricFrom + (lineTarget - mSmoothLyricFrom) * eased;
        // 符号必须是 target - pos，不能写成 pos - target。
        // pos 从「上一句行号」涨到「当前句行号」，target - pos 从 +1 落到 0，
        // 新句就从下方一个行高的位置平滑升到中线；
        // 写成 pos - target 会从 -1 涨到 0，新句先出现在上方再落下来。
        scrollRows = lineTarget - mSmoothLyricPos;
        // 这里原来写的是 mDirty = true，等于「620ms 内每一帧都重绘」。
        // refresh() 里的 needSubmit = mDirty 会绕过 forceInterval 节流，
        // 于是大面板（列表 + 歌词 + 封面，几百条绘制命令）每帧全量重建，
        // 表现就是玩家说的「大窗口运行得很不流畅」。
        // 现在改成只置一个「动画中」标志，由 forceInterval 统一控节奏：
        // 提交耗时高时自动降到 33/50ms，画面依然连续，但主线程不再被占满。
        mLyricAnimating.store(t01 < 1.0f);
      }
      // 歌词专用左边界：从面板左内边起，通栏绘制。
      // 封面只挤压歌名/进度条/三大件，不挤压歌词——
      // 歌词是本模组的主角，被封面挤到右边没有意义。
      const float lyricX = px;
      const std::size_t first = index > 0 ? index - 1 : 0;
      for (std::size_t i = first; i < mLyrics.size() && i < first + 6; ++i) {
        // 下一句在下方（加号）。原来用减号，下一句画在上面，
        // 所以整块是「从上往下滚」，方向反了。
        const float ly =
            midY + (static_cast<float>(i) - static_cast<float>(index)) * lineH +
            scrollRows * lineH;
        if (ly < baseY - lineH * 0.5f || ly > baseY + lineH * 5.5f) {
          continue;
        }
        const LyricLine &line = mLyrics[i];
        if (i == index) {
          // 当前行放大：随切句动画进度从 1.00 平滑涨到 1.14。
          // 原来字号是固定值，轮到时只有一个"啪"地变色，没有推上来的分量感。
          // 现在位移和放大共用同一条缓动曲线，视觉上是一起抬起来。
          float growT = 1.0f;
          if (mSmoothLyricStartMs > 0) {
            const long long gdt = nowMs() - mSmoothLyricStartMs;
            if (gdt >= 0 && gdt < 620) {
              const float gt = static_cast<float>(gdt) / 620.0f;
              growT = gt * gt * (3.0f - 2.0f * gt);
            }
          }
          const float grow = 1.0f + 0.14f * growT;
          const float lyricSize = 20.0f * mFontScale * grow;
          const float maxTextW = x + w - 24.0f - lyricX;
          // 前沿附近这段宽度内做颜色过渡。
          // 原来固定 96px，不跟字号缩放：20px 字号下英文单字约 11px，
          // 96px 会盖住近 9 个字，整句都算成半亮，推色看起来断裂。
          // 改为跟字号挂钩，约 1.6 个字号宽。
          const float fadeW = lyricSize * 1.6f;
          const bool timed = !line.chars.empty() &&
                             line.charStarts.size() == line.chars.size() &&
                             line.charEnds.size() == line.chars.size();
          // 先求颜色前沿：由当前字的真实时间轴决定，字慢就推得慢。
          //
          // 旧实现是「顺序扫，遇到第一个还没开始唱的字就 break」。
          // 一句歌词分成两段（KRC 里同一行有多个 <offset,duration,0> 标签，
          // 或多行时间戳衔接）时，第二段首字的 cs 明显晚于第一段末字的 ce，
          // 于是遍历到它就不满足任何条件、直接 break。跨段拼接的时间戳还
          // 常常不是严格单调，导致 front 卡在第一段末尾不再前进——这正是
          // 「一句两段只铺到第一段」的现象。
          //
          // 现在改成两遍扫描：
          //   第一遍找「最后一个已经唱完的字」，用它的右边界作已唱前沿；
          //   第二遍找「当前正在唱的那个字」，对它做插值。
          // 这样时间戳非单调也不会卡住（取的是最大右边界，不是遍历终点）。
          float front = 0.0f;
          if (timed) {
            const std::size_t n = line.chars.size();
            std::size_t sungEnd = 0;   // 已唱完的字有几个
            for (std::size_t g = 0; g < n; ++g) {
              const long long cs = line.charStarts[g];
              const long long ce = line.charEnds[g];
              // ce <= cs 是异常时间戳（时长为 0 或负），按「未知」跳过判断，
              // 但仍计入已唱，避免整句卡死。
              if (lyricPos >= ce || (ce <= cs && lyricPos >= cs)) {
                sungEnd = g + 1;
              }
            }
            if (sungEnd > 0) {
              front = line.charEndsPx[sungEnd - 1];
            }
            // 正在唱的那个字：优先取下标 sungEnd（紧随已唱之后）。
            // 若它因时间戳异常而无法插值，就再往后找一个「正在区间内」的字。
            for (std::size_t g = sungEnd; g < n; ++g) {
              const long long cs = line.charStarts[g];
              const long long ce = line.charEnds[g];
              if (ce <= cs) {
                continue;   // 时长为 0 或负，跳过
              }
              if (lyricPos < cs) {
                break;      // 还没到它，后续更晚（按正常排序）
              }
              if (lyricPos < ce) {
                const float left = (g == 0) ? 0.0f : line.charEndsPx[g - 1];
                const float right = line.charEndsPx[g];
                front = left + (right - left) *
                                   static_cast<float>(lyricPos - cs) /
                                   static_cast<float>(ce - cs);
              }
              break;
            }
          } else {
            const long long span = std::max(400LL, lineEnd - lineStart);
            float ratio = static_cast<float>(lyricPos - lineStart) /
                          static_cast<float>(span);
            ratio = std::min(1.0f, std::max(0.0f, ratio));
            front = std::min(line.totalWidth, maxTextW) * ratio;
          }
          // 整行绘制：按前沿位置把行切成「已唱」和「未唱」两段。
          // 原来逐字发 Text 命令，宿主的字距会逐字累积，
          // 整行看上去断成两截、字间有空洞。
          {
            const float spanW = std::min(line.totalWidth, maxTextW);
            const float frontClamped =
                std::min(spanW, std::max(0.0f, front));
            // 找到切分点：累计宽度第一次超过前沿的字符下标。
            std::size_t splitByte = line.text.size();
            if (!line.chars.empty() && line.charWidths.size() == line.chars.size()) {
              float acc = 0.0f;
              std::size_t bytePos = 0;
              for (std::size_t g = 0; g < line.chars.size(); ++g) {
                if (acc >= frontClamped) {
                  splitByte = bytePos;
                  break;
                }
                acc += line.charWidths[g];
                bytePos += line.chars[g].size();
              }
            } else {
              // 无逐字时间轴：按字宽比例粗略切分。
              if (line.totalWidth > 0.01f) {
                const float ratio = frontClamped / line.totalWidth;
                std::size_t bytePos = 0;
                float acc = 0.0f;
                for (std::size_t k = 0; k < line.text.size();) {
                  const unsigned char c =
                      static_cast<unsigned char>(line.text[k]);
                  std::size_t step = 1;
                  if ((c & 0xE0) == 0xC0) step = 2;
                  else if ((c & 0xF0) == 0xE0) step = 3;
                  else if ((c & 0xF8) == 0xF0) step = 4;
                  const float w =
                      estimateTextWidth(line.text.substr(k, step), lyricSize);
                  acc += w;
                  bytePos += step;
                  if (acc >= line.totalWidth * ratio) {
                    break;
                  }
                  k += step;
                }
                splitByte = bytePos;
              }
            }
            // 诊断：一句被拆成多段时（KRC 同段标签数 > 1），
            // 打一次真实时间轴，用来核对「两段只铺第一段」是否真的修好。
            // 只在纵向位置落在中线那一行打，避免每帧刷屏。
            if (i == index && std::fabs(ly - midY) < 4.0f) {
              static long long lastDiagMs = 0;
              const long long diagNow = nowMs();
              if (diagNow - lastDiagMs > 900) {
                lastDiagMs = diagNow;
                __android_log_print(
                    ANDROID_LOG_INFO, kTag,
                    "歌词推色 行=%zu 字=%zu 已唱=%zu 前沿=%.0f 总宽=%.0f 位置=%lld",
                    index, line.chars.size(), splitByte, frontClamped,
                    line.totalWidth, lyricPos);
              }
            }
            const std::string sung = line.text.substr(0, splitByte);
            // 整行先画主色，再把已唱部分覆盖成强调色。
            // 这样两段都从 infoX 起画，不靠估算宽度拼接，
            // 英文单词之间不会出现被挤出的缝隙。
            text(out, lyricX, ly, lyricSize, kTextMain, line.text);
            if (!sung.empty()) {
              text(out, lyricX, ly, lyricSize, kAccent, sung);
            }
          }
          // 左侧竖条标记当前行。
          rect(out, lyricX - 14.0f, ly - 4.0f, 3.0f, lyricSize * 1.4f,
               kAccent);
        } else {
          // 非当前行统一用实色 kTextDim，不再用 alpha 做远近层次。
          // 原因：日间白底上 alpha 会把深灰稀释成浅灰，读不清。
          // 层次改由字号和位置体现（当前行更大、左侧有竖条）。
          text(out, lyricX, ly, 17.0f * mFontScale, kTextDim, line.text);
        }
      }
    } else {
      text(out, px, y + 380.0f, 16.0f * mFontScale, kTextDim, "暂无歌词");
    }
  }
  }  // if (drawList)

  // 右下角竖向音量条（音乐音量，不是系统音量）。
  // 只在大面板的搜索/推荐页显示：
  // 「我的」页是设置页，且其铺底（y..y+h-64）会把音量条上半截盖住，
  // 只剩底部露出来，观感是「只露半截」。
  if (mMode == Mode::Expanded && !mLoginPage && mTab != Tab::Mine) {
    const float barW2 = 8.0f;
    const float barH2 = std::min(190.0f, h * 0.36f);
    const float barX2 = x + w - 26.0f;
    // 底端比原来高 44：右下角那个「缩成单行条」的减号按钮占
    // y+h-40…y+h-4，音量条原来底端在 y+h-34，两者直接叠在一起，
    // 视觉上糊成一坨、触摸也会打架。抬高后两者彻底分开。
    const float barY2 = y + h - 78.0f - barH2;
    mVolBarX = barX2 - 16.0f;
    mVolBarY = barY2 - 8.0f;
    mVolBarW = barW2 + 32.0f;
    mVolBarH = barH2 + 16.0f;
    roundRect(out, barX2, barY2, barW2, barH2, barW2 * 0.5f, kTrack);
    const float filled = barH2 * std::min(1.0f, std::max(0.0f, mVolume));
    if (filled > 1.0f) {
      roundRect(out, barX2, barY2 + barH2 - filled, barW2, filled,
                barW2 * 0.5f, kAccent);
    }
    const float knobY = barY2 + barH2 - filled;
    circleFill(out, barX2 + barW2 * 0.5f, knobY, 16.0f, kAccent);
    circleFill(out, barX2 + barW2 * 0.5f, knobY, 8.0f, 0xFF12151A);
    const int pct = static_cast<int>(mVolume * 100.0f + 0.5f);
    text(out, barX2 - 44.0f, knobY - 8.0f, 13.0f * mFontScale, kTextDim,
         std::to_string(pct));
    text(out, barX2 - 10.0f, barY2 - 24.0f, 13.0f * mFontScale, kTextDim,
         tr("音量", "Vol"));
  } else {
    mVolBarW = 0.0f;
    mVolBarH = 0.0f;
  }


  if (!mNotice.empty() && nowMs() < mNoticeUntil) {
      text(out, x + 24.0f, y + h - 32.0f, 15.0f * mFontScale, 0xFFFFC46B,
           mNotice);
    }

    // 推荐页顶部子分类只在展开页绘制。
    if (mMode == Mode::Expanded && mTab == Tab::Recommend &&
        mQueueView == 0) {
      rect(out, x + 24.0f, y + 46.0f, 540.0f, 56.0f, kSheetSoft);
      const char *recNames[3] = {"红心", "探索", "小众"};
      for (int i = 0; i < 3; ++i) {
        const float rx = x + 24.0f + i * 180.0f;
        const bool on = (mRecTab == i);
        if (on) {
          rect(out, rx + 4.0f, y + 52.0f, 172.0f, 40.0f, 0x2A4FE3C1);
          rect(out, rx + 4.0f, y + 88.0f, 172.0f, 4.0f, kAccent);
        }
        text(out, rx + 66.0f, y + 62.0f, 17.0f * mFontScale,
             on ? kTextMain : kTextDim, recNames[i]);
      }
      rect(out, x + 24.0f, y + 100.0f, 540.0f, 2.0f, 0x55FFFFFF);
    }
    // 我的：账号信息与推荐偏好。
    if (mMode == Mode::Expanded && mTab == Tab::Mine && mQueueView == 0) {
      // 这层铺底原来用不透明的 kSheet，把 glassPanel 的半透明底完全盖住，
      // 导致「我的」页调透明度毫无变化。改为跟随 mPanelAlpha。
      {
        const std::uint32_t a = static_cast<std::uint32_t>(
            std::min(1.0f, std::max(0.05f, mPanelAlpha)) * 255.0f);
        // 同样必须走圆角：这块铺底覆盖了面板上部，
        // 用直角画就等于把上边两个圆角抹平。
        roundRect(out, x, y, w, h - 64.0f, (h < 84.0f) ? 14.0f : 20.0f,
                  (a << 24) | (kSheet & 0x00FFFFFFu));
      }
      // 统一行距系统：所有纵向位置由 cursor 累加，不再写死固定偏移。
      // 原实现写死 y+14/y+46/y+96...，字号乘 1.35 后标题底部到 y+49，
      // 副标题在 y+46，必然重叠。
      const float padX = x + 24.0f;
      float cursor = y + 14.0f;

      // 标题
      text(out, padX, cursor, 26.0f * mFontScale, kTextMain,
           tr("我的", "Mine"));
      cursor += 26.0f * mFontScale + 8.0f;
      // 副标题
      text(out, padX, cursor, 14.0f * mFontScale, kTextDim,
           tr("账号与设置", "Account & settings"));
      cursor += 14.0f * mFontScale + 22.0f;
      // 账号
      text(out, padX, cursor, 20.0f * mFontScale, kTextMain,
           mLoggedIn ? (mUserLabel.empty() ? std::string("已登录")
                                          : "已登录  " + mUserLabel)
                     : std::string("未登录"));
      cursor += 20.0f * mFontScale + 20.0f;

      // 假偏好区已整块删除（12 个 chip + 3 档推送强度）。
      // 原因：prefW0~prefW11 只写不读，从未影响任何推荐结果；
      // 且「酷狗概念版」App 内并无该界面，本模组不该自造功能。
      // ---- 退出登录 / 领取会员 ----
      // 退出登录 / 领取会员
      const float btnH = std::max(52.0f, 17.0f * mFontScale + 26.0f);
      mMineLogoutY = cursor;
      mMineVipY = cursor;
      rect(out, padX, cursor, 200.0f, btnH, kTrack);
      text(out, padX + 38.0f, cursor + (btnH - 17.0f * mFontScale) * 0.5f,
           17.0f * mFontScale, kTextMain, "退出登录");
      rect(out, padX + 216.0f, cursor, 200.0f, btnH, 0xFF3A2E18);
      rect(out, padX + 216.0f, cursor, 200.0f, 2.0f, 0xFFFFC46B);
      text(out, padX + 252.0f, cursor + (btnH - 17.0f * mFontScale) * 0.5f,
           17.0f * mFontScale, 0xFFFFC46B, "领取会员");
      cursor += btnH + 20.0f;

      // 上传听歌数据
      const bool uploadOn = mUploadListen.load();
      const float upH = std::max(48.0f, 16.0f * mFontScale + 24.0f);
      mMineUploadY = cursor;
      rect(out, padX, cursor, 300.0f, upH, uploadOn ? 0xFF2F4C6E : kTrack);
      text(out, padX + 20.0f, cursor + (upH - 16.0f * mFontScale) * 0.5f,
           16.0f * mFontScale, uploadOn ? kAccent : kTextMain,
           uploadOn ? std::string("上传听歌数据  开")
                    : std::string("上传听歌数据  关"));
      cursor += upH + 10.0f;
      text(out, padX, cursor, 15.0f * mFontScale, kTextDim,
           "关闭后不再向酷狗同步播放记录");

      // 关闭中面板：开启后单行条直接展开成大面板，跳过中间那一层。
      cursor += 26.0f;
      const float dmH = std::max(48.0f, 16.0f * mFontScale + 24.0f);
      mMineDisableMiniY = cursor;
      rect(out, padX, cursor, 300.0f, dmH,
           mDisableMini ? 0xFF2F4C6E : kTrack);
      text(out, padX + 20.0f, cursor + (dmH - 16.0f * mFontScale) * 0.5f,
           16.0f * mFontScale, mDisableMini ? kAccent : kTextMain,
           mDisableMini ? std::string("关闭中面板  开")
                        : std::string("关闭中面板  关"));
      cursor += dmH + 8.0f;
      text(out, padX, cursor, 15.0f * mFontScale, kTextDim,
           "开启后单行条直接展开为大面板，不再经过中面板");

      // ---- 右栏：三形态独立透明度 + 主题 ----
      // 三个形态分开设置，因为大小面板对透明度的接受度完全不同：
      // 小面板面积小、字少，可以压得很透；大面板有歌词，太透就读不清。
      const float rx = x + 600.0f;
      float rc = y + 84.0f;
      const char *alphaNames[3] = {"大面板", "中面板", "单行条"};
      const float alphaVals[3] = {mPanelAlpha, effMiniAlpha(), effBarAlpha()};
      // 滑块几何写入成员，触摸侧直接读，两边不再各写一份坐标。
      mAlphaSliderX = rx + 100.0f;
      mAlphaSliderW = 220.0f;
      mAlphaSliderH = 10.0f;
      for (int i = 0; i < 3; ++i) {
        const int pct = static_cast<int>(alphaVals[i] * 100.0f + 0.5f);
        const float rowY = rc + 12.0f;
        mAlphaRowY[i] = rowY;
        mAlphaSliderY = rowY;
        text(out, rx, rc + 7.0f, 16.0f * mFontScale, kTextMain,
             alphaNames[i]);
        // 轨道
        roundRect(out, mAlphaSliderX, rowY, mAlphaSliderW, mAlphaSliderH,
                  mAlphaSliderH * 0.5f, kTrack);
        // 已填充段：把 0.10~1.00 线性映射到 0~全长
        const float t = std::min(
            1.0f, std::max(0.0f, (alphaVals[i] - 0.10f) / 0.90f));
        const float fillW = mAlphaSliderW * t;
        if (fillW > 1.0f) {
          roundRect(out, mAlphaSliderX, rowY, fillW, mAlphaSliderH,
                    mAlphaSliderH * 0.5f, kAccent);
        }
        // 圆钮
        const float knobX = mAlphaSliderX + fillW;
        circleFill(out, knobX, rowY + mAlphaSliderH * 0.5f, 20.0f, kAccent);
        circleFill(out, knobX, rowY + mAlphaSliderH * 0.5f, 9.0f, 0xFF12151A);
        text(out, mAlphaSliderX + mAlphaSliderW + 20.0f, rc + 6.0f,
             17.0f * mFontScale, kAccent, std::to_string(pct) + "%");
        rc += 44.0f;
      }
      text(out, rx, rc + 2.0f, 14.0f * mFontScale, kTextDim,
           "按住滑块左右拖动，各自独立保存");
      rc = y + 320.0f;

      // 主题
      text(out, rx, rc, 16.0f * mFontScale, kTextDim, "界面主题");
      rc += 16.0f * mFontScale + 12.0f;
      const char *themeNames[3] = {"跟随时间", "浅色", "深色"};
      const float thH = std::max(48.0f, 17.0f * mFontScale + 24.0f);
      rect(out, rx, rc, 240.0f, thH, mThemeMode == 0 ? kRowActive : kTrack);
      rect(out, rx, rc, 240.0f, 2.0f, kAccent);
      text(out, rx + 28.0f, rc + (thH - 17.0f * mFontScale) * 0.5f,
           17.0f * mFontScale, kTextMain,
           std::string("主题  ") + themeNames[mThemeMode]);
      rc += thH + 10.0f;
      text(out, rx, rc, 14.0f * mFontScale, kTextDim,
           "点一下切换，自动记住选择");
    }
    // 页头只由前面的统一入口绘制一次。
    // 这里原本又画了一遍标题与副标题，坐标与前面完全相同，
    // 两次叠字会让笔画变粗发糊（截图上像重影）。
    // 这里原本又画了一遍右上角的头像与「登录」按钮，坐标与页头统一入口
    // 完全相同。第二遍用不透明底色把第一遍盖掉，再在中间写一个昵称首字符，
    // 账号 id 是数字时就显示成「二」。已整块删除，右上角只由页头画一次。
    // 切页动画：内容淡入 + 轻微横向滑入，缓出曲线，不是匀速。
  {
    const long long tNow = nowMs();
    long long start = mTabAnimStartMs;
    if (mRecTabAnimStartMs > start) {
      start = mRecTabAnimStartMs;
    }
    // 这里原本还有一套「整块内容从 alpha 0.2 淡入 + 横移 24px」的动画，
    // 与列表区的滑入叠加后，画面会先变暗再恢复，观感就是闪一下。
    // 切页动效统一交给列表区的滑入处理，此处只负责在动画结束后清状态。
    if (start > 0) {
      const float raw = static_cast<float>(tNow - start) / 220.0f;
      if (raw >= 1.0f) {
        mTabAnimStartMs = 0;
        mRecTabAnimStartMs = 0;
      }
    }
  }
  // 水波纹：按下后 320 毫秒内从触点向外扩散。宿主没有裁剪能力，
  // 所以半径上限取「触点到按钮四边的最短距离」，保证圆不溢出按钮。
  if (mRippleStartMs > 0) {
    const long long elapsed = nowMs() - mRippleStartMs;
    if (elapsed >= 0 && elapsed < 320 && mPressRectW > 1.0f &&
        mPressRectH > 1.0f) {
      const float t = static_cast<float>(elapsed) / 320.0f;
      const float inv = 1.0f - t;
      const float e = 1.0f - inv * inv;  // ease-out quad
      const float left = mRippleX - mPressRectX;
      const float right = mPressRectX + mPressRectW - mRippleX;
      const float top = mRippleY - mPressRectY;
      const float bottom = mPressRectY + mPressRectH - mRippleY;
      const float limit =
          std::min(std::min(left, right), std::min(top, bottom));
      const float radius = std::max(2.0f, limit * e);
      const std::uint32_t alpha = static_cast<std::uint32_t>(0x60 * inv);
      circleFill(out, mRippleX, mRippleY, radius * 2.0f,
                 (alpha << 24) | 0xFFFFFFu);
    } else if (elapsed >= 320) {
      mRippleStartMs = 0;
    }
  }
  // 退出登录确认框：整屏压暗 + 居中卡片 + 取消/退出两个按钮。
  // 画在所有内容之后，保证盖在最上层。
  if (mLogoutConfirm) {
    rect(out, 0.0f, 0.0f, mScreenW, mScreenH, 0xB0000000);
    const float cw = std::min(mScreenW * 0.62f, 760.0f);
    const float ch = 250.0f;
    const float cx0 = (mScreenW - cw) * 0.5f;
    const float cy0 = (mScreenH - ch) * 0.5f;
    roundRect(out, cx0, cy0, cw, ch, 18.0f, 0xF21D2026);
    rect(out, cx0 + 18.0f, cy0 + 1.0f, cw - 36.0f, 1.0f, 0x33FFFFFF);
    text(out, cx0 + 40.0f, cy0 + 36.0f, 24.0f * mFontScale, kTextMain,
         "退出登录");
    text(out, cx0 + 40.0f, cy0 + 86.0f, 17.0f * mFontScale, kTextDim,
         "退出后需要重新扫码登录，听歌记录不会删除。");
    const float btnW = (cw - 132.0f) * 0.5f;
    const float btnY = cy0 + ch - 96.0f;
    roundRect(out, cx0 + 44.0f, btnY, btnW, 58.0f, 10.0f, kTrack);
    text(out, cx0 + 44.0f + (btnW - 34.0f) * 0.5f, btnY + 18.0f,
         18.0f * mFontScale, kTextMain, "取消");
    roundRect(out, cx0 + 88.0f + btnW, btnY, btnW, 58.0f, 10.0f, 0xFF7A2E2E);
    text(out, cx0 + 88.0f + btnW + (btnW - 68.0f) * 0.5f, btnY + 18.0f,
         18.0f * mFontScale, 0xFFFFE2E2, "确认退出");
  }

  auto submitFn = reinterpret_cast<SubmitDrawFn>(mSubmitDraw);
  if (mLoginPage) {
    __android_log_print(ANDROID_LOG_INFO, kTag, "登录页命令=%zu", out.size());
  }
  // 同上：先解锁再提交，避免宿主回调时死锁。
  lock.unlock();
  mStage.store(7);
  const auto submitStart = std::chrono::steady_clock::now();
  {
    static int submitLogCount = 0;
    if (submitLogCount < 6) {
      ++submitLogCount;
      __android_log_print(ANDROID_LOG_INFO, kTag, "提交命令=%zu 面板=%.0f,%.0f %.0fx%.0f 形态=%d", out.size(), mPanelX, mPanelY, mPanelW, mPanelH, static_cast<int>(mMode));
    }
  }
  submitFn(kModuleId, std::span<const Cmd>(out.data(), out.size()));
  mStage.store(8);
  const auto submitCost = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - submitStart).count();
  mSubmitCostMs = submitCost;
  if (submitCost > 50) {
    __android_log_print(ANDROID_LOG_WARN, kTag, "提交耗时 %lld 毫秒 命令=%zu",
                        static_cast<long long>(submitCost), out.size());
  }
  return true;
}

void HudUi::refresh(bool syncEngine) {
  bool needSubmit = false;
  mStage.store(1);
  if (syncEngine) {
    resolveSurface();
  }
  mStage.store(2);
  {
    mStage.store(3);
    std::lock_guard<std::mutex> lock(mMutex);
    if (!mInstalled) {
      return;
    }
    if (mEngine != nullptr) {
      if (!mManualList) {
        mTrackCount = mEngine->trackCount();
        mCurrentIndex = mEngine->currentIndex();
        bool listChanged = (mTrackCount != mTrackTitles.size());
        if (!listChanged) {
          for (std::size_t i = 0; i < mTrackCount; ++i) {
            const Track *t = mEngine->trackAt(i);
            const std::string title = t ? t->title : std::string();
            const std::string artist = t ? t->artist : std::string();
            if (title != mTrackTitles[i] || artist != mTrackArtists[i]) {
              listChanged = true;
              break;
            }
          }
        }
        if (listChanged) {
          mTrackTitles.clear();
          mTrackArtists.clear();
          mListScroll = 0.0f;
          for (std::size_t i = 0; i < mTrackCount; ++i) {
            const Track *t = mEngine->trackAt(i);
            mTrackTitles.push_back(t ? t->title : std::string());
            mTrackArtists.push_back(t ? t->artist : std::string());
          }
          mDirty = true;
        }
      } else {
        // 列表由菜单缓存提供：只按曲目 id 找当前播放项，不用队列下标覆盖。
        mCurrentIndex = static_cast<std::size_t>(-1);
        const Track *cur = mEngine->currentTrack();
        if (mQueueView == 1) {
          // 队列视图的数据源是 mQueueTracks，行数也必须用它。
          // 原来这里统一用 mTrackIds（浏览列表的 id），队列里的歌一个都
          // 匹配不上，mCurrentIndex 恒为 -1，表现就是「播放队列里正在
          // 播放的那一行没有高亮」；同时行数被设成浏览列表长度，
          // 队列比浏览列表长时，超出部分滚不出来。
          mTrackCount = mQueueTracks.size();
          if (cur != nullptr && !cur->id.empty()) {
            for (std::size_t i = 0; i < mQueueTracks.size(); ++i) {
              if (mQueueTracks[i].id == cur->id) {
                mCurrentIndex = i;
                break;
              }
            }
          }
        } else {
          mTrackCount = mTrackIds.size();
          if (cur != nullptr && !cur->id.empty()) {
            for (std::size_t i = 0; i < mTrackIds.size(); ++i) {
              if (mTrackIds[i] == cur->id) {
                mCurrentIndex = i;
                break;
              }
            }
          }
        }
      }
      // 曲目身份必须每拍都校验：切歌后如果等 syncEngine（约 500ms）
      // 才更新标题，面板会短暂显示上一首的歌名，看起来像串台。
      {
        const Track *cur = mEngine->currentTrack();
        const bool hasNow = cur != nullptr;
        if (hasNow != mHasTrack) {
          mHasTrack = hasNow;
          mDirty = true;
        }
        // 有过歌就正式激活 HUD，之后迷你条才开始吃触摸。
        if (hasNow) {
          mHudArmed = true;
        }
        if (cur != nullptr &&
            (mTitle != cur->title || mArtist != cur->artist)) {
          mTitle = cur->title;
          mArtist = cur->artist;
          mDirty = true;
        }
        // 收藏状态跟着当前曲目走。曲目一换就先把标记清掉，
        // 等 main 查询结果回来再点亮，避免旧状态套到新歌上。
        if (cur != nullptr && mFavoriteTrackId != cur->id) {
          mFavoriteTrackId = cur->id;
          mTrackFavorited = false;
          mDirty = true;
          // 记一个待查询标记，由 main 在下一拍发起查询。
          mFavoriteQueryPending.store(true);
        }
      }
      if (syncEngine) {
        const Track *cur = mEngine->currentTrack();
        if (cur != nullptr && mDisplayedId != cur->id) {
          mDisplayedId = cur->id;
          mTrackAnimStartMs = nowMs();
          mPositionMs = 0;
          mPlayableMs = 0;
          mDurationMs = cur->durationMs > 0 ? cur->durationMs : 0;
        }
        // 拖动进度时以手指位置为准，不要被播放器回读覆盖。
        const long long newPos = mEngine->positionMs();
        const long long newDur = mEngine->durationMs();
        const long long fullDur = (cur != nullptr && cur->durationMs > 0)
                                      ? cur->durationMs
                                      : newDur;
        const long long playable =
            (newDur > 0 && (fullDur <= 0 || newDur < fullDur)) ? newDur
                                                               : fullDur;
        const bool newPaused = mEngine->paused();
        // 播放位置必须按毫秒跟随：歌词扫色依赖它做连续插值。
        // 之前只按整秒更新，歌词看起来就是一秒跳一格。
        if (mDrag != Drag::Progress && newPos != mPositionMs) {
          mPositionMs = newPos;
          mDirty = true;
        }
        if (fullDur != mDurationMs || playable != mPlayableMs) {
          mDurationMs = fullDur;
          mPlayableMs = playable;
          mDirty = true;
        }
        if (newPaused != mPaused) {
          mPaused = newPaused;
          mDirty = true;
        }
        // 播完后引擎 playing=false 且 paused=false，必须同步 playing，
        // 否则按钮会停在暂停图标上不回来。
        const bool newPlaying = mEngine->playing();
        if (newPlaying != mPlaying) {
          mPlaying = newPlaying;
          mDirty = true;
        }
        if (!mLyrics.empty()) {
          const long long lyricPos = mPositionMs - mLyricDelay.load();
          std::size_t index = mCurrentLyric;
          if (index >= mLyrics.size()) {
            index = mLyrics.size() - 1;
          }
          while (index + 1 < mLyrics.size() &&
                 lyricPos >= mLyrics[index + 1].timeMs) {
            ++index;
          }
          while (index > 0 && lyricPos < mLyrics[index].timeMs) {
            --index;
          }
          if (index != mCurrentLyric) {
            mCurrentLyric = index;
            mLyricSwitchMs = nowMs();
            mDirty = true;
          }
        }
      }
    }
    const long long rotNow = nowMs();
    if (!mPaused && mPlaying && mHasTrack) {
      mCoverRotationMs += std::max(0LL, rotNow - mLastCoverTickMs);
    }
    mLastCoverTickMs = rotNow;
    updateAnim(rotNow);
    // 标题超出显示宽度时按 260 毫秒的节奏重绘一次，跑马灯才会动。
    if (mHasTrack) {
      // 单行迷你条的歌名可用宽度与中窗口不同，按形态分别估算，
      // 否则长歌名在迷你条上不会触发滚动刷新。
      const bool bar = (mMode == Mode::Bar);
      const bool mini = (mMode == Mode::Mini);
      // 单行条标题宽 = 面板宽 - 左边距12 - 右侧整串 - 中缝12。
      // 右侧整串（从右往左）：放大40 + 3 + 爱心40 + 3 + 模式40 + 3
      //   + 下曲40 + 3 + 播放46 + 3 + 上曲40 = 261。
      // 可用宽 = 面板宽 - 12 - 261 - 12 = 面板宽 - 285。
      // 这个数必须跟着绘制侧一起改，否则会出现「判定放得下、实际被截断」。
      const float titleSize = (bar ? 19.0f : (mini ? 27.0f : 30.0f)) * mFontScale;
      const float avail = bar ? (mPanelW - 285.0f)
                              : (mini ? (mPanelW - 78.0f) : (mPanelW - 500.0f));
      const bool overflow = estimateTextWidth(mTitle, titleSize) * 1.03f > avail;
      mTitleScrolling.store(overflow);
      if (overflow) {
        // 像素级滚动需要每拍重绘，不能按字符节拍跳。
        mDirty = true;
      }
    }
    // 按下遮罩期间保持刷新，让「变暗」持续到 140 毫秒结束。
    if (nowMs() < mPressedUntil) {
      mDirty = true;
    }
    // 发码倒计时每秒刷新一次按钮文字。
    if (nowMs() < mSmsSendUntil) {
      const long long stamp = nowMs();
      if (stamp - mSmsTickMs >= 1000) {
        mSmsTickMs = stamp;
        mDirty = true;
      }
    }
    // 歌词扫色或封面旋转时按 33 毫秒提交，暂停和无动画时降到 500 毫秒。
    // 形态切换动画期间必须走最高帧率。
    // 原来没有这一条：200ms 的动画只被 16ms 的歌词节拍顺带推几下，
    // 一旦那次提交耗时偏高（>12ms），间隔被拉到 33~50ms，
    // 整段动画就只剩 4~6 帧，看起来一格一格跳。
    // 标题超宽需要滚动时，250ms 一拍会让文字一顿一顿地跳。
    long long forceInterval = mTitleScrolling.load() ? 16 : 250;
    if (mAnimating) {
      forceInterval = 16;
    } else if (mDrag == Drag::List || mDrag == Drag::ScrollThumb ||
               mDrag == Drag::Panel || mDrag == Drag::Volume ||
               mDrag == Drag::AlphaSlider) {
      // 手指正在拖动时无条件按最高帧率提交。
      // 原来拖动只置 mDirty，但这一拍不到 forceInterval 的 250ms 门限
      // 就不提交，手指动得越快越跟不上，看起来一格一格跳。
      forceInterval = 16;
    } else if (!mPaused && mPlaying && mHasTrack &&
        (!mLyrics.empty() || mCoverIds.size() > 1 || mLyricAnimating.load())) {
      // 按「上一次提交实际耗时」自适应。
      // 大面板绘制命令多（列表几十行 + 歌词 + 封面），耗时天然更高，
      // 这里会自动降到 33/50ms；小面板耗时低，仍然是 16ms，不受影响。
      const long long cost = mSubmitCostMs;
      if (cost <= 6) {
        forceInterval = 16;
      } else if (cost <= 12) {
        forceInterval = 33;
      } else {
        forceInterval = 50;
      }
      // 大面板额外给一个 33ms 下限：它的命令数比中面板多一个量级，
      // 即便某次提交很快，也没必要每帧重建整个面板。
      if (mMode == Mode::Expanded && forceInterval < 33) {
        forceInterval = 33;
      }
      // 标题正在滚动时位移要连续，中小面板统一提到 60fps。
      // 大面板命令数多，保持上面的 33ms 下限，不然会掉帧。
      if (mTitleScrolling.load() && mMode != Mode::Expanded &&
          forceInterval > 16) {
        forceInterval = 16;
      }
    }
    const long long beat = nowMs();
    if (mInstalled && beat - mLastForceMs >= forceInterval) {
      mLastForceMs = beat;
      mDirty = true;
    }
    needSubmit = mDirty;
  }
  if (!needSubmit) {
    return;
  }
  // 提交仍在主循环线程里做：实测把提交搬到独立线程后，宿主不再刷新画面，
  // 面板会冻在最后一帧，所以这里保持原路径。
  // 只有提交成功才清脏，失败就留着下一拍重试。
  if (submit()) {
    std::lock_guard<std::mutex> lock(mMutex);
    mDirty = false;
  }

}

void HudUi::processPending(AudioEngine &engine) {
  // 结算 applyConfig 里入队的开关回调：见那里的说明。
  {
    std::unordered_map<std::string, std::string> pending;
    bool burst = false;
    {
      std::lock_guard<std::mutex> lock(mMutex);
      if (!mCfgPending.empty() && nowMs() - mCfgPendingMs > 200) {
        pending.swap(mCfgPending);
        burst = pending.size() >= 3;
      }
    }
    if (!pending.empty()) {
      if (burst) {
        __android_log_print(ANDROID_LOG_INFO, kTag,
                            "配置: 判定为宿主回放 %zu 项, 整批忽略",
                            pending.size());
      } else {
        for (const auto &kv : pending) {
          __android_log_print(ANDROID_LOG_INFO, kTag, "配置: 玩家操作 %s=%s",
                              kv.first.c_str(), kv.second.c_str());
          applyConfig(kv.first, kv.second, true);
        }
      }
    }
  }
  // 弹键盘放在这里执行：工作线程，不在宿主触摸回调栈里。
  if (mWantKeyboard.exchange(0) != 0) {
    if (mShowKeyboard != nullptr) {
      __android_log_print(ANDROID_LOG_INFO, kTag, "弹出键盘");
      auto showFn = reinterpret_cast<ShowKeyboardFn>(mShowKeyboard);
      showFn();
      __android_log_print(ANDROID_LOG_INFO, kTag, "弹出键盘返回");
      setSmsHint("键盘已弹出");
    } else {
      __android_log_print(ANDROID_LOG_WARN, kTag, "软键盘接口未解析到");
      setSmsHint("键盘接口不可用");
    }
  }
  const int action = mPendingAction.exchange(ActionNone);
  switch (action) {
  case ActionToggle:
    // 还没开播时点播放键应当是开始播放，而不是无动作。
    if (!engine.playing()) {
      engine.playIndex(engine.currentIndex());
    } else {
      engine.togglePause();
    }
    break;
  case ActionNext:
    engine.next();
    break;
  case ActionPrevious:
    engine.previous();
    break;
  default:
    break;
  }

  const int index = mPendingIndex.exchange(-1);
  if (index >= 0) {
    if (mTrackSelectHandler) {
      mTrackSelectHandler(index, static_cast<int>(mTab));
    } else {
      engine.playIndex(static_cast<std::size_t>(index));
    }
  }

  const long long seek = mPendingSeek.exchange(-1);
  if (seek >= 0) {
    engine.seek(seek);
  }

  if (mSearchSubmit.exchange(0) != 0) {
    std::function<void(const std::string &)> handler;
    std::string keyword;
    {
      std::lock_guard<std::mutex> lock(mMutex);
      keyword = mPendingSearchText;
      const std::size_t begin = keyword.find_first_not_of(" \t\r\n");
      const std::size_t end = keyword.find_last_not_of(" \t\r\n");
      if (begin == std::string::npos) {
        keyword.clear();
      } else {
        keyword = keyword.substr(begin, end - begin + 1);
      }
      mInputActive = false;
      mInputText = keyword;
      handler = mSearchHandler;
    }
    if (!keyword.empty() && handler) {
      __android_log_print(ANDROID_LOG_INFO, kTag, "提交搜索: %s",
                          keyword.c_str());
      handler(keyword);
    }
  }
}

void HudUi::uninstall() {
  if (!mInstalled) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mMutex);
    mInstalled = false;
  }
  // 先停提交线程，避免它在模组卸载后还调用宿主接口。
  stopSubmitThread();
  if (mRegisterTouch != nullptr) {
    auto registerFn = reinterpret_cast<RegisterTouchFn>(mRegisterTouch);
    registerFn(nullptr);
  }
  if (mUnregisterModule != nullptr) {
    auto unregisterFn = reinterpret_cast<UnregisterModuleFn>(mUnregisterModule);
    unregisterFn(kModuleId);
  }
  __android_log_print(ANDROID_LOG_INFO, kTag, "界面: 已注销");
}

} // namespace musicmod