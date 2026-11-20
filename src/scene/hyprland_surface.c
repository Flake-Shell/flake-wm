// SPDX-FileCopyrightText: CharOfString <root@charofstring.cc>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdlib.h>

#include <wlr/types/wlr_compositor.h>
#include <wlr/util/addon.h>

#include "hyprland-surface-v1-protocol.h"
#include "scene/surface.h"

#define HYPRLAND_SURFACE_VERSION 2

struct hyprland_surface_state {
    float opacity;
};

struct hyprland_surface {
    struct wlr_surface *surface;
    /* NULL once destroyed by the client, the reset then waits for a commit */
    struct wl_resource *resource;

    struct wlr_surface_synced synced;
    struct hyprland_surface_state pending, current;
    struct wlr_addon addon;
    struct wl_listener surface_commit;
};

static const struct hyprland_surface_v1_interface surface_impl;

static void hyprland_surface_destroy(struct hyprland_surface *hsurface)
{
    if (hsurface->resource) {
        wl_resource_set_user_data(hsurface->resource, NULL);
    }
    wl_list_remove(&hsurface->surface_commit.link);
    wlr_surface_synced_finish(&hsurface->synced);
    wlr_addon_finish(&hsurface->addon);
    free(hsurface);
}

static void surface_addon_destroy(struct wlr_addon *addon)
{
    struct hyprland_surface *hsurface = wl_container_of(addon, hsurface, addon);
    hyprland_surface_destroy(hsurface);
}

static const struct wlr_addon_interface surface_addon_impl = {
    .name = "hyprland_surface_v1",
    .destroy = surface_addon_destroy,
};

static struct hyprland_surface *hyprland_surface_from_wlr_surface(struct wlr_surface *surface)
{
    struct wlr_addon *addon = wlr_addon_find(&surface->addons, NULL, &surface_addon_impl);
    if (!addon) {
        return NULL;
    }
    struct hyprland_surface *hsurface = wl_container_of(addon, hsurface, addon);
    return hsurface;
}

bool hyprland_surface_get_opacity(struct wlr_surface *surface, float *opacity)
{
    struct hyprland_surface *hsurface = hyprland_surface_from_wlr_surface(surface);
    if (!hsurface) {
        return false;
    }
    *opacity = hsurface->current.opacity;
    return true;
}

static void handle_surface_commit(struct wl_listener *listener, void *data)
{
    struct hyprland_surface *hsurface = wl_container_of(listener, hsurface, surface_commit);
    /* the reset requested by destroy is applied now, nothing is left to track */
    if (!hsurface->resource && hsurface->current.opacity == 1.0f) {
        hyprland_surface_destroy(hsurface);
    }
}

static void surface_handle_set_opacity(struct wl_client *client, struct wl_resource *resource,
                                       wl_fixed_t opacity)
{
    struct hyprland_surface *hsurface = wl_resource_get_user_data(resource);
    if (!hsurface) {
        wl_resource_post_error(resource, HYPRLAND_SURFACE_V1_ERROR_NO_SURFACE,
                               "the wl_surface was destroyed");
        return;
    }

    double value = wl_fixed_to_double(opacity);
    if (value < 0.0 || value > 1.0) {
        wl_resource_post_error(resource, HYPRLAND_SURFACE_V1_ERROR_OUT_OF_RANGE,
                               "opacity %f is not in the range 0.0 - 1.0", value);
        return;
    }
    hsurface->pending.opacity = value;
}

static void surface_handle_set_visible_region(struct wl_client *client,
                                              struct wl_resource *resource,
                                              struct wl_resource *region)
{
    struct hyprland_surface *hsurface = wl_resource_get_user_data(resource);
    if (!hsurface) {
        wl_resource_post_error(resource, HYPRLAND_SURFACE_V1_ERROR_NO_SURFACE,
                               "the wl_surface was destroyed");
    }
    /* an optimization hint only, drawing the whole surface is always correct */
}

static void surface_handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
    wl_resource_destroy(resource);
}

static const struct hyprland_surface_v1_interface surface_impl = {
    .set_opacity = surface_handle_set_opacity,
    .destroy = surface_handle_destroy,
    .set_visible_region = surface_handle_set_visible_region,
};

static void surface_resource_destroy(struct wl_resource *resource)
{
    struct hyprland_surface *hsurface = wl_resource_get_user_data(resource);
    if (!hsurface) {
        return;
    }
    hsurface->resource = NULL;
    hsurface->pending.opacity = 1.0f;
    if (hsurface->current.opacity == 1.0f) {
        hyprland_surface_destroy(hsurface);
    }
}

static void surface_synced_init_state(void *data)
{
    struct hyprland_surface_state *state = data;
    state->opacity = 1.0f;
}

static const struct wlr_surface_synced_impl surface_synced_impl = {
    .state_size = sizeof(struct hyprland_surface_state),
    .init_state = surface_synced_init_state,
};

static void manager_handle_get_hyprland_surface(struct wl_client *client,
                                                struct wl_resource *manager_resource, uint32_t id,
                                                struct wl_resource *surface_resource)
{
    struct wlr_surface *surface = wlr_surface_from_resource(surface_resource);

    struct hyprland_surface *hsurface = hyprland_surface_from_wlr_surface(surface);
    if (hsurface && hsurface->resource) {
        wl_resource_post_error(manager_resource,
                               HYPRLAND_SURFACE_MANAGER_V1_ERROR_ALREADY_CONSTRUCTED,
                               "the wl_surface already has a hyprland_surface_v1");
        return;
    }

    struct wl_resource *resource = wl_resource_create(
        client, &hyprland_surface_v1_interface, wl_resource_get_version(manager_resource), id);
    if (!resource) {
        wl_resource_post_no_memory(manager_resource);
        return;
    }

    /* an object destroyed but still waiting for its reset commit is taken over */
    if (!hsurface) {
        hsurface = calloc(1, sizeof(*hsurface));
        if (!hsurface) {
            wl_resource_destroy(resource);
            wl_resource_post_no_memory(manager_resource);
            return;
        }
        if (!wlr_surface_synced_init(&hsurface->synced, surface, &surface_synced_impl,
                                     &hsurface->pending, &hsurface->current)) {
            free(hsurface);
            wl_resource_destroy(resource);
            wl_resource_post_no_memory(manager_resource);
            return;
        }
        hsurface->surface = surface;
        wlr_addon_init(&hsurface->addon, &surface->addons, NULL, &surface_addon_impl);
        hsurface->surface_commit.notify = handle_surface_commit;
        wl_signal_add(&surface->events.commit, &hsurface->surface_commit);
    }

    hsurface->resource = resource;
    wl_resource_set_implementation(resource, &surface_impl, hsurface, surface_resource_destroy);
}

static void manager_handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
    wl_resource_destroy(resource);
}

static const struct hyprland_surface_manager_v1_interface manager_impl = {
    .get_hyprland_surface = manager_handle_get_hyprland_surface,
    .destroy = manager_handle_destroy,
};

static void manager_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    struct wl_resource *resource =
        wl_resource_create(client, &hyprland_surface_manager_v1_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &manager_impl, NULL, NULL);
}

struct hyprland_surface_manager {
    struct wl_global *global;
    struct wl_listener display_destroy;
};

static void handle_display_destroy(struct wl_listener *listener, void *data)
{
    struct hyprland_surface_manager *manager =
        wl_container_of(listener, manager, display_destroy);
    wl_list_remove(&manager->display_destroy.link);
    wl_global_destroy(manager->global);
    free(manager);
}

bool hyprland_surface_manager_create(struct wl_display *display)
{
    struct hyprland_surface_manager *manager = calloc(1, sizeof(*manager));
    if (!manager) {
        return false;
    }

    manager->global = wl_global_create(display, &hyprland_surface_manager_v1_interface,
                                       HYPRLAND_SURFACE_VERSION, NULL, manager_bind);
    if (!manager->global) {
        free(manager);
        return false;
    }

    manager->display_destroy.notify = handle_display_destroy;
    wl_display_add_destroy_listener(display, &manager->display_destroy);
    return true;
}
