// SPDX-FileCopyrightText: CharOfString <root@charofstring.cc>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <kywc/log.h>

#include "hyprland-global-shortcuts-v1-protocol.h"
#include "input_p.h"
#include "server.h"

#define HYPRLAND_GLOBAL_SHORTCUTS_VERSION 1

struct global_shortcut {
    struct wl_list link;  // global_shortcuts_manager.shortcuts
    struct wl_resource *resource;
    char *app_id, *id, *description, *trigger_description;

    bool active;
    struct seat *seat;
    uint32_t keycode;
};

struct global_shortcuts_seat {
    struct wl_list link;  // global_shortcuts_manager.seats
    struct seat *seat;
    struct wl_listener keyboard_key;
    struct wl_listener destroy;
};

static struct global_shortcuts_manager {
    struct wl_global *global;
    struct wl_list shortcuts;  // global_shortcut.link
    struct wl_list seats;      // global_shortcuts_seat.link

    /* the key event being processed when a binding fires */
    struct {
        struct seat *seat;
        uint32_t keycode;
        bool pressed;
    } last_key;

    struct wl_listener new_seat;
    struct wl_listener display_destroy;
} *manager = NULL;

static void shortcut_send(struct global_shortcut *shortcut, bool pressed)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t sec = (uint64_t)now.tv_sec;
    if (pressed) {
        hyprland_global_shortcut_v1_send_pressed(shortcut->resource, sec >> 32, sec & 0xffffffff,
                                                 now.tv_nsec);
    } else {
        hyprland_global_shortcut_v1_send_released(shortcut->resource, sec >> 32, sec & 0xffffffff,
                                                  now.tv_nsec);
    }
}

static void shortcut_release(struct global_shortcut *shortcut)
{
    if (!shortcut->active) {
        return;
    }
    shortcut->active = false;
    shortcut->seat = NULL;
    shortcut_send(shortcut, false);
}

bool hyprland_global_shortcut_trigger(const char *target, bool from_key)
{
    if (!manager || !target) {
        return false;
    }

    const char *sep = strchr(target, ':');
    if (!sep) {
        kywc_log(KYWC_WARN, "global shortcut \"%s\" is not in app_id:id form", target);
        return false;
    }
    size_t app_id_len = sep - target;
    const char *id = sep + 1;

    bool found = false;
    struct global_shortcut *shortcut;
    wl_list_for_each(shortcut, &manager->shortcuts, link) {
        if (strlen(shortcut->app_id) != app_id_len ||
            strncmp(shortcut->app_id, target, app_id_len) || strcmp(shortcut->id, id)) {
            continue;
        }
        found = true;

        /* key repeat re-runs the binding, the shortcut is still held */
        if (shortcut->active) {
            continue;
        }

        shortcut_send(shortcut, true);
        if (from_key && manager->last_key.seat && manager->last_key.pressed) {
            shortcut->active = true;
            shortcut->seat = manager->last_key.seat;
            shortcut->keycode = manager->last_key.keycode;
        } else {
            shortcut_send(shortcut, false);
        }
    }

    if (!found) {
        kywc_log(KYWC_DEBUG, "global shortcut %s is not registered by any client", target);
    }
    return found;
}

static void seat_handle_keyboard_key(struct wl_listener *listener, void *data)
{
    struct global_shortcuts_seat *gs_seat = wl_container_of(listener, gs_seat, keyboard_key);
    struct seat_keyboard_key_event *event = data;

    manager->last_key.seat = gs_seat->seat;
    manager->last_key.keycode = event->keycode;
    manager->last_key.pressed = event->pressed;

    if (event->pressed) {
        return;
    }

    struct global_shortcut *shortcut;
    wl_list_for_each(shortcut, &manager->shortcuts, link) {
        if (shortcut->active && shortcut->seat == gs_seat->seat &&
            shortcut->keycode == event->keycode) {
            shortcut_release(shortcut);
        }
    }
}

static void seat_handle_destroy(struct wl_listener *listener, void *data)
{
    struct global_shortcuts_seat *gs_seat = wl_container_of(listener, gs_seat, destroy);

    struct global_shortcut *shortcut;
    wl_list_for_each(shortcut, &manager->shortcuts, link) {
        if (shortcut->active && shortcut->seat == gs_seat->seat) {
            shortcut_release(shortcut);
        }
    }
    if (manager->last_key.seat == gs_seat->seat) {
        manager->last_key.seat = NULL;
    }

    wl_list_remove(&gs_seat->keyboard_key.link);
    wl_list_remove(&gs_seat->destroy.link);
    wl_list_remove(&gs_seat->link);
    free(gs_seat);
}

static void manager_add_seat(struct seat *seat)
{
    struct global_shortcuts_seat *gs_seat = calloc(1, sizeof(*gs_seat));
    if (!gs_seat) {
        return;
    }

    gs_seat->seat = seat;
    gs_seat->keyboard_key.notify = seat_handle_keyboard_key;
    wl_signal_add(&seat->events.keyboard_key, &gs_seat->keyboard_key);
    gs_seat->destroy.notify = seat_handle_destroy;
    wl_signal_add(&seat->events.destroy, &gs_seat->destroy);
    wl_list_insert(&manager->seats, &gs_seat->link);
}

static void handle_new_seat(struct wl_listener *listener, void *data)
{
    manager_add_seat(data);
}

static void shortcut_handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
    wl_resource_destroy(resource);
}

static const struct hyprland_global_shortcut_v1_interface shortcut_impl = {
    .destroy = shortcut_handle_destroy,
};

static void shortcut_handle_resource_destroy(struct wl_resource *resource)
{
    struct global_shortcut *shortcut = wl_resource_get_user_data(resource);
    kywc_log(KYWC_DEBUG, "global shortcut %s:%s unregistered", shortcut->app_id, shortcut->id);

    wl_list_remove(&shortcut->link);
    free(shortcut->app_id);
    free(shortcut->id);
    free(shortcut->description);
    free(shortcut->trigger_description);
    free(shortcut);
}

static void manager_handle_register_shortcut(struct wl_client *client,
                                             struct wl_resource *manager_resource, uint32_t id,
                                             const char *shortcut_id, const char *app_id,
                                             const char *description,
                                             const char *trigger_description)
{
    struct global_shortcut *shortcut;
    wl_list_for_each(shortcut, &manager->shortcuts, link) {
        if (!strcmp(shortcut->app_id, app_id) && !strcmp(shortcut->id, shortcut_id)) {
            wl_resource_post_error(
                manager_resource, HYPRLAND_GLOBAL_SHORTCUTS_MANAGER_V1_ERROR_ALREADY_TAKEN,
                "global shortcut %s:%s is already registered", app_id, shortcut_id);
            return;
        }
    }

    shortcut = calloc(1, sizeof(*shortcut));
    if (!shortcut) {
        wl_client_post_no_memory(client);
        return;
    }

    shortcut->resource = wl_resource_create(client, &hyprland_global_shortcut_v1_interface,
                                            wl_resource_get_version(manager_resource), id);
    if (!shortcut->resource) {
        free(shortcut);
        wl_client_post_no_memory(client);
        return;
    }

    shortcut->app_id = strdup(app_id);
    shortcut->id = strdup(shortcut_id);
    shortcut->description = strdup(description);
    shortcut->trigger_description = strdup(trigger_description);
    wl_list_insert(manager->shortcuts.prev, &shortcut->link);
    wl_resource_set_implementation(shortcut->resource, &shortcut_impl, shortcut,
                                   shortcut_handle_resource_destroy);

    kywc_log(KYWC_INFO, "global shortcut %s:%s registered (%s, preferred trigger \"%s\")", app_id,
             shortcut_id, description, trigger_description);
}

static void manager_handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
    wl_resource_destroy(resource);
}

static const struct hyprland_global_shortcuts_manager_v1_interface manager_impl = {
    .register_shortcut = manager_handle_register_shortcut,
    .destroy = manager_handle_destroy,
};

static void manager_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    struct wl_resource *resource =
        wl_resource_create(client, &hyprland_global_shortcuts_manager_v1_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &manager_impl, manager, NULL);
}

void hyprland_global_shortcuts_for_each(hyprland_global_shortcut_iterator_func_t iterator,
                                        void *data)
{
    if (!manager) {
        return;
    }

    struct global_shortcut *shortcut;
    wl_list_for_each(shortcut, &manager->shortcuts, link) {
        if (iterator(shortcut->app_id, shortcut->id, shortcut->description,
                     shortcut->trigger_description, data)) {
            break;
        }
    }
}

static void handle_display_destroy(struct wl_listener *listener, void *data)
{
    /* clients, and with them every shortcut, are gone by now */
    struct global_shortcuts_seat *gs_seat, *tmp;
    wl_list_for_each_safe(gs_seat, tmp, &manager->seats, link) {
        wl_list_remove(&gs_seat->keyboard_key.link);
        wl_list_remove(&gs_seat->destroy.link);
        wl_list_remove(&gs_seat->link);
        free(gs_seat);
    }

    wl_list_remove(&manager->new_seat.link);
    wl_list_remove(&manager->display_destroy.link);
    wl_global_destroy(manager->global);
    free(manager);
    manager = NULL;
}

bool hyprland_global_shortcuts_create(struct input_manager *input_manager)
{
    manager = calloc(1, sizeof(*manager));
    if (!manager) {
        return false;
    }

    struct wl_display *display = input_manager->server->display;
    manager->global = wl_global_create(display, &hyprland_global_shortcuts_manager_v1_interface,
                                       HYPRLAND_GLOBAL_SHORTCUTS_VERSION, manager, manager_bind);
    if (!manager->global) {
        kywc_log(KYWC_WARN, "hyprland_global_shortcuts_manager_v1 create failed");
        free(manager);
        manager = NULL;
        return false;
    }

    wl_list_init(&manager->shortcuts);
    wl_list_init(&manager->seats);

    struct seat *seat;
    wl_list_for_each(seat, &input_manager->seats, link) {
        manager_add_seat(seat);
    }
    manager->new_seat.notify = handle_new_seat;
    wl_signal_add(&input_manager->events.new_seat, &manager->new_seat);

    manager->display_destroy.notify = handle_display_destroy;
    wl_display_add_destroy_listener(display, &manager->display_destroy);

    return true;
}
