// SPDX-FileCopyrightText: CharOfString <root@charofstring.cc>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <assert.h>
#include <stdlib.h>

#include <wlr/types/wlr_keyboard.h>

#include "input_p.h"
#include "server.h"
#include "xx-hotkey-v1-protocol.h"

#define XX_HOTKEY_MANAGER_VERSION 1

#define XX_MODIFIERS                                                                               \
    (XX_HOTKEY_MANAGER_V1_MODIFIERS_SHIFT | XX_HOTKEY_MANAGER_V1_MODIFIERS_CTRL |                  \
     XX_HOTKEY_MANAGER_V1_MODIFIERS_ALT | XX_HOTKEY_MANAGER_V1_MODIFIERS_SUPER)

struct xx_manager {
    struct wl_resource *resource;
    struct wl_list hotkeys;  // xx_hotkey.link
    bool app_id_set, committed;
};

struct xx_hotkey {
    struct wl_resource *resource;
    struct wl_list link;  // xx_manager.hotkeys, empty once the manager is gone
    struct xx_manager *manager;
    struct hotkey *hotkey;

    /* described by set_*, applied on commit */
    bool has_trigger;
    enum hotkey_trigger type;
    uint32_t code, modifiers;
};

static void hotkey_handle_pressed(struct hotkey *hotkey, uint32_t serial, uint32_t time, void *data)
{
    struct xx_hotkey *xx_hotkey = data;
    xx_hotkey_v1_send_triggered(xx_hotkey->resource, serial, time);
}

static void hotkey_handle_released(struct hotkey *hotkey, uint32_t serial, uint32_t time,
                                   void *data)
{
    struct xx_hotkey *xx_hotkey = data;
    xx_hotkey_v1_send_released(xx_hotkey->resource, serial, time);
}

static void hotkey_handle_revoked(struct hotkey *hotkey, const char *message, void *data)
{
    struct xx_hotkey *xx_hotkey = data;
    xx_hotkey_v1_send_revoked(xx_hotkey->resource, message);
}

static const struct hotkey_impl hotkey_impl = {
    .pressed = hotkey_handle_pressed,
    .released = hotkey_handle_released,
    .revoked = hotkey_handle_revoked,
};

static uint32_t modifiers_from_protocol(uint32_t modifiers)
{
    uint32_t out = 0;
    if (modifiers & XX_HOTKEY_MANAGER_V1_MODIFIERS_SHIFT) {
        out |= WLR_MODIFIER_SHIFT;
    }
    if (modifiers & XX_HOTKEY_MANAGER_V1_MODIFIERS_CTRL) {
        out |= WLR_MODIFIER_CTRL;
    }
    if (modifiers & XX_HOTKEY_MANAGER_V1_MODIFIERS_ALT) {
        out |= WLR_MODIFIER_ALT;
    }
    if (modifiers & XX_HOTKEY_MANAGER_V1_MODIFIERS_SUPER) {
        out |= WLR_MODIFIER_LOGO;
    }
    return out;
}

static struct xx_hotkey *xx_hotkey_from_resource(struct wl_resource *resource);

static void xx_hotkey_handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
    wl_resource_destroy(resource);
}

static void xx_hotkey_handle_set_description(struct wl_client *client,
                                             struct wl_resource *resource, const char *description)
{
    /* nothing shows hotkeys to the user yet */
}

static void xx_hotkey_handle_set_seat(struct wl_client *client, struct wl_resource *resource,
                                      struct wl_resource *seat)
{
    /* hotkeys fire on every seat */
}

static void xx_hotkey_handle_set_key_trigger(struct wl_client *client,
                                             struct wl_resource *resource, uint32_t keysym,
                                             uint32_t modifiers)
{
    struct xx_hotkey *xx_hotkey = xx_hotkey_from_resource(resource);
    if (keysym == 0) {
        wl_resource_post_error(resource, XX_HOTKEY_V1_ERROR_INVALID_TRIGGER,
                               "keysym must not be null");
        return;
    }
    if (modifiers & ~XX_MODIFIERS) {
        wl_resource_post_error(resource, XX_HOTKEY_V1_ERROR_INVALID_TRIGGER,
                               "unknown modifier bits 0x%x", modifiers & ~XX_MODIFIERS);
        return;
    }
    if (!xx_hotkey) {
        return;
    }
    xx_hotkey->has_trigger = true;
    xx_hotkey->type = HOTKEY_TRIGGER_KEY;
    xx_hotkey->code = keysym;
    xx_hotkey->modifiers = modifiers_from_protocol(modifiers);
}

static void xx_hotkey_handle_set_button_trigger(struct wl_client *client,
                                                struct wl_resource *resource, uint32_t button,
                                                uint32_t modifiers)
{
    struct xx_hotkey *xx_hotkey = xx_hotkey_from_resource(resource);
    if (modifiers & ~XX_MODIFIERS) {
        wl_resource_post_error(resource, XX_HOTKEY_V1_ERROR_INVALID_TRIGGER,
                               "unknown modifier bits 0x%x", modifiers & ~XX_MODIFIERS);
        return;
    }
    if (!xx_hotkey) {
        return;
    }
    xx_hotkey->has_trigger = true;
    xx_hotkey->type = HOTKEY_TRIGGER_BUTTON;
    xx_hotkey->code = button;
    xx_hotkey->modifiers = modifiers_from_protocol(modifiers);
}

static void xx_hotkey_handle_commit(struct wl_client *client, struct wl_resource *resource)
{
    struct xx_hotkey *xx_hotkey = xx_hotkey_from_resource(resource);
    if (!xx_hotkey) {
        return;
    }
    if (!xx_hotkey->has_trigger) {
        wl_resource_post_error(resource, XX_HOTKEY_V1_ERROR_NO_TRIGGER,
                               "commit sent before a trigger was described");
        return;
    }
    if (xx_hotkey->manager) {
        xx_hotkey->manager->committed = true;
    }

    const char *message;
    enum hotkey_deny deny =
        xx_hotkey->type == HOTKEY_TRIGGER_KEY
            ? hotkey_bind_key(xx_hotkey->hotkey, xx_hotkey->code, xx_hotkey->modifiers, &message)
            : hotkey_bind_button(xx_hotkey->hotkey, xx_hotkey->code, xx_hotkey->modifiers,
                                 &message);
    switch (deny) {
    case HOTKEY_DENY_NONE:
        xx_hotkey_v1_send_bound(resource);
        return;
    case HOTKEY_DENY_ALREADY_BOUND:
        xx_hotkey_v1_send_denied(resource, XX_HOTKEY_V1_DENY_REASON_ALREADY_BOUND, message);
        return;
    case HOTKEY_DENY_NOT_PERMITTED:
    case HOTKEY_DENY_INVALID:
        /* no invalid reason here, a keysym without a name is just not allowed */
        xx_hotkey_v1_send_denied(resource, XX_HOTKEY_V1_DENY_REASON_NOT_PERMITTED, message);
        return;
    }
}

static const struct xx_hotkey_v1_interface xx_hotkey_impl = {
    .destroy = xx_hotkey_handle_destroy,
    .set_description = xx_hotkey_handle_set_description,
    .set_seat = xx_hotkey_handle_set_seat,
    .set_key_trigger = xx_hotkey_handle_set_key_trigger,
    .set_button_trigger = xx_hotkey_handle_set_button_trigger,
    .commit = xx_hotkey_handle_commit,
};

/* NULL if the hotkey could not be allocated */
static struct xx_hotkey *xx_hotkey_from_resource(struct wl_resource *resource)
{
    assert(wl_resource_instance_of(resource, &xx_hotkey_v1_interface, &xx_hotkey_impl));
    return wl_resource_get_user_data(resource);
}

static void xx_hotkey_resource_destroy(struct wl_resource *resource)
{
    struct xx_hotkey *xx_hotkey = wl_resource_get_user_data(resource);
    if (!xx_hotkey) {
        return;
    }
    wl_list_remove(&xx_hotkey->link);
    hotkey_destroy(xx_hotkey->hotkey);
    free(xx_hotkey);
}

static struct xx_manager *manager_from_resource(struct wl_resource *resource);

static void manager_handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
    wl_resource_destroy(resource);
}

static void manager_handle_set_app_id(struct wl_client *client, struct wl_resource *resource,
                                      const char *app_id)
{
    struct xx_manager *manager = manager_from_resource(resource);
    if (!app_id || !*app_id) {
        wl_resource_post_error(resource, XX_HOTKEY_MANAGER_V1_ERROR_INVALID_APP_ID,
                               "app_id must not be empty");
        return;
    }
    if (!manager) {
        return;
    }
    if (manager->app_id_set) {
        wl_resource_post_error(resource, XX_HOTKEY_MANAGER_V1_ERROR_INVALID_APP_ID,
                               "app_id was already set");
        return;
    }
    if (manager->committed) {
        wl_resource_post_error(resource, XX_HOTKEY_MANAGER_V1_ERROR_INVALID_APP_ID,
                               "app_id must be set before the first commit");
        return;
    }
    /* advisory only, no policy depends on it yet */
    manager->app_id_set = true;
}

static void manager_handle_create_hotkey(struct wl_client *client,
                                         struct wl_resource *manager_resource, uint32_t id)
{
    struct xx_manager *manager = manager_from_resource(manager_resource);
    struct wl_resource *resource = wl_resource_create(
        client, &xx_hotkey_v1_interface, wl_resource_get_version(manager_resource), id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }

    struct xx_hotkey *xx_hotkey = calloc(1, sizeof(*xx_hotkey));
    if (xx_hotkey) {
        xx_hotkey->resource = resource;
        xx_hotkey->hotkey = hotkey_create(&hotkey_impl, xx_hotkey, client);
    }
    if (!xx_hotkey || !xx_hotkey->hotkey) {
        free(xx_hotkey);
        wl_resource_set_implementation(resource, &xx_hotkey_impl, NULL, NULL);
        wl_client_post_no_memory(client);
        return;
    }

    xx_hotkey->manager = manager;
    if (manager) {
        wl_list_insert(&manager->hotkeys, &xx_hotkey->link);
    } else {
        wl_list_init(&xx_hotkey->link);
    }
    wl_resource_set_implementation(resource, &xx_hotkey_impl, xx_hotkey,
                                   xx_hotkey_resource_destroy);
}

static const struct xx_hotkey_manager_v1_interface manager_impl = {
    .destroy = manager_handle_destroy,
    .set_app_id = manager_handle_set_app_id,
    .create_hotkey = manager_handle_create_hotkey,
};

/* NULL if the manager could not be allocated */
static struct xx_manager *manager_from_resource(struct wl_resource *resource)
{
    assert(wl_resource_instance_of(resource, &xx_hotkey_manager_v1_interface, &manager_impl));
    return wl_resource_get_user_data(resource);
}

static void manager_resource_destroy(struct wl_resource *resource)
{
    struct xx_manager *manager = wl_resource_get_user_data(resource);
    if (!manager) {
        return;
    }

    /* the hotkeys stay valid */
    struct xx_hotkey *xx_hotkey, *tmp;
    wl_list_for_each_safe(xx_hotkey, tmp, &manager->hotkeys, link) {
        xx_hotkey->manager = NULL;
        wl_list_remove(&xx_hotkey->link);
        wl_list_init(&xx_hotkey->link);
    }
    free(manager);
}

static void manager_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    struct wl_resource *resource =
        wl_resource_create(client, &xx_hotkey_manager_v1_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }

    struct xx_manager *manager = calloc(1, sizeof(*manager));
    if (!manager) {
        wl_resource_set_implementation(resource, &manager_impl, NULL, NULL);
        wl_client_post_no_memory(client);
        return;
    }
    manager->resource = resource;
    wl_list_init(&manager->hotkeys);
    wl_resource_set_implementation(resource, &manager_impl, manager, manager_resource_destroy);
}

static struct {
    struct wl_global *global;
    struct wl_listener display_destroy;
} global;

static void handle_display_destroy(struct wl_listener *listener, void *data)
{
    wl_list_remove(&global.display_destroy.link);
    wl_global_destroy(global.global);
    global.global = NULL;
}

bool xx_hotkey_manager_create(struct input_manager *input_manager)
{
    struct server *server = input_manager->server;
    global.global = wl_global_create(server->display, &xx_hotkey_manager_v1_interface,
                                     XX_HOTKEY_MANAGER_VERSION, NULL, manager_bind);
    if (!global.global) {
        return false;
    }

    global.display_destroy.notify = handle_display_destroy;
    wl_display_add_destroy_listener(server->display, &global.display_destroy);
    return true;
}
