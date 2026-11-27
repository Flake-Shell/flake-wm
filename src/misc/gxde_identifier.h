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
// The gxde_identifier_v1 global, which tells GXDE clients that they run on
// flakewm and which version. Implemented in C++, called from the C server
// code, hence the C linkage.

#ifndef SRC_MISC_GXDE_IDENTIFIER_H_
#define SRC_MISC_GXDE_IDENTIFIER_H_

#ifdef __cplusplus
extern "C" {
#else
#include <stdbool.h>
#endif

// NOLINTNEXTLINE(readability-identifier-naming): the C compositor's type
struct server;

// Creates the global on the server's display. It lives until the display is
// destroyed. Returns false if it could not be created. Named like the C code
// that calls it.
// NOLINTNEXTLINE(readability-identifier-naming)
bool gxde_identifier_create(struct server* server);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // SRC_MISC_GXDE_IDENTIFIER_H_
