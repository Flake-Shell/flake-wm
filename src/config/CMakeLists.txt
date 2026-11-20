target_sources(flakewm PRIVATE
  common.c
  config.c
  gtk_theme.c
)

if(WLCOM_KDE_GLOBAL_ACCEL)
  add_executable(qtkey-mapper qtkey_mapper.c)
  if(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
    # the xkbcommon keysym header is under /usr/local/include there
    target_include_directories(qtkey-mapper PRIVATE
      $<TARGET_PROPERTY:PkgConfig::WLCOM_DEPS,INTERFACE_INCLUDE_DIRECTORIES>
    )
  endif()
  wlcom_capture_header(flakewm qtkey_map_table.h qtkey-mapper)

  target_sources(flakewm PRIVATE
    kde_global_accel.c
  )
endif()

if(WLCOM_KDE_INPUT)
  target_sources(flakewm PRIVATE
    kde_input.c
  )
endif()

if(WLCOM_UKUI_SHORTCUT)
  target_sources(flakewm PRIVATE
    ukui_shortcut.c
  )
endif()

if(WLCOM_UKUI_GSETTINGS)
  target_sources(flakewm PRIVATE
    ukui_gsettings.c
  )
endif()

if(WLCOM_UKUI_VIEW_MODE)
  target_sources(flakewm PRIVATE
    ukui_view_mode.c
  )
endif()
