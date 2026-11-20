# Hyprland 协议差距

对照 Hyprland（`FlakeShell/Hyprland`，commit `84b512298`）的
`src/managers/ProtocolManager.cpp` 中注册的全部 global，列出 flake-wm
**尚未提供** 或 **版本较低** 的协议，以及已补齐部分的实现情况。

- flake-wm 一侧以 `src/` 中实际的 `wl_global_create` / `wlr_*_create` 调用为准，
  完整的支持列表见 `docs/PROTOCOLS.md`
- 「wlroots 助手」指 `libs/wlroots`（0.20.2）中是否已有现成实现
  （`libs/wlroots/types/`），有的话一般只需在 server 里创建并接上信号。

## 1. 上游 wayland-protocols（已实现）

原先缺失的 16 个上游协议已全部补上（`wp_drm_lease_device_v1` 只在 DRM 后端下广播）。
「验证」一列：**运行** 表示已在嵌套 flakewm 中用真实客户端跑通，**广播** 表示只确认了
global 存在、尚未用客户端实际操作过。

| 全局接口 | 版本 | 实现位置 | 说明 / 限制 | 验证 |
| :-- | :--: | :-- | :-- | :-- |
| `wp_cursor_shape_manager_v1` | 2 | `src/input/cursor_shape.c` | 指针与数位板笔均支持，焦点规则同 `wl_pointer.set_cursor` | 广播 |
| `wp_content_type_manager_v1` | 1 | `src/server.c` | 仅记录提示，tearing/VRR 策略尚未读取 | 广播 |
| `wp_single_pixel_buffer_manager_v1` | 1 | `src/server.c` | 走 data_ptr 纹理路径 | 广播（mpv 已绑定） |
| `wp_security_context_manager_v1` | 1 | `src/security/security.c` | 沙箱客户端看不到截屏、剪贴板监听、输入注入、窗口/工作区/输出管理、桌面 shell 类 global（列表见 `privileged_interfaces`） | 运行 |
| `wp_alpha_modifier_v1` | 1 | `src/scene/surface.c` | 乘到 scene buffer 不透明度；对象销毁后下次 commit 恢复为 1，不覆盖 xwayland 的窗口透明度 | 广播 |
| `wp_drm_lease_device_v1` | 1 | `src/output/drm_lease.c` | 仅 DRM 后端；`non_desktop` 输出（VR 头显）不再加入桌面布局，改为提供租用 | 未验证（嵌套无 DRM） |
| `zxdg_exporter_v2` / `zxdg_importer_v2` | 1 | `src/view/xdg_extras.c` | 父窗口经 xdg_toplevel `set_parent` 信号进入 `view_set_parent()` | 广播 |
| `ext_workspace_manager_v1` | 1 | `src/view/ext_workspace.c` | 所有输出共用一个 group；坐标为一维 position；支持创建/激活/删除，最后一个工作区不可删；deactivate/assign 忽略 | 运行 |
| `ext_data_control_manager_v1` | 1 | `src/input/selection.c` | — | 运行（wl-clipboard 2.3） |
| `wp_color_manager_v1` | 2 | `src/server.c` | 渲染没有色彩管线，因此只声明 parametric + gamma22 + sRGB primaries + perceptual，HDR 请求会被拒绝 | 广播 |
| `xdg_system_bell_v1` | 1 | `src/view/xdg_extras.c` | 目前只记日志，无声音/提醒 | 广播 |
| `xdg_toplevel_tag_manager_v1` | 1 | `src/view/xdg_extras.c` | 目前只记日志，窗口规则尚未匹配 tag | 广播 |
| `wp_fifo_manager_v1` | 1 | `src/scene/fifo.c` | barrier 在 surface 主输出下一次 frame_done 时清除；不可见的 surface 由 100ms 兜底定时器清除；同步模式子 surface 忽略约束 | 运行（mpv Vulkan FIFO，帧间隔中位数 16.8ms @60Hz） |
| `wp_commit_timing_manager_v1` | 1 | `src/scene/commit_timing.c` | 提前半个刷新周期释放，以落在最接近目标时间的那次刷新 | 运行（mpv） |
| `wp_pointer_warp_v1` | 1 | `src/input/pointer_warp.c` | 需拥有指针焦点、serial 有效、位置在 surface 内；锁屏或合成器 grab 期间忽略 | 广播 |
| `ext_background_effect_manager_v1` | 1 | `src/view/ext_background_effect.c` | 复用 kde blur 的 scene 模糊；空区域即无模糊 | 广播 |

## 2. Hyprland 私有协议（hyprland-protocols）

| 全局接口 | Hyprland 版本 | 用途 |
| :-- | :--: | :-- |
| `hyprland_workspace_image_capture_source_manager_v1` | 1 | 以工作区为来源的 ext-image-copy-capture |
| `hyprland_input_capture_manager_v1` | 2 | xdph InputCapture portal（Deskflow/Barrier 等） |

## 3. 缺失：其他第三方协议

| 全局接口 | Hyprland 版本 | 用途 |
| :-- | :--: | :-- |
| `vicinae_hotkey_manager_v1` | 1 | vicinae 启动器的全局热键 |
| `xx_hotkey_manager_v1` | 1 | 实验性全局热键协议 |

## 4. 已有但版本低于 Hyprland

| 全局接口 | flake-wm | Hyprland | 位置 / 说明 |
| :-- | :--: | :--: | :-- |
| `xdg_wm_base` | 5 | 7 | `src/view/xdg_shell.c:512`；wlroots 已支持 7 |
| `zwlr_layer_shell_v1` | 4 | 5 | `src/view/wlr_layer_shell.c:722`；wlroots 已支持 5 |
| `zwp_linux_dmabuf_v1` | 4 | 5 | `src/render/renderer.c:216`；wlroots 已支持 5 |
| `zxdg_decoration_manager_v1` | 1 | 2 | 需要更新的 wayland-protocols 与 wlroots（当前 vendored 版本只有 v1） |

## 5. 已对齐（无需处理）

`wl_compositor` 6、`wl_subcompositor`、`wl_shm` 2、`wl_seat` 9、`wl_output` 4、
`wl_data_device_manager` 3、`wl_drm` 2、`wp_presentation` 2、`wp_viewporter`、
`wp_fractional_scale_manager_v1`、`wp_tearing_control_manager_v1`、
`wp_linux_drm_syncobj_manager_v1`、`xdg_activation_v1`、`xdg_wm_dialog_v1`、
`xwayland_shell_v1`、`zxdg_output_manager_v1` 3、`zwp_tablet_manager_v2`、
`zwp_text_input_manager_v1`/`v3`、`zwp_input_method_manager_v2`、
`zwp_virtual_keyboard_manager_v1`、`zwlr_virtual_pointer_manager_v1`、
`zwp_pointer_gestures_v1` 3、`zwp_pointer_constraints_v1`、
`zwp_relative_pointer_manager_v1`、`zwp_idle_inhibit_manager_v1`、
`zwp_keyboard_shortcuts_inhibit_manager_v1`、`zwp_primary_selection_device_manager_v1`、
`ext_idle_notifier_v1` 2、`ext_session_lock_manager_v1`、`ext_foreign_toplevel_list_v1`、
`ext_image_copy_capture_manager_v1`（含 output / toplevel capture source）、
`zwlr_screencopy_manager_v1` 3、`zwlr_data_control_manager_v1` 2、
`zwlr_foreign_toplevel_manager_v1` 3、`zwlr_gamma_control_manager_v1`、
`zwlr_output_manager_v1` 4、`zwlr_output_power_manager_v1`、
`org_kde_kwin_server_decoration_manager`。
