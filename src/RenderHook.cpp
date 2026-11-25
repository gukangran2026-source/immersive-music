#include "RenderHook.hpp"

#include <android/log.h>
#include <dlfcn.h>

namespace musicmod {
namespace {

constexpr const char *kTag = "MusicMod";

// 打开已经加载的 preloader，只用于解析符号，不增加实际加载行为。
void *openPreloader() {
  void *handle = dlopen("libpreloader.so", RTLD_NOW | RTLD_NOLOAD);
  if (handle != nullptr) {
    return handle;
  }
  return dlopen("libpreloader.so", RTLD_NOW);
}

} // namespace

bool RenderHook::probeHostSymbols() {
  void *preloader = openPreloader();
  if (preloader == nullptr) {
    __android_log_print(ANDROID_LOG_WARN, kTag, "符号探测: 未能打开 libpreloader.so");
    return false;
  }

  void *glossInit = dlsym(preloader, "GlossInit");
  void *glossHook = dlsym(preloader, "GlossHookAddr");
  if (glossHook == nullptr) {
    glossHook = dlsym(preloader, "GlossHook");
  }
  void *glossDelete = dlsym(preloader, "GlossHookDelete");

  __android_log_print(ANDROID_LOG_INFO, kTag,
                      "符号探测: GlossInit=%d GlossHook=%d GlossHookDelete=%d",
                      glossInit != nullptr ? 1 : 0, glossHook != nullptr ? 1 : 0,
                      glossDelete != nullptr ? 1 : 0);

  return glossHook != nullptr;
}

void *RenderHook::resolveSwapBuffers() {
  // eglSwapBuffers 是 libEGL 的导出符号，与游戏版本无关。
  void *symbol = dlsym(RTLD_DEFAULT, "eglSwapBuffers");
  if (symbol == nullptr) {
    void *egl = dlopen("libEGL.so", RTLD_NOW);
    if (egl != nullptr) {
      symbol = dlsym(egl, "eglSwapBuffers");
    }
  }
  return symbol;
}

bool RenderHook::install() {
  if (mTarget != nullptr) {
    return true;
  }

  mTarget = resolveSwapBuffers();
  if (mTarget == nullptr) {
    __android_log_print(ANDROID_LOG_ERROR, kTag, "渲染入口: 找不到 eglSwapBuffers");
    return false;
  }

  const bool hostReady = probeHostSymbols();

  // 当前阶段只验证入口可定位、宿主符号可解析，不实际改写函数头。
  // 安装前必须先确认删除 API 的语义，否则卸载后残留跳转指向已释放代码。
  __android_log_print(ANDROID_LOG_INFO, kTag, "渲染入口已定位=%p, 可安装=%d",
                      mTarget, hostReady ? 1 : 0);
  return hostReady;
}

void RenderHook::remove() {
  // 尚未安装实际钩子。
  mTarget = nullptr;
}

} // namespace musicmod