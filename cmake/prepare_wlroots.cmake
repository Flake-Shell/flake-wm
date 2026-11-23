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
# Historical 0.17 overlay helper. The active 0.20.2 build does not invoke this
# script; retained only while auditing the old fork.

if(NOT DEFINED WLCOM_WLROOTS_VENDOR_SOURCE_DIR)
  message(FATAL_ERROR "WLCOM_WLROOTS_VENDOR_SOURCE_DIR is required")
endif()

if(NOT DEFINED WLCOM_WLROOTS_SOURCE_DIR)
  message(FATAL_ERROR "WLCOM_WLROOTS_SOURCE_DIR is required")
endif()

if(NOT DEFINED WLCOM_WLROOTS_OVERLAY_DIR)
  message(FATAL_ERROR "WLCOM_WLROOTS_OVERLAY_DIR is required")
endif()

file(REMOVE_RECURSE "${WLCOM_WLROOTS_SOURCE_DIR}")
file(MAKE_DIRECTORY "${WLCOM_WLROOTS_SOURCE_DIR}")
file(COPY "${WLCOM_WLROOTS_VENDOR_SOURCE_DIR}/"
  DESTINATION "${WLCOM_WLROOTS_SOURCE_DIR}"
)

file(COPY "${WLCOM_WLROOTS_OVERLAY_DIR}/"
  DESTINATION "${WLCOM_WLROOTS_SOURCE_DIR}"
)
