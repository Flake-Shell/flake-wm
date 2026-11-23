# Copyright (C) 2026 CharOfString <root@charofstring.cc>
#
# This program is free software: you can redistribute it and/or modify it under
# the terms of the GNU General Public License as published by the Free Software
# Foundation, either version 3 of the License, or (at your option) any later
# version.
#
# This program is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
# FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
# details.
#
# You should have received a copy of the GNU General Public License along with
# this program. If not, see <https://www.gnu.org/licenses/>. 
# -----------------------------------------------------------------------------
# A cmake helper that installs the file under the data/ folder.

set(WLCOM_DATA_DIR "${PROJECT_SOURCE_DIR}/data")

# the fbdev backend's device permissions
if(HAVE_FBDEV)
  install(FILES "${WLCOM_DATA_DIR}/udev-rules/90-framebuffer.rules"
    DESTINATION /usr/lib/udev/rules.d
    RENAME 90-flakewm-framebuffer.rules
  )
endif()

# the GXDE session runs startdde, there is no GXDE outside Linux
if(WLCOM_GXDE_SESSION)
  install(FILES "${WLCOM_DATA_DIR}/entries/gxde-flakewm.desktop"
    DESTINATION /usr/share/wayland-sessions
  )

  install(PROGRAMS "${WLCOM_DATA_DIR}/scripts/startflakewm"
    DESTINATION /usr/bin
  )
endif()

install(FILES "${WLCOM_DATA_DIR}/default_preferences/config.json"
  DESTINATION ${WLCOM_SYSCONFDIR}
)

set(GETTEXT_PACKAGE "${PROJECT_NAME}")
configure_file(
  "${WLCOM_DATA_DIR}/schemas/org.flakewm.peripherals-mouse.gschema.xml"
  "${PROJECT_BINARY_DIR}/data/schemas/org.flakewm.peripherals-mouse.gschema.xml"
  @ONLY
)

install(FILES
  "${PROJECT_BINARY_DIR}/data/schemas/org.flakewm.peripherals-mouse.gschema.xml"
  DESTINATION ${CMAKE_INSTALL_DATADIR}/glib-2.0/schemas
)
