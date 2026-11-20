// SPDX-FileCopyrightText: CharOfString <root@charofstring.cc>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <kywc/binding.h>
#include <kywc/log.h>

#include "input_p.h"
#include "server.h"
#include "util/spawn.h"

#define EMERGENCY_TERM_KEYBIND "Win+q:no"

static const char *const terminals[] = {
    "gxde-terminal", "deepin-terminal", "kitty", "alacritty", "foot", "wezterm", "gnome-terminal",
    "ghostty", "konsole", "xterm"
};

static bool find_in_path(const char *name)
{
    const char *path = getenv("PATH");
    if (!path || !*path) {
        path = "/usr/local/bin:/usr/bin:/bin";
    }

    char file[4096];
    const char *dir = path;
    while (dir) {
        const char *end = strchr(dir, ':');
        int len = end ? (int)(end - dir) : (int)strlen(dir);
        /* an empty PATH entry is the current directory */
        snprintf(file, sizeof(file), "%.*s/%s", len, len ? dir : ".", name);
        if (access(file, X_OK) == 0) {
            return true;
        }
        dir = end ? end + 1 : NULL;
    }
    return false;
}

static void emergency_term_action(struct key_binding *binding, void *data)
{
    const char *terminal = getenv("TERMINAL");
    if (terminal && *terminal) {
        kywc_log(KYWC_INFO, "emergency terminal: %s", terminal);
        if (spawn_invoke(terminal)) {
            return;
        }
    }

    for (size_t i = 0; i < sizeof(terminals) / sizeof(terminals[0]); i++) {
        if (find_in_path(terminals[i])) {
            kywc_log(KYWC_INFO, "emergency terminal: %s", terminals[i]);
            spawn_invoke(terminals[i]);
            return;
        }
    }

    kywc_log(KYWC_ERROR, "emergency terminal: none of $TERMINAL, kitty, alacritty, foot, "
                         "wezterm, gnome-terminal or xterm is installed");
}

void emergency_term_binding_create(struct input_manager *input_manager)
{
    if (!input_manager->server->options.emergency_term_keybind) {
        return;
    }

    struct key_binding *binding =
        kywc_key_binding_create(EMERGENCY_TERM_KEYBIND, "emergency terminal");
    if (!binding) {
        return;
    }

    /* the registry frees it on server destroy */
    if (!kywc_key_binding_register(binding, KEY_BINDING_TYPE_CUSTOM_DEF, emergency_term_action,
                                   NULL)) {
        kywc_log(KYWC_ERROR, "failed to register the emergency terminal binding");
        kywc_key_binding_destroy(binding);
        return;
    }
    kywc_log(KYWC_INFO, "emergency terminal binding Win+q enabled");
}
