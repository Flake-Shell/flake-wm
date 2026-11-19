// SPDX-FileCopyrightText: 2026 GXDE OS
//
// SPDX-License-Identifier: GPL-1.0-or-later

/**
 * wp_commit_timing_v1: a content update carrying a timestamp is not applied
 * before that time. The update is held back with wlr_surface_lock_pending()
 * and released by a timer.
 *
 * The timestamp is in the presentation clock domain (CLOCK_MONOTONIC). The
 * update is released half a refresh cycle before the target, so it is latched
 * for the refresh closest to the timestamp instead of the one after it.
 * Updates are still applied in commit order, wlroots keeps later cached states
 * behind an earlier locked one.
 */

#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdlib.h>
#include <time.h>

#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output.h>

#include <kywc/log.h>

#include "commit-timing-v1-protocol.h"
#include "scene/surface.h"

#define COMMIT_TIMING_MANAGER_VERSION 1
#define NSEC_PER_SEC 1000000000ll
#define NSEC_PER_MSEC 1000000ll

struct commit_timing_manager {
    struct wl_global *global;
    struct wl_list timers; // commit_timer.link
    struct wl_event_loop *event_loop;
    struct wl_listener display_destroy;
};

struct commit_timer_state {
    bool has_timestamp;
    int64_t timestamp_ns;
};

struct commit_timer {
    struct wl_list link; // commit_timing_manager.timers
    struct wl_resource *resource;

    /* NULL once the surface is destroyed, the object is inert then */
    struct wlr_surface *surface;
    struct wlr_surface_synced synced;
    struct commit_timer_state pending, current;

    struct wl_listener surface_client_commit;
    struct wl_listener surface_destroy;
};

/**
 * A held content update. Not owned by the commit timer: destroying the timer
 * object does not affect existing timing constraints.
 */
struct timed_commit {
    struct wlr_surface *surface;
    uint32_t seq;
    struct wl_event_source *timer;
    struct wl_listener surface_destroy;
};

static struct commit_timing_manager *manager = NULL;

static void timer_state_move(void *dst, void *src)
{
    /* a timestamp applies to one content update only */
    struct commit_timer_state *src_state = src;
    *(struct commit_timer_state *)dst = *src_state;
    *src_state = (struct commit_timer_state){ 0 };
}

static const struct wlr_surface_synced_impl timer_synced_impl = {
    .state_size = sizeof(struct commit_timer_state),
    .move_state = timer_state_move,
};

static void timed_commit_destroy(struct timed_commit *commit)
{
    wl_event_source_remove(commit->timer);
    wl_list_remove(&commit->surface_destroy.link);
    free(commit);
}

static int timed_commit_handle_timer(void *data)
{
    struct timed_commit *commit = data;
    struct wlr_surface *surface = commit->surface;
    uint32_t seq = commit->seq;

    timed_commit_destroy(commit);
    wlr_surface_unlock_cached(surface, seq);
    return 0;
}

static void timed_commit_handle_surface_destroy(struct wl_listener *listener, void *data)
{
    /* the locked cached state is freed together with the surface */
    struct timed_commit *commit = wl_container_of(listener, commit, surface_destroy);
    timed_commit_destroy(commit);
}

static int64_t timespec_to_ns(const struct timespec *ts)
{
    return (int64_t)ts->tv_sec * NSEC_PER_SEC + ts->tv_nsec;
}

/* half a refresh cycle of the output the surface is displayed on */
static int64_t surface_latch_lead_ns(struct wlr_surface *surface)
{
    struct ky_scene_buffer *buffer = ky_scene_buffer_try_from_surface(surface);
    if (!buffer || !buffer->primary_output) {
        return 0;
    }

    int32_t refresh = buffer->primary_output->output->refresh; // mHz
    if (refresh <= 0) {
        return 0;
    }
    return (NSEC_PER_SEC * 1000 / refresh) / 2;
}

static void commit_timer_handle_surface_client_commit(struct wl_listener *listener, void *data)
{
    struct commit_timer *timer = wl_container_of(listener, timer, surface_client_commit);
    if (!timer->pending.has_timestamp) {
        return;
    }

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int64_t release_ns = timer->pending.timestamp_ns - surface_latch_lead_ns(timer->surface);
    int64_t delay_ns = release_ns - timespec_to_ns(&now);
    /* the event loop timer has millisecond resolution */
    if (delay_ns < NSEC_PER_MSEC) {
        return;
    }

    struct wl_client *client = wl_resource_get_client(timer->resource);
    struct timed_commit *commit = calloc(1, sizeof(*commit));
    if (!commit) {
        wl_client_post_no_memory(client);
        return;
    }

    commit->timer = wl_event_loop_add_timer(manager->event_loop, timed_commit_handle_timer, commit);
    if (!commit->timer) {
        free(commit);
        wl_client_post_no_memory(client);
        return;
    }

    commit->surface = timer->surface;
    commit->seq = wlr_surface_lock_pending(timer->surface);
    commit->surface_destroy.notify = timed_commit_handle_surface_destroy;
    wl_signal_add(&timer->surface->events.destroy, &commit->surface_destroy);
    wl_event_source_timer_update(commit->timer, (int)(delay_ns / NSEC_PER_MSEC));
}

static void commit_timer_detach_surface(struct commit_timer *timer)
{
    if (!timer->surface) {
        return;
    }

    wl_list_remove(&timer->surface_client_commit.link);
    wl_list_remove(&timer->surface_destroy.link);
    wlr_surface_synced_finish(&timer->synced);
    timer->surface = NULL;
}

static void commit_timer_handle_surface_destroy(struct wl_listener *listener, void *data)
{
    struct commit_timer *timer = wl_container_of(listener, timer, surface_destroy);
    commit_timer_detach_surface(timer);
}

static struct commit_timer *commit_timer_from_resource(struct wl_resource *resource);

static void commit_timer_handle_set_timestamp(struct wl_client *client,
                                              struct wl_resource *resource, uint32_t tv_sec_hi,
                                              uint32_t tv_sec_lo, uint32_t tv_nsec)
{
    struct commit_timer *timer = commit_timer_from_resource(resource);
    if (!timer || !timer->surface) {
        wl_resource_post_error(resource, WP_COMMIT_TIMER_V1_ERROR_SURFACE_DESTROYED,
                               "the associated surface no longer exists");
        return;
    }
    if (tv_nsec >= NSEC_PER_SEC) {
        wl_resource_post_error(resource, WP_COMMIT_TIMER_V1_ERROR_INVALID_TIMESTAMP,
                               "tv_nsec %u is out of range", tv_nsec);
        return;
    }
    if (timer->pending.has_timestamp) {
        wl_resource_post_error(resource, WP_COMMIT_TIMER_V1_ERROR_TIMESTAMP_EXISTS,
                               "a timestamp is already set for this content update");
        return;
    }

    uint64_t tv_sec = ((uint64_t)tv_sec_hi << 32) | tv_sec_lo;
    /* far future timestamps would overflow, hold those as long as representable */
    if (tv_sec > INT64_MAX / NSEC_PER_SEC - 1) {
        tv_sec = INT64_MAX / NSEC_PER_SEC - 1;
    }

    timer->pending.has_timestamp = true;
    timer->pending.timestamp_ns = (int64_t)tv_sec * NSEC_PER_SEC + tv_nsec;
}

static void commit_timer_handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
    wl_resource_destroy(resource);
}

static const struct wp_commit_timer_v1_interface commit_timer_impl = {
    .set_timestamp = commit_timer_handle_set_timestamp,
    .destroy = commit_timer_handle_destroy,
};

static struct commit_timer *commit_timer_from_resource(struct wl_resource *resource)
{
    assert(wl_resource_instance_of(resource, &wp_commit_timer_v1_interface, &commit_timer_impl));
    return wl_resource_get_user_data(resource);
}

static void commit_timer_handle_resource_destroy(struct wl_resource *resource)
{
    struct commit_timer *timer = commit_timer_from_resource(resource);
    if (!timer) {
        return;
    }

    commit_timer_detach_surface(timer);
    wl_list_remove(&timer->link);
    free(timer);
}

static void manager_handle_get_timer(struct wl_client *client, struct wl_resource *manager_resource,
                                     uint32_t id, struct wl_resource *surface_resource)
{
    struct wlr_surface *surface = wlr_surface_from_resource(surface_resource);

    struct commit_timer *iter;
    wl_list_for_each(iter, &manager->timers, link) {
        if (iter->surface == surface) {
            wl_resource_post_error(manager_resource,
                                   WP_COMMIT_TIMING_MANAGER_V1_ERROR_COMMIT_TIMER_EXISTS,
                                   "a commit timer already exists for this surface");
            return;
        }
    }

    struct commit_timer *timer = calloc(1, sizeof(*timer));
    if (!timer) {
        wl_client_post_no_memory(client);
        return;
    }

    timer->resource = wl_resource_create(client, &wp_commit_timer_v1_interface,
                                         wl_resource_get_version(manager_resource), id);
    if (!timer->resource) {
        free(timer);
        wl_client_post_no_memory(client);
        return;
    }

    if (!wlr_surface_synced_init(&timer->synced, surface, &timer_synced_impl, &timer->pending,
                                 &timer->current)) {
        wl_resource_destroy(timer->resource);
        free(timer);
        wl_client_post_no_memory(client);
        return;
    }

    timer->surface = surface;
    timer->surface_client_commit.notify = commit_timer_handle_surface_client_commit;
    wl_signal_add(&surface->events.client_commit, &timer->surface_client_commit);
    timer->surface_destroy.notify = commit_timer_handle_surface_destroy;
    wl_signal_add(&surface->events.destroy, &timer->surface_destroy);

    wl_list_insert(&manager->timers, &timer->link);
    wl_resource_set_implementation(timer->resource, &commit_timer_impl, timer,
                                   commit_timer_handle_resource_destroy);
}

static void manager_handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
    wl_resource_destroy(resource);
}

static const struct wp_commit_timing_manager_v1_interface commit_timing_manager_impl = {
    .destroy = manager_handle_destroy,
    .get_timer = manager_handle_get_timer,
};

static void commit_timing_manager_bind(struct wl_client *client, void *data, uint32_t version,
                                       uint32_t id)
{
    struct wl_resource *resource =
        wl_resource_create(client, &wp_commit_timing_manager_v1_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &commit_timing_manager_impl, manager, NULL);
}

static void handle_display_destroy(struct wl_listener *listener, void *data)
{
    /* clients, and with them every timer and held update, are gone by now */
    wl_list_remove(&manager->display_destroy.link);
    wl_global_destroy(manager->global);
    free(manager);
    manager = NULL;
}

bool commit_timing_manager_create(struct wl_display *display)
{
    manager = calloc(1, sizeof(*manager));
    if (!manager) {
        return false;
    }

    manager->global =
        wl_global_create(display, &wp_commit_timing_manager_v1_interface,
                         COMMIT_TIMING_MANAGER_VERSION, manager, commit_timing_manager_bind);
    if (!manager->global) {
        kywc_log(KYWC_WARN, "wp_commit_timing_manager_v1 create failed");
        free(manager);
        manager = NULL;
        return false;
    }

    wl_list_init(&manager->timers);
    manager->event_loop = wl_display_get_event_loop(display);
    manager->display_destroy.notify = handle_display_destroy;
    wl_display_add_destroy_listener(display, &manager->display_destroy);

    return true;
}
