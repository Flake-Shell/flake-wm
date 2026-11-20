// SPDX-FileCopyrightText: 2026 CharOfString <root@charofstring.cc>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef _SESSION_LOCK_H_
#define _SESSION_LOCK_H_

#include <stdbool.h>

struct server;
struct wlr_surface;

bool session_lock_manager_create(struct server *server);

bool session_lock_is_active(void);

bool session_lock_surface_is_allowed(struct wlr_surface *surface);

struct wlr_surface *session_lock_get_focus_surface(void);

/* hyprland_lock_notifier_v1, fed by the session lock */
bool hyprland_lock_notifier_create(struct server *server);

void hyprland_lock_notify_set_locked(bool locked);

#endif  /* _SESSION_LOCK_H_ */
