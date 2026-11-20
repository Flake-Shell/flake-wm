target_sources(flakewm PRIVATE
  action.c
  binding.c
  config.c
  cursor.c
  cursor_shape.c
  event.c
  gesture.c
  emergency_term.c
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
  seat.c
  selection.c
  selection_data.c
)

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
