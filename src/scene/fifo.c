// SPDX-FileCopyrightText: 2026 GXDE OS
//
// SPDX-License-Identifier: GPL-1.0-or-later

/**
 * wp_fifo_v1: use the completion of a display refresh cycle as an additional
 * readiness constraint for a content update.
 *
 * - set_barrier: when the content update is applied, a barrier is set on the
 *   surface. It is cleared after the next refresh of the surface's primary
 *   output (scene buffer frame_done).
 * - wait_barrier: the content update is not ready while a barrier is set. The
 *   commit is held back with wlr_surface_lock_pending() and released, one per
 *   cleared barrier, in commit order.
 *
 * The protocol allows clearing early to keep the client making progress, so a
 * barrier on a surface that is not displayed (no primary output, hidden node,
 * inactive session) is cleared by a fallback timer instead.
 */

#include <assert.h>
#include <stdlib.h>

#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_subcompositor.h>

#include <kywc/log.h>

#include "fifo-v1-protocol.h"
#include "output.h"
#include "scene/surface.h"

#define FIFO_MANAGER_VERSION 1
/* clear a barrier no refresh cycle is going to clear, e.g. hidden surfaces */
#define FIFO_BARRIER_TIMEOUT_MS 100

struct fifo_manager {
    struct wl_global *global;
    struct wl_list fifos; // fifo.link
    struct wl_event_loop *event_loop;
    struct wl_listener display_destroy;
};

struct fifo_state {
    bool set_barrier;
    bool wait_barrier;
};

struct fifo_commit {
    struct wl_list link; // fifo.commits
    uint32_t seq;
};

struct fifo {
    struct wl_list link; // fifo_manager.fifos
    struct wl_resource *resource;
    struct fifo_manager *manager;

    /* NULL once the surface is destroyed, the object is inert then */
    struct wlr_surface *surface;
    struct wlr_surface_synced synced;
    struct fifo_state pending, current;

    /* commits locked by wait_barrier, in commit order */
    struct wl_list commits; // fifo_commit.link
    bool barrier;

    /* the scene buffer whose frame_done clears the barrier */
    struct ky_scene_buffer *buffer;
    struct wl_listener buffer_frame_done;
    struct wl_listener buffer_destroy;
    struct wl_event_source *timeout;
    struct wl_event_source *idle_clear;

    struct wl_listener surface_client_commit;
    struct wl_listener surface_destroy;
};

static struct fifo_manager *manager = NULL;

static void fifo_state_move(void *dst, void *src)
{
    /* set_barrier and wait_barrier are per content update, not sticky */
    struct fifo_state *src_state = src;
    *(struct fifo_state *)dst = *src_state;
    *src_state = (struct fifo_state){ 0 };
}

static void fifo_handle_synced_commit(struct wlr_surface_synced *synced);

static const struct wlr_surface_synced_impl fifo_synced_impl = {
    .state_size = sizeof(struct fifo_state),
    .move_state = fifo_state_move,
    .commit = fifo_handle_synced_commit,
};

static void fifo_disarm(struct fifo *fifo)
{
    if (fifo->buffer) {
        wl_list_remove(&fifo->buffer_frame_done.link);
        wl_list_remove(&fifo->buffer_destroy.link);
        wl_list_init(&fifo->buffer_frame_done.link);
        wl_list_init(&fifo->buffer_destroy.link);
        fifo->buffer = NULL;
    }
    if (fifo->timeout) {
        wl_event_source_timer_update(fifo->timeout, 0);
    }
}

/* release every held commit, e.g. when the protocol object goes away */
static void fifo_release_all(struct fifo *fifo)
{
    struct fifo_commit *commit, *tmp;
    wl_list_for_each_safe(commit, tmp, &fifo->commits, link) {
        wl_list_remove(&commit->link);
        uint32_t seq = commit->seq;
        free(commit);
        if (fifo->surface) {
            wlr_surface_unlock_cached(fifo->surface, seq);
        }
    }
}

static void fifo_clear_barrier(struct fifo *fifo)
{
    fifo_disarm(fifo);
    fifo->barrier = false;

    /**
     * Release held commits in order. Applying one may set a new barrier
     * (fifo_handle_synced_commit), then the rest wait for the next refresh.
     */
    while (!fifo->barrier && !wl_list_empty(&fifo->commits)) {
        struct fifo_commit *commit = wl_container_of(fifo->commits.next, commit, link);
        wl_list_remove(&commit->link);
        uint32_t seq = commit->seq;
        free(commit);
        wlr_surface_unlock_cached(fifo->surface, seq);
    }
}

static void fifo_handle_idle_clear(void *data)
{
    struct fifo *fifo = data;
    fifo->idle_clear = NULL;
    if (fifo->surface) {
        fifo_clear_barrier(fifo);
    }
}

/* never unlock cached states from inside a commit or a scene signal */
static void fifo_schedule_clear(struct fifo *fifo)
{
    fifo_disarm(fifo);
    if (!fifo->idle_clear) {
        fifo->idle_clear =
            wl_event_loop_add_idle(manager->event_loop, fifo_handle_idle_clear, fifo);
    }
}

static int fifo_handle_timeout(void *data)
{
    struct fifo *fifo = data;
    fifo_schedule_clear(fifo);
    return 0;
}

static void fifo_handle_buffer_frame_done(struct wl_listener *listener, void *data)
{
    struct fifo *fifo = wl_container_of(listener, fifo, buffer_frame_done);
    fifo_schedule_clear(fifo);
}

static void fifo_handle_buffer_destroy(struct wl_listener *listener, void *data)
{
    struct fifo *fifo = wl_container_of(listener, fifo, buffer_destroy);
    fifo_schedule_clear(fifo);
}

static void fifo_arm_barrier(struct fifo *fifo)
{
    fifo_disarm(fifo);

    struct ky_scene_buffer *buffer = ky_scene_buffer_try_from_surface(fifo->surface);
    if (!buffer || !buffer->primary_output) {
        /* not displayed, nothing will latch this update */
        fifo_schedule_clear(fifo);
        return;
    }

    fifo->buffer = buffer;
    wl_signal_add(&buffer->events.frame_done, &fifo->buffer_frame_done);
    wl_signal_add(&buffer->node.events.destroy, &fifo->buffer_destroy);
    output_schedule_frame(buffer->primary_output->output);

    if (fifo->timeout) {
        wl_event_source_timer_update(fifo->timeout, FIFO_BARRIER_TIMEOUT_MS);
    }
}

static void fifo_handle_synced_commit(struct wlr_surface_synced *synced)
{
    struct fifo *fifo = wl_container_of(synced, fifo, synced);
    if (!fifo->current.set_barrier) {
        return;
    }

    fifo->barrier = true;
    fifo_arm_barrier(fifo);
}

static bool fifo_barrier_pending(struct fifo *fifo)
{
    if (fifo->barrier || !wl_list_empty(&fifo->commits)) {
        return true;
    }

    /* a cached update held by someone else may set the barrier when applied */
    struct wlr_surface_state *cached;
    wl_list_for_each(cached, &fifo->surface->cached, cached_state_link) {
        struct fifo_state *state = wlr_surface_synced_get_state(&fifo->synced, cached);
        if (state && state->set_barrier) {
            return true;
        }
    }
    return false;
}

static void fifo_handle_surface_client_commit(struct wl_listener *listener, void *data)
{
    struct fifo *fifo = wl_container_of(listener, fifo, surface_client_commit);
    if (!fifo->pending.wait_barrier || !fifo_barrier_pending(fifo)) {
        return;
    }

    /* the constraint must be ignored for subsurfaces in synchronized mode */
    struct wlr_subsurface *subsurface = wlr_subsurface_try_from_wlr_surface(fifo->surface);
    if (subsurface && subsurface->synchronized) {
        return;
    }

    struct fifo_commit *commit = calloc(1, sizeof(*commit));
    if (!commit) {
        wl_client_post_no_memory(wl_resource_get_client(fifo->resource));
        return;
    }

    commit->seq = wlr_surface_lock_pending(fifo->surface);
    wl_list_insert(fifo->commits.prev, &commit->link);
}

static void fifo_detach_surface(struct fifo *fifo)
{
    if (!fifo->surface) {
        return;
    }

    /* releasing may apply an update that arms a new barrier, disarm after it */
    fifo_release_all(fifo);
    fifo_disarm(fifo);
    wl_list_remove(&fifo->surface_client_commit.link);
    wl_list_remove(&fifo->surface_destroy.link);
    wlr_surface_synced_finish(&fifo->synced);
    fifo->surface = NULL;
    fifo->barrier = false;
}

static void fifo_handle_surface_destroy(struct wl_listener *listener, void *data)
{
    struct fifo *fifo = wl_container_of(listener, fifo, surface_destroy);
    /* locked cached states are freed together with the surface */
    struct fifo_commit *commit, *tmp;
    wl_list_for_each_safe(commit, tmp, &fifo->commits, link) {
        wl_list_remove(&commit->link);
        free(commit);
    }
    fifo_detach_surface(fifo);
}

static void fifo_destroy(struct fifo *fifo)
{
    fifo_detach_surface(fifo);
    if (fifo->timeout) {
        wl_event_source_remove(fifo->timeout);
    }
    if (fifo->idle_clear) {
        wl_event_source_remove(fifo->idle_clear);
    }
    wl_list_remove(&fifo->link);
    wl_resource_set_user_data(fifo->resource, NULL);
    free(fifo);
}

static struct fifo *fifo_from_resource(struct wl_resource *resource);

static void fifo_handle_set_barrier(struct wl_client *client, struct wl_resource *resource)
{
    struct fifo *fifo = fifo_from_resource(resource);
    if (!fifo || !fifo->surface) {
        wl_resource_post_error(resource, WP_FIFO_V1_ERROR_SURFACE_DESTROYED,
                               "the associated surface no longer exists");
        return;
    }
    fifo->pending.set_barrier = true;
}

static void fifo_handle_wait_barrier(struct wl_client *client, struct wl_resource *resource)
{
    struct fifo *fifo = fifo_from_resource(resource);
    if (!fifo || !fifo->surface) {
        wl_resource_post_error(resource, WP_FIFO_V1_ERROR_SURFACE_DESTROYED,
                               "the associated surface no longer exists");
        return;
    }
    fifo->pending.wait_barrier = true;
}

static void fifo_handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
    wl_resource_destroy(resource);
}

static const struct wp_fifo_v1_interface fifo_impl = {
    .set_barrier = fifo_handle_set_barrier,
    .wait_barrier = fifo_handle_wait_barrier,
    .destroy = fifo_handle_destroy,
};

static struct fifo *fifo_from_resource(struct wl_resource *resource)
{
    assert(wl_resource_instance_of(resource, &wp_fifo_v1_interface, &fifo_impl));
    return wl_resource_get_user_data(resource);
}

static void fifo_handle_resource_destroy(struct wl_resource *resource)
{
    struct fifo *fifo = fifo_from_resource(resource);
    if (fifo) {
        fifo_destroy(fifo);
    }
}

static void manager_handle_get_fifo(struct wl_client *client, struct wl_resource *manager_resource,
                                    uint32_t id, struct wl_resource *surface_resource)
{
    struct wlr_surface *surface = wlr_surface_from_resource(surface_resource);

    struct fifo *iter;
    wl_list_for_each(iter, &manager->fifos, link) {
        if (iter->surface == surface) {
            wl_resource_post_error(manager_resource, WP_FIFO_MANAGER_V1_ERROR_ALREADY_EXISTS,
                                   "a fifo object already exists for this surface");
            return;
        }
    }

    struct fifo *fifo = calloc(1, sizeof(*fifo));
    if (!fifo) {
        wl_client_post_no_memory(client);
        return;
    }

    fifo->resource = wl_resource_create(client, &wp_fifo_v1_interface,
                                        wl_resource_get_version(manager_resource), id);
    if (!fifo->resource) {
        free(fifo);
        wl_client_post_no_memory(client);
        return;
    }

    if (!wlr_surface_synced_init(&fifo->synced, surface, &fifo_synced_impl, &fifo->pending,
                                 &fifo->current)) {
        wl_resource_destroy(fifo->resource);
        free(fifo);
        wl_client_post_no_memory(client);
        return;
    }

    fifo->manager = manager;
    fifo->surface = surface;
    wl_list_init(&fifo->commits);
    wl_list_init(&fifo->buffer_frame_done.link);
    wl_list_init(&fifo->buffer_destroy.link);
    fifo->buffer_frame_done.notify = fifo_handle_buffer_frame_done;
    fifo->buffer_destroy.notify = fifo_handle_buffer_destroy;
    fifo->timeout = wl_event_loop_add_timer(manager->event_loop, fifo_handle_timeout, fifo);

    fifo->surface_client_commit.notify = fifo_handle_surface_client_commit;
    wl_signal_add(&surface->events.client_commit, &fifo->surface_client_commit);
    fifo->surface_destroy.notify = fifo_handle_surface_destroy;
    wl_signal_add(&surface->events.destroy, &fifo->surface_destroy);

    wl_list_insert(&manager->fifos, &fifo->link);
    wl_resource_set_implementation(fifo->resource, &fifo_impl, fifo, fifo_handle_resource_destroy);
}

static void manager_handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
    wl_resource_destroy(resource);
}

static const struct wp_fifo_manager_v1_interface fifo_manager_impl = {
    .destroy = manager_handle_destroy,
    .get_fifo = manager_handle_get_fifo,
};

static void fifo_manager_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    struct wl_resource *resource =
        wl_resource_create(client, &wp_fifo_manager_v1_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &fifo_manager_impl, manager, NULL);
}

static void handle_display_destroy(struct wl_listener *listener, void *data)
{
    /* clients, and with them every fifo, are gone before the display destroy signal */
    wl_list_remove(&manager->display_destroy.link);
    wl_global_destroy(manager->global);
    free(manager);
    manager = NULL;
}

bool fifo_manager_create(struct wl_display *display)
{
    manager = calloc(1, sizeof(*manager));
    if (!manager) {
        return false;
    }

    manager->global = wl_global_create(display, &wp_fifo_manager_v1_interface, FIFO_MANAGER_VERSION,
                                       manager, fifo_manager_bind);
    if (!manager->global) {
        kywc_log(KYWC_WARN, "wp_fifo_manager_v1 create failed");
        free(manager);
        manager = NULL;
        return false;
    }

    wl_list_init(&manager->fifos);
    manager->event_loop = wl_display_get_event_loop(display);
    manager->display_destroy.notify = handle_display_destroy;
    wl_display_add_destroy_listener(display, &manager->display_destroy);

    return true;
}
