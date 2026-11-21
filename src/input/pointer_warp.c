// SPDX-FileCopyrightText: 2026 GXDE OS
//
// SPDX-License-Identifier: GPL-1.0-or-later

/**
 * wp_pointer_warp_v1: a client moves the pointer to a position on one of its
 * surfaces. Honored only when the client has pointer focus (an implicit grab
 * keeps the focus), the serial is one sent to that client, and the position is
 * inside the surface. Nothing is done while the session is locked or a
 * compositor grab (move, resize, switcher ...) owns the pointer.
 */

#include <stdlib.h>

#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_seat.h>

#include <kywc/log.h>

#include "input_p.h"
#include "pointer-warp-v1-protocol.h"
#include "server.h"
#include "view/session_lock.h"
#include "xwayland.h"

#define POINTER_WARP_VERSION 1

struct pointer_warp_manager {
    struct wl_global *global;
    struct wl_listener display_destroy;
};

static struct pointer_warp_manager *manager = NULL;

static void handle_warp_pointer(struct wl_client *client, struct wl_resource *resource,
                                struct wl_resource *surface_resource,
                                struct wl_resource *pointer_resource, wl_fixed_t x, wl_fixed_t y,
                                uint32_t serial)
{
    struct wlr_surface *surface = wlr_surface_from_resource(surface_resource);
    /* NULL if the wl_pointer is inert, e.g. its seat is gone */
    struct wlr_seat_client *seat_client = wlr_seat_client_from_pointer_resource(pointer_resource);
    if (!seat_client || seat_client->client != client ||
        wl_resource_get_client(surface->resource) != client) {
        return;
    }

    struct seat *seat = seat_from_wlr_seat(seat_client->seat);
    if (!seat || !seat->cursor || session_lock_is_active() || seat->pointer_grab) {
        return;
    }

    if (seat_client->seat->pointer_state.focused_client != seat_client ||
        !wlr_seat_client_validate_event_serial(seat_client, serial)) {
        kywc_log(KYWC_DEBUG, "rejecting pointer warp from unfocused client or wrong serial");
        return;
    }

    double sx = wl_fixed_to_double(x);
    double sy = wl_fixed_to_double(y);
    if (xwayland_check_client(client)) {
        sx = xwayland_unscale((int)sx);
        sy = xwayland_unscale((int)sy);
    }

    if (sx < 0 || sy < 0 || sx >= surface->current.width || sy >= surface->current.height) {
        kywc_log(KYWC_DEBUG, "rejecting pointer warp outside of the surface");
        return;
    }

    cursor_warp_to_surface(seat->cursor, surface, sx, sy);
}

static void handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
    wl_resource_destroy(resource);
}

static const struct wp_pointer_warp_v1_interface pointer_warp_impl = {
    .destroy = handle_destroy,
    .warp_pointer = handle_warp_pointer,
};

static void pointer_warp_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    struct wl_resource *resource =
        wl_resource_create(client, &wp_pointer_warp_v1_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &pointer_warp_impl, NULL, NULL);
}

static void handle_display_destroy(struct wl_listener *listener, void *data)
{
    wl_list_remove(&manager->display_destroy.link);
    wl_global_destroy(manager->global);
    free(manager);
    manager = NULL;
}

bool pointer_warp_manager_create(struct input_manager *input_manager)
{
    manager = calloc(1, sizeof(*manager));
    if (!manager) {
        return false;
    }

    struct wl_display *display = input_manager->server->display;
    manager->global = wl_global_create(display, &wp_pointer_warp_v1_interface, POINTER_WARP_VERSION,
                                       NULL, pointer_warp_bind);
    if (!manager->global) {
        kywc_log(KYWC_WARN, "wp_pointer_warp_v1 create failed");
        free(manager);
        manager = NULL;
        return false;
    }

    manager->display_destroy.notify = handle_display_destroy;
    wl_display_add_destroy_listener(display, &manager->display_destroy);

    return true;
}
