// SPDX-FileCopyrightText: CharOfString <root@charofstring.cc>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdlib.h>
#include <time.h>

#include <drm_fourcc.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>

#include <kywc/log.h>
#include <kywc/output.h>

#include "effect_p.h"
#include "hyprland-toplevel-export-v1-protocol.h"
#include "scene/thumbnail.h"
#include "server.h"
#include "view/view.h"

#define HYPRLAND_TOPLEVEL_EXPORT_VERSION 2
#define SESSION_IDLE_TIMEOUT_MS 1000

struct export_manager {
    struct server *server;
    struct wl_global *global;
    struct wl_list sessions;  // export_session.link
    struct wl_listener display_destroy;
};

struct export_session {
    struct wl_list link;
    struct export_manager *manager;
    struct wl_client *client;
    struct view *view;

    struct thumbnail *thumbnail;
    struct wlr_buffer *buffer;  // latest render, locked
    /* the content changed since the last frame was copied */
    bool damaged;
    /* inside the first thumbnail_update(), which may destroy the thumbnail */
    bool creating;

    struct wl_list frames;  // export_frame.link
    struct wl_event_source *idle_timer;

    struct wl_listener thumbnail_update;
    struct wl_listener thumbnail_destroy;
    struct wl_listener client_destroy;
};

struct export_frame {
    struct wl_resource *resource;
    /* NULL once the frame is done: ready or failed */
    struct export_session *session;
    struct wl_list link;          // < export_session.frames

    uint32_t width, height;       // < as advertised in the buffer event
    struct wlr_buffer *buffer;    // < the client buffer while the copy is pending
    bool used;                    // < copy was requested, a frame copies once
    bool ignore_damage;
};

static struct export_manager *manager = NULL;

static void session_destroy(struct export_session *session);

static void frame_finish(struct export_frame *frame)
{
    struct export_session *session = frame->session;
    if (!session) {
        return;
    }

    frame->session = NULL;
    wl_list_remove(&frame->link);
    wl_list_init(&frame->link);
    wlr_buffer_unlock(frame->buffer);
    frame->buffer = NULL;

    if (wl_list_empty(&session->frames)) {
        wl_event_source_timer_update(session->idle_timer, SESSION_IDLE_TIMEOUT_MS);
    }
}

static void frame_fail(struct export_frame *frame)
{
    hyprland_toplevel_export_frame_v1_send_failed(frame->resource);
    frame_finish(frame);
}

static bool read_into_buffer(struct export_session *session, struct wlr_buffer *dst)
{
    void *data = NULL;
    uint32_t format = DRM_FORMAT_INVALID;
    size_t stride = 0;
    if (!wlr_buffer_begin_data_ptr_access(dst, WLR_BUFFER_DATA_PTR_ACCESS_WRITE, &data, &format,
                                          &stride)) {
        return false;
    }

    bool ok = false;
    if (format == DRM_FORMAT_ARGB8888 || format == DRM_FORMAT_XRGB8888) {
        struct wlr_texture *texture =
            wlr_texture_from_buffer(session->manager->server->renderer, session->buffer);
        if (texture) {
            ok = wlr_texture_read_pixels(texture, &(struct wlr_texture_read_pixels_options){
                                                      .format = format,
                                                      .stride = stride,
                                                      .data = data,
                                                  });
            wlr_texture_destroy(texture);
        }
    }

    wlr_buffer_end_data_ptr_access(dst);
    return ok;
}

static void frame_copy(struct export_frame *frame)
{
    struct export_session *session = frame->session;
    if (!session->buffer || (uint32_t)session->buffer->width != frame->width ||
        (uint32_t)session->buffer->height != frame->height) {
        frame_fail(frame);
        return;
    }

    if (!read_into_buffer(session, frame->buffer)) {
        kywc_log(KYWC_WARN, "toplevel export: failed to copy into the client buffer");
        frame_fail(frame);
        return;
    }
    session->damaged = false;

    hyprland_toplevel_export_frame_v1_send_flags(frame->resource, 0);
    if (!frame->ignore_damage) {
        /* the thumbnail does not track sub-regions, report all of it */
        hyprland_toplevel_export_frame_v1_send_damage(frame->resource, 0, 0, frame->width,
                                                      frame->height);
    }

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t sec = (uint64_t)now.tv_sec;
    hyprland_toplevel_export_frame_v1_send_ready(frame->resource, sec >> 32, sec & 0xffffffff,
                                                 now.tv_nsec);
    frame_finish(frame);
}

static void session_handle_thumbnail_update(struct wl_listener *listener, void *data)
{
    struct export_session *session = wl_container_of(listener, session, thumbnail_update);
    struct thumbnail_update_event *event = data;

    if (session->buffer != event->buffer) {
        wlr_buffer_unlock(session->buffer);
        session->buffer = wlr_buffer_lock(event->buffer);
    }
    session->damaged = true;

    struct export_frame *frame, *tmp;
    wl_list_for_each_safe(frame, tmp, &session->frames, link) {
        if (frame->buffer) {
            frame_copy(frame);
        }
    }
}

static void session_handle_thumbnail_destroy(struct wl_listener *listener, void *data)
{
    struct export_session *session = wl_container_of(listener, session, thumbnail_destroy);
    wl_list_remove(&session->thumbnail_update.link);
    wl_list_remove(&session->thumbnail_destroy.link);
    session->thumbnail = NULL;
    if (!session->creating) {
        session_destroy(session);
    }
}

static void session_handle_client_destroy(struct wl_listener *listener, void *data)
{
    struct export_session *session = wl_container_of(listener, session, client_destroy);
    session_destroy(session);
}

static int session_handle_idle_timeout(void *data)
{
    struct export_session *session = data;
    if (wl_list_empty(&session->frames)) {
        session_destroy(session);
    }
    return 0;
}

static void session_destroy(struct export_session *session)
{
    struct export_frame *frame, *tmp;
    wl_list_for_each_safe(frame, tmp, &session->frames, link) {
        frame_fail(frame);
    }

    if (session->thumbnail) {
        wl_list_remove(&session->thumbnail_update.link);
        wl_list_remove(&session->thumbnail_destroy.link);
        thumbnail_destroy(session->thumbnail);
    }
    wlr_buffer_unlock(session->buffer);
    wl_event_source_remove(session->idle_timer);
    wl_list_remove(&session->client_destroy.link);
    wl_list_remove(&session->link);
    free(session);
}

static struct export_session *session_get_or_create(struct wl_client *client, struct view *view)
{
    struct export_session *session;
    wl_list_for_each(session, &manager->sessions, link) {
        if (session->client == client && session->view == view) {
            return session;
        }
    }

    session = calloc(1, sizeof(*session));
    if (!session) {
        return NULL;
    }

    float scale = view->output ? view->output->state.scale : 1.0f;
    session->thumbnail = thumbnail_create_from_view(view,
                                                    THUMBNAIL_DISABLE_DECOR |
                                                        THUMBNAIL_DISABLE_ROUND_CORNER |
                                                        THUMBNAIL_ENABLE_SECURITY,
                                                    scale > 0 ? scale : 1.0f);
    if (!session->thumbnail) {
        free(session);
        return NULL;
    }

    struct wl_event_loop *loop = wl_display_get_event_loop(manager->server->display);
    session->idle_timer = wl_event_loop_add_timer(loop, session_handle_idle_timeout, session);
    if (!session->idle_timer) {
        thumbnail_destroy(session->thumbnail);
        free(session);
        return NULL;
    }

    session->manager = manager;
    session->client = client;
    session->view = view;
    wl_list_init(&session->frames);
    wl_list_insert(&manager->sessions, &session->link);

    session->client_destroy.notify = session_handle_client_destroy;
    wl_client_add_destroy_listener(client, &session->client_destroy);
    session->thumbnail_update.notify = session_handle_thumbnail_update;
    thumbnail_add_update_listener(session->thumbnail, &session->thumbnail_update);
    session->thumbnail_destroy.notify = session_handle_thumbnail_destroy;
    thumbnail_add_destroy_listener(session->thumbnail, &session->thumbnail_destroy);

    session->creating = true;
    thumbnail_update(session->thumbnail);
    session->creating = false;
    if (!session->thumbnail) {
        session_destroy(session);
        return NULL;
    }
    return session;
}

static void frame_handle_copy(struct wl_client *client, struct wl_resource *resource,
                              struct wl_resource *buffer_resource, int32_t ignore_damage)
{
    struct export_frame *frame = wl_resource_get_user_data(resource);
    if (frame->used) {
        wl_resource_post_error(resource, HYPRLAND_TOPLEVEL_EXPORT_FRAME_V1_ERROR_ALREADY_USED,
                               "frame already used");
        return;
    }

    struct wlr_buffer *buffer = wlr_buffer_try_from_resource(buffer_resource);
    if (!buffer) {
        wl_resource_post_error(resource, HYPRLAND_TOPLEVEL_EXPORT_FRAME_V1_ERROR_INVALID_BUFFER,
                               "invalid buffer");
        return;
    }

    frame->used = true;

    if (!frame->session) {
        wlr_buffer_unlock(buffer);
        hyprland_toplevel_export_frame_v1_send_failed(resource);
        return;
    }

    frame->buffer = buffer;
    frame->ignore_damage = ignore_damage;
    if ((uint32_t)buffer->width != frame->width || (uint32_t)buffer->height != frame->height) {
        frame_fail(frame);
        return;
    }

    if (ignore_damage || frame->session->damaged) {
        frame_copy(frame);
    }
}

static void frame_handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
    wl_resource_destroy(resource);
}

static const struct hyprland_toplevel_export_frame_v1_interface frame_impl = {
    .copy = frame_handle_copy,
    .destroy = frame_handle_destroy,
};

static void frame_handle_resource_destroy(struct wl_resource *resource)
{
    struct export_frame *frame = wl_resource_get_user_data(resource);
    frame_finish(frame);
    free(frame);
}

static void manager_capture(struct wl_client *client, struct wl_resource *manager_resource,
                            uint32_t id, struct view *view)
{
    struct export_frame *frame = calloc(1, sizeof(*frame));
    if (!frame) {
        wl_client_post_no_memory(client);
        return;
    }

    frame->resource = wl_resource_create(client, &hyprland_toplevel_export_frame_v1_interface,
                                         wl_resource_get_version(manager_resource), id);
    if (!frame->resource) {
        free(frame);
        wl_client_post_no_memory(client);
        return;
    }
    wl_list_init(&frame->link);
    wl_resource_set_implementation(frame->resource, &frame_impl, frame,
                                   frame_handle_resource_destroy);

    if (!view || !view->base.mapped) {
        kywc_log(KYWC_DEBUG, "Failed to export view");
        hyprland_toplevel_export_frame_v1_send_failed(frame->resource);
        return;
    }

    struct export_session *session = session_get_or_create(client, view);
    if (!session) {
        hyprland_toplevel_export_frame_v1_send_failed(frame->resource);
        return;
    }

    wl_event_source_timer_update(session->idle_timer, 0);
    frame->session = session;
    wl_list_insert(session->frames.prev, &frame->link);

    if (!session->buffer) {
        frame_fail(frame);
        return;
    }

    frame->width = session->buffer->width;
    frame->height = session->buffer->height;
    hyprland_toplevel_export_frame_v1_send_buffer(frame->resource, WL_SHM_FORMAT_ARGB8888,
                                                  frame->width, frame->height, frame->width * 4);
    hyprland_toplevel_export_frame_v1_send_buffer_done(frame->resource);
}

static void manager_handle_capture_toplevel(struct wl_client *client,
                                            struct wl_resource *resource, uint32_t frame,
                                            int32_t overlay_cursor, uint32_t handle)
{
    manager_capture(client, resource, frame, view_from_hyprland_handle(handle));
}

static void manager_handle_capture_toplevel_with_wlr_toplevel_handle(
    struct wl_client *client, struct wl_resource *resource, uint32_t frame, int32_t overlay_cursor,
    struct wl_resource *handle)
{
    manager_capture(client, resource, frame, view_from_wlr_foreign_toplevel_resource(handle));
}

static void manager_handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
    wl_resource_destroy(resource);
}

static const struct hyprland_toplevel_export_manager_v1_interface manager_impl = {
    .capture_toplevel = manager_handle_capture_toplevel,
    .destroy = manager_handle_destroy,
    .capture_toplevel_with_wlr_toplevel_handle =
        manager_handle_capture_toplevel_with_wlr_toplevel_handle,
};

static void manager_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    struct wl_resource *resource =
        wl_resource_create(client, &hyprland_toplevel_export_manager_v1_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &manager_impl, NULL, NULL);
}

static void handle_display_destroy(struct wl_listener *listener, void *data)
{
    wl_list_remove(&manager->display_destroy.link);
    wl_global_destroy(manager->global);
    free(manager);
    manager = NULL;
}

bool hyprland_toplevel_export_manager_create(struct server *server)
{
    manager = calloc(1, sizeof(*manager));
    if (!manager) {
        return false;
    }

    manager->global = wl_global_create(server->display,
                                       &hyprland_toplevel_export_manager_v1_interface,
                                       HYPRLAND_TOPLEVEL_EXPORT_VERSION, NULL, manager_bind);
    if (!manager->global) {
        kywc_log(KYWC_WARN, "hyprland_toplevel_export_manager_v1 creation failed");
        free(manager);
        manager = NULL;
        return false;
    }

    manager->server = server;
    wl_list_init(&manager->sessions);
    manager->display_destroy.notify = handle_display_destroy;
    wl_display_add_destroy_listener(server->display, &manager->display_destroy);

    return true;
}
