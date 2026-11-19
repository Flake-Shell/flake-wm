// SPDX-FileCopyrightText: 2026 GXDE OS
//
// SPDX-License-Identifier: GPL-1.0-or-later

/**
 * Small xdg-shell companion protocols backed by wlroots:
 *
 * - zxdg_exporter_v2 / zxdg_importer_v2: a toplevel of one client becomes the
 *   parent of a toplevel of another (portal dialogs). wlroots sets the parent
 *   with wlr_xdg_toplevel_set_parent(), which reaches view_set_parent() through
 *   the toplevel set_parent signal in xdg_shell.c.
 * - xdg_system_bell_v1: there is no sound or attention feedback to drive yet,
 *   the request is accepted and logged.
 * - xdg_toplevel_tag_manager_v1: tags and descriptions are logged, nothing
 *   matches on them yet.
 */

#include <stdlib.h>

#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_xdg_foreign_registry.h>
#include <wlr/types/wlr_xdg_foreign_v2.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_xdg_system_bell_v1.h>
#include <wlr/types/wlr_xdg_toplevel_tag_v1.h>

#include "view_p.h"

#define XDG_SYSTEM_BELL_VERSION 1
#define XDG_TOPLEVEL_TAG_VERSION 1

struct xdg_extras {
    struct wlr_xdg_system_bell_v1 *system_bell;
    struct wl_listener bell_ring;
    struct wl_listener bell_destroy;

    struct wlr_xdg_toplevel_tag_manager_v1 *toplevel_tag;
    struct wl_listener set_tag;
    struct wl_listener set_description;
    struct wl_listener tag_destroy;
};

static struct xdg_extras *extras = NULL;

static void handle_bell_ring(struct wl_listener *listener, void *data)
{
    struct wlr_xdg_system_bell_v1_ring_event *event = data;
    pid_t pid = 0;
    wl_client_get_credentials(event->client, &pid, NULL, NULL);
    kywc_log(KYWC_DEBUG, "system bell rung by client(pid=%d)%s", pid,
             event->surface ? " for a surface" : "");
}

static void handle_bell_destroy(struct wl_listener *listener, void *data)
{
    wl_list_remove(&extras->bell_ring.link);
    wl_list_remove(&extras->bell_destroy.link);
    extras->system_bell = NULL;
}

static void handle_set_tag(struct wl_listener *listener, void *data)
{
    struct wlr_xdg_toplevel_tag_manager_v1_set_tag_event *event = data;
    kywc_log(KYWC_DEBUG, "toplevel(app_id=%s) tag set to %s",
             event->toplevel->app_id ? event->toplevel->app_id : "", event->tag);
}

static void handle_set_description(struct wl_listener *listener, void *data)
{
    struct wlr_xdg_toplevel_tag_manager_v1_set_description_event *event = data;
    kywc_log(KYWC_DEBUG, "toplevel(app_id=%s) description set to %s",
             event->toplevel->app_id ? event->toplevel->app_id : "", event->description);
}

static void handle_tag_destroy(struct wl_listener *listener, void *data)
{
    wl_list_remove(&extras->set_tag.link);
    wl_list_remove(&extras->set_description.link);
    wl_list_remove(&extras->tag_destroy.link);
    extras->toplevel_tag = NULL;
}

static void handle_server_destroy(struct wl_listener *listener, void *data)
{
    /* the globals are gone with the display, server destroy comes after it */
    free(extras);
    extras = NULL;
}

static struct wl_listener server_destroy = { .notify = handle_server_destroy };

bool xdg_extras_create(struct server *server)
{
    struct wlr_xdg_foreign_registry *registry = wlr_xdg_foreign_registry_create(server->display);
    if (!registry || !wlr_xdg_foreign_v2_create(server->display, registry)) {
        kywc_log(KYWC_WARN, "zxdg_exporter_v2/zxdg_importer_v2 create failed");
    }

    extras = calloc(1, sizeof(*extras));
    if (!extras) {
        return false;
    }
    server_add_destroy_listener(server, &server_destroy);

    extras->system_bell = wlr_xdg_system_bell_v1_create(server->display, XDG_SYSTEM_BELL_VERSION);
    if (extras->system_bell) {
        extras->bell_ring.notify = handle_bell_ring;
        wl_signal_add(&extras->system_bell->events.ring, &extras->bell_ring);
        extras->bell_destroy.notify = handle_bell_destroy;
        wl_signal_add(&extras->system_bell->events.destroy, &extras->bell_destroy);
    } else {
        kywc_log(KYWC_WARN, "xdg_system_bell_v1 create failed");
    }

    extras->toplevel_tag =
        wlr_xdg_toplevel_tag_manager_v1_create(server->display, XDG_TOPLEVEL_TAG_VERSION);
    if (extras->toplevel_tag) {
        extras->set_tag.notify = handle_set_tag;
        wl_signal_add(&extras->toplevel_tag->events.set_tag, &extras->set_tag);
        extras->set_description.notify = handle_set_description;
        wl_signal_add(&extras->toplevel_tag->events.set_description, &extras->set_description);
        extras->tag_destroy.notify = handle_tag_destroy;
        wl_signal_add(&extras->toplevel_tag->events.destroy, &extras->tag_destroy);
    } else {
        kywc_log(KYWC_WARN, "xdg_toplevel_tag_manager_v1 create failed");
    }

    return true;
}
