// Copyright (C) 2026 CharOfString <root@charofstring.cc>
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version.
//
// This program is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
// details.
//
// You should have received a copy of the GNU General Public License along with
// this program. If not, see <https://www.gnu.org/licenses/>.
// -----------------------------------------------------------------------------
// The one place where C++ sources include the compositor's C headers. Those
// headers have no extern "C" guards of their own, so their declarations get C
// linkage here. Add a header to this list when C++ code starts using it, and
// remove it once the C code behind it has been ported.

#ifndef SRC_C_COMPACT_H_
#define SRC_C_COMPACT_H_

#ifdef __cplusplus
extern "C" {
#endif
// IWYU pragma: begin_exports
#include "include/server.h"
#include "include/util/debug.h"
#include "include/util/limit.h"
#include "include/util/logger.h"
#include "include/util/spawn.h"
#include "include/xwayland.h"
// IWYU pragma: end_exports
#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // SRC_C_COMPACT_H_
