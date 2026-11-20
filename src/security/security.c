// SPDX-FileCopyrightText: 2023 KylinSoft Co., Ltd.
//
// SPDX-License-Identifier: GPL-1.0-or-later

#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <wlr/types/wlr_security_context_v1.h>

#include <kywc/log.h>

#include "security.h"
#include "server.h"

struct security_global_filter {
    struct wl_list link;
    struct wl_global *global;
    security_global_filter_func_t filter;
    void *filter_data;
};

struct security_manager {
    /* wp_security_context_v1, tags clients spawned by sandbox engines */
    struct wlr_security_context_manager_v1 *security_context;
    struct wl_list clients;
    struct wl_listener new_client;
    struct wl_list global_filters;
    struct wl_listener server_destroy;
};

static struct security_manager *manager = NULL;

static char *get_path_from_pid(const pid_t pid)
{
    char link_file[32];
    snprintf(link_file, 20, "/proc/%d/exe", pid);

    char *link_target = NULL;
    ssize_t read_len = INT_MAX;
    ssize_t link_len = 1;

    while (read_len >= link_len) {
        link_len += NAME_MAX;

        free(link_target);
        link_target = malloc(link_len * sizeof(char));
        if (link_target == NULL) {
            kywc_log(KYWC_ERROR, "could not allocate memory for link target");
            return NULL;
        }

        read_len = readlink(link_file, link_target, link_len);
        if (read_len < 0) {
            kywc_log_errno(KYWC_ERROR, "pid(%d) could not read link", pid);
            free(link_target);
            return NULL;
        }
    }

    /* should not assume that the returned contents are null-terminated */
    link_target[read_len] = '\0';

    return link_target;
}

static void handle_client_destroy(struct wl_listener *listener, void *data)
{
    struct security_client *client = wl_container_of(listener, client, destroy);
    kywc_log(KYWC_DEBUG, "client(pid=%d) %s is disconnect", client->pid, client->path);

    wl_list_remove(&client->destroy.link);
    wl_list_remove(&client->new_resource.link);
    wl_list_remove(&client->link);
    free(client->path);
    free(client);
}

static void client_handle_new_resource(struct wl_listener *listener, void *data)
{
    struct security_client *client = wl_container_of(listener, client, new_resource);
    // struct wl_resource *wl_resource = data;

    // kywc_log(KYWC_DEBUG, "client(pid=%d) %s create resource for %s", client->pid, client->path,
    //          wl_resource_get_class(wl_resource));
}

static struct security_client *security_client_from_client(const struct wl_client *wl_client)
{
    struct security_client *client;
    wl_list_for_each(client, &manager->clients, link) {
        if (client->client == wl_client) {
            return client;
        }
    }
    return NULL;
}

static void handle_new_client(struct wl_listener *listener, void *data)
{
    struct security_client *client = calloc(1, sizeof(*client));
    if (!client) {
        return;
    }

    struct wl_client *wl_client = data;
    client->fd = wl_client_get_fd(wl_client);
    wl_client_get_credentials(wl_client, &client->pid, &client->uid, &client->gid);
    client->path = get_path_from_pid(client->pid);
    kywc_log(KYWC_DEBUG, "client(pid=%d) %s is connect", client->pid, client->path);

    client->client = wl_client;
    client->destroy.notify = handle_client_destroy;
    wl_client_add_destroy_listener(wl_client, &client->destroy);

    client->new_resource.notify = client_handle_new_resource;
    wl_client_add_resource_created_listener(wl_client, &client->new_resource);

    wl_list_insert(&manager->clients, &client->link);
}

static struct security_global_filter *global_filter_from_global(const struct wl_global *global)
{
    struct security_global_filter *global_filter;
    wl_list_for_each(global_filter, &manager->global_filters, link) {
        if (global_filter->global == global) {
            return global_filter;
        }
    }
    return NULL;
}

/**
 * Globals a sandboxed client (connected through wp_security_context_v1, e.g. a
 * flatpak app) must not see: they read other clients' content or input, inject
 * input, or control outputs, windows and the desktop shell.
 */
static const char *const privileged_interfaces[] = {
    /* screen and window capture */
    "zwlr_screencopy_manager_v1",
    "zwlr_export_dmabuf_manager_v1",
    "ext_image_copy_capture_manager_v1",
    "ext_output_image_capture_source_manager_v1",
    "ext_foreign_toplevel_image_capture_source_manager_v1",
    "gxde_screenshot_manager_v1",
    "kywc_capture_manager_v1",
    "treeland_capture_manager_v1",
    /* clipboard of other clients */
    "zwlr_data_control_manager_v1",
    "ext_data_control_manager_v1",
    /* input injection and grabs */
    "zwp_virtual_keyboard_manager_v1",
    "zwlr_virtual_pointer_manager_v1",
    "zwp_input_method_manager_v2",
    "zwp_keyboard_shortcuts_inhibit_manager_v1",
    "ext_transient_seat_manager_v1",
    "org_kde_kwin_keystate",
    /* sandboxed apps use the GlobalShortcuts portal instead */
    "hyprland_global_shortcuts_manager_v1",
    /* window and workspace management */
    "zwlr_foreign_toplevel_manager_v1",
    "ext_foreign_toplevel_list_v1",
    "ext_workspace_manager_v1",
    "kywc_toplevel_manager_v1",
    "kywc_workspace_manager_v1",
    "org_kde_plasma_window_management",
    "org_kde_plasma_virtual_desktop_management",
    "ukui_window_management",
    /* outputs */
    "zwlr_output_manager_v1",
    "zwlr_output_power_manager_v1",
    "zwlr_gamma_control_manager_v1",
    "wp_drm_lease_device_v1",
    "kde_output_device_v2",
    "kde_output_management_v2",
    "kde_primary_output_v1",
    "org_kde_kwin_dpms_manager",
    "kywc_output_manager_v1",
    "ukui_output_management_v1",
    /* desktop shell components */
    "zwlr_layer_shell_v1",
    "ext_session_lock_manager_v1",
    "org_kde_plasma_shell",
    "ukui_shell",
    "treeland_dde_shell_manager_v1",
    "treeland_app_id_resolver_manager_v1",
    /* a sandboxed client must not create nested security contexts */
    "wp_security_context_manager_v1",
};

static bool global_is_privileged(const struct wl_global *wl_global)
{
    const char *name = wl_global_get_interface(wl_global)->name;
    for (size_t i = 0; i < sizeof(privileged_interfaces) / sizeof(privileged_interfaces[0]); i++) {
        if (strcmp(name, privileged_interfaces[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool filter_global(const struct wl_client *wl_client, const struct wl_global *wl_global,
                          void *data)
{
    const struct wlr_security_context_v1_state *context =
        manager->security_context
            ? wlr_security_context_manager_v1_lookup_client(manager->security_context, wl_client)
            : NULL;
    if (context && global_is_privileged(wl_global)) {
        return false;
    }

    struct security_global_filter *filter = global_filter_from_global(wl_global);
    if (!filter) {
        return true;
    }

    struct security_client *client = security_client_from_client(wl_client);
    return filter->filter(client, filter->filter_data);
}

bool security_add_global_filter(struct wl_global *global, security_global_filter_func_t filter,
                                void *data)
{
    struct security_global_filter *global_filter = global_filter_from_global(global);
    if (!global_filter) {
        global_filter = calloc(1, sizeof(*global_filter));
        if (!global_filter) {
            return false;
        }
        wl_list_insert(&manager->global_filters, &global_filter->link);
    }

    global_filter->global = global;
    global_filter->filter = filter;
    global_filter->filter_data = data;

    return true;
}

void security_remove_global_filter(struct wl_global *global)
{
    struct security_global_filter *filter = global_filter_from_global(global);
    if (filter) {
        wl_list_remove(&filter->link);
        free(filter);
    }
}

static void handle_server_destroy(struct wl_listener *listener, void *data)
{
    assert(wl_list_empty(&manager->clients));
    /* no global destroy signal */
    struct security_global_filter *filter, *tmp;
    wl_list_for_each_safe(filter, tmp, &manager->global_filters, link) {
        wl_list_remove(&filter->link);
        free(filter);
    }
    wl_list_remove(&manager->server_destroy.link);
    free(manager);
    manager = NULL;
}

bool security_manager_create(struct server *server)
{
    manager = calloc(1, sizeof(*manager));
    if (!manager) {
        return false;
    }

    wl_list_init(&manager->clients);
    manager->new_client.notify = handle_new_client;
    wl_display_add_client_created_listener(server->display, &manager->new_client);

    manager->server_destroy.notify = handle_server_destroy;
    server_add_destroy_listener(server, &manager->server_destroy);

    wl_list_init(&manager->global_filters);
    wl_display_set_global_filter(server->display, filter_global, NULL);

    manager->security_context = wlr_security_context_manager_v1_create(server->display);
    if (!manager->security_context) {
        kywc_log(KYWC_WARN, "wp_security_context_manager_v1 create failed");
    }

    return true;
}
