// SPDX-FileCopyrightText: 2026 GXDE OS
//
// SPDX-License-Identifier: GPL-1.0-or-later

/**
 * wp_cursor_shape_manager_v1: clients set the cursor by shape name instead of
 * attaching a cursor surface, the image comes from the seat's xcursor theme.
 * The same focus rules as wl_pointer.set_cursor and the tablet tool
 * set_cursor apply.
 */

#include <stdlib.h>

#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_tablet_v2.h>

#include <kywc/log.h>

#include "input_p.h"
#include "server.h"

#define CURSOR_SHAPE_MANAGER_VERSION 2

struct cursor_shape_manager {
    struct wlr_cursor_shape_manager_v1 *manager;
    struct wl_listener request_set_shape;
    struct wl_listener destroy;
};

static bool tablet_tool_client_may_set_cursor(struct cursor *cursor,
                                              struct wlr_tablet_v2_tablet_tool *tool,
                                              struct wlr_seat_client *seat_client)
{
    if (cursor->seat->pointer_grab || cursor->hidden || cursor->image_locks > 0) {
        return false;
    }

    struct wlr_surface *focused_surface = tool ? tool->focused_surface : NULL;
    return focused_surface &&
           wl_resource_get_client(focused_surface->resource) == seat_client->client;
}

static void handle_request_set_shape(struct wl_listener *listener, void *data)
{
    struct wlr_cursor_shape_manager_v1_request_set_shape_event *event = data;
    struct seat *seat = seat_from_wlr_seat(event->seat_client->seat);
    if (!seat || !seat->cursor) {
        return;
    }

    struct cursor *cursor = seat->cursor;
    bool allowed = false;
    switch (event->device_type) {
    case WLR_CURSOR_SHAPE_MANAGER_V1_DEVICE_TYPE_POINTER:
        allowed = cursor_pointer_client_may_set_cursor(cursor, event->seat_client);
        break;
    case WLR_CURSOR_SHAPE_MANAGER_V1_DEVICE_TYPE_TABLET_TOOL:
        allowed = tablet_tool_client_may_set_cursor(cursor, event->tablet_tool, event->seat_client);
        break;
    }
    if (!allowed) {
        kywc_log(KYWC_DEBUG, "denying cursor shape request from unfocused client");
        return;
    }

    cursor_set_client_xcursor(cursor, wlr_cursor_shape_v1_name(event->shape),
                              event->seat_client->client);
}

static void handle_destroy(struct wl_listener *listener, void *data)
{
    struct cursor_shape_manager *shape_manager = wl_container_of(listener, shape_manager, destroy);
    wl_list_remove(&shape_manager->request_set_shape.link);
    wl_list_remove(&shape_manager->destroy.link);
    free(shape_manager);
}

bool cursor_shape_manager_create(struct input_manager *input_manager)
{
    struct cursor_shape_manager *shape_manager = calloc(1, sizeof(*shape_manager));
    if (!shape_manager) {
        return false;
    }

    shape_manager->manager = wlr_cursor_shape_manager_v1_create(input_manager->server->display,
                                                                CURSOR_SHAPE_MANAGER_VERSION);
    if (!shape_manager->manager) {
        kywc_log(KYWC_WARN, "wp_cursor_shape_manager_v1 create failed");
        free(shape_manager);
        return false;
    }

    shape_manager->request_set_shape.notify = handle_request_set_shape;
    wl_signal_add(&shape_manager->manager->events.request_set_shape,
                  &shape_manager->request_set_shape);
    shape_manager->destroy.notify = handle_destroy;
    wl_signal_add(&shape_manager->manager->events.destroy, &shape_manager->destroy);

    return true;
}
