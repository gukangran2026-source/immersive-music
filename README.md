# 沉浸式音乐 · Immersive Music

Minecraft 基岩版（Bedrock）原生模组。在游戏世界内绘制一个可交互的音乐播放面板，播放酷狗概念版在线音乐，支持歌词同步、封面显示与扫码登录。

> 中文为主。英文完整版见 [README.en.md](README.en.md)，本文末尾附英文摘要。

---

## 目录

- [这是什么](#这是什么)
- [功能](#功能)
- [运行环境](#运行环境)
- [三种形态](#三种形态)
- [安装](#安装)
- [构建](#构建)
- [架构](#架构)
- [几个关键技术点](#几个关键技术点)
- [目录结构](#目录结构)
- [已知限制](#已知限制)
- [免责声明](#免责声明)
- [技术支持与致谢](#技术支持与致谢)
- [English Summary](#english-summary)

---

## 这是什么

这是一个 **preload-native 类型**的 Minecraft 基岩版模组，运行在 LeviLauncher 的 preloader 框架里。

它不做资源替换，也不改游戏数据。它做的是：在游戏的渲染线程上挂一个绘制钩子，把一块自己画的面板叠在世界画面之上，同时接管落在这块面板范围内的触摸事件。

面板本身是一个完整的音乐播放器界面——封面、歌名、歌手、进度条、播放控制、歌词、搜索、歌单。

## 功能

| 功能 | 说明 |
|---|---|
| 播放控制 | 播放 / 暂停 / 上一首 / 下一首 / 拖拽进度 |
| 歌词 | 逐行高亮滚动，支持 KRC 逐字格式，自动跟随播放进度 |
| 封面 | 拉取专辑封面，可在模组菜单里开关 |
| 搜索 | 关键词搜索在线曲库 |
| 推荐 | 推荐歌单与每日推荐 |
| 我的 | 登录后的收藏、自建歌单 |
| 扫码登录 | 酷狗概念版扫码授权，令牌本地保存 |
| 音量 | 独立音量滑杆，接入游戏音频引擎 |
| 外观 | 主题（深色 / 浅色 / 跟随时间）、字号、面板透明度 |
| 位置记忆 | 单行条与中面板各自记住被拖到的位置 |
| 实时保存 | 所有设置与播放状态实时写入本地，不丢 |

## 运行环境

| 项 | 要求 |
|---|---|
| 游戏 | Minecraft 基岩版（见 `manifest.json` 的 `minecraft_versions`） |
| 启动器 | LeviLauncher（preloader 框架） |
| 架构 | arm64-v8a |
| 系统 | Android 8.0 以上（已在 Android 14 验证） |
| 模组类型 | `preload-native`，入口 `libmusicmod.so` |

## 三种形态

面板有三种尺寸，由缩放按钮循环切换，切换带过渡动画：

| 形态 | 尺寸 | 说明 |
|---|---|---|
| 单行条 | 520 × 56 | 最省地方，只有歌名和三个按钮，可拖动 |
| 中面板 | 560 × 192 | 带封面的紧凑面板，可拖动 |
| 大面板 | 屏幕 82% 宽 / 70% 高 | 完整界面：搜索、歌单、歌词，**固定居中不可拖动** |

## 安装

1. 下载 `.levipack` 包。
2. 打开 LeviLauncher，进入「模组」页面，导入该包。
3. 启动游戏并进入任意世界，面板会出现。

玩家侧不需要手动放任何文件。模组第一次运行时会自己生成数据目录。

> **重要**：`levipack` 内的目录名会成为模组 ID，包内目录固定为 `MusicMod`。若导入后目录被改名，模组在菜单中仍会显示，但内部注册名会跟随目录名自动适配（见下文「模组 ID 必须等于目录名」）。

## 构建

### 依赖

- Android NDK r27c（含 arm64-v8a sysroot）
- CMake 3.22 以上
- Ninja
- Python 3（打包脚本用）

### 步骤

```sh
export ANDROID_NDK_HOME=/path/to/android-ndk-r27c

# 1. 准备服务库（本仓库不含，需自行获取）
sh tools/make_embed.sh /path/to/libkugou_server.so

# 2. 构建
sh build.sh
```

产物：`build/final/libmusicmod.so`

本仓库的 `build.sh` 假定在 aarch64 Linux（proot / Termux 类）环境构建，
使用 `cmake/android-arm64.cmake` 里「系统 clang + NDK sysroot」的做法。
在 x86_64 电脑上请改用官方 NDK 工具链，该文件顶部有说明。

### 一个必须说明的前提

构建需要 `embed/libkugou_server.so`——一个独立的酷狗 API 本地服务库（arm64-v8a，约 4 MB）。**本仓库不包含该文件**，原因见[免责声明](#免责声明)与[技术支持](#技术支持与致谢)。

没有这个文件，`objcopy` 内嵌那一步会失败，编译出的 so 也不会包含服务库，运行时无法播放音乐。其余界面、触摸、设置部分仍可正常编译与运行。

## 架构

```
main.cpp            模组生命周期（load / enable / disable）
   │
   ├─ HudUi          面板绘制 + 触摸处理（软件渲染，自绘矩形与文字）
   │    ├─ 布局与动画（三种形态、过渡插值）
   │    ├─ 命中测试（按钮、列表、滑杆、拖动区域）
   │    └─ 状态持久化（data/ui.json，实时写入）
   │
   ├─ RenderHook     挂钩游戏渲染线程，提交绘制指令
   │
   ├─ KugouProvider  业务层：搜索 / 歌单 / 歌词 / 封面 / 登录
   │    └─ HTTP → 127.0.0.1:<随机端口> → 本地服务库
   │
   ├─ PlaylistProvider  歌单与队列
   └─ AudioEngine / AudioPlayer  解码与输出
```

音乐的获取路径是：模组向本机 `127.0.0.1` 上的一个随机端口发 HTTP 请求，那个端口由服务库在模组进程内监听。业务逻辑与接口实现完全解耦——模组不需要关心签名、设备指纹这些东西。

## 几个关键技术点

这几条是踩过坑之后写下来的，都在工程文档里有记录。

### 1. 面板必须自绘，不能用 Android 控件

模组运行在游戏进程里，挂的是渲染线程的钩子。所有绘制的矩形、圆角、文字、图标都要自己算，自己提交。这也是为什么 `HudUi.cpp` 有五千多行——布局、动画、命中测试、状态机全在里面。

### 2. 服务库不能放在 sdcard 上

Android 14 起，不允许从可写位置 `dlopen` 库。日志原文是这样的：

```
E/linker: library ".../mods/MusicMod/data/libkugou_server.so"
          ... is not accessible for the namespace "classloader-namespace"
```

所以运行时会解压到应用私有目录（`/data/data/<launcher>/cache/...`），再 `chmod 0500` 后加载。

### 3. 包里只放一个 so，服务库以二进制段内嵌

编译期用 `objcopy` 把服务库转成目标文件，链进主 so 的 `.rodata` 段，符号是 `_binary_libkugou_server_so_start` / `_end`。运行时按 64 KB 分块写出到一个私有路径，再 `dlopen`。

好处是分发物只有一个 `.so`，玩家不需要手动放第二个文件。

> 曾尝试过「往 so 尾部追加数据 + 尾部索引」的方案，实测在宿主加载时不可用，已废弃。

### 4. 模组 ID 必须等于模组目录名

这是个很容易踩的坑。`pl::modmenu::ModuleInfo::modId` 必须与宿主认定的 owning lifecycle mod 完全一致，而这个名字是 **`mods/` 下的目录名**，不是 `manifest.json` 里的 `name`。

写错的话，日志会报：

```
E Preloader: Rejected Mod Menu module musicmod.hud:
             declared mod_id X does not match owning lifecycle mod Y
```

后果是**游戏内模组菜单里根本看不到这个模组**，但音乐、歌词、封面全部在后台照常运行——非常容易误判成「模组没加载」。

本项目的做法是从 `ctx.modRootPath()` 取最后一段路径作为 ID，动态适配任意目录名。

### 5. 渲染节流

大面板元素多，每帧全量重建会掉帧。做法是用一个 `mLyricAnimating` 标志位配合统一的节流间隔，而不是在动画里直接置脏标记去绕过节流。

## 目录结构

```
.
├── README.md              本文件（中文）
├── README.en.md           英文版
├── CMakeLists.txt         构建配置
├── build.sh               一键构建脚本
├── manifest.json          模组清单
├── src/                   全部源码
│   ├── main.cpp               生命周期与模块注册
│   ├── HudUi.cpp / .hpp       面板绘制、触摸、状态机（核心）
│   ├── RenderHook.cpp         渲染钩子
│   ├── KugouProvider.cpp      业务层与本地 HTTP 客户端
│   ├── PlaylistProvider.cpp   歌单与队列
│   ├── AudioEngine.cpp        音频引擎
│   ├── AudioPlayer.cpp        播放器
│   ├── HttpClient.cpp         HTTP 客户端
│   ├── MusicTypes.hpp         数据结构
│   └── qrcodegen.cpp          二维码生成（扫码登录用）
├── docs/
│   ├── 架构与实现.md
│   └── 打包与交付规范.md
└── tools/
    └── pack_levipack.py   打包脚本
```

## 已知限制

1. **不包含服务库二进制**，clone 后不能直接构建出可用产物。
2. 只支持 arm64-v8a。
3. 只支持 LeviLauncher，其它启动器的加载路径不同。
4. 大面板固定居中，不可拖动（设计决定，不是缺陷）。
5. 音乐来源依赖第三方服务，接口随时可能变化导致失效。

## 免责声明

> 完整条款见 **[DISCLAIMER.md](DISCLAIMER.md)**，请在构建或使用前完整阅读。以下是要点摘要。

本项目是**个人独立开发的第三方模组**，与 Mojang Studios、Microsoft、《Minecraft》/《我的世界》、酷狗音乐、LeviLauncher 及其开发者**均无任何隶属、合作、授权、赞助或背书关系**，也未获得上述任何主体的认可或授权。文中所有产品名称与商标归其各自所有者。

- **不包含任何受版权保护的内容。** 仓库不存储、不分发音乐、音频、歌词或封面。所有可播放内容均由你已登录的第三方平台直接提供，本项目仅为客户端调用方。构建所需的本地服务库须由使用者自行合法获取，本仓库不提供，也不指引获取途径。
- **不绕过任何付费或权限机制。** 不提供破解，不规避会员限制、DRM 或地区限制。账号权限不足时，播放失败或只返回试听片段属于平台正常行为，**不是缺陷**。
- **账号凭据只留在本机。** 登录令牌仅存放于设备本地的应用私有目录，不上传、不收集、不回传、不分享。本项目没有自建服务器，网络请求只发生在你的设备与第三方平台之间。
- **仅供个人学习与技术研究。** 禁止商业使用、付费分发、二次售卖，以及任何侵犯他人著作权或违法的用途。
- **风险自担。** 使用可能导致游戏崩溃、存档损坏、数据丢失、设备异常，或第三方平台账号被限制、功能降级乃至封禁。**上述风险全部由使用者承担，作者不对任何直接或间接损失负责。** 建议不要使用重要账号。
- **按「现状」提供，不作任何担保**，不承诺可用、稳定、安全或无缺陷，也不承诺修复问题或持续维护。
- **依赖未公开的第三方接口。** 这些接口可能随时变更、限流或关闭，导致相关功能失效。这是本项目的结构性风险，作者无法控制，也不作可用性保证。
- 请遵守所在国家或地区的法律法规及第三方平台的用户协议，并支持正版音乐。

**如果你不同意上述任何一条，请立即停止使用并删除本项目。**

另外说明一句：如果你是从搜索引擎找过来的，**这个项目不是给普通玩家的成品**。它需要特定启动器（LeviLauncher）、特定架构（arm64-v8a），而且服务库依赖需要你自己合法解决。

## 技术支持与致谢

这个项目的完成离不开下面这些参考：

### MD3 Music

- 包名：`com.md3music.md3music`
- 参考内容：**酷狗 API 本地服务库**（`libkugou_server.so`，Rust 实现）与「独立服务 + 本地 HTTP」的接口分层设计思路。

这个设计是整套方案的基石：把平台接口逻辑关在一个独立库里，界面只跟 `127.0.0.1` 发 HTTP 请求。`KugouProvider.cpp` 里的请求组织方式与返回解析结构，都以它的行为为参照。

需要说明的是：该服务库是第三方产物，本项目**没有**其源码，也**不会**在本仓库分发它的二进制。使用者需自行获取。

### MagicPort 2.6.3

- 参考内容：`levipack` 打包结构、`ZIP_STORED` + 自实现解压的运行时方案、应用私有目录落盘与 `chmod 0500` 的做法。

它明确写着「运行时不再追加到 .so 尾部（尾部内嵌形态已废弃）」，这一条直接否定了本项目早期的一个错误方案，节省了很多时间。

### LeviLauncher / preloader SDK

- 参考内容：模组生命周期（`load` / `enable` / `disable`）、Mod Menu 模块注册、渲染钩子与触摸/文字输入接口、`pl::mod::Context` 提供的路径与日志能力。

没有这个框架，native 模组在基岩版上无从下手。

### 其它

- `qrcodegen`（Project Nayuki，MIT）—— 扫码登录的二维码生成。
- 感谢所有把踩坑经验写进日志和文档的人。

## English Summary

**Immersive Music** is a native Minecraft Bedrock Edition mod for the LeviLauncher preloader framework.

It draws an interactive music player overlay directly on top of the game world: album art, track info, progress bar, playback controls, synced lyrics, search, and playlists. Audio is streamed from Kugou Music by talking to a local HTTP service that runs inside the same process.

Highlights:

- Three panel sizes (bar / medium / full), animated transitions.
- Lyrics with per-character highlighting (KRC format).
- QR-code login; tokens stay on device only.
- Custom software renderer — every rectangle, glyph and icon is drawn manually onto the game's render thread.
- The service library is embedded into the mod `.so` at build time via `objcopy`, so users only import a single file.

Requirements: LeviLauncher, arm64-v8a, Android 8.0+.

**This repository does not include the third-party service library binary**, so a clean clone will not produce a fully functional build. See the Chinese sections above for details.

Credits: **MD3 Music** (Kugou API service layer and its local-HTTP design), **MagicPort 2.6.3** (levipack packaging and runtime extraction), and the **LeviLauncher preloader SDK**.

For personal study and research only. No music content is distributed here.
