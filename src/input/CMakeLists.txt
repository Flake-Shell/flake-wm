target_sources(flakewm PRIVATE
  action.c
  binding.c
  config.c
  cursor.c
  cursor_shape.c
  event.c
  gesture.c
  hotkey.c
  emergency_term.c
  hyprland_focus_grab.c
  hyprland_global_shortcuts.c
  idle.c
  idle_inhibit.c
  input.c
  text_input.c
  keyboard.c
  keyboard_group.c
  libinput.c
  monitor.c
  pointer_warp.c
  tablet.c
  text_input_v1.c
  text_input_v2.c
  toplevel_drag.c
  touch.c
  transient_seat.c
  vicinae_hotkey.c
  seat.c
  selection.c
  selection_data.c
)

if(HAVE_LIBEIS)
  target_sources(flakewm PRIVATE
    hyprland_input_capture.c
  )
endif()

if(WLCOM_KDE_KEYSTATE)
  target_sources(flakewm PRIVATE
    kde_keystate.c
  )
endif()

if(WLCOM_CLIPBOARD_PERSIST)
  target_sources(flakewm PRIVATE
    selection_persist.c
  )
endif()
