// SPDX-FileCopyrightText: 2026 CharOfString <root@charofstring.cc>
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Replaces hyprland/src/hyprpaper/Hyprpaper.cpp, which talks to hyprpaper
// over hyprwire. flakewm has no hyprpaper, and hyprwire is not packaged for
// the distributions flakewm builds on.

#include <expected>
#include <string>
#include <string_view>

#include "hyprpaper/Hyprpaper.hpp"

std::expected<void, std::string> Hyprpaper::makeHyprpaperRequest(
    const std::string_view& rq) {
  (void)rq;
  return std::unexpected("hyprpaper is not supported by flakectl");
}
