#!/bin/sh
# 一键构建：交叉编译出 Android arm64-v8a 的 libmusicmod.so
#
# 前提：
#   1. 已设置 ANDROID_NDK_HOME 指向 NDK r27c
#   2. 已准备 embed/libkugou_blob.o（见 tools/make_embed.sh）
#
# 说明：本脚本假定在 aarch64 Linux（proot / Termux 类）环境构建，
#      使用 cmake/android-arm64.cmake 里「系统 clang + NDK sysroot」的做法。
#      在 x86_64 电脑上请改用官方 NDK 工具链，见该文件里的注释。

set -u

PROJ="$(cd "$(dirname "$0")" && pwd)"
cd "$PROJ" || exit 1

if [ -z "${ANDROID_NDK_HOME:-}" ]; then
  echo "请先设置 ANDROID_NDK_HOME 指向 Android NDK r27c" >&2
  exit 1
fi

if [ ! -f embed/libkugou_blob.o ]; then
  echo "缺少 embed/libkugou_blob.o" >&2
  echo "请先准备服务库，然后执行：" >&2
  echo "  sh tools/make_embed.sh <你的 libkugou_server.so>" >&2
  exit 1
fi

cmake -S . -B build -G Ninja \
    -DCMAKE_MAKE_PROGRAM=/usr/bin/ninja \
    -DCMAKE_TOOLCHAIN_FILE=cmake/android-arm64.cmake || exit 1

cmake --build build -j4 || exit 1

mkdir -p build/final
cp -f build/libmusicmod.so build/final/libmusicmod.so

# 可选：打一个 runtime.zip 作为兜底。
# 模组运行时会优先用内嵌段；若 mod 目录里存在 runtime.zip，则改用它，
# 方便不重编主 so 就替换服务库。不需要就别打，包更大没有好处。
if [ -f embed/libkugou_server.so ]; then
  echo "=== 打包 runtime.zip ==="
  python3 tools/pack_runtime.py \
      embed/libkugou_server.so build/final/runtime.zip || exit 1
fi

echo "=== 构建产物 ==="
ls -lh build/final/