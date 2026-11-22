# SPDX-FileCopyrightText: 2026 CharOfString <root@charofstring.cc>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# flakectl: Hyprland's hyprctl (hyprland/, BSD-3-Clause), built against
# the vendored hyprutils parts (hyprutils/, BSD-3-Clause). See README.md.

if(NOT CMAKE_CXX_COMPILER)
  message(FATAL_ERROR "flakectl needs a C++26 compiler, or pass -DWLCOM_FLAKECTL=OFF")
endif()
if(CMAKE_VERSION VERSION_LESS 3.25)
  message(FATAL_ERROR "flakectl needs CMake >= 3.25 for C++26, or pass -DWLCOM_FLAKECTL=OFF")
endif()

pkg_check_modules(FLAKECTL_DEPS REQUIRED IMPORTED_TARGET re2 readline)

add_executable(flakectl
  hyprland/src/main.cpp
  hyprutils/src/string/String.cpp
  hyprpaper_stub.cpp
)
target_include_directories(flakectl PRIVATE
  hyprland/src
  hyprutils/include
)
target_link_libraries(flakectl PRIVATE PkgConfig::FLAKECTL_DEPS)
# Same as upstream: Hyprland builds with C++26 and hidden visibility.
set_target_properties(flakectl PROPERTIES
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
  CXX_VISIBILITY_PRESET hidden
)

# Upstream code is kept unmodified, silence what our warning flags add.
target_compile_options(flakectl PRIVATE -Wno-unused-parameter)

install(TARGETS flakectl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
