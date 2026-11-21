// SPDX-FileCopyrightText: 2024 KylinSoft Co., Ltd.
//
// SPDX-License-Identifier: GPL-1.0-or-later

#ifndef _BACKEND_FBDEV_H_
#define _BACKEND_FBDEV_H_

#include <wlr/backend.h>
#include <wlr/types/wlr_output.h>

#if HAVE_FBDEV
struct wlr_backend *fbdev_backend_create(struct wl_display *display, struct wlr_session *session,
                                         const char **devices, int devices_len);

bool wlr_backend_is_fbdev(struct wlr_backend *backend);

bool wlr_output_is_fbdev(struct wlr_output *wlr_output);
#else
/* no Linux framebuffer interface (FreeBSD): there is never an fbdev backend */
static inline struct wlr_backend *fbdev_backend_create(struct wl_display *display,
                                                       struct wlr_session *session,
                                                       const char **devices, int devices_len)
{
    return NULL;
}

static inline bool wlr_backend_is_fbdev(struct wlr_backend *backend)
{
    return false;
}

static inline bool wlr_output_is_fbdev(struct wlr_output *wlr_output)
{
    return false;
}
#endif

#endif /* _BACKEND_FBDEV_H_ */
