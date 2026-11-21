// SPDX-FileCopyrightText: 2026 GXDE OS
//
// SPDX-License-Identifier: GPL-1.0-or-later

/**
 * ext_background_effect_manager_v1: blur behind translucent surfaces, using
 * the same scene blur as org_kde_kwin_blur.
 *
 * The blur region is double-buffered wl_surface state, clipped to the surface.
 * Unlike org_kde_kwin_blur an empty region means no blur, so an empty region
 * removes the effect from the scene node instead of blurring the whole node.
 */

#include <assert.h>
#include <stdlib.h>

#include <wlr/types/wlr_compositor.h>

#include "ext-background-effect-v1-protocol.h"
#include "scene/surface.h"
#include "view_p.h"

#define EXT_BACKGROUND_EFFECT_MANAGER_VERSION 1

struct background_effect_manager {
    struct wl_global *global;
    struct wl_list effects; // background_effect.link
    struct wl_listener display_destroy;
};

struct background_effect_state {
    bool committed;
    pixman_region32_t blur_region;
};

struct background_effect {
    struct wl_list link; // background_effect_manager.effects
    struct wl_resource *resource;

    /* NULL once the surface is destroyed, the object is inert then */
    struct wlr_surface *surface;
    /* false once the object is destroyed, the effect is removed on the next commit */
    bool alive;
    struct wlr_surface_synced synced;
    struct background_effect_state pending, current;

    struct wl_listener surface_commit;
    struct wl_listener surface_map;
    struct wl_listener surface_destroy;
};

static struct background_effect_manager *manager = NULL;

static void effect_state_init(void *data)
{
    struct background_effect_state *state = data;
    state->committed = false;
    pixman_region32_init(&state->blur_region);
}

static void effect_state_finish(void *data)
{
    struct background_effect_state *state = data;
    pixman_region32_fini(&state->blur_region);
}

static void effect_state_move(void *dst_data, void *src_data)
{
    struct background_effect_state *dst = dst_data, *src = src_data;
    /* only a content update that set the region changes it */
    if (src->committed) {
        pixman_region32_copy(&dst->blur_region, &src->blur_region);
        dst->committed = true;
    }
    src->committed = false;
}

static const struct wlr_surface_synced_impl effect_synced_impl = {
    .state_size = sizeof(struct background_effect_state),
    .init_state = effect_state_init,
    .finish_state = effect_state_finish,
    .move_state = effect_state_move,
};

static void effect_apply(struct background_effect *effect)
{
    struct ky_scene_buffer *buffer = ky_scene_buffer_try_from_surface(effect->surface);
    if (!buffer) {
        return;
    }

    if (!effect->alive) {
        ky_scene_node_set_blur_region(&buffer->node, NULL);
        return;
    }

    pixman_region32_t region;
    pixman_region32_init(&region);
    pixman_region32_intersect_rect(&region, &effect->current.blur_region, 0, 0,
                                   effect->surface->current.width, effect->surface->current.height);
    ky_scene_node_set_blur_region(&buffer->node,
                                  pixman_region32_not_empty(&region) ? &region : NULL);
    pixman_region32_fini(&region);
}

static void effect_free(struct background_effect *effect)
{
    if (effect->surface) {
        wl_list_remove(&effect->surface_commit.link);
        wl_list_remove(&effect->surface_map.link);
        wl_list_remove(&effect->surface_destroy.link);
        if (effect->alive) {
            wlr_surface_synced_finish(&effect->synced);
        }
    }
    wl_list_remove(&effect->link);
    free(effect);
}

static void effect_handle_surface_commit(struct wl_listener *listener, void *data)
{
    struct background_effect *effect = wl_container_of(listener, effect, surface_commit);
    effect_apply(effect);
    if (!effect->alive) {
        effect_free(effect);
    }
}

static void effect_handle_surface_map(struct wl_listener *listener, void *data)
{
    struct background_effect *effect = wl_container_of(listener, effect, surface_map);
    effect_apply(effect);
}

static void effect_handle_surface_destroy(struct wl_listener *listener, void *data)
{
    struct background_effect *effect = wl_container_of(listener, effect, surface_destroy);
    wl_list_remove(&effect->surface_commit.link);
    wl_list_remove(&effect->surface_map.link);
    wl_list_remove(&effect->surface_destroy.link);
    if (effect->alive) {
        wlr_surface_synced_finish(&effect->synced);
    }
    effect->surface = NULL;

    if (!effect->alive) {
        effect_free(effect);
    }
}

static struct background_effect *effect_from_resource(struct wl_resource *resource);

static void effect_handle_set_blur_region(struct wl_client *client, struct wl_resource *resource,
                                          struct wl_resource *region_resource)
{
    struct background_effect *effect = effect_from_resource(resource);
    if (!effect || !effect->surface) {
        wl_resource_post_error(resource, EXT_BACKGROUND_EFFECT_SURFACE_V1_ERROR_SURFACE_DESTROYED,
                               "the associated surface has been destroyed");
        return;
    }

    if (region_resource) {
        pixman_region32_copy(&effect->pending.blur_region,
                             wlr_region_from_resource(region_resource));
    } else {
        pixman_region32_clear(&effect->pending.blur_region);
    }
    effect->pending.committed = true;
}

static void effect_handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
    wl_resource_destroy(resource);
}

static const struct ext_background_effect_surface_v1_interface effect_impl = {
    .destroy = effect_handle_destroy,
    .set_blur_region = effect_handle_set_blur_region,
};

static struct background_effect *effect_from_resource(struct wl_resource *resource)
{
    assert(wl_resource_instance_of(resource, &ext_background_effect_surface_v1_interface,
                                   &effect_impl));
    return wl_resource_get_user_data(resource);
}

static void effect_handle_resource_destroy(struct wl_resource *resource)
{
    struct background_effect *effect = effect_from_resource(resource);
    if (!effect) {
        return;
    }

    effect->resource = NULL;
    if (!effect->surface) {
        effect_free(effect);
        return;
    }

    /* keep listening to the surface to remove the blur on its next commit */
    wlr_surface_synced_finish(&effect->synced);
    effect->alive = false;
}

static void manager_handle_get_background_effect(struct wl_client *client,
                                                 struct wl_resource *manager_resource, uint32_t id,
                                                 struct wl_resource *surface_resource)
{
    struct wlr_surface *surface = wlr_surface_from_resource(surface_resource);

    struct background_effect *iter;
    wl_list_for_each(iter, &manager->effects, link) {
        if (iter->surface == surface && iter->alive) {
            wl_resource_post_error(manager_resource,
                                   EXT_BACKGROUND_EFFECT_MANAGER_V1_ERROR_BACKGROUND_EFFECT_EXISTS,
                                   "the surface already has a background effect object");
            return;
        }
    }

    struct background_effect *effect = calloc(1, sizeof(*effect));
    if (!effect) {
        wl_client_post_no_memory(client);
        return;
    }

    effect->resource = wl_resource_create(client, &ext_background_effect_surface_v1_interface,
                                          wl_resource_get_version(manager_resource), id);
    if (!effect->resource) {
        free(effect);
        wl_client_post_no_memory(client);
        return;
    }

    if (!wlr_surface_synced_init(&effect->synced, surface, &effect_synced_impl, &effect->pending,
                                 &effect->current)) {
        wl_resource_destroy(effect->resource);
        free(effect);
        wl_client_post_no_memory(client);
        return;
    }

    effect->surface = surface;
    effect->alive = true;
    effect->surface_commit.notify = effect_handle_surface_commit;
    wl_signal_add(&surface->events.commit, &effect->surface_commit);
    effect->surface_map.notify = effect_handle_surface_map;
    wl_signal_add(&surface->events.map, &effect->surface_map);
    effect->surface_destroy.notify = effect_handle_surface_destroy;
    wl_signal_add(&surface->events.destroy, &effect->surface_destroy);

    wl_list_insert(&manager->effects, &effect->link);
    wl_resource_set_implementation(effect->resource, &effect_impl, effect,
                                   effect_handle_resource_destroy);
}

static void manager_handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
    wl_resource_destroy(resource);
}

static const struct ext_background_effect_manager_v1_interface manager_impl = {
    .destroy = manager_handle_destroy,
    .get_background_effect = manager_handle_get_background_effect,
};

static void manager_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    struct wl_resource *resource =
        wl_resource_create(client, &ext_background_effect_manager_v1_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &manager_impl, manager, NULL);

    ext_background_effect_manager_v1_send_capabilities(
        resource, EXT_BACKGROUND_EFFECT_MANAGER_V1_CAPABILITY_BLUR);
}

static void handle_display_destroy(struct wl_listener *listener, void *data)
{
    /* clients are destroyed before the display, only inert leftovers may remain */
    struct background_effect *effect, *tmp;
    wl_list_for_each_safe(effect, tmp, &manager->effects, link) {
        effect_free(effect);
    }
    wl_list_remove(&manager->display_destroy.link);
    wl_global_destroy(manager->global);
    free(manager);
    manager = NULL;
}

bool ext_background_effect_manager_create(struct server *server)
{
    manager = calloc(1, sizeof(*manager));
    if (!manager) {
        return false;
    }

    manager->global =
        wl_global_create(server->display, &ext_background_effect_manager_v1_interface,
                         EXT_BACKGROUND_EFFECT_MANAGER_VERSION, manager, manager_bind);
    if (!manager->global) {
        kywc_log(KYWC_WARN, "ext_background_effect_manager_v1 create failed");
        free(manager);
        manager = NULL;
        return false;
    }

    wl_list_init(&manager->effects);
    manager->display_destroy.notify = handle_display_destroy;
    wl_display_add_destroy_listener(server->display, &manager->display_destroy);

    return true;
}
