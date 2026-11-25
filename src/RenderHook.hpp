#pragma once

namespace musicmod {

// 渲染入口探测。当前阶段只验证两件事：
// 一是 eglSwapBuffers 这个系统符号可定位；二是宿主 preloader 的
// GlossHook 系列符号可解析。确认之后才安装实际钩子。
class RenderHook {
public:
  bool install();
  void remove();

private:
  static bool probeHostSymbols();
  static void *resolveSwapBuffers();

  void *mTarget{nullptr};
};

} // namespace musicmod