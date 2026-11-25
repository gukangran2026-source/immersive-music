# 交叉编译工具链：在 aarch64 Linux（proot / Termux 类环境）上，
# 用系统 clang 18 + NDK r27c 的 sysroot 编译 Android arm64-v8a。
#
# 背景：官方 NDK 只提供 x86_64 主机版客户端，无法在手机 proot 内直接运行；
#      但 sysroot 与目标库与主机架构无关，系统 clang 可以直接充当交叉编译器。
#
# 在 x86_64 电脑上构建时不需要这个文件，直接用官方 NDK 的工具链即可：
#   cmake -S . -B build -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake \
#     -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24
#
# 使用本文件前请设置环境变量 ANDROID_NDK_HOME 指向 NDK r27c 根目录。

set(NDK_ROOT "$ENV{ANDROID_NDK_HOME}")
if(NOT NDK_ROOT)
  message(FATAL_ERROR
    "请先设置环境变量 ANDROID_NDK_HOME，指向 Android NDK r27c 的根目录。")
endif()

# NDK r27c 的预编译目录在 Linux / macOS 上都叫 linux-x86_64
set(NDK_HOST "${NDK_ROOT}/toolchains/llvm/prebuilt/linux-x86_64")
if(NOT EXISTS "${NDK_HOST}")
  message(FATAL_ERROR "找不到 NDK 预编译目录: ${NDK_HOST}")
endif()

set(ANDROID_SYSROOT "${NDK_HOST}/sysroot")
set(ANDROID_API 24)
set(ANDROID_TARGET "aarch64-linux-android${ANDROID_API}")
set(CLANG_RESOURCE_DIR "${NDK_HOST}/lib/clang/18")

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_SYSROOT ${ANDROID_SYSROOT})

# 用系统 clang 充当交叉编译器（proot 内没有 x86_64 的 NDK 客户端）
set(CMAKE_C_COMPILER /usr/bin/clang)
set(CMAKE_CXX_COMPILER /usr/bin/clang++)
set(CMAKE_AR /usr/bin/llvm-ar)
set(CMAKE_RANLIB /usr/bin/llvm-ranlib)
set(CMAKE_STRIP /usr/bin/llvm-strip)

set(CMAKE_C_COMPILER_TARGET ${ANDROID_TARGET})
set(CMAKE_CXX_COMPILER_TARGET ${ANDROID_TARGET})

set(CMAKE_C_FLAGS_INIT "-resource-dir=${CLANG_RESOURCE_DIR} -rtlib=compiler-rt")
set(CMAKE_CXX_FLAGS_INIT "-resource-dir=${CLANG_RESOURCE_DIR} -rtlib=compiler-rt -stdlib=libc++")

set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-fuse-ld=lld")