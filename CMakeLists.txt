cmake_minimum_required(VERSION 3.22)
project(musicmod LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(PRELOADER_SDK "${CMAKE_CURRENT_SOURCE_DIR}/../reference/preloader-sdk")
set(THIRD_PARTY "${CMAKE_CURRENT_SOURCE_DIR}/../reference/third_party")

add_library(musicmod SHARED
    src/main.cpp
    src/AudioPlayer.cpp
    src/AudioEngine.cpp
    src/PlaylistProvider.cpp
    src/KugouProvider.cpp
    src/HttpClient.cpp
    src/HudUi.cpp
    src/RenderHook.cpp
    src/qrcodegen.cpp
    # 酷狗服务库以二进制目标文件的形式链接进主 so（objcopy 产物），
    # 运行时释放到应用私有目录再 dlopen。这样分发包里只有一个 .so，
    # 单文件自包含，导入时不会因为缺少附加文件而失效。
    #
    # 另有 runtime.zip 兜底路径（见 KugouProvider.cpp）：如果 mod 目录里
    # 放了 runtime.zip，就优先用它，方便不改主 so 就换服务库。
    # 注意：绝不要用「追加到 .so 尾部」的方式——那种形态实测会被拒绝加载。
    embed/libkugou_blob.o
)

target_include_directories(musicmod PRIVATE
    ${PRELOADER_SDK}/include
    ${THIRD_PARTY}/fmt/include
    ${THIRD_PARTY}
)

target_compile_definitions(musicmod PRIVATE FMT_HEADER_ONLY=1)

# 静态链接 C++ 运行库的位置，从工具链的 sysroot 推导，避免写死本机 NDK 路径
if(NOT DEFINED CMAKE_SYSROOT)
  message(FATAL_ERROR "CMAKE_SYSROOT 未设置，请通过工具链文件指定 NDK sysroot")
endif()
set(ANDROID_SYSROOT_LIB "${CMAKE_SYSROOT}/usr/lib/aarch64-linux-android")

target_link_libraries(musicmod PRIVATE
    log
    dl
    android
    EGL
    GLESv2
    # 静态链接 C++ 运行库：游戏进程里没有 libc++_shared.so，
    # 动态依赖会导致 dlopen 直接失败、整个模组不加载。
    ${ANDROID_SYSROOT_LIB}/libc++_static.a
    ${ANDROID_SYSROOT_LIB}/libc++abi.a
)

target_link_options(musicmod PRIVATE
    -nostdlib++
    -Wl,--exclude-libs,libc++_static.a
    -Wl,--exclude-libs,libc++abi.a
)

target_compile_options(musicmod PRIVATE
    -Wall -Wextra -O2 -ffunction-sections -fdata-sections
    -fvisibility=hidden -fvisibility-inlines-hidden
)

target_link_options(musicmod PRIVATE
    -Wl,--gc-sections
    -Wl,-z,noexecstack
)

set_target_properties(musicmod PROPERTIES
    OUTPUT_NAME "musicmod"
)