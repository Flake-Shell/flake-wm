# the fbdev backend's device permissions
if(HAVE_FBDEV)
  install(FILES udev-rules/90-framebuffer.rules
    DESTINATION /usr/lib/udev/rules.d
    RENAME 90-flakewm-framebuffer.rules
  )
endif()

# the GXDE session runs startdde, there is no GXDE outside Linux
if(WLCOM_GXDE_SESSION)
  install(FILES gxde-flakewm.desktop
    DESTINATION /usr/share/wayland-sessions
  )

  install(PROGRAMS startflakewm
    DESTINATION /usr/bin
  )
endif()

install(FILES config.json
  DESTINATION ${WLCOM_SYSCONFDIR}
)

set(GETTEXT_PACKAGE "${PROJECT_NAME}")
configure_file(
  org.flakewm.peripherals-mouse.gschema.xml
  org.flakewm.peripherals-mouse.gschema.xml
  @ONLY
)

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/org.flakewm.peripherals-mouse.gschema.xml"
  DESTINATION ${CMAKE_INSTALL_DATADIR}/glib-2.0/schemas
)
