// SPDX-FileCopyrightText: CharOfString <root@charofstring.cc>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdbool.h>

#include <wayland-server-core.h>

#include "hyprland-lock-notify-v1-protocol.h"
#include "server.h"
#include "view/session_lock.h"

#define HYPRLAND_LOCK_NOTIFIER_VERSION 1

static struct {
    struct wl_global *global;
    struct wl_list notifications;  // wl_resource_get_link
    bool locked;
    struct wl_listener display_destroy;
} notifier = {
    .notifications = { &notifier.notifications, &notifier.notifications },
};

void hyprland_lock_notify_set_locked(bool locked)
{
    /* the protocol forbids two locked or two unlocked events in a row */
    if (notifier.locked == locked) {
        return;
    }
    notifier.locked = locked;

    struct wl_resource *resource;
    wl_resource_for_each(resource, &notifier.notifications) {
        if (locked) {
            hyprland_lock_notification_v1_send_locked(resource);
        } else {
            hyprland_lock_notification_v1_send_unlocked(resource);
        }
    }
}

static void notification_handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
    wl_resource_destroy(resource);
}

static const struct hyprland_lock_notification_v1_interface notification_impl = {
    .destroy = notification_handle_destroy,
};

static void notification_resource_destroy(struct wl_resource *resource)
{
    wl_list_remove(wl_resource_get_link(resource));
}

static void manager_handle_get_lock_notification(struct wl_client *client,
                                                 struct wl_resource *manager_resource, uint32_t id)
{
    struct wl_resource *resource =
        wl_resource_create(client, &hyprland_lock_notification_v1_interface,
                           wl_resource_get_version(manager_resource), id);
    if (!resource) {
        wl_resource_post_no_memory(manager_resource);
        return;
    }
    wl_resource_set_implementation(resource, &notification_impl, NULL,
                                   notification_resource_destroy);
    wl_list_insert(&notifier.notifications, wl_resource_get_link(resource));

    if (notifier.locked) {
        hyprland_lock_notification_v1_send_locked(resource);
    }
}

static void manager_handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
    wl_resource_destroy(resource);
}

static const struct hyprland_lock_notifier_v1_interface manager_impl = {
    .destroy = manager_handle_destroy,
    .get_lock_notification = manager_handle_get_lock_notification,
};

static void manager_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    struct wl_resource *resource =
        wl_resource_create(client, &hyprland_lock_notifier_v1_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &manager_impl, NULL, NULL);
}

static void handle_display_destroy(struct wl_listener *listener, void *data)
{
    wl_list_remove(&notifier.display_destroy.link);
    wl_global_destroy(notifier.global);
    notifier.global = NULL;
}

bool hyprland_lock_notifier_create(struct server *server)
{
    notifier.global = wl_global_create(server->display, &hyprland_lock_notifier_v1_interface,
                                       HYPRLAND_LOCK_NOTIFIER_VERSION, NULL, manager_bind);
    if (!notifier.global) {
        return false;
    }

    notifier.display_destroy.notify = handle_display_destroy;
    wl_display_add_destroy_listener(server->display, &notifier.display_destroy);
    return true;
}
