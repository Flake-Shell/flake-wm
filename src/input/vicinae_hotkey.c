// SPDX-FileCopyrightText: CharOfString <root@charofstring.cc>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <assert.h>
#include <stdlib.h>

#include <wlr/types/wlr_keyboard.h>

#include "input_p.h"
#include "server.h"
#include "vicinae-hotkey-v1-protocol.h"

#define VICINAE_HOTKEY_MANAGER_VERSION 1

struct vicinae_hotkey {
    struct wl_resource *resource;
    struct hotkey *hotkey;
};

static void hotkey_handle_pressed(struct hotkey *hotkey, uint32_t serial, uint32_t time, void *data)
{
    struct vicinae_hotkey *vicinae_hotkey = data;
    vicinae_hotkey_v1_send_pressed(vicinae_hotkey->resource, serial, time);
}

static void hotkey_handle_released(struct hotkey *hotkey, uint32_t serial, uint32_t time,
                                   void *data)
{
    struct vicinae_hotkey *vicinae_hotkey = data;
    vicinae_hotkey_v1_send_released(vicinae_hotkey->resource, serial, time);
}

static void hotkey_handle_revoked(struct hotkey *hotkey, const char *message, void *data)
{
    struct vicinae_hotkey *vicinae_hotkey = data;
    vicinae_hotkey_v1_send_revoked(vicinae_hotkey->resource,
                                   VICINAE_HOTKEY_V1_REVOKE_REASON_SUPERSEDED, message);
}

static const struct hotkey_impl hotkey_impl = {
    .pressed = hotkey_handle_pressed,
    .released = hotkey_handle_released,
    .revoked = hotkey_handle_revoked,
};

static void vicinae_hotkey_handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
    wl_resource_destroy(resource);
}

static const struct vicinae_hotkey_v1_interface vicinae_hotkey_impl = {
    .destroy = vicinae_hotkey_handle_destroy,
};

static void vicinae_hotkey_resource_destroy(struct wl_resource *resource)
{
    struct vicinae_hotkey *vicinae_hotkey = wl_resource_get_user_data(resource);
    if (!vicinae_hotkey) {
        return;
    }
    hotkey_destroy(vicinae_hotkey->hotkey);
    free(vicinae_hotkey);
}

static uint32_t modifiers_from_protocol(uint32_t modifiers)
{
    uint32_t out = 0;
    if (modifiers & VICINAE_HOTKEY_MANAGER_V1_MODIFIERS_SHIFT) {
        out |= WLR_MODIFIER_SHIFT;
    }
    if (modifiers & VICINAE_HOTKEY_MANAGER_V1_MODIFIERS_CTRL) {
        out |= WLR_MODIFIER_CTRL;
    }
    if (modifiers & VICINAE_HOTKEY_MANAGER_V1_MODIFIERS_ALT) {
        out |= WLR_MODIFIER_ALT;
    }
    if (modifiers & VICINAE_HOTKEY_MANAGER_V1_MODIFIERS_SUPER) {
        out |= WLR_MODIFIER_LOGO;
    }
    return out;
}

static void manager_handle_bind(struct wl_client *client, struct wl_resource *manager_resource,
                                uint32_t id, uint32_t keysym, uint32_t modifiers,
                                struct wl_resource *seat, const char *app_id,
                                const char *description)
{
    struct wl_resource *resource = wl_resource_create(
        client, &vicinae_hotkey_v1_interface, wl_resource_get_version(manager_resource), id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }

    struct vicinae_hotkey *vicinae_hotkey = calloc(1, sizeof(*vicinae_hotkey));
    if (vicinae_hotkey) {
        vicinae_hotkey->resource = resource;
        vicinae_hotkey->hotkey = hotkey_create(&hotkey_impl, vicinae_hotkey, client);
    }
    if (!vicinae_hotkey || !vicinae_hotkey->hotkey) {
        free(vicinae_hotkey);
        wl_resource_set_implementation(resource, &vicinae_hotkey_impl, NULL, NULL);
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &vicinae_hotkey_impl, vicinae_hotkey,
                                   vicinae_hotkey_resource_destroy);

    /* the seat is ignored, hotkeys fire on every keyboard */
    const uint32_t all = VICINAE_HOTKEY_MANAGER_V1_MODIFIERS_SHIFT |
                         VICINAE_HOTKEY_MANAGER_V1_MODIFIERS_CTRL |
                         VICINAE_HOTKEY_MANAGER_V1_MODIFIERS_ALT |
                         VICINAE_HOTKEY_MANAGER_V1_MODIFIERS_SUPER;
    if (modifiers & ~all) {
        vicinae_hotkey_v1_send_denied(resource, VICINAE_HOTKEY_V1_DENY_REASON_INVALID,
                                      "unknown modifier bits");
        return;
    }

    const char *message;
    enum hotkey_deny deny = hotkey_bind_key(vicinae_hotkey->hotkey, keysym,
                                            modifiers_from_protocol(modifiers), &message);
    switch (deny) {
    case HOTKEY_DENY_NONE:
        vicinae_hotkey_v1_send_bound(resource);
        return;
    case HOTKEY_DENY_ALREADY_BOUND:
        vicinae_hotkey_v1_send_denied(resource, VICINAE_HOTKEY_V1_DENY_REASON_ALREADY_BOUND,
                                      message);
        return;
    case HOTKEY_DENY_NOT_PERMITTED:
        vicinae_hotkey_v1_send_denied(resource, VICINAE_HOTKEY_V1_DENY_REASON_NOT_PERMITTED,
                                      message);
        return;
    case HOTKEY_DENY_INVALID:
        vicinae_hotkey_v1_send_denied(resource, VICINAE_HOTKEY_V1_DENY_REASON_INVALID, message);
        return;
    }
}

static void manager_handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
    /* the hotkeys outlive the manager */
    wl_resource_destroy(resource);
}

static const struct vicinae_hotkey_manager_v1_interface manager_impl = {
    .destroy = manager_handle_destroy,
    .bind = manager_handle_bind,
};

static void manager_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    struct wl_resource *resource =
        wl_resource_create(client, &vicinae_hotkey_manager_v1_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &manager_impl, NULL, NULL);
}

static struct {
    struct wl_global *global;
    struct wl_listener display_destroy;
} manager;

static void handle_display_destroy(struct wl_listener *listener, void *data)
{
    wl_list_remove(&manager.display_destroy.link);
    wl_global_destroy(manager.global);
    manager.global = NULL;
}

bool vicinae_hotkey_manager_create(struct input_manager *input_manager)
{
    struct server *server = input_manager->server;
    manager.global = wl_global_create(server->display, &vicinae_hotkey_manager_v1_interface,
                                      VICINAE_HOTKEY_MANAGER_VERSION, NULL, manager_bind);
    if (!manager.global) {
        return false;
    }

    manager.display_destroy.notify = handle_display_destroy;
    wl_display_add_destroy_listener(server->display, &manager.display_destroy);
    return true;
}
