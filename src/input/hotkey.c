// SPDX-FileCopyrightText: CharOfString <root@charofstring.cc>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdlib.h>

#include <linux/input-event-codes.h>

#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_seat.h>
#include <xkbcommon/xkbcommon.h>

#include <kywc/binding.h>
#include <kywc/log.h>

#include "input_p.h"
#include "server.h"
#include "view/session_lock.h"

#define RELEVANT_MODIFIERS                                                                         \
    (WLR_MODIFIER_SHIFT | WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO)

struct hotkey {
    struct wl_list link;  // hotkeys.list
    const struct hotkey_impl *impl;
    void *data;
    struct wl_client *client;

    bool bound;
    enum hotkey_trigger type;
    uint32_t code;  // keysym or button
    uint32_t modifiers;  // WLR_MODIFIER_*

    /* the press that fired, its release fires released */
    bool held, tap_pending;
    uint32_t held_code;
    const void *held_device;  // struct keyboard, or struct seat for buttons
};

static struct {
    struct wl_list list;
    struct wl_display *display;
} hotkeys = {
    .list = { &hotkeys.list, &hotkeys.list },
};

static bool keysym_is_modifier(xkb_keysym_t keysym)
{
    switch (keysym) {
    case XKB_KEY_ISO_Level3_Shift:
    case XKB_KEY_ISO_Level3_Latch:
    case XKB_KEY_ISO_Level3_Lock:
    case XKB_KEY_ISO_Level5_Shift:
    case XKB_KEY_ISO_Level5_Latch:
    case XKB_KEY_ISO_Level5_Lock:
    case XKB_KEY_Mode_switch:
    case XKB_KEY_Num_Lock:
    case XKB_KEY_Scroll_Lock:
        return true;
    default:
        return keysym >= XKB_KEY_Shift_L && keysym <= XKB_KEY_Hyper_R;
    }
}

static bool keysym_is_dead(xkb_keysym_t keysym)
{
    return keysym >= XKB_KEY_dead_grave && keysym <= XKB_KEY_dead_longsolidusoverlay;
}

static bool hotkey_is_tap(const struct hotkey *hotkey)
{
    return hotkey->type == HOTKEY_TRIGGER_KEY && keysym_is_modifier(hotkey->code);
}

static bool button_is_auxiliary(uint32_t button)
{
    switch (button) {
    case BTN_SIDE:
    case BTN_EXTRA:
    case BTN_FORWARD:
    case BTN_BACK:
    case BTN_TASK:
        return true;
    default:
        return false;
    }
}

static bool trigger_permitted(xkb_keysym_t keysym, uint32_t modifiers)
{
    if (modifiers & (WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO)) {
        return true;
    }
    if (keysym_is_modifier(keysym) || (keysym >= XKB_KEY_KP_Space && keysym <= XKB_KEY_KP_Equal)) {
        return true;
    }
    if (keysym_is_dead(keysym)) {
        return false;
    }
    /* Shift alone still types text */
    return xkb_keysym_to_utf32(keysym) == 0;
}

/* the modifier a modifier key sets, for the bindings that fire on its tap */
static uint32_t keysym_modifier(xkb_keysym_t keysym)
{
    switch (keysym) {
    case XKB_KEY_Shift_L:
    case XKB_KEY_Shift_R:
        return WLR_MODIFIER_SHIFT;
    case XKB_KEY_Control_L:
    case XKB_KEY_Control_R:
        return WLR_MODIFIER_CTRL;
    case XKB_KEY_Alt_L:
    case XKB_KEY_Alt_R:
    case XKB_KEY_Meta_L:
    case XKB_KEY_Meta_R:
        return WLR_MODIFIER_ALT;
    case XKB_KEY_Super_L:
    case XKB_KEY_Super_R:
        return WLR_MODIFIER_LOGO;
    default:
        return 0;
    }
}

struct conflict_search {
    xkb_keysym_t keysym;
    uint32_t modifiers;
    const char *found;  // description of the compositor binding
};

static bool check_binding(struct key_binding *binding, enum key_binding_type type, char *keybind,
                          char *desc, int32_t modifiers, int32_t keysym, void *data)
{
    struct conflict_search *search = data;
    uint32_t mods = (uint32_t)modifiers & RELEVANT_MODIFIERS;

    if (keysym) {
        if (xkb_keysym_to_lower((xkb_keysym_t)keysym) == search->keysym &&
            mods == search->modifiers) {
            search->found = desc && *desc ? desc : keybind ? keybind : "?";
        }
    } else if (!search->modifiers && keysym_modifier(search->keysym) &&
               mods == keysym_modifier(search->keysym)) {
        /* a binding on the tap of a modifier, e.g. Super for the menu */
        search->found = desc && *desc ? desc : keybind ? keybind : "?";
    }
    return search->found != NULL;
}

/* the description of a compositor binding on this combination, or NULL */
static const char *compositor_binding_on(xkb_keysym_t keysym, uint32_t modifiers)
{
    struct conflict_search search = { .keysym = keysym, .modifiers = modifiers };
    kywc_key_binding_for_each_typed(check_binding, &search);
    return search.found;
}

static void hotkey_reset_held(struct hotkey *hotkey)
{
    hotkey->held = false;
    hotkey->tap_pending = false;
    hotkey->held_code = 0;
    hotkey->held_device = NULL;
}

struct hotkey *hotkey_create(const struct hotkey_impl *impl, void *data, struct wl_client *client)
{
    struct hotkey *hotkey = calloc(1, sizeof(*hotkey));
    if (!hotkey) {
        return NULL;
    }
    hotkey->impl = impl;
    hotkey->data = data;
    hotkey->client = client;
    hotkeys.display = wl_client_get_display(client);
    wl_list_insert(hotkeys.list.prev, &hotkey->link);
    return hotkey;
}

void hotkey_destroy(struct hotkey *hotkey)
{
    if (!hotkey) {
        return;
    }
    wl_list_remove(&hotkey->link);
    free(hotkey);
}

static bool trigger_taken(struct hotkey *hotkey, enum hotkey_trigger type, uint32_t code,
                          uint32_t modifiers)
{
    struct hotkey *other;
    wl_list_for_each(other, &hotkeys.list, link) {
        if (other != hotkey && other->bound && other->type == type && other->code == code &&
            other->modifiers == modifiers) {
            return true;
        }
    }
    return false;
}

/* only after the checks passed: a denied bind keeps the previous binding */
static void hotkey_set_trigger(struct hotkey *hotkey, enum hotkey_trigger type, uint32_t code,
                               uint32_t modifiers)
{
    if (hotkey->type != type || hotkey->code != code || hotkey->modifiers != modifiers) {
        hotkey_reset_held(hotkey);
    }
    hotkey->type = type;
    hotkey->code = code;
    hotkey->modifiers = modifiers;
    hotkey->bound = true;
}

enum hotkey_deny hotkey_bind_key(struct hotkey *hotkey, xkb_keysym_t keysym, uint32_t modifiers,
                                 const char **message)
{
    *message = "";
    char name[64];
    if (keysym == XKB_KEY_NoSymbol || xkb_keysym_get_name(keysym, name, sizeof(name)) <= 0) {
        *message = "the keysym is not a valid trigger";
        return HOTKEY_DENY_INVALID;
    }

    keysym = xkb_keysym_to_lower(keysym);
    modifiers &= RELEVANT_MODIFIERS;

    if (!trigger_permitted(keysym, modifiers)) {
        *message = "an unmodified key trigger must not be a key that produces text";
        return HOTKEY_DENY_NOT_PERMITTED;
    }
    if (trigger_taken(hotkey, HOTKEY_TRIGGER_KEY, keysym, modifiers)) {
        *message = "the trigger is already bound by another hotkey";
        return HOTKEY_DENY_ALREADY_BOUND;
    }
    if (compositor_binding_on(keysym, modifiers)) {
        *message = "the trigger is reserved by a compositor shortcut";
        return HOTKEY_DENY_ALREADY_BOUND;
    }

    hotkey_set_trigger(hotkey, HOTKEY_TRIGGER_KEY, keysym, modifiers);
    kywc_log(KYWC_INFO, "hotkey %s with modifiers 0x%x bound", name, modifiers);
    return HOTKEY_DENY_NONE;
}

enum hotkey_deny hotkey_bind_button(struct hotkey *hotkey, uint32_t button, uint32_t modifiers,
                                    const char **message)
{
    *message = "";
    modifiers &= RELEVANT_MODIFIERS;

    /* an unmodified primary button would take the pointer away from every client */
    if (!(modifiers & (WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO)) &&
        !button_is_auxiliary(button)) {
        *message = "an unmodified button trigger must be an auxiliary button";
        return HOTKEY_DENY_NOT_PERMITTED;
    }
    if (trigger_taken(hotkey, HOTKEY_TRIGGER_BUTTON, button, modifiers)) {
        *message = "the trigger is already bound by another hotkey";
        return HOTKEY_DENY_ALREADY_BOUND;
    }

    hotkey_set_trigger(hotkey, HOTKEY_TRIGGER_BUTTON, button, modifiers);
    kywc_log(KYWC_INFO, "hotkey button 0x%x with modifiers 0x%x bound", button, modifiers);
    return HOTKEY_DENY_NONE;
}

static void hotkey_revoke(struct hotkey *hotkey, const char *message)
{
    if (!hotkey->bound) {
        return;
    }
    hotkey->bound = false;
    hotkey_reset_held(hotkey);
    hotkey->impl->revoked(hotkey, message, hotkey->data);
}

void hotkeys_revoke_conflicting(void)
{
    struct hotkey *hotkey, *tmp;
    wl_list_for_each_safe(hotkey, tmp, &hotkeys.list, link) {
        if (hotkey->bound && hotkey->type == HOTKEY_TRIGGER_KEY &&
            compositor_binding_on(hotkey->code, hotkey->modifiers)) {
            hotkey_revoke(hotkey, "the trigger is now reserved by a compositor shortcut");
        }
    }
}

/* the serial counts as user interaction of this client, for xdg-activation */
static uint32_t hotkey_serial(struct hotkey *hotkey)
{
    struct seat *seat = input_manager_get_default_seat();
    struct wlr_seat_client *seat_client =
        seat ? wlr_seat_client_for_wl_client(seat->wlr_seat, hotkey->client) : NULL;
    return seat_client ? wlr_seat_client_next_serial(seat_client)
                       : wl_display_next_serial(hotkeys.display);
}

static bool key_produces_keysym(struct keyboard *keyboard, uint32_t key, xkb_keysym_t keysym)
{
    struct xkb_keymap *keymap = keyboard->wlr_keyboard->keymap;
    if (!keymap) {
        return false;
    }

    xkb_layout_index_t layouts = xkb_keymap_num_layouts_for_key(keymap, key + 8);
    for (xkb_layout_index_t layout = 0; layout < layouts; layout++) {
        const xkb_keysym_t *syms;
        int count = xkb_keymap_key_get_syms_by_level(keymap, key + 8, layout, 0, &syms);
        for (int i = 0; i < count; i++) {
            if (xkb_keysym_to_lower(syms[i]) == keysym) {
                return true;
            }
        }
    }
    return false;
}

/* a pending tap ends when anything else is pressed */
static void hotkeys_cancel_taps_except(const void *device, uint32_t code)
{
    struct hotkey *hotkey;
    wl_list_for_each(hotkey, &hotkeys.list, link) {
        if (!hotkey->held || !hotkey->tap_pending) {
            continue;
        }
        if (device && hotkey->held_device == device && hotkey->held_code == code) {
            continue;
        }
        hotkey_reset_held(hotkey);
    }
}

void hotkeys_cancel_taps(void)
{
    hotkeys_cancel_taps_except(NULL, 0);
}

void hotkeys_keyboard_destroy(struct keyboard *keyboard)
{
    struct hotkey *hotkey;
    wl_list_for_each(hotkey, &hotkeys.list, link) {
        if (hotkey->held && hotkey->held_device == keyboard) {
            hotkey_reset_held(hotkey);
        }
    }
}

static bool hotkey_release(struct hotkey *hotkey, const void *device, uint32_t code,
                           uint32_t time)
{
    if (!hotkey->held || hotkey->held_code != code || hotkey->held_device != device) {
        return false;
    }

    bool tap = hotkey->tap_pending;
    hotkey_reset_held(hotkey);
    if (tap) {
        hotkey->impl->pressed(hotkey, hotkey_serial(hotkey), time, hotkey->data);
    }
    hotkey->impl->released(hotkey, hotkey_serial(hotkey), time, hotkey->data);
    /* the press of a tap went to the clients, so does its release */
    return !tap;
}

bool hotkeys_handle_key(struct keyboard *keyboard, uint32_t key, bool pressed, uint32_t time,
                        uint32_t modifiers, bool allow_press)
{
    if (wl_list_empty(&hotkeys.list)) {
        return false;
    }

    bool consumed = false;
    if (pressed) {
        hotkeys_cancel_taps_except(allow_press ? keyboard : NULL, key);
    }
    modifiers &= RELEVANT_MODIFIERS;

    struct hotkey *hotkey, *tmp;
    wl_list_for_each_safe(hotkey, tmp, &hotkeys.list, link) {
        if (!pressed) {
            consumed |= hotkey_release(hotkey, keyboard, key, time);
            continue;
        }
        if (!allow_press || !hotkey->bound || hotkey->type != HOTKEY_TRIGGER_KEY || hotkey->held ||
            hotkey->modifiers != modifiers || !key_produces_keysym(keyboard, key, hotkey->code)) {
            continue;
        }

        hotkey->held = true;
        hotkey->held_code = key;
        hotkey->held_device = keyboard;
        if (hotkey_is_tap(hotkey)) {
            hotkey->tap_pending = true;
            continue;
        }
        hotkey->impl->pressed(hotkey, hotkey_serial(hotkey), time, hotkey->data);
        consumed = true;
    }
    return consumed;
}

/* the modifiers held on any of the seat's keyboards */
static uint32_t seat_modifiers(struct seat *seat)
{
    uint32_t modifiers = 0;
    struct keyboard *keyboard;
    wl_list_for_each(keyboard, &seat->keyboards, link) {
        modifiers |= wlr_keyboard_get_modifiers(keyboard->wlr_keyboard);
    }
    if (seat->keyboard) {
        modifiers |= wlr_keyboard_get_modifiers(seat->keyboard->wlr_keyboard);
    }
    return modifiers & RELEVANT_MODIFIERS;
}

bool hotkeys_handle_button(struct seat *seat, uint32_t button, bool pressed, uint32_t time,
                           bool allow_press)
{
    if (wl_list_empty(&hotkeys.list)) {
        return false;
    }

    hotkeys_cancel_taps();
    bool consumed = false;
    uint32_t modifiers = pressed ? seat_modifiers(seat) : 0;

    struct hotkey *hotkey, *tmp;
    wl_list_for_each_safe(hotkey, tmp, &hotkeys.list, link) {
        if (!pressed) {
            consumed |= hotkey_release(hotkey, seat, button, time);
            continue;
        }
        if (!allow_press || !hotkey->bound || hotkey->type != HOTKEY_TRIGGER_BUTTON ||
            hotkey->held || hotkey->code != button || hotkey->modifiers != modifiers) {
            continue;
        }

        hotkey->held = true;
        hotkey->held_code = button;
        hotkey->held_device = seat;
        hotkey->impl->pressed(hotkey, hotkey_serial(hotkey), time, hotkey->data);
        consumed = true;
    }
    return consumed;
}
