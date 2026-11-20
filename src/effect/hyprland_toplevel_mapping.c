// SPDX-FileCopyrightText: CharOfString <root@charofstring.cc>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdint.h>
#include <stdlib.h>

#include "effect_p.h"
#include "hyprland-toplevel-mapping-v1-protocol.h"
#include "server.h"
#include "view/view.h"

#define HYPRLAND_TOPLEVEL_MAPPING_VERSION 1

struct mapping_manager {
    struct wl_global *global;
    struct wl_listener display_destroy;
};

static void handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
    wl_resource_destroy(resource);
}

static const struct hyprland_toplevel_window_mapping_handle_v1_interface handle_impl = {
    .destroy = handle_destroy,
};

static void handle_create(struct wl_resource *manager_resource, uint32_t id, struct view *view)
{
    struct wl_client *client = wl_resource_get_client(manager_resource);
    struct wl_resource *resource =
        wl_resource_create(client, &hyprland_toplevel_window_mapping_handle_v1_interface,
                           wl_resource_get_version(manager_resource), id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &handle_impl, NULL, NULL);

    if (!view || !view->base.mapped) {
        hyprland_toplevel_window_mapping_handle_v1_send_failed(resource);
        return;
    }

    uint64_t address = (uint64_t)(uintptr_t)view;
    hyprland_toplevel_window_mapping_handle_v1_send_window_address(
        resource, (uint32_t)(address >> 32), (uint32_t)address);
}

static void manager_get_window_for_toplevel(struct wl_client *client,
                                            struct wl_resource *resource, uint32_t id,
                                            struct wl_resource *toplevel)
{
    handle_create(resource, id, ext_foreign_toplevel_view_from_resource(toplevel));
}

static void manager_get_window_for_toplevel_wlr(struct wl_client *client,
                                                struct wl_resource *resource, uint32_t id,
                                                struct wl_resource *toplevel)
{
    handle_create(resource, id, view_from_wlr_foreign_toplevel_resource(toplevel));
}

static void manager_destroy(struct wl_client *client, struct wl_resource *resource)
{
    wl_resource_destroy(resource);
}

static const struct hyprland_toplevel_mapping_manager_v1_interface manager_impl = {
    .get_window_for_toplevel = manager_get_window_for_toplevel,
    .get_window_for_toplevel_wlr = manager_get_window_for_toplevel_wlr,
    .destroy = manager_destroy,
};

static void manager_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    struct wl_resource *resource =
        wl_resource_create(client, &hyprland_toplevel_mapping_manager_v1_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &manager_impl, NULL, NULL);
}

static void handle_display_destroy(struct wl_listener *listener, void *data)
{
    struct mapping_manager *manager = wl_container_of(listener, manager, display_destroy);
    wl_list_remove(&manager->display_destroy.link);
    wl_global_destroy(manager->global);
    free(manager);
}

bool hyprland_toplevel_mapping_manager_create(struct server *server)
{
    struct mapping_manager *manager = calloc(1, sizeof(*manager));
    if (!manager) {
        return false;
    }

    manager->global =
        wl_global_create(server->display, &hyprland_toplevel_mapping_manager_v1_interface,
                         HYPRLAND_TOPLEVEL_MAPPING_VERSION, NULL, manager_bind);
    if (!manager->global) {
        free(manager);
        return false;
    }

    manager->display_destroy.notify = handle_display_destroy;
    wl_display_add_destroy_listener(server->display, &manager->display_destroy);
    return true;
}
