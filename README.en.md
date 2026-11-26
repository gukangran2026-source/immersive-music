# Immersive Music

A native Minecraft **Bedrock Edition** mod that renders an interactive music player overlay directly on top of the game world. It streams music from Kugou Music (酷狗概念版), with synced lyrics, album art, search, and QR-code login.

> The primary documentation is in Chinese — see [README.md](README.md). This file is the English translation.

---

## What it is

This is a `preload-native` mod built for the LeviLauncher preloader framework.

It does not replace resources or modify game data. Instead it:

1. Hooks the game's render thread and draws its own panel on top of the world.
2. Intercepts touch events that land inside that panel's bounds.

The panel is a complete music player UI: album art, track info, progress bar, transport controls, lyrics, search and playlists.

## Features

| Feature | Description |
|---|---|
| Playback | Play / pause / previous / next / seek by dragging the progress bar |
| Lyrics | Line-by-line highlighting, KRC per-character format, follows playback |
| Album art | Fetched from the platform, can be toggled in the mod menu |
| Search | Keyword search over the online catalogue |
| Recommendations | Recommended playlists and daily picks |
| Library | Favourites and user playlists once signed in |
| QR login | Scan-to-authorise; tokens are stored on device only |
| Volume | Independent volume slider wired into the game audio engine |
| Appearance | Theme (dark / light / follow-time), font scale, panel opacity |
| Position memory | The bar and medium panels each remember where you dragged them |
| Autosave | All settings and playback state are written to disk in real time |

## Requirements

| Item | Requirement |
|---|---|
| Game | Minecraft Bedrock Edition (see `minecraft_versions` in `manifest.json`) |
| Launcher | LeviLauncher (preloader framework) |
| ABI | arm64-v8a |
| OS | Android 8.0+ (verified on Android 14) |
| Mod type | `preload-native`, entry point `libmusicmod.so` |

## Three panel sizes

Cycled by the resize button, with an animated transition:

| Form | Size | Notes |
|---|---|---|
| Bar | 520 × 56 | Most compact: title plus three buttons, draggable |
| Medium | 560 × 192 | Compact panel with cover art, draggable |
| Full | 82% width / 70% height | Complete UI: search, playlists, lyrics. **Centred and fixed — not draggable** |

## Installation

1. Download the `.levipack` package.
2. Open LeviLauncher, go to the **Mods** page, import the package.
3. Launch the game and enter any world.

Players never have to place files by hand. The mod creates its own data directory on first run.

> The directory name inside the `levipack` becomes the mod ID. The package ships with a directory named `MusicMod`. If it gets renamed, the mod adapts automatically — see *Mod ID must equal the directory name* below.

## Building

### Dependencies

- Android NDK r27c (with the arm64-v8a sysroot)
- CMake 3.22+
- Ninja
- Python 3 (for the packaging script)

### Steps

```sh
export ANDROID_NDK_HOME=/path/to/android-ndk-r27c
sh build.sh
```

Output: `build/final/libmusicmod.so`

### A necessary caveat

The build requires `embed/libkugou_server.so` — a standalone Kugou API service library (arm64-v8a, about 4 MB). **This repository does not include that file.**

Without it, the `objcopy` embedding step fails, and the resulting `.so` will not contain the service library, so music playback will not work. The UI, touch handling and settings code still compile and run.

## Architecture

```
main.cpp            mod lifecycle (load / enable / disable)
   │
   ├─ HudUi          panel drawing + touch handling (software renderer)
   │    ├─ layout and animation (three forms, interpolated transitions)
   │    ├─ hit testing (buttons, lists, sliders, drag regions)
   │    └─ state persistence (data/ui.json, written live)
   │
   ├─ RenderHook     hooks the game render thread, submits draw commands
   │
   ├─ KugouProvider  business layer: search / playlists / lyrics / cover / login
   │    └─ HTTP → 127.0.0.1:<random port> → local service library
   │
   ├─ PlaylistProvider  playlists and queue
   └─ AudioEngine / AudioPlayer  decoding and output
```

Music is fetched by sending HTTP requests to a random port on `127.0.0.1`, which the service library listens on inside the mod process. Business logic is fully decoupled from the platform implementation — the mod does not care about signing or device fingerprinting.

## Technical notes

These are lessons learned the hard way.

### 1. The panel must be drawn by hand

The mod lives inside the game process and hooks the render thread. Every rectangle, rounded corner, glyph and icon is computed and submitted manually. That is why `HudUi.cpp` runs over five thousand lines — layout, animation, hit testing and state machine all live there.

### 2. The service library cannot live on sdcard

Since Android 14, libraries cannot be `dlopen`ed from writable locations:

```
E/linker: library ".../mods/MusicMod/data/libkugou_server.so"
          ... is not accessible for the namespace "classloader-namespace"
```

So it is extracted into the app's private directory (`/data/data/<launcher>/cache/...`), `chmod 0500`, then loaded.

### 3. One `.so` in the package, service library embedded as a binary section

At build time `objcopy` turns the service library into an object file, linked into the main `.so`'s `.rodata` section under the symbols `_binary_libkugou_server_so_start` / `_end`. At runtime it is written out in 64 KB chunks to a private path, then `dlopen`ed.

The benefit: the distributed artifact is a single `.so`, and users never have to place a second file.

> An earlier approach — appending data plus a tail index to the `.so` — proved unusable when the host loads the library, and has been dropped.

### 4. Mod ID must equal the mod directory name

An easy trap. `pl::modmenu::ModuleInfo::modId` must exactly match the mod the host considers the owner, and that name is the **directory name under `mods/`**, not the `name` field in `manifest.json`.

Get it wrong and the log says:

```
E Preloader: Rejected Mod Menu module musicmod.hud:
             declared mod_id X does not match owning lifecycle mod Y
```

The consequence is that **the mod does not appear in the in-game mod menu at all**, while music, lyrics and album art keep working in the background — very easy to misdiagnose as "the mod did not load".

This project reads the last path segment of `ctx.modRootPath()` at runtime, so any directory name works.

### 5. Render throttling

The full panel has many elements; rebuilding everything each frame drops frames. The fix is a `mLyricAnimating` flag feeding a unified throttle interval, rather than setting a dirty flag from inside animations and bypassing the throttle.

## Repository layout

```
.
├── README.md              Chinese documentation (primary)
├── README.en.md           this file
├── CMakeLists.txt         build configuration
├── build.sh               one-shot build script
├── manifest.json          mod manifest
├── src/
│   ├── main.cpp               lifecycle and module registration
│   ├── HudUi.cpp / .hpp       panel drawing, touch, state machine (core)
│   ├── RenderHook.cpp         render hook
│   ├── KugouProvider.cpp      business layer and local HTTP client
│   ├── PlaylistProvider.cpp   playlists and queue
│   ├── AudioEngine.cpp        audio engine
│   ├── AudioPlayer.cpp        player
│   ├── HttpClient.cpp         HTTP client
│   ├── MusicTypes.hpp         data structures
│   └── qrcodegen.cpp          QR code generation (login)
├── docs/
│   ├── 架构与实现.md          architecture and implementation (Chinese)
│   └── 打包与交付规范.md      packaging and delivery rules (Chinese)
└── tools/
    └── pack_levipack.py   packaging script
```

## Known limitations

1. **No service library binary included** — a clean clone does not build a functional artifact.
2. arm64-v8a only.
3. LeviLauncher only; other launchers use different load paths.
4. The full panel is fixed and centred by design, not by accident.
5. Playback depends on a third-party service whose API may change at any time.

## Disclaimer

> Full terms: **[DISCLAIMER.md](DISCLAIMER.md)**. Please read it before building or using this project. Key points below.

This is an **unofficial, independent third-party mod**. It is **not affiliated with, endorsed by, sponsored by, or authorised by** Mojang Studios, Microsoft, Minecraft, KuGou Music, LeviLauncher, or their developers. All product names and trademarks belong to their respective owners.

- **No copyrighted content is included.** This repository does not store or distribute music, audio, lyrics, or cover art. All playable media is provided directly by third-party platforms you have logged into; this project is merely a client. The service library required for building must be obtained lawfully by the user — it is neither included nor pointed to here.
- **No paywall or permission mechanism is bypassed.** No cracking, no circumvention of membership limits, DRM, or regional restrictions. When your account lacks permission, playback failing or returning only a preview clip is **normal platform behaviour, not a bug**.
- **Credentials stay on your device.** Login tokens are stored only in the app's private directory on your own device. Nothing is uploaded, collected, or shared. There is no backend server; network requests only occur between your device and the third-party platform.
- **For personal study and technical research only.** Commercial use, paid redistribution, resale, and any infringing or unlawful use are prohibited.
- **Use entirely at your own risk.** Usage may cause game crashes, corrupted saves, data loss, device issues, or restrictions on your third-party account up to a ban. **You bear all such risk; the author accepts no liability for any direct or indirect damages.** Do not use an account you cannot afford to lose.
- **Provided "AS IS", without warranty of any kind**, with no promise of usability, stability, security, or maintenance.
- **Depends on undocumented third-party APIs** that may change, rate-limit, or shut down at any time. This is a structural risk the author cannot control and does not guarantee against.
- Comply with the laws of your jurisdiction and the platform's terms of service, and please support legitimate music.

**If you disagree with any part of the above, stop using this project and delete it.**

One more note: if you found this via a search engine — **this is not a ready-to-use product for regular players.** It requires a specific launcher (LeviLauncher) on a specific architecture (arm64-v8a), and you must supply the service library dependency yourself.

## Credits

### MD3 Music

- Package name: `com.md3music.md3music`
- What was referenced: the **Kugou API local service library** (`libkugou_server.so`, Rust) and the "standalone service + local HTTP" layering.

That layering is the foundation of this entire project: platform logic lives in a separate library, and the UI only speaks HTTP to `127.0.0.1`. The request construction and response parsing in `KugouProvider.cpp` follow its behaviour.

That service library is a third-party artifact. This project has **no source for it**, and does **not** redistribute its binary. You need to obtain it yourself.

### MagicPort 2.6.3

- What was referenced: the `levipack` package layout, the `ZIP_STORED` + hand-written extraction approach, writing into the app private directory, and `chmod 0500`.

Its `build.sh` states plainly that runtime no longer appends data to the tail of the `.so` — which directly ruled out an early, wrong approach in this project and saved a lot of time.

### LeviLauncher / preloader SDK

- What was referenced: the mod lifecycle (`load` / `enable` / `disable`), Mod Menu module registration, render hooks, touch and text input interfaces, and the path/logging helpers on `pl::mod::Context`.

Without this framework, native modding on Bedrock would have no starting point.

### Other

- `qrcodegen` (Project Nayuki, MIT) — QR code generation for login.
- Thanks to everyone who wrote down what broke and why.

## License

No license file is included. The source is published for reading and study; it is not offered for redistribution as a product. Third-party components retain their own terms.
