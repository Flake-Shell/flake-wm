// SPDX-FileCopyrightText: CharOfString <root@charofstring.cc>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdlib.h>

#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>

#include <kywc/log.h>

#include "hyprland-focus-grab-v1-protocol.h"
#include "input/cursor.h"
#include "input/seat.h"
#include "input_p.h"
#include "scene/surface.h"
#include "server.h"
#include "view/session_lock.h"
#include "view/view.h"

#define HYPRLAND_FOCUS_GRAB_VERSION 1

enum grab_surface_state {
    GRAB_SURFACE_PENDING_ADD,
    GRAB_SURFACE_PENDING_REMOVE,
    GRAB_SURFACE_COMMITTED,
};

struct focus_grab;

struct grab_surface {
    struct wl_list link;  // focus_grab.surfaces
    struct focus_grab *grab;
    struct wlr_surface *surface;
    enum grab_surface_state state;
    struct wl_listener destroy;
};

struct focus_grab {
    struct wl_resource *resource;
    struct wl_list surfaces;  // grab_surface.link
    bool active;
};

static struct focus_grab_manager {
    struct wl_global *global;
    struct input_manager *input_manager;

    struct focus_grab *active;
    struct seat *seat;  // seat of the active grab
    struct wl_listener seat_destroy;

    /* a press outside the whitelist ended the grab, swallow its release too */
    struct seat *eaten_seat;
    uint32_t eaten_button;

    struct wl_listener display_destroy;
} *manager;

static void grab_surface_destroy(struct grab_surface *gs)
{
    wl_list_remove(&gs->destroy.link);
    wl_list_remove(&gs->link);
    free(gs);
}

static struct grab_surface *grab_find_surface(struct focus_grab *grab, struct wlr_surface *surface)
{
    struct grab_surface *gs;
    wl_list_for_each(gs, &grab->surfaces, link) {
        if (gs->surface == surface) {
            return gs;
        }
    }
    return NULL;
}

static bool grab_has_committed(struct focus_grab *grab, struct wlr_surface *surface)
{
    struct grab_surface *gs = grab_find_surface(grab, surface);
    return gs && gs->state == GRAB_SURFACE_COMMITTED;
}

static bool grab_accepts(struct focus_grab *grab, struct wlr_surface *surface)
{
    /* bounded walk up subsurfaces and popup parents */
    for (int depth = 0; surface && depth < 32; depth++) {
        if (grab_has_committed(grab, surface)) {
            return true;
        }
        struct wlr_surface *root = wlr_surface_get_root_surface(surface);
        if (root != surface) {
            surface = root;
            continue;
        }
        struct wlr_xdg_popup *popup = wlr_xdg_popup_try_from_wlr_surface(surface);
        surface = popup ? popup->parent : NULL;
    }
    return false;
}

static struct wlr_surface *grab_first_committed(struct focus_grab *grab)
{
    struct grab_surface *gs;
    wl_list_for_each(gs, &grab->surfaces, link) {
        if (gs->state == GRAB_SURFACE_COMMITTED) {
            return gs->surface;
        }
    }
    return NULL;
}

/* bring keyboard and pointer focus inside the whitelist */
static void grab_refocus(struct focus_grab *grab, struct seat *seat)
{
    struct wlr_seat *wlr_seat = seat->wlr_seat;

    struct wlr_surface *keyboard_focus = wlr_seat->keyboard_state.focused_surface;
    if (!keyboard_focus || !grab_accepts(grab, keyboard_focus)) {
        seat_focus_surface(seat, grab_first_committed(grab));
    }

    struct wlr_surface *pointer_focus = wlr_seat->pointer_state.focused_surface;
    if (pointer_focus && !grab_accepts(grab, pointer_focus)) {
        wlr_seat_pointer_notify_clear_focus(wlr_seat);
    }
    /* enter a whitelisted surface already under the cursor */
    if (!seat->pointer_grab) {
        cursor_rebase(seat->cursor);
    }
}

static void grab_finish(struct focus_grab *grab, bool send_cleared, bool restore,
                        struct wlr_surface *dying)
{
    if (!grab->active) {
        return;
    }
    grab->active = false;

    struct seat *seat = manager->seat;
    manager->active = NULL;
    manager->seat = NULL;
    wl_list_remove(&manager->seat_destroy.link);
    wl_list_init(&manager->seat_destroy.link);

    struct grab_surface *gs, *tmp;
    wl_list_for_each_safe(gs, tmp, &grab->surfaces, link) {
        grab_surface_destroy(gs);
    }

    if (send_cleared && grab->resource) {
        hyprland_focus_grab_v1_send_cleared(grab->resource);
    }

    if (!restore || session_lock_is_active() || seat->pointer_grab || seat->keyboard_grab) {
        return;
    }

    struct wlr_surface *keyboard_focus = seat->wlr_seat->keyboard_state.focused_surface;
    struct wlr_layer_surface_v1 *layer = NULL;
    if (keyboard_focus && keyboard_focus != dying) {
        layer = wlr_layer_surface_v1_try_from_wlr_surface(keyboard_focus);
    }
    if (!layer ||
        layer->current.keyboard_interactive == ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE) {
        struct view *view = view_manager_get_activated();
        if (view) {
            view_set_focus(view, seat);
        } else {
            seat_focus_surface(seat, NULL);
        }
    }
    cursor_rebase(seat->cursor);
}

static void grab_start(struct focus_grab *grab)
{
    struct seat *seat = input_manager_get_default_seat();
    if (!seat) {
        return;
    }

    if (!grab->active) {
        /* no grab over the lock screen */
        if (session_lock_is_active()) {
            struct grab_surface *gs, *tmp;
            wl_list_for_each_safe(gs, tmp, &grab->surfaces, link) {
                grab_surface_destroy(gs);
            }
            hyprland_focus_grab_v1_send_cleared(grab->resource);
            return;
        }
        if (manager->active) {
            grab_finish(manager->active, true, false, NULL);
        }
        grab->active = true;
        manager->active = grab;
        manager->seat = seat;
        wl_list_remove(&manager->seat_destroy.link);
        wl_signal_add(&seat->events.destroy, &manager->seat_destroy);
    }

    grab_refocus(grab, seat);
}

/* apply pending changes; a destroyed surface applies its own removal only */
static void grab_commit(struct focus_grab *grab, struct wlr_surface *dying)
{
    bool remove_only = dying != NULL;
    bool changed = false, any_committed = false;

    struct grab_surface *gs, *tmp;
    wl_list_for_each_safe(gs, tmp, &grab->surfaces, link) {
        switch (gs->state) {
        case GRAB_SURFACE_PENDING_REMOVE:
            grab_surface_destroy(gs);
            changed = true;
            break;
        case GRAB_SURFACE_PENDING_ADD:
            if (!remove_only) {
                gs->state = GRAB_SURFACE_COMMITTED;
                changed = any_committed = true;
            }
            break;
        case GRAB_SURFACE_COMMITTED:
            any_committed = true;
            break;
        }
    }

    if (!changed) {
        return;
    }
    if (any_committed) {
        grab_start(grab);
    } else {
        grab_finish(grab, true, true, dying);
    }
}

static void grab_remove_surface(struct focus_grab *grab, struct wlr_surface *surface)
{
    struct grab_surface *gs = grab_find_surface(grab, surface);
    if (!gs) {
        return;
    }
    if (gs->state == GRAB_SURFACE_PENDING_ADD) {
        grab_surface_destroy(gs);
    } else {
        gs->state = GRAB_SURFACE_PENDING_REMOVE;
    }
}

static void grab_surface_handle_destroy(struct wl_listener *listener, void *data)
{
    struct grab_surface *gs = wl_container_of(listener, gs, destroy);
    struct focus_grab *grab = gs->grab;
    struct wlr_surface *surface = gs->surface;
    grab_remove_surface(grab, surface);
    grab_commit(grab, surface);
}

static void grab_handle_add_surface(struct wl_client *client, struct wl_resource *resource,
                                    struct wl_resource *surface_resource)
{
    struct focus_grab *grab = wl_resource_get_user_data(resource);
    struct wlr_surface *surface = wlr_surface_from_resource(surface_resource);

    struct grab_surface *gs = grab_find_surface(grab, surface);
    if (gs) {
        /* duplicate additions are ignored, but an addition undoes a pending removal */
        if (gs->state == GRAB_SURFACE_PENDING_REMOVE) {
            gs->state = GRAB_SURFACE_COMMITTED;
        }
        return;
    }

    gs = calloc(1, sizeof(*gs));
    if (!gs) {
        wl_resource_post_no_memory(resource);
        return;
    }
    gs->grab = grab;
    gs->surface = surface;
    gs->state = GRAB_SURFACE_PENDING_ADD;
    gs->destroy.notify = grab_surface_handle_destroy;
    wl_signal_add(&surface->events.destroy, &gs->destroy);
    wl_list_insert(grab->surfaces.prev, &gs->link);
}

static void grab_handle_remove_surface(struct wl_client *client, struct wl_resource *resource,
                                       struct wl_resource *surface_resource)
{
    struct focus_grab *grab = wl_resource_get_user_data(resource);
    grab_remove_surface(grab, wlr_surface_from_resource(surface_resource));
}

static void grab_handle_commit(struct wl_client *client, struct wl_resource *resource)
{
    grab_commit(wl_resource_get_user_data(resource), NULL);
}

static void grab_handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
    wl_resource_destroy(resource);
}

static const struct hyprland_focus_grab_v1_interface grab_impl = {
    .add_surface = grab_handle_add_surface,
    .remove_surface = grab_handle_remove_surface,
    .commit = grab_handle_commit,
    .destroy = grab_handle_destroy,
};

static void grab_resource_destroy(struct wl_resource *resource)
{
    struct focus_grab *grab = wl_resource_get_user_data(resource);
    grab->resource = NULL;
    grab_finish(grab, false, true, NULL);

    struct grab_surface *gs, *tmp;
    wl_list_for_each_safe(gs, tmp, &grab->surfaces, link) {
        grab_surface_destroy(gs);
    }
    free(grab);
}

static void manager_handle_create_grab(struct wl_client *client, struct wl_resource *resource,
                                       uint32_t id)
{
    struct focus_grab *grab = calloc(1, sizeof(*grab));
    if (!grab) {
        wl_resource_post_no_memory(resource);
        return;
    }
    wl_list_init(&grab->surfaces);

    grab->resource = wl_resource_create(client, &hyprland_focus_grab_v1_interface,
                                        wl_resource_get_version(resource), id);
    if (!grab->resource) {
        free(grab);
        wl_resource_post_no_memory(resource);
        return;
    }
    wl_resource_set_implementation(grab->resource, &grab_impl, grab, grab_resource_destroy);
}

static void manager_handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
    wl_resource_destroy(resource);
}

static const struct hyprland_focus_grab_manager_v1_interface manager_impl = {
    .create_grab = manager_handle_create_grab,
    .destroy = manager_handle_destroy,
};

static void manager_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    struct wl_resource *resource =
        wl_resource_create(client, &hyprland_focus_grab_manager_v1_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &manager_impl, NULL, NULL);
}

static void handle_seat_destroy(struct wl_listener *listener, void *data)
{
    if (manager->active) {
        grab_finish(manager->active, true, false, NULL);
    }
}

bool focus_grab_rejects(struct seat *seat, struct wlr_surface *surface)
{
    return manager && manager->active && manager->seat == seat &&
           !grab_accepts(manager->active, surface);
}

void focus_grab_cancel(struct seat *seat)
{
    if (manager && manager->active && manager->seat == seat) {
        grab_finish(manager->active, true, false, NULL);
    }
}

bool focus_grab_handle_button(struct seat *seat, uint32_t button, bool pressed)
{
    if (!manager) {
        return false;
    }

    if (!pressed) {
        if (manager->eaten_seat == seat && manager->eaten_button == button) {
            manager->eaten_seat = NULL;
            return true;
        }
        return false;
    }

    if (!manager->active || manager->seat != seat) {
        return false;
    }

    double sx, sy;
    struct ky_scene_node *node = ky_scene_node_at(&seat->scene->tree.node, seat->cursor->lx,
                                                  seat->cursor->ly, &sx, &sy);
    struct wlr_surface *surface = node ? wlr_surface_try_from_node(node) : NULL;
    if (grab_accepts(manager->active, surface)) {
        return false;
    }

    manager->eaten_seat = seat;
    manager->eaten_button = button;
    grab_finish(manager->active, true, true, NULL);
    return true;
}

bool focus_grab_handle_touch_down(struct seat *seat, struct wlr_surface *surface)
{
    if (!focus_grab_rejects(seat, surface)) {
        return false;
    }
    grab_finish(manager->active, true, true, NULL);
    return true;
}

static void handle_display_destroy(struct wl_listener *listener, void *data)
{
    wl_list_remove(&manager->display_destroy.link);
    wl_list_remove(&manager->seat_destroy.link);
    wl_global_destroy(manager->global);
    free(manager);
    manager = NULL;
}

bool hyprland_focus_grab_manager_create(struct input_manager *input_manager)
{
    manager = calloc(1, sizeof(*manager));
    if (!manager) {
        return false;
    }

    struct server *server = input_manager->server;
    manager->global = wl_global_create(server->display, &hyprland_focus_grab_manager_v1_interface,
                                       HYPRLAND_FOCUS_GRAB_VERSION, NULL, manager_bind);
    if (!manager->global) {
        free(manager);
        manager = NULL;
        return false;
    }

    manager->input_manager = input_manager;
    manager->seat_destroy.notify = handle_seat_destroy;
    wl_list_init(&manager->seat_destroy.link);
    manager->display_destroy.notify = handle_display_destroy;
    wl_display_add_destroy_listener(server->display, &manager->display_destroy);
    return true;
}
