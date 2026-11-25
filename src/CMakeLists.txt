add_executable(flakewm)

target_sources(flakewm PRIVATE
  main.cc
  server.c
  gxde_identifier.c
  app_id_resolver.c
  patches/matrix.c
  patches/damage_ring.c
  patches/linux_dmabuf.c
  patches/vulkan_sync.c
  patches/buffer_completion.c
  patches/text_source.c

  plugin/config.c
  plugin/plugin.c
)

# backend
target_sources(flakewm PRIVATE
  backend/backend.c
)

if(HAVE_FBDEV)
  target_sources(flakewm PRIVATE
    backend/fbdev/backend.c
    backend/fbdev/fbdev.c
  )
endif()

# config
target_sources(flakewm PRIVATE
  config/common.c
  config/config.c
  config/gtk_theme.c
)

if(WLCOM_KDE_GLOBAL_ACCEL)
  add_executable(qtkey-mapper config/qtkey_mapper.c)
  if(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
    # the xkbcommon keysym header is under /usr/local/include there
    target_include_directories(qtkey-mapper PRIVATE
      $<TARGET_PROPERTY:PkgConfig::WLCOM_DEPS,INTERFACE_INCLUDE_DIRECTORIES>
    )
  endif()
  wlcom_capture_header(flakewm config/qtkey_map_table.h qtkey-mapper)

  target_sources(flakewm PRIVATE
    config/kde_global_accel.c
  )
endif()

if(WLCOM_KDE_INPUT)
  target_sources(flakewm PRIVATE
    config/kde_input.c
  )
endif()

if(WLCOM_UKUI_SHORTCUT)
  target_sources(flakewm PRIVATE
    config/ukui_shortcut.c
  )
endif()

if(WLCOM_UKUI_GSETTINGS)
  target_sources(flakewm PRIVATE
    config/ukui_gsettings.c
  )
endif()

if(WLCOM_UKUI_VIEW_MODE)
  target_sources(flakewm PRIVATE
    config/ukui_view_mode.c
  )
endif()

# effect
target_sources(flakewm PRIVATE
  effect/animator.c
  effect/blur.c
  effect/capture.c
  effect/circle_progressbar.c
  effect/config.c
  effect/effect.c
  effect/ext_image_copy_capture.c
  effect/fade.c
  effect/gxde_screenshot.c
  effect/hyprland_toplevel_export.c
  effect/hyprland_toplevel_mapping.c
  effect/locate_pointer.c
  effect/magic_lamp.c
  effect/mouse_click.c
  effect/mouse_trail.c
  effect/move.c
  effect/output_transform.c
  effect/showfps.c
  effect/scale.c
  effect/shake_cursor.c
  effect/shake_view.c
  effect/soft_gamma.c
  effect/tap_ripple.c
  effect/touch_click.c
  effect/touch_long.c
  effect/touch_trail.c
  effect/trail.c
  effect/translation.c
  effect/transform.c
  effect/zoom.c
)

target_sources(flakewm PRIVATE
  effect/ky_capture.c
)

target_sources(flakewm PRIVATE
  effect/treeland_capture.c
)

if(WLCOM_UKUI_SCREENSHOT)
  target_sources(flakewm PRIVATE
    effect/screenshot.c
  )
endif()

if(WLCOM_UKUI_WATERMARK)
  target_sources(flakewm PRIVATE
    effect/watermark.c
  )
endif()

if(WLCOM_KDE_SLIDE)
  target_sources(flakewm PRIVATE
    effect/slide.c
  )
endif()

wlcom_embed_files(flakewm FILES
  effect/shaders/blur_tex.vert
  effect/shaders/blur_first_down.frag
  effect/shaders/blur_first_down.vert
  effect/shaders/blur_tex_rgba.frag
  effect/shaders/blur.vert
  effect/shaders/blur_up.frag
  effect/shaders/blur_down.frag
  effect/shaders/circle_progressbar.vert
  effect/shaders/circle_progressbar.frag
  effect/shaders/magic_lamp.vert
  effect/shaders/magic_lamp.frag
  effect/shaders/shape_color.vert
  effect/shaders/shape_color.frag
  effect/shaders/tap_ripple.vert
  effect/shaders/tap_ripple.frag
)

# input
target_sources(flakewm PRIVATE
  input/action.c
  input/binding.c
  input/config.c
  input/cursor.c
  input/cursor_shape.c
  input/event.c
  input/gesture.c
  input/hotkey.c
  input/emergency_term.c
  input/hyprland_focus_grab.c
  input/hyprland_global_shortcuts.c
  input/idle.c
  input/idle_inhibit.c
  input/input.c
  input/text_input.c
  input/keyboard.c
  input/keyboard_group.c
  input/libinput.c
  input/monitor.c
  input/pointer_warp.c
  input/tablet.c
  input/text_input_v1.c
  input/text_input_v2.c
  input/toplevel_drag.c
  input/touch.c
  input/transient_seat.c
  input/vicinae_hotkey.c
  input/xx_hotkey.c
  input/seat.c
  input/selection.c
  input/selection_data.c
)

if(HAVE_LIBEIS)
  target_sources(flakewm PRIVATE
    input/hyprland_input_capture.c
  )
endif()

if(WLCOM_KDE_KEYSTATE)
  target_sources(flakewm PRIVATE
    input/kde_keystate.c
  )
endif()

if(WLCOM_CLIPBOARD_PERSIST)
  target_sources(flakewm PRIVATE
    input/selection_persist.c
  )
endif()

# output
target_sources(flakewm PRIVATE
  output/drm_lease.c
  output/gamma.c
  output/gamma_dbus.c
  output/hyprland_ctm_control.c
  output/night_mode.c
  output/config.c
  output/layout.c
  output/modes.c
  output/output.c
  output/xdg_output.c
  output/ky_output.c
)

if(WLCOM_KDE_OUTPUT)
  target_sources(flakewm PRIVATE
    output/kde_output.c
  )
endif()

if(WLCOM_WLR_OUTPUT)
  target_sources(flakewm PRIVATE
    output/wlr_output.c
  )
endif()

if(WLCOM_UKUI_OUTPUT)
  target_sources(flakewm PRIVATE
    output/ukui_output.c
  )
endif()

# painter
target_sources(flakewm PRIVATE
  painter/image.c
  painter/painter.c
  painter/render.c
)

# render
target_sources(flakewm PRIVATE
  render/pass.c
  render/pixel_format.c
  render/renderer.c
  render/shm_buffer.c
  render/wayland_drm.c
)

if(WLCOM_TRACY)
  target_sources(flakewm PRIVATE
    render/profile.c
    render/opengl/profile.c
  )
endif()

target_sources(flakewm PRIVATE
  render/opengl/buffer.c
  render/opengl/egl.c
  render/opengl/pass.c
  render/opengl/renderer.c
  render/opengl/texture.c
)

wlcom_embed_files(flakewm SUFFIX _str FILES
  render/opengl/shaders/common.vert
  render/opengl/shaders/quad.frag
  render/opengl/shaders/tex_rgba.frag
  render/opengl/shaders/tex_rgbx.frag
  render/opengl/shaders/tex_external.frag
  render/opengl/shaders/quad_ex.frag
  render/opengl/shaders/tex_rgba_ex.frag
  render/opengl/shaders/tex_rgbx_ex.frag
  render/opengl/shaders/tex_external_ex.frag
)

# scene
target_sources(flakewm PRIVATE
  scene/animation.c
  scene/box.c
  scene/buffer.c
  scene/commit_timing.c
  scene/decoration.c
  scene/fifo.c
  scene/hyprland_surface.c
  scene/output.c
  scene/rect.c
  scene/render.c
  scene/scene.c
  scene/surface.c
  scene/subsurface.c
  scene/thumbnail.c
  scene/xdg_shell.c
)

wlcom_embed_files(flakewm FILES
  scene/shaders/decoration.vert
  scene/shaders/decoration.frag
)

# security
target_sources(flakewm PRIVATE
  security/security.c
)

# theme
target_sources(flakewm PRIVATE
  theme/config.c
  theme/icon.c
  theme/theme.c
)

if(WLCOM_UKUI_THEME)
  target_sources(flakewm PRIVATE
    theme/ukui_theme.c
  )
endif()

wlcom_embed_files(flakewm SUFFIX _src FILES
  theme/icons/base_light.svg
  theme/icons/base_dark.svg
  theme/icons/unknown.svg
)

# util
target_sources(flakewm PRIVATE
  util/dbus.c
  util/debug.c
  util/file.c
  util/hash_table.c
  util/identifier.c
  util/kywc_log_sink.cc
  util/limit.c
  util/logger.c
  util/matrix.c
  util/vec2.c
  util/queue.c
  util/quirks.c
  util/region.c
  util/spawn.c
  util/string.c
  util/sysfs.c
  util/time.c
  util/wayland.c
)

# view
target_sources(flakewm PRIVATE
  view/action.c
  view/config.c
  view/decoration.c
  view/ext_background_effect.c
  view/ext_workspace.c
  view/global_authentication.c
  view/hyprland_lock_notify.c
  view/interactive.c
  view/maximize_switcher.c
  view/window_switcher.c
  view/modal.c
  view/positioner.c
  view/session_lock.c
  view/split_screen_switcher.c
  view/ssd.c
  view/stack_mode.c
  view/tile_preview.c
  view/view.c
  view/window_menu.c
  view/workspace.c
  view/xdg_dialog.c
  view/xdg_popup.c
  view/xdg_shell.c
  view/xdg_activation.c
  view/xdg_extras.c
)

target_sources(flakewm PRIVATE
  view/ky_toplevel.c
  view/dbus_toplevel.c
  view/hyprland_ipc.c
  view/ky_workspace.c
)

if(WLCOM_KDE_VIRTUAL_DESKTOP)
  target_sources(flakewm PRIVATE
    view/kde_virtual_desktop.c
  )
endif()

if(WLCOM_WLR_FOREIGN_TOPLEVEL)
  target_sources(flakewm PRIVATE
    view/wlr_foreign_toplevel.c
  )
endif()

if(WLCOM_WLR_LAYER_SHELL)
  target_sources(flakewm PRIVATE
    view/wlr_layer_shell.c
  )
endif()

if(WLCOM_KDE_PLASMA_SHELL)
  target_sources(flakewm PRIVATE
    view/kde_plasma_shell.c
  )
endif()

if(WLCOM_KDE_PLASMA_WINDOW_MANAGEMENT)
  target_sources(flakewm PRIVATE
    view/kde_plasma_window.c
  )
endif()

if(WLCOM_KDE_APPMENU)
  target_sources(flakewm PRIVATE
    view/kde_appmenu.c
  )
endif()

if(WLCOM_KDE_BLUR)
  target_sources(flakewm PRIVATE
    view/kde_blur.c
  )
endif()

if(WLCOM_KDE_SLIDE)
  target_sources(flakewm PRIVATE
    view/kde_slide.c
  )
endif()

if(WLCOM_UKUI_SHELL)
  target_sources(flakewm PRIVATE
    view/ukui_shell.c
  )
endif()

if(WLCOM_UKUI_WINDOW_MANAGEMENT)
  target_sources(flakewm PRIVATE
    view/ukui_window.c
  )
endif()

if(WLCOM_UKUI_BLUR)
  target_sources(flakewm PRIVATE
    view/ukui_blur.c
  )
endif()

# 从Treeland移植的部分DDE Shell协议的实现
target_sources(flakewm PRIVATE
  view/dde_shell.c
  view/treeland_dde_shell.c
  view/treeland_personalization.c
  view/treeland_color_scheme.c
)

# widget
target_sources(flakewm PRIVATE
  widget/menu.c
  widget/scaled_buffer.c
  widget/widget.c
)

# xwayland
if(HAVE_XWAYLAND)
  target_sources(flakewm PRIVATE
    xwayland/dnd.c
    xwayland/drag_x11.c
    xwayland/selection.c
    xwayland/unmanaged.c
    xwayland/view.c
    xwayland/xsettings.c
    xwayland/xwayland.c
  )
endif()

# shell/gxde/multitasking
#
# Compositor-native ports of Deepin KWin's multitask view and Present Windows
# effect. buttons/ holds the upstream button icons; not all are used yet.
set(MULTITASK_SOURCE_ASSET_DIR
  "${CMAKE_CURRENT_SOURCE_DIR}/shell/gxde/multitasking/buttons"
)
set(MULTITASK_INSTALL_ASSET_DIR
  "${CMAKE_INSTALL_FULL_DATADIR}/flakewm/multitask/buttons"
)
file(CONFIGURE
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/shell/gxde/multitasking/multitask_assets.h"
  CONTENT [[
#pragma once

#define MULTITASK_INSTALL_ASSET_DIR "@MULTITASK_INSTALL_ASSET_DIR@"

#define MULTITASK_SOURCE_ASSET_DIR "@MULTITASK_SOURCE_ASSET_DIR@"
]]
  @ONLY
)

install(DIRECTORY shell/gxde/multitasking/buttons
  DESTINATION ${CMAKE_INSTALL_DATADIR}/flakewm/multitask
)

target_sources(flakewm PRIVATE
  shell/gxde/multitasking/wlcom_multitask.c
  shell/gxde/multitasking/wlcom_multitask_dbus.c
  shell/gxde/multitasking/wlcom_presentwindows.c
)

# vendor/gxde/portal
target_sources(flakewm PRIVATE
  vendor/gxde/portal/portal.c
  vendor/gxde/portal/portal_dbus.c
)

target_include_directories(flakewm PRIVATE
  "${PROJECT_SOURCE_DIR}"
  "${PROJECT_BINARY_DIR}"
  "${PROJECT_SOURCE_DIR}/include"
)

target_link_libraries(flakewm PRIVATE
  absl::log
  absl::log_entry
  absl::log_globals
  absl::log_initialize
  absl::log_sink
  absl::log_sink_registry
  absl::raw_logging_internal
  wlcom-protocols
  Wlroots::wlroots
  PkgConfig::WAYLAND_SERVER
  PkgConfig::WLCOM_DEPS
  PkgConfig::SDBUS
  Threads::Threads
  m
  dl
  rt
)

if(HAVE_NLS)
  target_link_libraries(flakewm PRIVATE Intl::Intl)
endif()

if(HAVE_XWAYLAND)
  target_link_libraries(flakewm PRIVATE PkgConfig::WLCOM_XWAYLAND_DEPS)
endif()

if(HAVE_LIBUNWIND)
  target_link_libraries(flakewm PRIVATE PkgConfig::LIBUNWIND)
else()
  target_include_directories(flakewm PRIVATE ${Backtrace_INCLUDE_DIRS})
  target_link_libraries(flakewm PRIVATE ${Backtrace_LIBRARIES})
endif()

if(HAVE_LIBEIS)
  target_link_libraries(flakewm PRIVATE PkgConfig::LIBEIS)
endif()

if(WLCOM_TRACY)
  target_link_libraries(flakewm PRIVATE PkgConfig::TRACY)
endif()

# Plugins resolve compositor symbols at load time, see wlcom.syms.
set(wlcom_symbols_file "${PROJECT_SOURCE_DIR}/wlcom.syms")
if(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
  # also keeps environ and __progname, which live in the executable there
  set(wlcom_symbols_file "${PROJECT_SOURCE_DIR}/wlcom.freebsd.syms")
endif()
set_target_properties(flakewm PROPERTIES
  # Keep ./build/flakewm working for nested test runs.
  RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}"
  ENABLE_EXPORTS ON
  LINK_DEPENDS "${wlcom_symbols_file}"
)
target_link_options(flakewm PRIVATE
  "LINKER:--version-script,${wlcom_symbols_file}"
)

install(TARGETS flakewm RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(WLCOM_PRIVATE_LIBDIR)
  file(RELATIVE_PATH _wlcom_private_lib_rel
    "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}"
    "${CMAKE_INSTALL_PREFIX}/${WLCOM_PRIVATE_LIBDIR}")
  set_property(TARGET flakewm PROPERTY INSTALL_RPATH "\$ORIGIN/${_wlcom_private_lib_rel}")
endif()

# shell/gxde/kwin_shim
#
# A Qt shim that stands in for the parts of KWin the gxde-kwin QML frontends
# (shell/gxde/window_switcher) use, backed by flakewm through libkywc.
if(WLCOM_LIBKYWC)
  # thumbnail_item uses <qpa/...>. Qt >= 6.9 ships the private module as its
  # own package, older releases create Qt6::GuiPrivate along with Qt6::Gui.
  find_package(Qt6 QUIET COMPONENTS Core DBus Gui Quick Qml
    OPTIONAL_COMPONENTS GuiPrivate
  )
  if(Qt6_FOUND AND TARGET Qt6::GuiPrivate)
    pkg_check_modules(KWIN_SHIM_DEPS REQUIRED IMPORTED_TARGET
      egl glesv2 libdrm
    )

    qt6_add_resources(kwin_shim_resources shell/gxde/kwin_shim/resources.qrc)

    add_library(occultation-exoskeleton STATIC
      shell/gxde/kwin_shim/exoskeleton/exoskeleton.cc
      shell/gxde/kwin_shim/exoskeleton/exoskeleton.h
      shell/gxde/kwin_shim/icon_item/icon_item.cc
      shell/gxde/kwin_shim/icon_item/icon_item.h
      shell/gxde/kwin_shim/registration/registration.cc
      shell/gxde/kwin_shim/switcher_item/switcher_item.cc
      shell/gxde/kwin_shim/switcher_item/switcher_item.h
      shell/gxde/kwin_shim/switcher_model/switcher_model.cc
      shell/gxde/kwin_shim/switcher_model/switcher_model.h
      shell/gxde/kwin_shim/thumbnail_item/thumbnail_item.cc
      shell/gxde/kwin_shim/thumbnail_item/thumbnail_item.h
      ${kwin_shim_resources}
    )
    set_target_properties(occultation-exoskeleton PROPERTIES AUTOMOC ON)
    target_compile_options(occultation-exoskeleton PUBLIC -Wno-unused-parameter)
    target_include_directories(occultation-exoskeleton PUBLIC
      "${PROJECT_SOURCE_DIR}"
    )
    target_link_libraries(occultation-exoskeleton PUBLIC
      Qt6::Core
      Qt6::DBus
      Qt6::Gui
      Qt6::GuiPrivate
      Qt6::Quick
      Qt6::Qml
      kywc
      PkgConfig::KWIN_SHIM_DEPS
    )

    add_executable(occultation-qml-smoke EXCLUDE_FROM_ALL
      shell/gxde/kwin_shim/smoke/smoke.cc
    )
    target_link_libraries(occultation-qml-smoke PRIVATE occultation-exoskeleton)

    add_executable(occultation-switcher-preview EXCLUDE_FROM_ALL
      shell/gxde/kwin_shim/preview/preview.cc
      shell/gxde/kwin_shim/preview_model/preview_model.cc
      shell/gxde/kwin_shim/preview_model/preview_model.h
      shell/gxde/kwin_shim/preview_switcher/preview_switcher.cc
      shell/gxde/kwin_shim/preview_switcher/preview_switcher.h
      shell/gxde/kwin_shim/preview_thumbnail_item/preview_thumbnail_item.cc
      shell/gxde/kwin_shim/preview_thumbnail_item/preview_thumbnail_item.h
    )
    set_target_properties(occultation-switcher-preview PROPERTIES AUTOMOC ON)
    target_link_libraries(occultation-switcher-preview PRIVATE
      occultation-exoskeleton
    )

    # occultation-qml-smoke is not built by default; build it on demand before
    # the test runs.
    add_test(NAME occultation-qml-smoke-build
      COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}"
        --target occultation-qml-smoke --config $<CONFIG>
    )
    set_tests_properties(occultation-qml-smoke-build PROPERTIES
      FIXTURES_SETUP occultation-qml-smoke-binary
    )
    add_test(NAME occultation-qml-smoke COMMAND occultation-qml-smoke)
    set_tests_properties(occultation-qml-smoke PROPERTIES
      LABELS occultation
      FIXTURES_REQUIRED occultation-qml-smoke-binary
      ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software"
    )
  else()
    message(WARNING "Qt 6 Quick was not found; the KWin shim will not be built")
  endif()
endif()
