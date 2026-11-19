// SPDX-FileCopyrightText: 2026 GXDE OS
//
// SPDX-License-Identifier: GPL-1.0-or-later

/**
 * ext_workspace_manager_v1 on top of the workspace manager.
 *
 * Workspaces are shared by all outputs, so there is a single group that
 * every enabled output enters. Workspaces are laid out in one dimension by
 * their position. Clients may create, activate and remove workspaces; the last
 * workspace is never removed, and deactivate/assign are ignored since exactly
 * one workspace is always active and there is only one group.
 */

#include <stdlib.h>

#include <wlr/types/wlr_ext_workspace_v1.h>
#include <wlr/types/wlr_output.h>

#include "output.h"
#include "view/workspace.h"
#include "view_p.h"

#define EXT_WORKSPACE_MANAGER_VERSION 1

struct ext_workspace_manager {
    struct wlr_ext_workspace_manager_v1 *manager;
    struct wlr_ext_workspace_group_handle_v1 *group;
    struct wl_list workspaces; // ext_workspace.link
    struct wl_list outputs;    // ext_workspace_output.link

    struct wl_listener new_workspace;
    struct wl_listener new_enabled_output;
    struct wl_listener commit;
    struct wl_listener destroy;
};

struct ext_workspace {
    struct wl_list link;
    struct workspace *workspace;
    struct wlr_ext_workspace_handle_v1 *handle;

    struct wl_listener name;
    struct wl_listener position;
    struct wl_listener activate;
    struct wl_listener destroy;
};

struct ext_workspace_output {
    struct wl_list link;
    struct output *output;
    struct wl_listener disable;
};

static struct ext_workspace_manager *manager = NULL;

static void ext_workspace_update_position(struct ext_workspace *ext_workspace)
{
    uint32_t coords[] = { ext_workspace->workspace->position };
    wlr_ext_workspace_handle_v1_set_coordinates(ext_workspace->handle, coords, 1);
}

static void ext_workspace_destroy(struct ext_workspace *ext_workspace)
{
    wl_list_remove(&ext_workspace->name.link);
    wl_list_remove(&ext_workspace->position.link);
    wl_list_remove(&ext_workspace->activate.link);
    wl_list_remove(&ext_workspace->destroy.link);
    wl_list_remove(&ext_workspace->link);
    free(ext_workspace);
}

static void handle_workspace_name(struct wl_listener *listener, void *data)
{
    struct ext_workspace *ext_workspace = wl_container_of(listener, ext_workspace, name);
    wlr_ext_workspace_handle_v1_set_name(ext_workspace->handle, ext_workspace->workspace->name);
}

static void handle_workspace_position(struct wl_listener *listener, void *data)
{
    struct ext_workspace *ext_workspace = wl_container_of(listener, ext_workspace, position);
    ext_workspace_update_position(ext_workspace);
}

static void handle_workspace_activate(struct wl_listener *listener, void *data)
{
    struct ext_workspace *ext_workspace = wl_container_of(listener, ext_workspace, activate);
    wlr_ext_workspace_handle_v1_set_active(ext_workspace->handle,
                                           ext_workspace->workspace->activated);
}

static void handle_workspace_destroy(struct wl_listener *listener, void *data)
{
    struct ext_workspace *ext_workspace = wl_container_of(listener, ext_workspace, destroy);
    wlr_ext_workspace_handle_v1_destroy(ext_workspace->handle);
    ext_workspace_destroy(ext_workspace);
}

static void ext_workspace_create(struct workspace *workspace)
{
    struct ext_workspace *ext_workspace = calloc(1, sizeof(*ext_workspace));
    if (!ext_workspace) {
        return;
    }

    ext_workspace->handle = wlr_ext_workspace_handle_v1_create(
        manager->manager, workspace->uuid,
        EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE |
            EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_REMOVE);
    if (!ext_workspace->handle) {
        free(ext_workspace);
        return;
    }

    ext_workspace->workspace = workspace;
    ext_workspace->handle->data = ext_workspace;
    wl_list_insert(manager->workspaces.prev, &ext_workspace->link);

    wlr_ext_workspace_handle_v1_set_group(ext_workspace->handle, manager->group);
    wlr_ext_workspace_handle_v1_set_name(ext_workspace->handle, workspace->name);
    wlr_ext_workspace_handle_v1_set_active(ext_workspace->handle, workspace->activated);
    ext_workspace_update_position(ext_workspace);

    ext_workspace->name.notify = handle_workspace_name;
    wl_signal_add(&workspace->events.name, &ext_workspace->name);
    ext_workspace->position.notify = handle_workspace_position;
    wl_signal_add(&workspace->events.position, &ext_workspace->position);
    ext_workspace->activate.notify = handle_workspace_activate;
    wl_signal_add(&workspace->events.activate, &ext_workspace->activate);
    ext_workspace->destroy.notify = handle_workspace_destroy;
    wl_signal_add(&workspace->events.destroy, &ext_workspace->destroy);
}

static void handle_new_workspace(struct wl_listener *listener, void *data)
{
    ext_workspace_create(data);
}

static void ext_workspace_output_destroy(struct ext_workspace_output *ext_output)
{
    wl_list_remove(&ext_output->disable.link);
    wl_list_remove(&ext_output->link);
    free(ext_output);
}

static void handle_output_disable(struct wl_listener *listener, void *data)
{
    struct ext_workspace_output *ext_output = wl_container_of(listener, ext_output, disable);
    wlr_ext_workspace_group_handle_v1_output_leave(manager->group, ext_output->output->wlr_output);
    ext_workspace_output_destroy(ext_output);
}

static void ext_workspace_add_output(struct output *output)
{
    struct ext_workspace_output *ext_output;
    wl_list_for_each(ext_output, &manager->outputs, link) {
        if (ext_output->output == output) {
            return;
        }
    }

    ext_output = calloc(1, sizeof(*ext_output));
    if (!ext_output) {
        return;
    }

    ext_output->output = output;
    ext_output->disable.notify = handle_output_disable;
    wl_signal_add(&output->events.disable, &ext_output->disable);
    wl_list_insert(&manager->outputs, &ext_output->link);

    wlr_ext_workspace_group_handle_v1_output_enter(manager->group, output->wlr_output);
}

static void handle_new_enabled_output(struct wl_listener *listener, void *data)
{
    ext_workspace_add_output(output_from_kywc_output(data));
}

static bool add_enabled_output(struct kywc_output *kywc_output, int index, void *data)
{
    ext_workspace_add_output(output_from_kywc_output(kywc_output));
    return false;
}

static void handle_commit(struct wl_listener *listener, void *data)
{
    struct wlr_ext_workspace_v1_commit_event *event = data;

    struct wlr_ext_workspace_v1_request *request;
    wl_list_for_each(request, event->requests, link) {
        switch (request->type) {
        case WLR_EXT_WORKSPACE_V1_REQUEST_CREATE_WORKSPACE:
            if (request->create_workspace.group) {
                workspace_create(request->create_workspace.name, workspace_manager_get_count());
            }
            break;
        case WLR_EXT_WORKSPACE_V1_REQUEST_ACTIVATE:
            if (request->activate.workspace) {
                struct ext_workspace *ext_workspace = request->activate.workspace->data;
                workspace_activate(ext_workspace->workspace);
            }
            break;
        case WLR_EXT_WORKSPACE_V1_REQUEST_REMOVE:
            if (request->remove.workspace) {
                if (workspace_manager_get_count() <= 1) {
                    kywc_log(KYWC_WARN, "reject to remove the last workspace");
                    break;
                }
                struct ext_workspace *ext_workspace = request->remove.workspace->data;
                workspace_destroy(ext_workspace->workspace);
            }
            break;
        case WLR_EXT_WORKSPACE_V1_REQUEST_DEACTIVATE:
        case WLR_EXT_WORKSPACE_V1_REQUEST_ASSIGN:
            /* one workspace is always active, and there is a single group */
            break;
        }
    }
}

static void handle_manager_destroy(struct wl_listener *listener, void *data)
{
    /* the handles are destroyed by the manager */
    struct ext_workspace *ext_workspace, *tmp_workspace;
    wl_list_for_each_safe(ext_workspace, tmp_workspace, &manager->workspaces, link) {
        ext_workspace_destroy(ext_workspace);
    }
    struct ext_workspace_output *ext_output, *tmp_output;
    wl_list_for_each_safe(ext_output, tmp_output, &manager->outputs, link) {
        ext_workspace_output_destroy(ext_output);
    }

    wl_list_remove(&manager->new_workspace.link);
    wl_list_remove(&manager->new_enabled_output.link);
    wl_list_remove(&manager->commit.link);
    wl_list_remove(&manager->destroy.link);
    free(manager);
    manager = NULL;
}

bool ext_workspace_manager_create(struct server *server)
{
    manager = calloc(1, sizeof(*manager));
    if (!manager) {
        return false;
    }

    manager->manager =
        wlr_ext_workspace_manager_v1_create(server->display, EXT_WORKSPACE_MANAGER_VERSION);
    if (!manager->manager) {
        kywc_log(KYWC_WARN, "ext_workspace_manager_v1 create failed");
        free(manager);
        manager = NULL;
        return false;
    }

    wl_list_init(&manager->workspaces);
    wl_list_init(&manager->outputs);
    wl_list_init(&manager->new_workspace.link);
    wl_list_init(&manager->new_enabled_output.link);

    manager->commit.notify = handle_commit;
    wl_signal_add(&manager->manager->events.commit, &manager->commit);
    manager->destroy.notify = handle_manager_destroy;
    wl_signal_add(&manager->manager->events.destroy, &manager->destroy);

    /* freed together with the manager on display destroy */
    manager->group = wlr_ext_workspace_group_handle_v1_create(
        manager->manager, EXT_WORKSPACE_GROUP_HANDLE_V1_GROUP_CAPABILITIES_CREATE_WORKSPACE);
    if (!manager->group) {
        kywc_log(KYWC_WARN, "ext_workspace_group_handle_v1 create failed");
        return false;
    }

    manager->new_enabled_output.notify = handle_new_enabled_output;
    output_manager_add_new_enabled_listener(&manager->new_enabled_output);
    output_manager_for_each_output(add_enabled_output, true, NULL);

    manager->new_workspace.notify = handle_new_workspace;
    workspace_manager_add_new_listener(&manager->new_workspace);
    for (uint32_t i = 0; i < workspace_manager_get_count(); i++) {
        ext_workspace_create(workspace_by_position(i));
    }

    return true;
}
