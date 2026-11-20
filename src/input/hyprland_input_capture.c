// SPDX-FileCopyrightText: CharOfString <root@charofstring.cc>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libeis.h>
#include <linux/input-event-codes.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_seat.h>
#include <xkbcommon/xkbcommon.h>

#include <kywc/log.h>

#include "hyprland-input-capture-v1-protocol.h"
#include "input_p.h"
#include "output.h"
#include "server.h"
#include "view/session_lock.h"

#define HYPRLAND_INPUT_CAPTURE_VERSION 2
#define MAX_HELD 32
#define MAX_LAYOUT_OUTPUTS 16

/**
 * hyprland_input_capture_manager_v1, the backend of xdg-desktop-portal-hyprland
 * for the InputCapture portal (Deskflow, Input Leap and the like).
 *
 * A session places barriers on the outer edges of the outputs. When the
 * pointer crosses the barrier of an enabled session, input is captured: the
 * cursor stops and hides, and pointer motion, buttons, scrolling and keys go
 * to the session's EIS socket instead of the local clients, until the session
 * releases it. Super+Shift+Escape always releases it, as in KWin; VT switching
 * keeps working, and the lock screen and an output layout change end it.
 */

enum session_status {
    SESSION_CREATED,
    SESSION_ENABLED,
    SESSION_ACTIVATED,
};

struct capture_barrier {
    struct wl_list link;  // capture_session.barriers
    uint32_t id;
    int x1, y1, x2, y2;
};

struct held {
    uint32_t codes[MAX_HELD];
    size_t count;
};

struct capture_session {
    struct wl_list link;  // capture_manager.sessions
    struct wl_resource *resource;  // NULL while the session is destroyed
    struct wl_list barriers;
    enum session_status status;
    uint32_t activation_id;

    struct eis *eis;
    struct wl_event_source *eis_source;
    struct eis_client *client;
    struct eis_seat *eis_seat;
    struct eis_device *pointer, *keyboard;
    struct xkb_keymap *keymap;  // the keymap the keyboard device was given
};

static struct capture_manager {
    struct wl_global *global;
    struct wl_list sessions;
    struct capture_session *active;
    struct seat *seat;  // the seat whose input is captured
    struct wl_listener seat_destroy;

    /* forwarded presses, released on the EIS side when the capture ends */
    struct held keys, buttons;
    /* presses that went to the EIS side, their releases must not reach clients */
    struct held swallow_keys, swallow_buttons;

    struct wl_event_source *repeat_timer;
    uint32_t repeat_key;

    /* the output layout the barriers were checked against */
    struct layout {
        int count;
        struct {
            struct kywc_box box;
            float scale;
        } outputs[MAX_LAYOUT_OUTPUTS];
    } layout;

    struct wl_listener configured;
    struct wl_listener display_destroy;
} *manager = NULL;

static bool held_remove(struct held *held, uint32_t code)
{
    for (size_t i = 0; i < held->count; i++) {
        if (held->codes[i] == code) {
            held->codes[i] = held->codes[--held->count];
            return true;
        }
    }
    return false;
}

static void held_add(struct held *held, uint32_t code)
{
    held_remove(held, code);
    if (held->count < MAX_HELD) {
        held->codes[held->count++] = code;
    }
}

static uint64_t session_now(struct capture_session *session)
{
    return eis_now(session->eis);
}

/* ---------- EIS devices ---------- */

struct region_data {
    struct eis_device *device;
};

static bool add_region(struct kywc_output *kywc_output, int index, void *data)
{
    struct region_data *region_data = data;
    struct output *output = output_from_kywc_output(kywc_output);
    struct eis_region *region = eis_device_new_region(region_data->device);
    eis_region_set_offset(region, output->geometry.x, output->geometry.y);
    eis_region_set_size(region, output->geometry.width, output->geometry.height);
    eis_region_set_physical_scale(region, output->wlr_output->scale);
    eis_region_add(region);
    eis_region_unref(region);
    return false;
}

static void session_clear_pointer(struct capture_session *session)
{
    if (!session->pointer) {
        return;
    }
    eis_device_remove(session->pointer);
    eis_device_unref(session->pointer);
    session->pointer = NULL;
}

static void session_clear_keyboard(struct capture_session *session)
{
    if (!session->keyboard) {
        return;
    }
    eis_device_remove(session->keyboard);
    eis_device_unref(session->keyboard);
    session->keyboard = NULL;
    if (session->keymap) {
        xkb_keymap_unref(session->keymap);
        session->keymap = NULL;
    }
}

static void session_ensure_pointer(struct capture_session *session)
{
    if (session->pointer || !session->eis_seat) {
        return;
    }

    struct eis_device *pointer = eis_seat_new_device(session->eis_seat);
    eis_device_configure_name(pointer, "captured pointer");
    eis_device_configure_capability(pointer, EIS_DEVICE_CAP_POINTER);
    eis_device_configure_capability(pointer, EIS_DEVICE_CAP_BUTTON);
    eis_device_configure_capability(pointer, EIS_DEVICE_CAP_SCROLL);
    struct region_data region_data = { .device = pointer };
    output_manager_for_each_output(add_region, true, &region_data);
    eis_device_add(pointer);
    eis_device_resume(pointer);
    session->pointer = pointer;

    if (session->status == SESSION_ACTIVATED) {
        eis_device_start_emulating(pointer, session->activation_id);
    }
}

static struct wlr_keyboard *seat_wlr_keyboard(void)
{
    struct seat *seat = manager->seat ? manager->seat : input_manager_get_default_seat();
    return seat && seat->keyboard ? seat->keyboard->wlr_keyboard : NULL;
}

static void session_ensure_keyboard(struct capture_session *session)
{
    if (session->keyboard || !session->eis_seat) {
        return;
    }

    struct eis_device *keyboard = eis_seat_new_device(session->eis_seat);
    eis_device_configure_name(keyboard, "captured keyboard");
    eis_device_configure_capability(keyboard, EIS_DEVICE_CAP_KEYBOARD);

    struct wlr_keyboard *wlr_keyboard = seat_wlr_keyboard();
    if (wlr_keyboard && wlr_keyboard->keymap && wlr_keyboard->keymap_fd >= 0) {
        struct eis_keymap *keymap = eis_device_new_keymap(
            keyboard, EIS_KEYMAP_TYPE_XKB, wlr_keyboard->keymap_fd, wlr_keyboard->keymap_size);
        if (keymap) {
            eis_keymap_add(keymap);
            eis_keymap_unref(keymap);
            session->keymap = xkb_keymap_ref(wlr_keyboard->keymap);
        }
    }

    eis_device_add(keyboard);
    eis_device_resume(keyboard);
    session->keyboard = keyboard;

    if (session->status == SESSION_ACTIVATED) {
        eis_device_start_emulating(keyboard, session->activation_id);
    }
}

/* the layout changed since the keyboard device was announced */
static void session_check_keymap(struct capture_session *session)
{
    struct wlr_keyboard *wlr_keyboard = seat_wlr_keyboard();
    if (session->keyboard && wlr_keyboard && wlr_keyboard->keymap != session->keymap) {
        session_clear_keyboard(session);
        session_ensure_keyboard(session);
    }
}

static void session_handle_eis_event(struct capture_session *session, struct eis_event *event)
{
    struct eis_client *client = eis_event_get_client(event);

    switch (eis_event_get_type(event)) {
    case EIS_EVENT_CLIENT_CONNECT:
        /* one receiver per session, input only flows to it */
        if (eis_client_is_sender(client) || session->client) {
            kywc_log(KYWC_WARN, "input capture: refusing EIS client %s",
                     eis_client_get_name(client) ? eis_client_get_name(client) : "?");
            eis_client_disconnect(client);
            break;
        }
        session->client = client;
        eis_client_connect(client);
        session->eis_seat = eis_client_new_seat(client, "default");
        eis_seat_configure_capability(session->eis_seat, EIS_DEVICE_CAP_POINTER);
        eis_seat_configure_capability(session->eis_seat, EIS_DEVICE_CAP_BUTTON);
        eis_seat_configure_capability(session->eis_seat, EIS_DEVICE_CAP_SCROLL);
        eis_seat_configure_capability(session->eis_seat, EIS_DEVICE_CAP_KEYBOARD);
        eis_seat_add(session->eis_seat);
        break;
    case EIS_EVENT_CLIENT_DISCONNECT:
        if (client != session->client) {
            break;
        }
        session_clear_pointer(session);
        session_clear_keyboard(session);
        eis_seat_unref(session->eis_seat);
        session->eis_seat = NULL;
        session->client = NULL;
        eis_client_disconnect(client);
        break;
    case EIS_EVENT_SEAT_BIND:
        if (eis_event_seat_has_capability(event, EIS_DEVICE_CAP_POINTER) &&
            eis_event_seat_has_capability(event, EIS_DEVICE_CAP_BUTTON) &&
            eis_event_seat_has_capability(event, EIS_DEVICE_CAP_SCROLL)) {
            session_ensure_pointer(session);
        } else {
            session_clear_pointer(session);
        }
        if (eis_event_seat_has_capability(event, EIS_DEVICE_CAP_KEYBOARD)) {
            session_ensure_keyboard(session);
        } else {
            session_clear_keyboard(session);
        }
        break;
    case EIS_EVENT_DEVICE_CLOSED:
        if (eis_event_get_device(event) == session->pointer) {
            session_clear_pointer(session);
        } else if (eis_event_get_device(event) == session->keyboard) {
            session_clear_keyboard(session);
        }
        break;
    default:
        break;
    }
}

static int session_handle_eis_readable(int fd, uint32_t mask, void *data)
{
    struct capture_session *session = data;
    eis_dispatch(session->eis);

    struct eis_event *event;
    while ((event = eis_get_event(session->eis))) {
        session_handle_eis_event(session, event);
        eis_event_unref(event);
    }
    return 0;
}

/* ---------- capture state ---------- */

static void repeat_stop(void)
{
    manager->repeat_key = 0;
    wl_event_source_timer_update(manager->repeat_timer, 0);
}

static void send_key(struct capture_session *session, uint32_t key, bool pressed)
{
    if (!session->keyboard) {
        return;
    }
    eis_device_keyboard_key(session->keyboard, key, pressed);
    eis_device_frame(session->keyboard, session_now(session));
}

static int handle_repeat(void *data)
{
    struct capture_session *session = manager->active;
    struct wlr_keyboard *wlr_keyboard = seat_wlr_keyboard();
    if (!session || !manager->repeat_key || !wlr_keyboard) {
        return 0;
    }

    /* EIS has no key repeat, the receiver sees it as presses */
    send_key(session, manager->repeat_key, false);
    send_key(session, manager->repeat_key, true);
    if (wlr_keyboard->repeat_info.rate > 0) {
        wl_event_source_timer_update(manager->repeat_timer,
                                     1000 / wlr_keyboard->repeat_info.rate);
    }
    return 0;
}

static void cursor_hide(bool hidden)
{
    if (manager->seat && manager->seat->cursor) {
        cursor_set_hidden(manager->seat->cursor, hidden);
    }
}

static bool session_activate(struct capture_session *session, double x, double y,
                             uint32_t barrier_id)
{
    if (session->status != SESSION_ENABLED || session_lock_is_active()) {
        return false;
    }

    session->activation_id += 5;
    session->status = SESSION_ACTIVATED;
    manager->active = session;
    session_check_keymap(session);

    if (session->pointer) {
        eis_device_start_emulating(session->pointer, session->activation_id);
    }
    if (session->keyboard) {
        eis_device_start_emulating(session->keyboard, session->activation_id);
    }

    kywc_log(KYWC_INFO, "input capture: captured at %.1f,%.1f by barrier %u", x, y, barrier_id);
    hyprland_input_capture_v1_send_activated(session->resource, session->activation_id,
                                             wl_fixed_from_double(x), wl_fixed_from_double(y),
                                             barrier_id);
    cursor_hide(true);
    return true;
}

static void session_deactivate(struct capture_session *session)
{
    if (session->status != SESSION_ACTIVATED) {
        return;
    }

    repeat_stop();

    /* nothing stays pressed on the other side; the local releases are swallowed */
    for (size_t i = 0; i < manager->keys.count; i++) {
        send_key(session, manager->keys.codes[i], false);
        held_add(&manager->swallow_keys, manager->keys.codes[i]);
    }
    manager->keys.count = 0;
    if (session->pointer) {
        for (size_t i = 0; i < manager->buttons.count; i++) {
            eis_device_button_button(session->pointer, manager->buttons.codes[i], false);
            held_add(&manager->swallow_buttons, manager->buttons.codes[i]);
        }
        if (manager->buttons.count) {
            eis_device_frame(session->pointer, session_now(session));
        }
        eis_device_stop_emulating(session->pointer);
    }
    manager->buttons.count = 0;
    if (session->keyboard) {
        eis_device_stop_emulating(session->keyboard);
    }

    session->status = SESSION_ENABLED;
    manager->active = NULL;
    kywc_log(KYWC_INFO, "input capture: released");
    if (session->resource) {
        hyprland_input_capture_v1_send_deactivated(session->resource, session->activation_id);
    }

    /* the local clients did not see the modifier changes */
    struct wlr_keyboard *wlr_keyboard = seat_wlr_keyboard();
    if (manager->seat && wlr_keyboard) {
        wlr_seat_set_keyboard(manager->seat->wlr_seat, wlr_keyboard);
        wlr_seat_keyboard_notify_modifiers(manager->seat->wlr_seat, &wlr_keyboard->modifiers);
    }
    cursor_hide(false);
}

static void session_disable(struct capture_session *session)
{
    session_deactivate(session);
    if (session->status != SESSION_ENABLED) {
        return;
    }
    session->status = SESSION_CREATED;
    if (session->resource) {
        hyprland_input_capture_v1_send_disabled(session->resource);
    }
}

static void session_clear_barriers(struct capture_session *session)
{
    struct capture_barrier *barrier, *tmp;
    wl_list_for_each_safe(barrier, tmp, &session->barriers, link) {
        wl_list_remove(&barrier->link);
        free(barrier);
    }
}

void input_capture_force_release(void)
{
    if (manager && manager->active) {
        session_disable(manager->active);
    }
}

/*
 * A transient seat can go away at any time. Other destroy listeners may already have
 * torn down parts of it (its cursor's selection state), so forget the seat before
 * ending the capture: the release then leaves the dying seat's keyboard and cursor alone.
 */
static void handle_seat_destroy(struct wl_listener *listener, void *data)
{
    wl_list_remove(&manager->seat_destroy.link);
    wl_list_init(&manager->seat_destroy.link);
    manager->seat = NULL;
    input_capture_force_release();
}

static void manager_set_seat(struct seat *seat)
{
    if (manager->seat == seat) {
        return;
    }
    wl_list_remove(&manager->seat_destroy.link);
    wl_signal_add(&seat->events.destroy, &manager->seat_destroy);
    manager->seat = seat;
}

bool input_capture_is_active(void)
{
    return manager && manager->active;
}

/* ---------- barriers ---------- */

enum barrier_fit {
    BARRIER_INVALID,
    BARRIER_PARTIAL,
    BARRIER_VALID,
};

struct barrier_check {
    int x1, y1, x2, y2;
    int valid, partial;
};

/* a barrier must run along a whole outer edge of exactly one output */
static bool check_barrier_output(struct kywc_output *kywc_output, int index, void *data)
{
    struct barrier_check *check = data;
    struct kywc_box *box = &output_from_kywc_output(kywc_output)->geometry;
    int mx1 = box->x, my1 = box->y;
    int mx2 = box->x + box->width - 1, my2 = box->y + box->height - 1;
    enum barrier_fit fit = BARRIER_VALID;

    if (check->x1 == check->x2) {
        if (check->x1 != mx1 && check->x1 != mx2 + 1) {
            fit = BARRIER_INVALID;
        } else if (check->y1 != my1 || check->y2 != my2) {
            fit = (my1 <= check->y1 && check->y1 <= my2) || (my1 <= check->y2 && check->y2 <= my2)
                      ? BARRIER_PARTIAL
                      : BARRIER_INVALID;
        }
    } else {
        if (check->y1 != my1 && check->y1 != my2 + 1) {
            fit = BARRIER_INVALID;
        } else if (check->x1 != mx1 || check->x2 != mx2) {
            fit = (mx1 <= check->x1 && check->x1 <= mx2) || (mx1 <= check->x2 && check->x2 <= mx2)
                      ? BARRIER_PARTIAL
                      : BARRIER_INVALID;
        }
    }

    check->valid += fit == BARRIER_VALID;
    check->partial += fit == BARRIER_PARTIAL;
    return false;
}

static bool barrier_is_valid(int x1, int y1, int x2, int y2)
{
    if ((x1 != x2 && y1 != y2) || (x1 == x2 && y1 == y2)) {
        return false;
    }

    struct barrier_check check = {
        .x1 = x1 < x2 ? x1 : x2,
        .y1 = y1 < y2 ? y1 : y2,
        .x2 = x1 < x2 ? x2 : x1,
        .y2 = y1 < y2 ? y2 : y1,
    };
    output_manager_for_each_output(check_barrier_output, true, &check);
    return check.valid == 1 && check.partial == 0;
}

static bool segments_intersect(double p0x, double p0y, double p1x, double p1y, double p2x,
                               double p2y, double p3x, double p3y)
{
    double s1x = p1x - p0x, s1y = p1y - p0y;
    double s2x = p3x - p2x, s2y = p3y - p2y;
    double denom = -s2x * s1y + s1x * s2y;
    if (denom == 0) {
        return false;
    }
    double s = (-s1y * (p0x - p2x) + s1x * (p0y - p2y)) / denom;
    double t = (s2x * (p0y - p2y) - s2y * (p0x - p2x)) / denom;
    return s >= 0 && s <= 1 && t >= 0 && t <= 1;
}

/* ---------- input routing, called from cursor.c and keyboard.c ---------- */

bool input_capture_handle_motion(struct seat *seat, double lx, double ly, double dx, double dy)
{
    if (!manager) {
        return false;
    }

    struct capture_session *session = manager->active;
    if (session) {
        if (session_lock_is_active()) {
            input_capture_force_release();
            return false;
        }
        if (session->pointer) {
            eis_device_pointer_motion(session->pointer, dx, dy);
        }
        return true;
    }

    /* no capture out of a pointer constraint or an interactive move */
    if (session_lock_is_active() || seat->pointer_grab || seat->cursor->active_constraint) {
        return false;
    }

    double nx = lx + dx, ny = ly + dy;
    wl_list_for_each(session, &manager->sessions, link) {
        if (session->status != SESSION_ENABLED) {
            continue;
        }
        struct capture_barrier *barrier;
        wl_list_for_each(barrier, &session->barriers, link) {
            if (!segments_intersect(barrier->x1, barrier->y1, barrier->x2, barrier->y2, lx, ly,
                                    nx, ny)) {
                continue;
            }
            manager_set_seat(seat);
            if (!session_activate(session, nx, ny, barrier->id)) {
                return false;
            }
            if (session->pointer) {
                eis_device_pointer_motion(session->pointer, dx, dy);
            }
            return true;
        }
    }
    return false;
}

bool input_capture_handle_button(uint32_t button, bool pressed)
{
    if (!manager) {
        return false;
    }
    if (!pressed && held_remove(&manager->swallow_buttons, button)) {
        return true;
    }

    struct capture_session *session = manager->active;
    if (!session) {
        return false;
    }
    /* released here but pressed before the capture: the local client gets it */
    if (!pressed && !held_remove(&manager->buttons, button)) {
        return false;
    }
    if (pressed) {
        held_add(&manager->buttons, button);
    }
    if (session->pointer) {
        eis_device_button_button(session->pointer, button, pressed);
    }
    return true;
}

bool input_capture_handle_axis(enum wl_pointer_axis_source source, bool vertical, double delta,
                               int32_t delta_discrete)
{
    struct capture_session *session = manager ? manager->active : NULL;
    if (!session) {
        return false;
    }
    if (!session->pointer) {
        return true;
    }

    if (source == WL_POINTER_AXIS_SOURCE_WHEEL && delta_discrete) {
        eis_device_scroll_discrete(session->pointer, vertical ? 0 : delta_discrete,
                                   vertical ? delta_discrete : 0);
    } else if (delta == 0) {
        eis_device_scroll_stop(session->pointer, !vertical, vertical);
    } else {
        eis_device_scroll_delta(session->pointer, vertical ? 0 : delta, vertical ? delta : 0);
    }
    return true;
}

bool input_capture_handle_frame(void)
{
    struct capture_session *session = manager ? manager->active : NULL;
    if (!session) {
        return false;
    }
    if (session->pointer) {
        eis_device_frame(session->pointer, session_now(session));
    }
    return true;
}

bool input_capture_handle_key(struct keyboard *keyboard, uint32_t key, bool pressed,
                              uint32_t modifiers)
{
    if (!manager) {
        return false;
    }
    if (!pressed && held_remove(&manager->swallow_keys, key)) {
        return true;
    }

    struct capture_session *session = manager->active;
    if (!session) {
        return false;
    }
    if (session_lock_is_active()) {
        input_capture_force_release();
        return false;
    }

    const xkb_keysym_t *keysyms;
    int count = xkb_state_key_get_syms(keyboard->wlr_keyboard->xkb_state, key + 8, &keysyms);
    for (int i = 0; i < count; i++) {
        /* VT switching stays a compositor escape */
        if (keysyms[i] >= XKB_KEY_XF86Switch_VT_1 && keysyms[i] <= XKB_KEY_XF86Switch_VT_12) {
            return false;
        }
        uint32_t mods = modifiers & (WLR_MODIFIER_SHIFT | WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT |
                                     WLR_MODIFIER_LOGO);
        if (pressed && keysyms[i] == XKB_KEY_Escape &&
            mods == (WLR_MODIFIER_LOGO | WLR_MODIFIER_SHIFT)) {
            kywc_log(KYWC_INFO, "input capture: released by Super+Shift+Escape");
            input_capture_force_release();
            held_add(&manager->swallow_keys, key);
            return true;
        }
    }

    /* released here but pressed before the capture: the local client gets it */
    if (!pressed && !held_remove(&manager->keys, key)) {
        return false;
    }
    if (pressed) {
        held_add(&manager->keys, key);
    }
    send_key(session, key, pressed);

    if (pressed && xkb_keymap_key_repeats(keyboard->wlr_keyboard->keymap, key + 8) &&
        keyboard->wlr_keyboard->repeat_info.delay > 0 &&
        keyboard->wlr_keyboard->repeat_info.rate > 0) {
        manager->repeat_key = key;
        wl_event_source_timer_update(manager->repeat_timer,
                                     keyboard->wlr_keyboard->repeat_info.delay);
    } else if (!pressed && key == manager->repeat_key) {
        repeat_stop();
    }
    return true;
}

bool input_capture_handle_modifiers(void)
{
    /* the keys carry the modifiers to the receiver, local clients get them on release */
    return input_capture_is_active();
}

/* ---------- protocol ---------- */

static struct capture_session *session_from_resource(struct wl_resource *resource);

static void session_handle_clear_barriers(struct wl_client *client, struct wl_resource *resource)
{
    struct capture_session *session = session_from_resource(resource);
    if (session) {
        session_clear_barriers(session);
    }
}

static void session_handle_add_barrier(struct wl_client *client, struct wl_resource *resource,
                                       uint32_t zone_set, uint32_t id, uint32_t ux1, uint32_t uy1,
                                       uint32_t ux2, uint32_t uy2)
{
    struct capture_session *session = session_from_resource(resource);
    if (!session) {
        return;
    }

    /* negative layout coordinates come as two's complement */
    int x1 = (int32_t)ux1, y1 = (int32_t)uy1, x2 = (int32_t)ux2, y2 = (int32_t)uy2;
    if (!barrier_is_valid(x1, y1, x2, y2)) {
        wl_resource_post_error(resource, HYPRLAND_INPUT_CAPTURE_V1_ERROR_INVALID_BARRIER,
                               "barrier %u (%d,%d)-(%d,%d) is not a whole outer output edge", id,
                               x1, y1, x2, y2);
        return;
    }

    struct capture_barrier *barrier = calloc(1, sizeof(*barrier));
    if (!barrier) {
        wl_resource_post_no_memory(resource);
        return;
    }
    barrier->id = id;
    barrier->x1 = x1;
    barrier->y1 = y1;
    barrier->x2 = x2;
    barrier->y2 = y2;
    wl_list_insert(session->barriers.prev, &barrier->link);
}

static void session_handle_enable(struct wl_client *client, struct wl_resource *resource)
{
    struct capture_session *session = session_from_resource(resource);
    if (session && session->status == SESSION_CREATED) {
        session->status = SESSION_ENABLED;
    }
}

static void session_handle_disable(struct wl_client *client, struct wl_resource *resource)
{
    struct capture_session *session = session_from_resource(resource);
    if (session) {
        session_disable(session);
    }
}

static void session_handle_release(struct wl_client *client, struct wl_resource *resource,
                                   uint32_t activation_id, wl_fixed_t x, wl_fixed_t y)
{
    struct capture_session *session = session_from_resource(resource);
    if (!session || session->status != SESSION_ACTIVATED) {
        return;
    }
    /* a stale id still releases, a stuck capture is worse than a wrong one */
    if (activation_id != session->activation_id) {
        kywc_log(KYWC_WARN, "input capture: release with activation id %u, expected %u",
                 activation_id, session->activation_id);
    }

    struct seat *seat = manager->seat;
    session_deactivate(session);

    double lx = wl_fixed_to_double(x), ly = wl_fixed_to_double(y);
    if (seat && lx != -1 && ly != -1) {
        cursor_move(seat->cursor, NULL, lx, ly, false, false);
        cursor_rebase(seat->cursor);
    }
}

static void session_handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
    wl_resource_destroy(resource);
}

static const struct hyprland_input_capture_v1_interface session_impl = {
    .clear_barriers = session_handle_clear_barriers,
    .add_barrier = session_handle_add_barrier,
    .enable = session_handle_enable,
    .disable = session_handle_disable,
    .release = session_handle_release,
    .destroy = session_handle_destroy,
};

/* NULL if the session could not be allocated */
static struct capture_session *session_from_resource(struct wl_resource *resource)
{
    assert(wl_resource_instance_of(resource, &hyprland_input_capture_v1_interface, &session_impl));
    return wl_resource_get_user_data(resource);
}

static void session_destroy(struct capture_session *session)
{
    /* the client destroyed the object or is gone, no events */
    wl_resource_set_user_data(session->resource, NULL);
    session->resource = NULL;
    session_deactivate(session);
    session_clear_barriers(session);
    wl_list_remove(&session->link);

    if (session->eis_source) {
        wl_event_source_remove(session->eis_source);
    }
    session_clear_pointer(session);
    session_clear_keyboard(session);
    if (session->eis_seat) {
        eis_seat_unref(session->eis_seat);
    }
    if (session->client) {
        eis_client_disconnect(session->client);
    }
    if (session->eis) {
        eis_unref(session->eis);
    }
    free(session);
}

static void session_resource_destroy(struct wl_resource *resource)
{
    struct capture_session *session = wl_resource_get_user_data(resource);
    if (session) {
        session_destroy(session);
    }
}

static void manager_handle_create_session(struct wl_client *client,
                                          struct wl_resource *manager_resource, uint32_t id,
                                          const char *handle)
{
    struct wl_resource *resource =
        wl_resource_create(client, &hyprland_input_capture_v1_interface,
                           wl_resource_get_version(manager_resource), id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }

    struct capture_session *session = calloc(1, sizeof(*session));
    if (!session) {
        wl_resource_set_implementation(resource, &session_impl, NULL, NULL);
        wl_client_post_no_memory(client);
        return;
    }
    session->resource = resource;
    wl_list_init(&session->barriers);
    wl_list_insert(&manager->sessions, &session->link);
    wl_resource_set_implementation(resource, &session_impl, session, session_resource_destroy);

    /* the fd backend keeps the socket private to this session */
    session->eis = eis_new(session);
    int fd = -1;
    if (session->eis && eis_setup_backend_fd(session->eis) == 0) {
        fd = eis_backend_fd_add_client(session->eis);
    }
    if (fd < 0) {
        kywc_log(KYWC_ERROR, "input capture: cannot set up the EIS socket for %s", handle);
        return;
    }

    struct wl_event_loop *loop = wl_display_get_event_loop(wl_client_get_display(client));
    session->eis_source = wl_event_loop_add_fd(loop, eis_get_fd(session->eis), WL_EVENT_READABLE,
                                               session_handle_eis_readable, session);
    hyprland_input_capture_v1_send_eis_fd(resource, fd);
    close(fd);
    kywc_log(KYWC_INFO, "input capture: new session %s", handle);
}

static const struct hyprland_input_capture_manager_v1_interface manager_impl = {
    .create_session = manager_handle_create_session,
};

static void manager_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    struct wl_resource *resource =
        wl_resource_create(client, &hyprland_input_capture_manager_v1_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &manager_impl, NULL, NULL);
}

static bool layout_add_output(struct kywc_output *kywc_output, int index, void *data)
{
    struct layout *layout = data;
    struct output *output = output_from_kywc_output(kywc_output);
    if (layout->count < MAX_LAYOUT_OUTPUTS) {
        layout->outputs[layout->count].box = output->geometry;
        layout->outputs[layout->count].scale = output->wlr_output->scale;
    }
    layout->count++;
    return false;
}

/* true if the outputs moved, resized or changed scale since the last call */
static bool layout_update(void)
{
    struct layout layout;
    memset(&layout, 0, sizeof(layout));
    output_manager_for_each_output(layout_add_output, true, &layout);
    bool changed = layout.count > MAX_LAYOUT_OUTPUTS || memcmp(&layout, &manager->layout,
                                                                sizeof(layout)) != 0;
    manager->layout = layout;
    return changed;
}

/**
 * The signal also comes when nothing moved. On a real change the barriers
 * refer to the old layout: the client sets new ones and enables again.
 */
static void handle_configured(struct wl_listener *listener, void *data)
{
    if (!layout_update()) {
        return;
    }

    struct capture_session *session;
    wl_list_for_each(session, &manager->sessions, link) {
        session_clear_barriers(session);
        session_disable(session);
        if (session->pointer) {
            session_clear_pointer(session);
            session_ensure_pointer(session);
        }
    }
}

static void handle_display_destroy(struct wl_listener *listener, void *data)
{
    wl_list_remove(&manager->configured.link);
    wl_list_remove(&manager->seat_destroy.link);
    wl_list_remove(&manager->display_destroy.link);
    wl_event_source_remove(manager->repeat_timer);
    wl_global_destroy(manager->global);
    free(manager);
    manager = NULL;
}

bool hyprland_input_capture_manager_create(struct input_manager *input_manager)
{
    manager = calloc(1, sizeof(*manager));
    if (!manager) {
        return false;
    }
    wl_list_init(&manager->sessions);
    manager->seat_destroy.notify = handle_seat_destroy;
    wl_list_init(&manager->seat_destroy.link);

    struct server *server = input_manager->server;
    manager->repeat_timer = wl_event_loop_add_timer(server->event_loop, handle_repeat, NULL);
    manager->global =
        wl_global_create(server->display, &hyprland_input_capture_manager_v1_interface,
                         HYPRLAND_INPUT_CAPTURE_VERSION, NULL, manager_bind);
    if (!manager->repeat_timer || !manager->global) {
        if (manager->repeat_timer) {
            wl_event_source_remove(manager->repeat_timer);
        }
        if (manager->global) {
            wl_global_destroy(manager->global);
        }
        free(manager);
        manager = NULL;
        return false;
    }

    layout_update();
    manager->configured.notify = handle_configured;
    output_manager_add_configured_listener(&manager->configured);
    manager->display_destroy.notify = handle_display_destroy;
    wl_display_add_destroy_listener(server->display, &manager->display_destroy);
    return true;
}
