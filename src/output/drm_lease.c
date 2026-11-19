// SPDX-FileCopyrightText: 2026 GXDE OS
//
// SPDX-License-Identifier: GPL-1.0-or-later

/**
 * wp_drm_lease_device_v1: hand non-desktop connectors (VR headsets) to clients
 * such as Monado/SteamVR instead of putting them into the desktop layout.
 * Only DRM backends can lease, the device is not advertised otherwise.
 */

#include <stdlib.h>

#include <wlr/types/wlr_drm_lease_v1.h>
#include <wlr/types/wlr_output.h>

#include <kywc/log.h>

#include "output_p.h"
#include "server.h"

struct drm_lease_manager {
    struct wlr_drm_lease_v1_manager *manager;
    struct wl_listener request;
    struct wl_listener destroy;
};

static struct drm_lease_manager *lease_manager = NULL;

static void handle_request(struct wl_listener *listener, void *data)
{
    struct wlr_drm_lease_request_v1 *request = data;

    /* every offered connector is non-desktop, nothing to take off the desktop */
    struct wlr_drm_lease_v1 *lease = wlr_drm_lease_request_v1_grant(request);
    if (!lease) {
        /* grant already sent wp_drm_lease_v1.finished to the client */
        kywc_log(KYWC_WARN, "failed to grant DRM lease request");
        return;
    }
    kywc_log(KYWC_INFO, "granted a DRM lease for %zu connector(s)", request->n_connectors);
}

static void handle_destroy(struct wl_listener *listener, void *data)
{
    wl_list_remove(&lease_manager->request.link);
    wl_list_remove(&lease_manager->destroy.link);
    free(lease_manager);
    lease_manager = NULL;
}

bool output_drm_lease_create(struct server *server)
{
    struct wlr_drm_lease_v1_manager *manager =
        wlr_drm_lease_v1_manager_create(server->display, server->backend);
    if (!manager) {
        kywc_log(KYWC_DEBUG, "no DRM backend, DRM leasing is not available");
        return false;
    }

    lease_manager = calloc(1, sizeof(*lease_manager));
    if (!lease_manager) {
        return false;
    }

    lease_manager->manager = manager;
    lease_manager->request.notify = handle_request;
    wl_signal_add(&manager->events.request, &lease_manager->request);
    lease_manager->destroy.notify = handle_destroy;
    wl_signal_add(&manager->events.destroy, &lease_manager->destroy);

    return true;
}

bool output_drm_lease_offer(struct wlr_output *wlr_output)
{
    if (!lease_manager || !wlr_output->non_desktop) {
        return false;
    }

    if (!wlr_drm_lease_v1_manager_offer_output(lease_manager->manager, wlr_output)) {
        kywc_log(KYWC_WARN, "failed to offer non-desktop output %s for lease", wlr_output->name);
        return false;
    }

    kywc_log(KYWC_INFO, "offering non-desktop output %s for lease", wlr_output->name);
    return true;
}
