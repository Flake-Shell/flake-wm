// SPDX-FileCopyrightText: CharOfString <root@charofstring.cc>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <wlr/types/wlr_output.h>

#include <kywc/log.h>

#include "hyprland-ctm-control-v1-protocol.h"
#include "output_p.h"
#include "server.h"

#define HYPRLAND_CTM_CONTROL_VERSION 2

struct ctm_entry {
    struct wl_list link;  // ctm_manager.ctms
    char *output_name;
    float matrix[9];
};

struct ctm_manager {
    struct wl_resource *resource;
    struct wl_list ctms;  // ctm_entry.link, kept across commits like Hyprland
    bool blocked;
    bool warned;
};

static struct ctm_control {
    struct wl_global *global;
    struct ctm_manager *active;
    struct wl_listener display_destroy;
} *control;

static const float identity[3] = { 1.0f, 1.0f, 1.0f };

static bool reset_output(struct kywc_output *kywc_output, int index, void *data)
{
    output_set_ctm(output_from_kywc_output(kywc_output), identity);
    return false;
}

static void reset_all_outputs(void)
{
    output_manager_for_each_output(reset_output, true, NULL);
    output_manager_for_each_output(reset_output, false, NULL);
}

static struct ctm_entry *manager_find_entry(struct ctm_manager *manager, const char *name)
{
    struct ctm_entry *entry;
    wl_list_for_each(entry, &manager->ctms, link) {
        if (!strcmp(entry->output_name, name)) {
            return entry;
        }
    }
    return NULL;
}

static void handle_set_ctm_for_output(struct wl_client *client, struct wl_resource *resource,
                                      struct wl_resource *output_resource, wl_fixed_t mat0,
                                      wl_fixed_t mat1, wl_fixed_t mat2, wl_fixed_t mat3,
                                      wl_fixed_t mat4, wl_fixed_t mat5, wl_fixed_t mat6,
                                      wl_fixed_t mat7, wl_fixed_t mat8)
{
    struct ctm_manager *manager = wl_resource_get_user_data(resource);
    if (manager->blocked) {
        return;
    }

    struct wlr_output *wlr_output = wlr_output_from_resource(output_resource);
    struct output *output = wlr_output ? output_find_by_wlr_output(wlr_output) : NULL;
    if (!output) {
        return;
    }

    const wl_fixed_t fixed[9] = { mat0, mat1, mat2, mat3, mat4, mat5, mat6, mat7, mat8 };
    float matrix[9];
    for (int i = 0; i < 9; i++) {
        matrix[i] = wl_fixed_to_double(fixed[i]);
        if (!isfinite(matrix[i]) || matrix[i] < 0.0f) {
            wl_resource_post_error(resource, HYPRLAND_CTM_CONTROL_MANAGER_V1_ERROR_INVALID_MATRIX,
                                   "matrix component %d is out of range", i);
            return;
        }
    }

    struct ctm_entry *entry = manager_find_entry(manager, output->base.name);
    if (!entry) {
        entry = calloc(1, sizeof(*entry));
        if (!entry || !(entry->output_name = strdup(output->base.name))) {
            free(entry);
            wl_resource_post_no_memory(resource);
            return;
        }
        wl_list_insert(manager->ctms.prev, &entry->link);
    }
    memcpy(entry->matrix, matrix, sizeof(matrix));
}

struct commit_data {
    struct ctm_manager *manager;
};

static bool commit_output(struct kywc_output *kywc_output, int index, void *data)
{
    struct commit_data *commit = data;
    struct output *output = output_from_kywc_output(kywc_output);

    struct ctm_entry *entry = manager_find_entry(commit->manager, kywc_output->name);
    if (!entry) {
        output_set_ctm(output, identity);
        return false;
    }

    const float *m = entry->matrix;
    if ((m[1] != 0.0f || m[2] != 0.0f || m[3] != 0.0f || m[5] != 0.0f || m[6] != 0.0f ||
         m[7] != 0.0f) &&
        !commit->manager->warned) {
        commit->manager->warned = true;
        kywc_log(KYWC_WARN, "hyprland ctm: only per-channel scale is supported, "
                            "off-diagonal terms are ignored");
    }
    const float ctm[3] = { m[0], m[4], m[8] };
    output_set_ctm(output, ctm);
    return false;
}

static void handle_commit(struct wl_client *client, struct wl_resource *resource)
{
    struct ctm_manager *manager = wl_resource_get_user_data(resource);
    if (manager->blocked) {
        return;
    }

    struct commit_data data = { .manager = manager };
    output_manager_for_each_output(commit_output, true, &data);
    output_manager_for_each_output(commit_output, false, &data);
}

static void handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
    wl_resource_destroy(resource);
}

static const struct hyprland_ctm_control_manager_v1_interface manager_impl = {
    .set_ctm_for_output = handle_set_ctm_for_output,
    .commit = handle_commit,
    .destroy = handle_destroy,
};

static void manager_resource_destroy(struct wl_resource *resource)
{
    struct ctm_manager *manager = wl_resource_get_user_data(resource);

    struct ctm_entry *entry, *tmp;
    wl_list_for_each_safe(entry, tmp, &manager->ctms, link) {
        wl_list_remove(&entry->link);
        free(entry->output_name);
        free(entry);
    }

    if (control && control->active == manager) {
        control->active = NULL;
        reset_all_outputs();
    }
    free(manager);
}

static void manager_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    struct ctm_manager *manager = calloc(1, sizeof(*manager));
    if (!manager) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_list_init(&manager->ctms);

    manager->resource =
        wl_resource_create(client, &hyprland_ctm_control_manager_v1_interface, version, id);
    if (!manager->resource) {
        free(manager);
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(manager->resource, &manager_impl, manager,
                                   manager_resource_destroy);

    if (control->active) {
        manager->blocked = true;
        if (version >= 2) {
            hyprland_ctm_control_manager_v1_send_blocked(manager->resource);
        }
        return;
    }
    control->active = manager;
}

static void handle_display_destroy(struct wl_listener *listener, void *data)
{
    wl_list_remove(&control->display_destroy.link);
    wl_global_destroy(control->global);
    free(control);
    control = NULL;
}

bool hyprland_ctm_control_create(struct server *server)
{
    control = calloc(1, sizeof(*control));
    if (!control) {
        return false;
    }

    control->global = wl_global_create(server->display, &hyprland_ctm_control_manager_v1_interface,
                                       HYPRLAND_CTM_CONTROL_VERSION, NULL, manager_bind);
    if (!control->global) {
        free(control);
        control = NULL;
        return false;
    }

    control->display_destroy.notify = handle_display_destroy;
    wl_display_add_destroy_listener(server->display, &control->display_destroy);
    return true;
}
