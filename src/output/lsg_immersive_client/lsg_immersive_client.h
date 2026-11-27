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
// --lsg-immersive-client: FlakeWM nested in gxde-lsg's immersive session asks
// the host compositor, through the dde_shell protocol, to drop the titlebar
// and the border of its output windows and to make them fullscreen.
//
// Implemented in C++, driven by the C output code, hence the C linkage. The
// C side reads struct wlr_output: its wlroots header pulls in C-only array
// parameters ([static N]) that C++ cannot compile.

#ifndef SRC_OUTPUT_LSG_IMMERSIVE_CLIENT_LSG_IMMERSIVE_CLIENT_H_
#define SRC_OUTPUT_LSG_IMMERSIVE_CLIENT_LSG_IMMERSIVE_CLIENT_H_

#ifdef __cplusplus
extern "C" {
#endif

// NOLINTNEXTLINE(readability-identifier-naming): the C compositor's types
struct wlr_backend;
// NOLINTNEXTLINE(readability-identifier-naming)
struct wlr_output;

/// The dde_shell state of one nested output, opaque to C.
// NOLINTNEXTLINE(readability-identifier-naming)
struct lsg_immersive_client;

/**
 * @brief Asks the host to undecorate the window of a new nested output.
 *
 * Call it when the output is created, before its first commit maps the host
 * window.
 *
 * @param wlr_output The new output.
 * @param backend The backend of the output.
 * @param name The name of the output, for the log.
 * @return The state to pass to the other functions, or NULL when there is
 * nothing to do: not a wayland backend output, or a host without dde_shell.
 */
// NOLINTNEXTLINE(readability-identifier-naming)
struct lsg_immersive_client* lsg_immersive_client_create(
    struct wlr_output* wlr_output, struct wlr_backend* backend,
    const char* name);

/**
 * @brief Tells that the output committed a buffer.
 *
 * The first one maps the host window; fullscreen is asked for after it.
 *
 * @param client The state, NULL is ignored.
 */
// NOLINTNEXTLINE(readability-identifier-naming)
void lsg_immersive_client_buffer_committed(struct lsg_immersive_client* client);

/**
 * @brief Frees the state. Call it when the output is destroyed.
 * @param client The state, NULL is ignored.
 */
// NOLINTNEXTLINE(readability-identifier-naming)
void lsg_immersive_client_destroy(struct lsg_immersive_client* client);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // SRC_OUTPUT_LSG_IMMERSIVE_CLIENT_LSG_IMMERSIVE_CLIENT_H_
