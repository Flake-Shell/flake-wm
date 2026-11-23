# KWin垫片（Project Occultation · Exoskeleton）
## 介绍
GXDE OS的Wayland会话早已从`gxde-kwin`转至`GXWM`，不过我们是由于更新破坏依赖而被迫这么做的，但我们还是想尽可能带回原版体验。然而，要重现原版的UI是很难的，尤其是动效这一块，于是Project Occultation的主意是直接复用原版`gxde-kwin`的UI。

本目录就是实现这一点的垫片：一个基于Qt的薄层，模拟`gxde-kwin`的QML前端用到的最小一部分KWin功能。底层功能都由flakewm提供，合成器与垫片之间通过`libkywc`交流。

前端本身放在旁边，例如Alt+Tab窗口切换器位于[`../window_switcher`](../window_switcher)。

## 构建
当`WLCOM_LIBKYWC`开启且找到Qt 6 Quick（及其私有GUI模块）时，垫片会编译为静态库`occultation-exoskeleton`。另有两个默认不编译的目标：
- `occultation-qml-smoke`：用垫片加载切换器QML，由`ctest -L occultation`运行。
- `occultation-switcher-preview`：脱离flakewm，用假窗口展示切换器。

## 许可证
以GPLv3或更高版本授权，请参阅项目的主许可证。导入的`gxde-kwin`前端有各自的许可证，见各自目录下的README。
