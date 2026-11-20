# protocol

wayland协议支持情况
> https://wayland.app/protocols/

## Wayland core
> https://gitlab.freedesktop.org/wayland/wayland.git

| 协议名称               | 支持情况 | 说明       |
| :--------------------- | :------: | :--------- |
| wl_compositor          |    6     | 7          |
| wl_shm                 |    2     | 3          |
| wl_data_device_manager |    3     | 4          |
| wl_shell               |    -     | deprecated |
| wl_seat                |    9     | 11         |
| wl_output              |    4     |            |
| wl_subcompositor       |    1     |            |
| wl_fixes               |    no    | 2          |

## Wayland protocols
> https://gitlab.freedesktop.org/wayland/wayland-protocols.git

### stable

| 协议名称              | 支持情况 | 说明  |
| :-------------------- | :------: | :---- |
| wp_presentation       |    2     | 3     |
| wp_viewporter         |    1     |       |
| xdg_wm_base           |    5     | 7     |
| zwp_linux_dmabuf_v1   |    4     | 6     |
| zwp_tablet_manager_v2 |    1     | 2     |

### staging

| 协议名称                                             | 支持情况 | 说明                                  |
| :--------------------------------------------------- | :------: | :------------------------------------ |
| wp_alpha_modifier_v1                                 |    1     |                                       |
| wp_color_manager_v1                                  |    2     | 3；仅 parametric gamma22 + sRGB       |
| wp_color_representation_manager_v1                   |    no    | 1                                     |
| wp_commit_timing_manager_v1                          |    1     |                                       |
| wp_content_type_manager_v1                           |    1     |                                       |
| wp_cursor_shape_manager_v1                           |    2     |                                       |
| wp_drm_lease_device_v1                               |    1     | 仅 DRM 后端                           |
| ext_background_effect_manager_v1                     |    1     |                                       |
| ext_data_control_manager_v1                          |    1     |                                       |
| ext_foreign_toplevel_list_v1                         |    1     |                                       |
| ext_idle_notifier_v1                                 |    2     |                                       |
| ext_image_copy_capture_manager_v1                    |    1     |                                       |
| ext_output_image_capture_source_manager_v1           |    1     |                                       |
| ext_foreign_toplevel_image_capture_source_manager_v1 |    1     |                                       |
| ext_session_lock_manager_v1                          |    1     |                                       |
| ext_transient_seat_manager_v1                        |    1     |                                       |
| ext_workspace_manager_v1                             |    1     |                                       |
| wp_fifo_manager_v1                                   |    1     |                                       |
| wp_fractional_scale_manager_v1                       |    1     |                                       |
| wp_linux_drm_syncobj_manager_v1                      |    1     | 需后端/渲染器支持；`KYWC_SYNCOBJ=0` 关闭 |
| wp_pointer_warp_v1                                   |    1     |                                       |
| wp_security_context_manager_v1                       |    1     | 沙箱客户端看不到特权协议              |
| wp_single_pixel_buffer_manager_v1                    |    1     |                                       |
| wp_tearing_control_manager_v1                        |    1     |                                       |
| xdg_activation_v1                                    |    1     |                                       |
| xdg_wm_dialog_v1                                     |    1     |                                       |
| xdg_session_manager_v1                               |    no    | 1                                     |
| xdg_system_bell_v1                                   |    1     |                                       |
| xdg_toplevel_drag_manager_v1                         |    1     |                                       |
| xdg_toplevel_icon_manager_v1                         |    no    | 1                                     |
| xdg_toplevel_tag_manager_v1                          |    1     |                                       |
| xwayland_shell_v1                                    |    1     | 仅启用 Xwayland 时                    |

### unstable

| 协议名称                                  | 支持情况 | 说明                 |
| :---------------------------------------- | :------: | :------------------- |
| zwp_fullscreen_shell_v1                   |    no    | 1                    |
| zwp_idle_inhibit_manager_v1               |    1     |                      |
| zwp_input_method_context_v1               |    no    | 1                    |
| zwp_input_timestamps_manager_v1           |    no    | 1                    |
| zwp_keyboard_shortcuts_inhibit_manager_v1 |    1     |                      |
| zwp_linux_dmabuf_v1                       |    -     | 见 stable            |
| zwp_linux_explicit_synchronization_v1     |    no    | 2                    |
| zwp_pointer_constraints_v1                |    1     |                      |
| zwp_pointer_gestures_v1                   |    3     |                      |
| zwp_primary_selection_device_manager_v1   |    1     |                      |
| zwp_relative_pointer_manager_v1           |    1     |                      |
| zwp_tablet_manager_v1                     |    -     | v2                   |
| zwp_text_input_manager_v1                 |    1     |                      |
| zwp_text_input_manager_v3                 |    1     | 2                    |
| zxdg_decoration_manager_v1                |    1     | 2                    |
| zxdg_exporter_v1 / zxdg_importer_v1       |    no    | 1，已有 v2           |
| zxdg_exporter_v2 / zxdg_importer_v2       |    1     |                      |
| zxdg_output_manager_v1                    |    3     |                      |
| xdg_shell                                 |    -     | xdg_wm_base          |
| zxdg_shell_v6                             |    -     | xdg_wm_base          |
| zwp_xwayland_keyboard_grab_manager_v1     |    no    | 1                    |

## wlr
> https://gitlab.freedesktop.org/wlroots/wlr-protocols.git

| 协议名称                         | 支持情况 | 说明       |
| :------------------------------- | :------: | :--------- |
| zwlr_data_control_manager_v1     |    2     |            |
| zwlr_export_dmabuf_manager_v1    |    1     |            |
| zwlr_foreign_toplevel_manager_v1 |    3     |            |
| zwlr_gamma_control_manager_v1    |    1     |            |
| zwlr_input_inhibit_manager_v1    |    -     | deprecated |
| zwlr_layer_shell_v1              |    4     | 5          |
| zwlr_output_manager_v1           |    4     |            |
| zwlr_output_power_manager_v1     |    1     |            |
| zwlr_screencopy_manager_v1       |    3     |            |
| zwlr_virtual_pointer_manager_v1  |    2     |            |

## kde
> https://invent.kde.org/libraries/plasma-wayland-protocols.git

| 协议名称                                       | 支持情况 | 说明  |
| :--------------------------------------------- | :------: | :---- |
| org_kde_kwin_appmenu_manager                   |    2     |       |
| org_kde_kwin_blur_manager                      |    1     | ukui  |
| org_kde_kwin_contrast_manager                  |    no    | 2     |
| org_kde_kwin_dpms_manager                      |    1     |       |
| org_kde_kwin_fake_input                        |    no    | 5     |
| org_kde_kwin_idle                              |    1     |       |
| kde_lockscreen_overlay_v1                      |    no    | 1     |
| kde_output_device_v2                           |    8     |       |
| kde_output_management_v2                       |    9     |       |
| kde_output_order_v1                            |    no    | 1     |
| kde_primary_output_v1                          |    2     |       |
| kde_screen_edge_manager_v1                     |    no    | 1     |
| org_kde_kwin_keystate                          |    5     |       |
| org_kde_plasma_virtual_desktop_management      |    2     |       |
| org_kde_kwin_outputmanagement                  |    -     | 4 v2  |
| org_kde_kwin_outputdevice                      |    -     | 4 v2  |
| org_kde_plasma_shell                           |    8     | ukui  |
| org_kde_plasma_window_management               |    17    |       |
| org_kde_kwin_remote_access_manager             |    no    | 1     |
| org_kde_kwin_server_decoration_palette_manager |    no    | 1     |
| org_kde_kwin_server_decoration_manager         |    1     |       |
| org_kde_kwin_shadow_manager                    |    no    | 2     |
| org_kde_kwin_slide_manager                     |    1     |       |
| zkde_screencast_unstable_v1                    |    no    | 3     |

## weston
> https://gitlab.freedesktop.org/wayland/weston.git

| 协议名称                  | 支持情况 | 说明 |
| :------------------------ | :------: | :--- |
| ivi_surface               |    no    | 1    |
| ivi_hmi_controller        |    no    | 1    |
| text_cursor_position      |    no    | 1    |
| weston_content_protection |    no    | 1    |
| weston_debug_v1           |    no    | 1    |
| weston_desktop_shell      |    no    | 1    |
| weston_direct_display_v1  |    no    | 1    |
| weston_capture_v1         |    no    | 1    |
| weston_test               |    no    | 1    |
| weston_touch_calibration  |    no    | 1    |

## external

| 协议名称                   | 支持情况 | 说明 |
| :------------------------- | :------: | :--- |
| wl_drm                     |    2     |      |
| wl_eglstream               |    no    | 1    |
| wl_eglstream_controller    |    no    | 2    |
| zwp_input_method_manager_v2 |    1     |      |
| zwp_virtual_keyboard_manager_v1 |    1     |      |
| zwp_text_input_manager_v2  |    1     |      |
| qt_surface_extension       |    no    | 1    |
| gtk_shell1                 |    no    | 5    |
| vicinae_hotkey_manager_v1  |    1     | 与合成器快捷键冲突的组合会被拒绝 |
| xx_hotkey_manager_v1       |    1     | 与 vicinae 共用同一套规则；另支持按钮触发（不带 Ctrl/Alt/Super 时只允许侧键等辅助键）；set_seat、set_description 只接受不使用 |

## hyprland
> https://github.com/hyprwm/hyprland-protocols

| 协议名称                             | 支持情况 | 说明                                       |
| :----------------------------------- | :------: | :----------------------------------------- |
| hyprland_global_shortcuts_manager_v1 |    1     |       |
| hyprland_toplevel_export_manager_v1  |    2     |                                            |
| hyprland_toplevel_mapping_manager_v1 |    1     |                                            |
| hyprland_focus_grab_manager_v1       |    1     |                                            |
| hyprland_ctm_control_manager_v1      |    2     | 仅对角线（各通道缩放），非对角项忽略       |
| hyprland_surface_manager_v1          |    2     | visible region 仅接受，不裁剪              |
| hyprland_lock_notifier_v1            |    1     |                                            |
| hyprland_workspace_image_capture_source_manager_v1 | 1 | 工作区跨所有输出，按主输出截取 |
| hyprland_input_capture_manager_v1    |    2     | 需 libeis；屏障只能放在输出外边缘；Super+Shift+Escape 强制释放，锁屏或输出布局变化时也会释放 |

## gxwm
| 协议名称                   | 支持情况 | 说明 |
| :------------------------- | :------: | :--- |
| gxde_identifier_v1         |    1     |      |
| gxde_screenshot_manager_v1 |    1     |      |

## kywc
> 继承自 kylin-wlcom

| 协议名称                  | 支持情况 | 说明 |
| :------------------------ | :------: | :--- |
| kywc_capture_manager_v1   |    2     |      |
| kywc_output_manager_v1    |    1     |      |
| kywc_toplevel_manager_v1  |    1     |      |
| kywc_workspace_manager_v1 |    1     |      |

## ukui
> https://gitee.com/openkylin/kylin-wayland-protocols

| 协议名称                  | 支持情况 | 说明 |
| :------------------------ | :------: | :--- |
| ukui_blur_manager_v1      |    1     |      |
| ukui_output_management_v1 |    1     |      |
| ukui_shell                |    4     |      |
| ukui_window_management    |    1     |      |

## dde (DWayland)

| 协议名称  | 支持情况 | 说明                                       |
| :-------- | :------: | :----------------------------------------- |
| dde_shell |    2     | Deepin/DTK 窗口属性（圆角、标题栏等） |

## treeland
> https://github.com/linuxdeepin/treeland-protocols

| 协议名称                            | 支持情况 | 说明                                            |
| :---------------------------------- | :------: | :---------------------------------------------- |
| treeland_dde_shell_manager_v1       |    2     |                                                 |
| treeland_app_id_resolver_manager_v1 |    1     |                                                 |
| treeland_capture_manager_v1         |    1     | 供 deepin-screen-recorder 录屏与点击窗口截图使用 |
| treeland_personalization_manager_v1 |    2     | Treeland Protocol包0.5.8和0.5.9都是导出v1版本，但是实际这两个版本里面的XML不一致，导致签名会对不上，我们内置了一套机制可以自适应这两个版本的变化 |
