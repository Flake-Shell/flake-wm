// SPDX-FileCopyrightText: 2026 CharOfString <root@charofstring.cc>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef _VIEW_HYPRLAND_IPC_H_
#define _VIEW_HYPRLAND_IPC_H_

#include <stdbool.h>
#include <stdint.h>

struct input;
struct wlr_surface;

/*
 * State the Hyprland IPC (src/view/hyprland_ipc.c) cannot observe through
 * signals. All of these are no-ops unless flakewm was started with
 * --hypr-unix-socket-sim-enable.
 */

/* what a screencast shares, values of Hyprland's eScreenshareType */
enum hyprland_ipc_share_type {
    HYPRLAND_IPC_SHARE_MONITOR = 1,
    HYPRLAND_IPC_SHARE_WINDOW = 2,
    HYPRLAND_IPC_SHARE_REGION = 3,
    HYPRLAND_IPC_SHARE_WORKSPACE = 4,
};

/* a capture session started (active) or ended, name is the monitor, window title or workspace */
void hyprland_ipc_screencast(bool active, enum hyprland_ipc_share_type type, const char *name);

/* xdg_system_bell_v1 ring, surface may be NULL */
void hyprland_ipc_bell(struct wlr_surface *surface);

/* xdg_toplevel_tag_v1, tag or description NULL leaves that one as it is */
void hyprland_ipc_toplevel_tag(struct wlr_surface *surface, const char *tag,
                               const char *description);

/* a keyboard switched to another layout (xkb group) */
void hyprland_ipc_keyboard_layout(struct input *input, uint32_t group);

#endif /* _VIEW_HYPRLAND_IPC_H_ */
