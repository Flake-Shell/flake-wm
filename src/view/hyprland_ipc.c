// SPDX-FileCopyrightText: 2026 CharOfString <root@charofstring.cc>
// SPDX-License-Identifier: GPL-3.0-or-later

/* This feat simulates Hyprland's UNIX IPC socket.
 * Hyprland's IPC socket exposes lots of WM's internal info to your system.
 * It also exposes WM control to programs like hyprctl.
 * You'll have to start with --hypr-unix-socket-sim-enable and please only start it when you HAVE TO.
 * The arg is intentionally designed to be super long to force you think "Should I do so?"
 */
#define _GNU_SOURCE

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <regex.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <linux/input-event-codes.h>
#include <wlr/backend/drm.h>
#include <wlr/backend/headless.h>
#include <wlr/backend/multi.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_content_type_v1.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_tablet_tool.h>
#include <xkbcommon/xkbcommon.h>

#include <kywc/binding.h>
#include <kywc/log.h>

#include "input/cursor.h"
#include "input/event.h"
#include "input/input.h"
#include "input/keyboard.h"
#include "input/seat.h"
#include "output.h"
#include "scene/scene.h"
#include "util/dbus.h"
#include "util/limit.h"
#include "util/logger.h"
#include "view/hyprland_ipc.h"
#include "view/session_lock.h"
#include "view/workspace.h"
#include "view_p.h"
#include "xwayland.h"

/* what flakewm answers for: the Hyprland release the replies are modelled on */
#define HYPR_VERSION "0.57.2"
#define HYPR_GIT_BRANCH "flakewm"
#define HYPR_GIT_COMMIT_MESSAGE "flakewm Hyprland IPC compatibility layer"
#define HYPR_STABLE_ID_BASE 0x18000000
#define HYPR_MAX_NOTIFICATIONS 64
#define HYPR_MAX_REQUEST (64 * 1024)
#define HYPR_MAX_REQUEST_CLIENTS 32
#define HYPR_MAX_SUBSCRIBERS 64
#define HYPR_MAX_LOG_FOLLOWERS 16
/* Lua coordinates and sizes are clamped to this, so sums with geometry stay in int */
#define HYPR_LUA_COORD_MAX (1 << 24)
/* drop an event subscriber that stops reading once this much is queued */
#define HYPR_MAX_EVENT_BACKLOG (1024 * 1024)
#define HYPR_REQUEST_TIMEOUT_MS 5000

/* ------------------------------------------------------------------------- */
/* growable string buffer */

struct sbuf {
    char *data;
    size_t len, cap;
    bool oom;
};

static bool sbuf_reserve(struct sbuf *b, size_t extra)
{
    if (b->oom) {
        return false;
    }
    if (b->len + extra + 1 <= b->cap) {
        return true;
    }
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->len + extra + 1) {
        cap *= 2;
    }
    char *data = realloc(b->data, cap);
    if (!data) {
        b->oom = true;
        return false;
    }
    b->data = data;
    b->cap = cap;
    return true;
}

static void sbuf_append(struct sbuf *b, const char *s, size_t n)
{
    if (!sbuf_reserve(b, n)) {
        return;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void sbuf_puts(struct sbuf *b, const char *s)
{
    sbuf_append(b, s, strlen(s));
}

__attribute__((format(printf, 2, 3))) static void sbuf_printf(struct sbuf *b, const char *fmt,
                                                               ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0 || !sbuf_reserve(b, n)) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(b->data + b->len, n + 1, fmt, ap);
    va_end(ap);
    b->len += n;
}

static void sbuf_consume(struct sbuf *b, size_t n)
{
    if (n >= b->len) {
        b->len = 0;
    } else {
        memmove(b->data, b->data + n, b->len - n);
        b->len -= n;
    }
    if (b->data) {
        b->data[b->len] = '\0';
    }
}

static void sbuf_finish(struct sbuf *b)
{
    free(b->data);
    *b = (struct sbuf){ 0 };
}

/* ------------------------------------------------------------------------- */
/* state */

struct hypr_closed_monitor {
    int id;
    char *name, *desc;
};

struct hypr_ws_snapshot {
    uint32_t id;
    char *name;
};

struct hypr_ipc {
    struct view_manager *view_manager;
    struct server *server;

    char *dir;
    char *request_path, *event_path, *lock_path;
    char signature[96];
    int request_fd, event_fd;
    struct wl_event_source *request_source, *event_source;

    struct wl_list requests;    // hypr_request.link
    struct wl_list subscribers; // hypr_subscriber.link
    int request_count, subscriber_count;

    struct wl_list windows;    // hypr_window.link
    struct wl_list mru;        // hypr_window.mru_link, most recent first
    struct wl_list monitors;   // hypr_monitor.link
    struct wl_list workspaces; // hypr_workspace.link

    struct wl_event_source *idle;
    bool terminating;

    /* what has been announced on the event socket */
    struct hypr_ws_snapshot ws_snapshot[MAX_WORKSPACES]; // existing workspaces, creation order
    size_t ws_snapshot_count;
    uint64_t *closed_windows;
    size_t closed_window_count, closed_window_cap;
    struct hypr_closed_monitor *closed_monitors;
    size_t closed_monitor_count, closed_monitor_cap;

    uint32_t announced_ws_id;
    int announced_mon_id;
    uint64_t announced_window;
    char *announced_class, *announced_title;

    uint32_t previous_ws_id;
    uint64_t next_stable_id;

    /* xdg_toplevel_tag_v1, known before the toplevel maps */
    struct wl_list toplevel_tags; // hypr_toplevel_tag.link

    /* notify/seterror, through org.freedesktop.Notifications */
    uint32_t notifications[HYPR_MAX_NOTIFICATIONS]; // oldest first
    size_t notification_count;
    uint32_t error_notification;

    /* outputs created by "output create", the only ones "output remove" takes */
    struct wlr_output *user_outputs[16];
    size_t user_output_count;

    /* "kill": click a window to SIGKILL it */
    struct {
        bool active;
        struct seat *seat;
        struct seat_pointer_grab pointer;
        struct seat_keyboard_grab keyboard;
    } kill;

    /* "f/rollinglog" connections */
    struct wl_list log_followers; // hypr_request.link

    struct wl_listener new_view;
    struct wl_listener new_mapped_view;
    struct wl_listener layer_map;
    struct wl_listener layer_unmap;
    struct wl_listener activate_view;
    struct wl_listener new_workspace;
    struct wl_listener new_output;
    struct wl_listener primary_output;
    struct wl_listener server_start;
    struct wl_listener server_terminate;
    struct wl_listener server_destroy;
};

struct hypr_window {
    struct wl_list link;
    struct wl_list mru_link;
    struct hypr_ipc *ipc;
    struct view *view;
    uint64_t address;
    uint64_t stable_id;
    char *initial_class, *initial_title;
    bool initial_set;

    bool announced;
    uint32_t announced_ws_id;
    char *announced_title;
    int announced_fullscreen;
    bool announced_pinned, announced_floating, announced_urgent;

    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener destroy;
    struct wl_listener title;
    struct wl_listener app_id;
    struct wl_listener activate;
    struct wl_listener fullscreen;
    struct wl_listener maximize;
    struct wl_listener minimize;
    struct wl_listener workspace;
    struct wl_listener output;
    struct wl_listener tile;
    struct wl_listener capabilities;
};

struct hypr_toplevel_tag {
    struct wl_list link;
    struct wlr_surface *surface;
    char *tag, *description;
    struct wl_listener destroy;
};

struct hypr_monitor {
    struct wl_list link;
    struct hypr_ipc *ipc;
    struct kywc_output *output;
    int id;
    bool announced;

    struct wl_listener on;
    struct wl_listener off;
    struct wl_listener destroy;
};

struct hypr_workspace {
    struct wl_list link;
    struct hypr_ipc *ipc;
    struct workspace *workspace;

    struct wl_listener name;
    struct wl_listener position;
    struct wl_listener activate;
    struct wl_listener destroy;
};

struct hypr_request {
    struct wl_list link;
    struct hypr_ipc *ipc;
    int fd;
    struct wl_event_source *source;
    struct wl_event_source *timer;
    struct sbuf in, out;
    bool replying;
    /* f/rollinglog: stays open and gets every new log line */
    bool follow;
};

struct hypr_subscriber {
    struct wl_list link;
    struct hypr_ipc *ipc;
    int fd;
    struct wl_event_source *source;
    struct sbuf out;
};

static struct hypr_ipc *hypr_ipc = NULL;

static void schedule_reconcile(struct hypr_ipc *ipc);
static char *monitor_short_desc(struct kywc_output *output);
static char *sanitize_line(const char *s);

/* m_shortDescription, made safe for one event line */
static char *monitor_event_desc(struct kywc_output *output)
{
    char *desc = monitor_short_desc(output);
    char *line = sanitize_line(desc);
    free(desc);
    return line;
}

/* ------------------------------------------------------------------------- */
/* model helpers */

static uint32_t ws_id(struct workspace *ws)
{
    return ws->position + 1;
}

static const char *ws_name(struct workspace *ws, char buf[16])
{
    if (ws->has_custom_name && ws->name && *ws->name) {
        return ws->name;
    }
    snprintf(buf, 16, "%" PRIu32, ws_id(ws));
    return buf;
}

static struct workspace *ws_by_id(uint32_t id)
{
    return id ? workspace_by_position(id - 1) : NULL;
}

static struct hypr_monitor *monitor_from_output(struct hypr_ipc *ipc, struct kywc_output *output)
{
    if (!output) {
        return NULL;
    }
    struct hypr_monitor *mon;
    wl_list_for_each(mon, &ipc->monitors, link) {
        if (mon->output == output) {
            return mon;
        }
    }
    return NULL;
}

static bool monitor_enabled(struct hypr_monitor *mon)
{
    return mon && !mon->output->destroying && mon->output->state.enabled;
}

static struct hypr_monitor *monitor_by_id(struct hypr_ipc *ipc, int id)
{
    struct hypr_monitor *mon;
    wl_list_for_each(mon, &ipc->monitors, link) {
        if (mon->id == id) {
            return mon;
        }
    }
    return NULL;
}

static struct hypr_monitor *first_enabled_monitor(struct hypr_ipc *ipc)
{
    struct hypr_monitor *mon, *best = NULL;
    wl_list_for_each(mon, &ipc->monitors, link) {
        if (monitor_enabled(mon) && (!best || mon->id < best->id)) {
            best = mon;
        }
    }
    return best;
}

/* workspaces span every output, report them on the primary one */
static struct hypr_monitor *workspace_monitor(struct hypr_ipc *ipc)
{
    struct hypr_monitor *mon = monitor_from_output(ipc, kywc_output_get_primary());
    return monitor_enabled(mon) ? mon : first_enabled_monitor(ipc);
}

static bool window_eligible(struct hypr_window *win)
{
    struct view *view = win->view;
    return view->base.mapped && view->base.role == KYWC_VIEW_ROLE_NORMAL &&
           view->current_proxy && view->current_proxy->workspace;
}

static struct workspace *window_workspace(struct hypr_window *win)
{
    struct view *view = win->view;
    if (view->base.sticky) {
        struct workspace *current = workspace_manager_get_current();
        if (current) {
            return current;
        }
    }
    return view->current_proxy ? view->current_proxy->workspace : NULL;
}

static struct hypr_window *window_from_view(struct hypr_ipc *ipc, struct view *view)
{
    if (!view) {
        return NULL;
    }
    struct hypr_window *win;
    wl_list_for_each(win, &ipc->windows, link) {
        if (win->view == view) {
            return win;
        }
    }
    return NULL;
}

static struct hypr_window *active_window(struct hypr_ipc *ipc)
{
    struct hypr_window *win = window_from_view(ipc, view_manager_get_activated());
    if (!win || !window_eligible(win) || win->view->base.minimized) {
        return NULL;
    }
    /* flakewm keeps the last view activated on an empty workspace */
    return window_workspace(win) == workspace_manager_get_current() ? win : NULL;
}

static bool ws_exists(struct hypr_ipc *ipc, struct workspace *ws)
{
    if (ws == workspace_manager_get_current()) {
        return true;
    }
    struct hypr_window *win;
    wl_list_for_each(win, &ipc->windows, link) {
        if (window_eligible(win) && window_workspace(win) == ws) {
            return true;
        }
    }
    return false;
}

static struct hypr_monitor *focused_monitor(struct hypr_ipc *ipc)
{
    struct hypr_window *win = active_window(ipc);
    if (win) {
        struct hypr_monitor *mon = monitor_from_output(ipc, win->view->output);
        if (monitor_enabled(mon)) {
            return mon;
        }
    }
    struct seat *seat = input_manager_get_default_seat();
    if (seat && seat->cursor) {
        struct output *output = input_current_output(seat);
        struct hypr_monitor *mon = output ? monitor_from_output(ipc, &output->base) : NULL;
        if (monitor_enabled(mon)) {
            return mon;
        }
    }
    return workspace_monitor(ipc);
}

static int window_fullscreen_mode(struct view *view)
{
    if (view->base.fullscreen) {
        return 2;
    }
    return view->base.maximized ? 1 : 0;
}

static int window_focus_history_id(struct hypr_ipc *ipc, struct hypr_window *win)
{
    int index = 0;
    struct hypr_window *iter;
    wl_list_for_each(iter, &ipc->mru, mru_link) {
        if (iter == win) {
            return index;
        }
        if (window_eligible(iter)) {
            index++;
        }
    }
    return -1;
}

static const char *str_or_empty(const char *s)
{
    return s ? s : "";
}

/* event payloads are line based */
static char *sanitize_line(const char *s)
{
    char *copy = strdup(str_or_empty(s));
    if (!copy) {
        return NULL;
    }
    for (char *p = copy; *p; p++) {
        if (*p == '\n' || *p == '\r') {
            *p = ' ';
        }
    }
    return copy;
}

static bool str_changed(const char *a, const char *b)
{
    return strcmp(str_or_empty(a), str_or_empty(b)) != 0;
}

static void replace_str(char **dst, const char *src)
{
    free(*dst);
    *dst = src ? strdup(src) : NULL;
}

/* ------------------------------------------------------------------------- */
/* event socket */

static void subscriber_destroy(struct hypr_subscriber *sub)
{
    wl_list_remove(&sub->link);
    sub->ipc->subscriber_count--;
    wl_event_source_remove(sub->source);
    close(sub->fd);
    sbuf_finish(&sub->out);
    free(sub);
}

/* returns false if the subscriber was destroyed */
static bool subscriber_flush(struct hypr_subscriber *sub)
{
    while (sub->out.len > 0) {
        ssize_t n = send(sub->fd, sub->out.data, sub->out.len, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n > 0) {
            sbuf_consume(&sub->out, n);
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }
        subscriber_destroy(sub);
        return false;
    }

    uint32_t mask = WL_EVENT_READABLE;
    if (sub->out.len > 0) {
        mask |= WL_EVENT_WRITABLE;
    }
    wl_event_source_fd_update(sub->source, mask);
    return true;
}

static int handle_subscriber_fd(int fd, uint32_t mask, void *data)
{
    struct hypr_subscriber *sub = data;

    if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
        subscriber_destroy(sub);
        return 0;
    }

    if (mask & WL_EVENT_READABLE) {
        /* nothing is expected from subscribers, drain and detect EOF */
        char buf[256];
        ssize_t n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
            subscriber_destroy(sub);
            return 0;
        }
    }

    if (mask & WL_EVENT_WRITABLE) {
        subscriber_flush(sub);
    }
    return 0;
}

static int handle_event_accept(int fd, uint32_t mask, void *data)
{
    struct hypr_ipc *ipc = data;

    int client = accept4(fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (client < 0) {
        return 0;
    }

    if (ipc->subscriber_count >= HYPR_MAX_SUBSCRIBERS) {
        kywc_log(KYWC_WARN, "hyprland ipc: too many event subscribers, rejecting");
        close(client);
        return 0;
    }

    struct hypr_subscriber *sub = calloc(1, sizeof(*sub));
    if (!sub) {
        close(client);
        return 0;
    }
    sub->ipc = ipc;
    sub->fd = client;
    sub->source = wl_event_loop_add_fd(ipc->server->event_loop, client, WL_EVENT_READABLE,
                                       handle_subscriber_fd, sub);
    if (!sub->source) {
        close(client);
        free(sub);
        return 0;
    }
    wl_list_insert(ipc->subscribers.prev, &sub->link);
    ipc->subscriber_count++;
    return 0;
}

__attribute__((format(printf, 3, 4))) static void emit_event(struct hypr_ipc *ipc,
                                                              const char *event, const char *fmt,
                                                              ...)
{
    if (wl_list_empty(&ipc->subscribers)) {
        return;
    }

    struct sbuf line = { 0 };
    sbuf_puts(&line, event);
    sbuf_puts(&line, ">>");
    va_list ap;
    va_start(ap, fmt);
    char *payload = NULL;
    if (vasprintf(&payload, fmt, ap) < 0) {
        payload = NULL;
    }
    va_end(ap);
    if (payload) {
        sbuf_puts(&line, payload);
        free(payload);
    }
    sbuf_puts(&line, "\n");
    if (line.oom) {
        sbuf_finish(&line);
        return;
    }

    struct hypr_subscriber *sub, *tmp;
    wl_list_for_each_safe(sub, tmp, &ipc->subscribers, link) {
        if (sub->out.len + line.len > HYPR_MAX_EVENT_BACKLOG) {
            kywc_log(KYWC_WARN, "hyprland ipc: event subscriber stopped reading, dropping it");
            subscriber_destroy(sub);
            continue;
        }
        sbuf_append(&sub->out, line.data, line.len);
        if (sub->out.oom) {
            subscriber_destroy(sub);
            continue;
        }
        subscriber_flush(sub);
    }
    sbuf_finish(&line);
}

/* ------------------------------------------------------------------------- */
/* reconcile: diff live state against what was announced */

static void reconcile_monitors(struct hypr_ipc *ipc)
{
    for (size_t i = 0; i < ipc->closed_monitor_count; i++) {
        struct hypr_closed_monitor *m = &ipc->closed_monitors[i];
        emit_event(ipc, "monitorremoved", "%s", m->name);
        emit_event(ipc, "monitorremovedv2", "%d,%s,%s", m->id, m->name, m->desc);
        free(m->name);
        free(m->desc);
    }
    ipc->closed_monitor_count = 0;

    struct hypr_monitor *mon;
    wl_list_for_each(mon, &ipc->monitors, link) {
        bool enabled = monitor_enabled(mon);
        if (enabled == mon->announced) {
            continue;
        }
        char *desc = monitor_event_desc(mon->output);
        if (enabled) {
            emit_event(ipc, "monitoradded", "%s", mon->output->name);
            emit_event(ipc, "monitoraddedv2", "%d,%s,%s", mon->id, mon->output->name,
                       str_or_empty(desc));
        } else {
            emit_event(ipc, "monitorremoved", "%s", mon->output->name);
            emit_event(ipc, "monitorremovedv2", "%d,%s,%s", mon->id, mon->output->name,
                       str_or_empty(desc));
        }
        free(desc);
        mon->announced = enabled;
    }
}

/*
 * Hyprland workspaces are dynamic: one exists while it is shown or holds a
 * window, and is created/destroyed (with events) as that changes. flakewm's
 * workspaces are static, so present them the same way.
 */
static size_t live_workspaces(struct hypr_ipc *ipc, struct hypr_ws_snapshot live[MAX_WORKSPACES])
{
    size_t live_count = 0;
    uint32_t count = workspace_manager_get_count();
    for (uint32_t i = 0; i < count && live_count < MAX_WORKSPACES; i++) {
        struct workspace *ws = workspace_by_position(i);
        if (!ws || !ws_exists(ipc, ws)) {
            continue;
        }
        char buf[16];
        live[live_count].id = ws_id(ws);
        live[live_count].name = sanitize_line(ws_name(ws, buf));
        live_count++;
    }
    return live_count;
}

static struct hypr_ws_snapshot *find_ws(struct hypr_ws_snapshot *list, size_t count, uint32_t id)
{
    for (size_t i = 0; i < count; i++) {
        if (list[i].id == id) {
            return &list[i];
        }
    }
    return NULL;
}

/* events carry the addressable name (the id) first, then the display name */
static void reconcile_workspaces_created(struct hypr_ipc *ipc, struct hypr_ws_snapshot *live,
                                         size_t live_count)
{
    for (size_t j = 0; j < live_count; j++) {
        struct hypr_ws_snapshot *old = find_ws(ipc->ws_snapshot, ipc->ws_snapshot_count, live[j].id);
        const char *name = str_or_empty(live[j].name);
        if (!old) {
            emit_event(ipc, "createworkspace", "%" PRIu32, live[j].id);
            emit_event(ipc, "createworkspacev2", "%" PRIu32 ",%s", live[j].id, name);
        } else if (str_changed(old->name, name)) {
            emit_event(ipc, "renameworkspace", "%" PRIu32 ",%s", live[j].id, name);
        }
    }
}

/* after the focus events, like Hyprland's workspace cleanup; takes live[] */
static void reconcile_workspaces_destroyed(struct hypr_ipc *ipc, struct hypr_ws_snapshot *live,
                                           size_t live_count)
{
    struct hypr_ws_snapshot next[MAX_WORKSPACES];
    size_t next_count = 0;
    bool taken[MAX_WORKSPACES] = { false };

    for (size_t i = 0; i < ipc->ws_snapshot_count; i++) {
        struct hypr_ws_snapshot *old = &ipc->ws_snapshot[i];
        struct hypr_ws_snapshot *cur = find_ws(live, live_count, old->id);
        if (cur) {
            taken[cur - live] = true;
            next[next_count++] = *cur;
        } else {
            emit_event(ipc, "destroyworkspace", "%" PRIu32, old->id);
            emit_event(ipc, "destroyworkspacev2", "%" PRIu32 ",%s", old->id,
                       str_or_empty(old->name));
        }
        free(old->name);
    }
    for (size_t j = 0; j < live_count; j++) {
        if (!taken[j]) {
            next[next_count++] = live[j];
        }
    }
    memcpy(ipc->ws_snapshot, next, sizeof(next[0]) * next_count);
    ipc->ws_snapshot_count = next_count;
}

static void reconcile_windows(struct hypr_ipc *ipc)
{
    for (size_t i = 0; i < ipc->closed_window_count; i++) {
        emit_event(ipc, "closewindow", "%" PRIx64, ipc->closed_windows[i]);
    }
    ipc->closed_window_count = 0;

    struct hypr_window *active = active_window(ipc);
    struct hypr_window *win;
    wl_list_for_each_reverse(win, &ipc->windows, link) {
        bool eligible = window_eligible(win);
        if (!eligible) {
            if (win->announced) {
                emit_event(ipc, "closewindow", "%" PRIx64, win->address);
                win->announced = false;
            }
            win->announced_urgent = false;
            continue;
        }

        struct workspace *ws = window_workspace(win);
        char buf[16];
        uint32_t id = ws ? ws_id(ws) : 0;
        char *ws_label = sanitize_line(ws ? ws_name(ws, buf) : "");
        char *title = sanitize_line(win->view->base.title);
        int fullscreen = window_fullscreen_mode(win->view);

        bool pinned = win->view->base.sticky;
        bool floating = win->view->base.tiled == KYWC_TILE_NONE;
        bool urgent = win->view->base.demands_attention && win != active;

        if (!win->announced) {
            /* initialClass/initialTitle are taken when the window first maps */
            if (!win->initial_set) {
                replace_str(&win->initial_class, win->view->base.app_id);
                replace_str(&win->initial_title, win->view->base.title);
                win->initial_set = true;
            }
            /* Hyprland reports the title set before the first map */
            if (title && *title) {
                emit_event(ipc, "windowtitle", "%" PRIx64, win->address);
                emit_event(ipc, "windowtitlev2", "%" PRIx64 ",%s", win->address, title);
            }
            char *cls = sanitize_line(win->view->base.app_id);
            emit_event(ipc, "openwindow", "%" PRIx64 ",%" PRIu32 ",%s,%s", win->address, id,
                       str_or_empty(cls), str_or_empty(title));
            free(cls);
            /* a pending fullscreen/maximize request is applied after openwindow */
            if (fullscreen) {
                emit_event(ipc, "fullscreen", "1");
            }
            win->announced = true;
            win->announced_ws_id = id;
            win->announced_fullscreen = fullscreen;
            win->announced_pinned = pinned;
            win->announced_floating = floating;
            replace_str(&win->announced_title, title);
        } else {
            if (win->announced_ws_id != id && !win->view->base.sticky) {
                emit_event(ipc, "movewindow", "%" PRIx64 ",%" PRIu32, win->address, id);
                emit_event(ipc, "movewindowv2", "%" PRIx64 ",%" PRIu32 ",%s", win->address, id,
                           str_or_empty(ws_label));
            }
            win->announced_ws_id = id;
            if (str_changed(win->announced_title, title)) {
                emit_event(ipc, "windowtitle", "%" PRIx64, win->address);
                emit_event(ipc, "windowtitlev2", "%" PRIx64 ",%s", win->address,
                           str_or_empty(title));
                replace_str(&win->announced_title, title);
            }
            /* any change of the fullscreen mode, maximized (1) included */
            if (fullscreen != win->announced_fullscreen) {
                emit_event(ipc, "fullscreen", "%d", fullscreen != 0);
                win->announced_fullscreen = fullscreen;
            }
            if (pinned != win->announced_pinned) {
                emit_event(ipc, "pin", "%" PRIx64 ",%d", win->address, pinned);
                win->announced_pinned = pinned;
            }
            if (floating != win->announced_floating) {
                emit_event(ipc, "changefloatingmode", "%" PRIx64 ",%d", win->address, floating);
                win->announced_floating = floating;
            }
        }
        /* demands-attention while not focused, once per request */
        if (urgent && !win->announced_urgent) {
            emit_event(ipc, "urgent", "%" PRIx64, win->address);
        }
        win->announced_urgent = urgent;
        free(ws_label);
        free(title);
    }
}

/* same order as Hyprland: focusedmon, activewindow, then workspace */
static void reconcile_focus(struct hypr_ipc *ipc)
{
    struct workspace *ws = workspace_manager_get_current();
    char buf[16];
    uint32_t id = ws ? ws_id(ws) : 0;

    struct hypr_monitor *mon = focused_monitor(ipc);
    int mon_id = mon ? mon->id : -1;
    if (mon && mon_id != ipc->announced_mon_id) {
        emit_event(ipc, "focusedmon", "%s,%" PRIu32, mon->output->name, id);
        emit_event(ipc, "focusedmonv2", "%s,%" PRIu32, mon->output->name, id);
    }
    ipc->announced_mon_id = mon_id;

    struct hypr_window *win = active_window(ipc);
    uint64_t address = win ? win->address : 0;
    char *cls = win ? sanitize_line(win->view->base.app_id) : NULL;
    char *title = win ? sanitize_line(win->view->base.title) : NULL;
    if (address != ipc->announced_window || str_changed(ipc->announced_class, cls) ||
        str_changed(ipc->announced_title, title)) {
        if (win) {
            emit_event(ipc, "activewindow", "%s,%s", str_or_empty(cls), str_or_empty(title));
            emit_event(ipc, "activewindowv2", "%" PRIx64, address);
        } else {
            emit_event(ipc, "activewindow", ",");
            emit_event(ipc, "activewindowv2", "%s", "");
        }
        ipc->announced_window = address;
        replace_str(&ipc->announced_class, cls);
        replace_str(&ipc->announced_title, title);
    }
    free(cls);
    free(title);

    /* a rename alone is reported by renameworkspace */
    if (ws && id != ipc->announced_ws_id) {
        if (ipc->announced_ws_id) {
            ipc->previous_ws_id = ipc->announced_ws_id;
        }
        char *name = sanitize_line(ws_name(ws, buf));
        emit_event(ipc, "workspace", "%" PRIu32, id);
        emit_event(ipc, "workspacev2", "%" PRIu32 ",%s", id, str_or_empty(name));
        free(name);
        ipc->announced_ws_id = id;
    }
}

static void reconcile(struct hypr_ipc *ipc)
{
    if (ipc->idle) {
        wl_event_source_remove(ipc->idle);
        ipc->idle = NULL;
    }
    if (ipc->terminating) {
        return;
    }
    reconcile_monitors(ipc);
    struct hypr_ws_snapshot live[MAX_WORKSPACES];
    size_t live_count = live_workspaces(ipc, live);
    reconcile_workspaces_created(ipc, live, live_count);
    reconcile_windows(ipc);
    reconcile_focus(ipc);
    reconcile_workspaces_destroyed(ipc, live, live_count);
}

static void handle_reconcile_idle(void *data)
{
    struct hypr_ipc *ipc = data;
    ipc->idle = NULL;
    reconcile(ipc);
}

static void schedule_reconcile(struct hypr_ipc *ipc)
{
    if (ipc->idle || ipc->terminating) {
        return;
    }
    ipc->idle = wl_event_loop_add_idle(ipc->server->event_loop, handle_reconcile_idle, ipc);
}

/* ------------------------------------------------------------------------- */
/* tracking */

/* shared by several window listeners, only the ipc is needed */
static void handle_window_changed(struct wl_listener *listener, void *data)
{
    schedule_reconcile(hypr_ipc);
}

static void handle_window_minimize(struct wl_listener *listener, void *data)
{
    struct hypr_window *win = wl_container_of(listener, win, minimize);
    struct hypr_ipc *ipc = win->ipc;

    /* bring the stream up to date so the address is known to clients */
    reconcile(ipc);
    if (win->announced) {
        emit_event(ipc, "minimized", "%" PRIx64 ",%d", win->address,
                   win->view->base.minimized ? 1 : 0);
    }
    schedule_reconcile(ipc);
}

static void handle_window_activate(struct wl_listener *listener, void *data)
{
    struct hypr_window *win = wl_container_of(listener, win, activate);
    if (win->view->base.activated) {
        wl_list_remove(&win->mru_link);
        wl_list_insert(&win->ipc->mru, &win->mru_link);
    }
    schedule_reconcile(win->ipc);
}

/* closewindow is sent on the next reconcile, even if the window maps again before */
static void window_closed(struct hypr_window *win)
{
    struct hypr_ipc *ipc = win->ipc;
    if (!win->announced) {
        return;
    }
    win->announced = false;
    if (ipc->closed_window_count == ipc->closed_window_cap) {
        size_t cap = ipc->closed_window_cap ? ipc->closed_window_cap * 2 : 16;
        uint64_t *list = realloc(ipc->closed_windows, cap * sizeof(*list));
        if (list) {
            ipc->closed_windows = list;
            ipc->closed_window_cap = cap;
        }
    }
    if (ipc->closed_window_count < ipc->closed_window_cap) {
        ipc->closed_windows[ipc->closed_window_count++] = win->address;
    }
    schedule_reconcile(ipc);
}

static void window_destroy(struct hypr_window *win)
{
    window_closed(win);

    wl_list_remove(&win->link);
    wl_list_remove(&win->mru_link);
    wl_list_remove(&win->map.link);
    wl_list_remove(&win->unmap.link);
    wl_list_remove(&win->destroy.link);
    wl_list_remove(&win->title.link);
    wl_list_remove(&win->app_id.link);
    wl_list_remove(&win->activate.link);
    wl_list_remove(&win->fullscreen.link);
    wl_list_remove(&win->maximize.link);
    wl_list_remove(&win->minimize.link);
    wl_list_remove(&win->workspace.link);
    wl_list_remove(&win->output.link);
    wl_list_remove(&win->tile.link);
    wl_list_remove(&win->capabilities.link);
    free(win->initial_class);
    free(win->initial_title);
    free(win->announced_title);
    free(win);
}

static void handle_window_unmap(struct wl_listener *listener, void *data)
{
    struct hypr_window *win = wl_container_of(listener, win, unmap);
    window_closed(win);
    schedule_reconcile(win->ipc);
}

static void handle_window_destroy(struct wl_listener *listener, void *data)
{
    struct hypr_window *win = wl_container_of(listener, win, destroy);
    window_destroy(win);
}

/* every view from creation on, like Hyprland's window list ("a/clients") */
static void track_view(struct hypr_ipc *ipc, struct view *view)
{
    if (!view || window_from_view(ipc, view)) {
        return;
    }

    struct hypr_window *win = calloc(1, sizeof(*win));
    if (!win) {
        return;
    }
    win->ipc = ipc;
    win->view = view;
    /* same address as hyprland-toplevel-mapping, toplevel-export takes its low 32 bits */
    win->address = (uint64_t)(uintptr_t)view;
    win->stable_id = ipc->next_stable_id++;
    wl_list_insert(&ipc->windows, &win->link);
    if (view->base.activated) {
        wl_list_insert(&ipc->mru, &win->mru_link);
    } else {
        wl_list_insert(ipc->mru.prev, &win->mru_link);
    }

    win->map.notify = handle_window_changed;
    wl_signal_add(&view->base.events.map, &win->map);
    win->unmap.notify = handle_window_unmap;
    wl_signal_add(&view->base.events.unmap, &win->unmap);
    win->destroy.notify = handle_window_destroy;
    wl_signal_add(&view->base.events.destroy, &win->destroy);
    win->activate.notify = handle_window_activate;
    wl_signal_add(&view->base.events.activate, &win->activate);
    win->minimize.notify = handle_window_minimize;
    wl_signal_add(&view->base.events.minimize, &win->minimize);

    win->title.notify = handle_window_changed;
    wl_signal_add(&view->base.events.title, &win->title);
    win->app_id.notify = handle_window_changed;
    wl_signal_add(&view->base.events.app_id, &win->app_id);
    win->fullscreen.notify = handle_window_changed;
    wl_signal_add(&view->base.events.fullscreen, &win->fullscreen);
    win->maximize.notify = handle_window_changed;
    wl_signal_add(&view->base.events.maximize, &win->maximize);
    win->workspace.notify = handle_window_changed;
    wl_signal_add(&view->events.workspace, &win->workspace);
    win->output.notify = handle_window_changed;
    wl_signal_add(&view->events.output, &win->output);
    win->tile.notify = handle_window_changed;
    wl_signal_add(&view->base.events.tile, &win->tile);
    /* demands_attention changes come with it */
    win->capabilities.notify = handle_window_changed;
    wl_signal_add(&view->base.events.capabilities, &win->capabilities);

    schedule_reconcile(ipc);
}

static void handle_new_view(struct wl_listener *listener, void *data)
{
    struct hypr_ipc *ipc = wl_container_of(listener, ipc, new_view);
    track_view(ipc, view_from_kywc_view(data));
}

static void handle_new_mapped_view(struct wl_listener *listener, void *data)
{
    struct hypr_ipc *ipc = wl_container_of(listener, ipc, new_mapped_view);
    track_view(ipc, view_from_kywc_view(data));
}

static void handle_activate_view(struct wl_listener *listener, void *data)
{
    struct hypr_ipc *ipc = wl_container_of(listener, ipc, activate_view);
    schedule_reconcile(ipc);
}

static void handle_workspace_changed(struct wl_listener *listener, void *data)
{
    schedule_reconcile(hypr_ipc);
}

static void hypr_workspace_free(struct hypr_workspace *hws)
{
    wl_list_remove(&hws->link);
    wl_list_remove(&hws->name.link);
    wl_list_remove(&hws->position.link);
    wl_list_remove(&hws->activate.link);
    wl_list_remove(&hws->destroy.link);
    free(hws);
}

static void handle_workspace_destroy(struct wl_listener *listener, void *data)
{
    struct hypr_workspace *hws = wl_container_of(listener, hws, destroy);
    struct hypr_ipc *ipc = hws->ipc;
    hypr_workspace_free(hws);
    schedule_reconcile(ipc);
}

static void track_workspace(struct hypr_ipc *ipc, struct workspace *workspace)
{
    struct hypr_workspace *hws;
    wl_list_for_each(hws, &ipc->workspaces, link) {
        if (hws->workspace == workspace) {
            return;
        }
    }

    hws = calloc(1, sizeof(*hws));
    if (!hws) {
        return;
    }
    hws->ipc = ipc;
    hws->workspace = workspace;
    wl_list_insert(&ipc->workspaces, &hws->link);

    hws->name.notify = handle_workspace_changed;
    wl_signal_add(&workspace->events.name, &hws->name);
    hws->position.notify = handle_workspace_changed;
    wl_signal_add(&workspace->events.position, &hws->position);
    hws->activate.notify = handle_workspace_changed;
    wl_signal_add(&workspace->events.activate, &hws->activate);
    hws->destroy.notify = handle_workspace_destroy;
    wl_signal_add(&workspace->events.destroy, &hws->destroy);

    schedule_reconcile(ipc);
}

static void handle_new_workspace(struct wl_listener *listener, void *data)
{
    struct hypr_ipc *ipc = wl_container_of(listener, ipc, new_workspace);
    track_workspace(ipc, data);
}

static void handle_monitor_changed(struct wl_listener *listener, void *data)
{
    schedule_reconcile(hypr_ipc);
}

static void handle_monitor_destroy(struct wl_listener *listener, void *data)
{
    struct hypr_monitor *mon = wl_container_of(listener, mon, destroy);
    struct hypr_ipc *ipc = mon->ipc;

    if (mon->announced) {
        if (ipc->closed_monitor_count == ipc->closed_monitor_cap) {
            size_t cap = ipc->closed_monitor_cap ? ipc->closed_monitor_cap * 2 : 4;
            struct hypr_closed_monitor *list =
                realloc(ipc->closed_monitors, cap * sizeof(*list));
            if (list) {
                ipc->closed_monitors = list;
                ipc->closed_monitor_cap = cap;
            }
        }
        if (ipc->closed_monitor_count < ipc->closed_monitor_cap) {
            struct hypr_closed_monitor *m = &ipc->closed_monitors[ipc->closed_monitor_count++];
            m->id = mon->id;
            m->name = strdup(str_or_empty(mon->output->name));
            m->desc = monitor_event_desc(mon->output);
        }
    }

    struct output *output = output_from_kywc_output(mon->output);
    for (size_t i = 0; output && i < ipc->user_output_count; i++) {
        if (ipc->user_outputs[i] == output->wlr_output) {
            ipc->user_outputs[i] = ipc->user_outputs[--ipc->user_output_count];
            break;
        }
    }

    wl_list_remove(&mon->link);
    wl_list_remove(&mon->on.link);
    wl_list_remove(&mon->off.link);
    wl_list_remove(&mon->destroy.link);
    free(mon);
    schedule_reconcile(ipc);
}

static void track_output(struct hypr_ipc *ipc, struct kywc_output *output)
{
    if (monitor_from_output(ipc, output)) {
        return;
    }

    struct hypr_monitor *mon = calloc(1, sizeof(*mon));
    if (!mon) {
        return;
    }
    mon->ipc = ipc;
    mon->output = output;
    int id = 0;
    while (monitor_by_id(ipc, id)) {
        id++;
    }
    mon->id = id;
    wl_list_insert(ipc->monitors.prev, &mon->link);

    mon->on.notify = handle_monitor_changed;
    wl_signal_add(&output->events.on, &mon->on);
    mon->off.notify = handle_monitor_changed;
    wl_signal_add(&output->events.off, &mon->off);
    mon->destroy.notify = handle_monitor_destroy;
    wl_signal_add(&output->events.destroy, &mon->destroy);

    schedule_reconcile(ipc);
}

static void handle_new_output(struct wl_listener *listener, void *data)
{
    struct hypr_ipc *ipc = wl_container_of(listener, ipc, new_output);
    track_output(ipc, data);
}

static void handle_primary_output(struct wl_listener *listener, void *data)
{
    struct hypr_ipc *ipc = wl_container_of(listener, ipc, primary_output);
    schedule_reconcile(ipc);
}

/* ------------------------------------------------------------------------- */
/* model queries */

static void workspace_stats(struct hypr_ipc *ipc, struct workspace *ws, int *windows,
                            bool *fullscreen, struct hypr_window **last)
{
    *windows = 0;
    *fullscreen = false;
    *last = NULL;
    struct hypr_window *win;
    wl_list_for_each(win, &ipc->windows, link) {
        if (!window_eligible(win) || window_workspace(win) != ws) {
            continue;
        }
        (*windows)++;
        *fullscreen |= win->view->base.fullscreen;
    }
    wl_list_for_each(win, &ipc->mru, mru_link) {
        if (window_eligible(win) && window_workspace(win) == ws) {
            *last = win;
            break;
        }
    }
}


static void monitor_reserved(struct hypr_monitor *mon, int reserved[4])
{
    struct output *output = output_from_kywc_output(mon->output);
    reserved[0] = reserved[1] = reserved[2] = reserved[3] = 0;
    if (!output) {
        return;
    }
    struct kywc_box *g = &output->geometry, *u = &output->usable_area;
    if (u->width <= 0 || u->height <= 0) {
        return;
    }
    reserved[0] = u->x - g->x;
    reserved[1] = u->y - g->y;
    reserved[2] = (g->x + g->width) - (u->x + u->width);
    reserved[3] = (g->y + g->height) - (u->y + u->height);
}

/* ------------------------------------------------------------------------- */
/* formatting: the replies follow Hyprland's std::format templates
 * (src/ipc/s1/Commands.cpp and src/helpers/SystemInfo.cpp of 0.57.2) byte for byte */

struct hypr_flags {
    bool json, refresh, all, config, follow;
};

/* escapeJSONStrings */
static void sbuf_escaped(struct sbuf *out, const char *s)
{
    for (const unsigned char *p = (const unsigned char *)str_or_empty(s); *p; p++) {
        switch (*p) {
        case '"':
            sbuf_puts(out, "\\\"");
            break;
        case '\\':
            sbuf_puts(out, "\\\\");
            break;
        case '\b':
            sbuf_puts(out, "\\b");
            break;
        case '\f':
            sbuf_puts(out, "\\f");
            break;
        case '\n':
            sbuf_puts(out, "\\n");
            break;
        case '\r':
            sbuf_puts(out, "\\r");
            break;
        case '\t':
            sbuf_puts(out, "\\t");
            break;
        default:
            if (*p <= 0x1f) {
                sbuf_printf(out, "\\u%04x", *p);
            } else {
                sbuf_append(out, (const char *)p, 1);
            }
        }
    }
}

/* escaped copies for one printf, released by strs_free */
struct strs {
    char *items[48];
    size_t count;
};

static const char *J(struct strs *pool, const char *s)
{
    struct sbuf b = { 0 };
    sbuf_escaped(&b, s);
    sbuf_puts(&b, "");
    if (b.oom || pool->count == sizeof(pool->items) / sizeof(pool->items[0])) {
        sbuf_finish(&b);
        return "";
    }
    pool->items[pool->count++] = b.data;
    return b.data;
}

static void strs_free(struct strs *pool)
{
    for (size_t i = 0; i < pool->count; i++) {
        free(pool->items[i]);
    }
    pool->count = 0;
}

/* trimTrailingComma */
static void sbuf_trim_comma(struct sbuf *b)
{
    if (b->len && b->data[b->len - 1] == ',') {
        b->data[--b->len] = '\0';
    }
}

/* std::format("{}", v) of a float or double: std::to_chars' shortest round-trip text, in
 * fixed or scientific notation, whichever is shorter, fixed on a tie */
static const char *fmt_shortest(char buf[32], double v, bool is_float)
{
    if (isinf(v)) {
        return v < 0 ? "-inf" : "inf";
    }
    if (isnan(v)) {
        return "nan";
    }
    char sci[48] = "", fixed[400] = "";
    int max = is_float ? 9 : 17;
    for (int precision = 0; precision < max; precision++) {
        snprintf(sci, sizeof(sci), "%.*e", precision, v);
        if (is_float ? strtof(sci, NULL) == (float)v : strtod(sci, NULL) == v) {
            break;
        }
    }
    for (int decimals = 0; decimals < 340; decimals++) {
        snprintf(fixed, sizeof(fixed), "%.*f", decimals, v);
        if (is_float ? strtof(fixed, NULL) == (float)v : strtod(fixed, NULL) == v) {
            break;
        }
    }
    /* to_chars writes "1e+20", printf "1e+20" too, the exponent has at least two digits */
    const char *pick = strlen(fixed) <= strlen(sci) ? fixed : sci;
    snprintf(buf, 32, "%s", pick);
    return buf;
}

static const char *fmt_float(char buf[32], float v)
{
    return fmt_shortest(buf, v, true);
}

static const char *fmt_double(char buf[32], double v)
{
    return fmt_shortest(buf, v, false);
}

/* Hyprutils CVarList(in, 0, ' '): empty fields kept, each trimmed, past the end is "" */
struct varlist {
    char *buf;
    char **v;
    size_t n;
};

static char *trim_in_place(char *s)
{
    while (isspace((unsigned char)*s)) {
        s++;
    }
    size_t len = strlen(s);
    while (len && isspace((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
    return s;
}

static bool varlist_init(struct varlist *vl, const char *in)
{
    *vl = (struct varlist){ 0 };
    vl->buf = strdup(str_or_empty(in));
    if (!vl->buf) {
        return false;
    }
    size_t fields = 1;
    for (const char *p = vl->buf; *p; p++) {
        fields += *p == ' ';
    }
    vl->v = calloc(fields, sizeof(*vl->v));
    if (!vl->v) {
        free(vl->buf);
        return false;
    }
    char *start = vl->buf;
    for (char *p = vl->buf;; p++) {
        if (*p == ' ' || *p == '\0') {
            bool end = *p == '\0';
            *p = '\0';
            vl->v[vl->n++] = trim_in_place(start);
            if (end) {
                break;
            }
            start = p + 1;
        }
    }
    return true;
}

static const char *var(struct varlist *vl, size_t i)
{
    return i < vl->n ? vl->v[i] : "";
}

/* fields from..end joined by spaces, into out */
static void var_join(struct varlist *vl, size_t from, struct sbuf *out)
{
    for (size_t i = from; i < vl->n; i++) {
        if (i > from) {
            sbuf_puts(out, " ");
        }
        sbuf_puts(out, vl->v[i]);
    }
    sbuf_puts(out, "");
}

static void varlist_finish(struct varlist *vl)
{
    free(vl->v);
    free(vl->buf);
    *vl = (struct varlist){ 0 };
}

/* isNumber(str, allowfloat) */
static bool is_number(const char *s, bool allow_float)
{
    if (!s || !*s) {
        return false;
    }
    if (*s == '-') {
        s++;
    }
    bool digits = false, dot = false;
    for (; *s; s++) {
        if (isdigit((unsigned char)*s)) {
            digits = true;
        } else if (*s == '.' && allow_float && !dot) {
            dot = true;
        } else {
            return false;
        }
    }
    return digits;
}

/* std::stoi: leading whitespace, a number, anything after it ignored */
static bool parse_stoi(const char *s, int *value)
{
    char *end = NULL;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (!end || end == s || errno || v < INT32_MIN || v > INT32_MAX) {
        return false;
    }
    *value = (int)v;
    return true;
}

/* deviceNameToInternalString */
static void device_name(const char *in, char *out, size_t size)
{
    size_t i = 0;
    for (const char *p = str_or_empty(in); *p && i + 1 < size; p++) {
        char c = *p;
        out[i++] = (c == ' ' || c == '\n' || c == ',') ? '-' : (char)tolower((unsigned char)c);
    }
    out[i] = '\0';
}

/* ------------------------------------------------------------------------- */
/* model facts the formatters share */

static struct wlr_output *monitor_wlr_output(struct hypr_monitor *mon)
{
    struct output *output = output_from_kywc_output(mon->output);
    return output ? output->wlr_output : NULL;
}

/* exported by wlroots, <wlr/backend/wayland.h> would pull in wayland-client */
bool wlr_backend_is_wl(struct wlr_backend *backend);
bool wlr_output_is_wl(struct wlr_output *output);
struct wlr_output *wlr_wl_output_create(struct wlr_backend *backend);

/* StringUtils::backendStr */
static const char *output_backend_name(struct wlr_output *output)
{
    if (!output) {
        return "?";
    }
    if (wlr_output_is_drm(output)) {
        return "drm";
    }
    if (wlr_output_is_headless(output)) {
        return "headless";
    }
    if (wlr_output_is_wl(output)) {
        return "wayland";
    }
    return "?";
}

/* formatToString */
static const char *drm_format_name(uint32_t format)
{
    switch (format) {
    case DRM_FORMAT_XRGB2101010:
        return "XRGB2101010";
    case DRM_FORMAT_XBGR2101010:
        return "XBGR2101010";
    case DRM_FORMAT_XRGB8888:
        return "XRGB8888";
    case DRM_FORMAT_XBGR8888:
        return "XBGR8888";
    default:
        return "Invalid";
    }
}

/* m_shortDescription: trim("{make} {model} {serial}") */
static char *monitor_short_desc(struct kywc_output *output)
{
    char *desc = NULL;
    if (asprintf(&desc, "%s %s %s", str_or_empty(output->prop.make),
                 str_or_empty(output->prop.model), str_or_empty(output->prop.serial)) < 0) {
        return NULL;
    }
    char *trimmed = trim_in_place(desc);
    memmove(desc, trimmed, strlen(trimmed) + 1);
    return desc;
}

static const char *window_tag(struct hypr_ipc *ipc, struct view *view, bool description)
{
    struct hypr_toplevel_tag *tag;
    wl_list_for_each(tag, &ipc->toplevel_tags, link) {
        if (tag->surface == view->surface) {
            return str_or_empty(description ? tag->description : tag->tag);
        }
    }
    return "";
}

/* NContentType::toString */
static const char *window_content_type(struct hypr_ipc *ipc, struct view *view)
{
    if (!ipc->server->content_type_manager || !view->surface) {
        return "none";
    }
    switch (wlr_surface_get_content_type_v1(ipc->server->content_type_manager, view->surface)) {
    case WP_CONTENT_TYPE_V1_TYPE_PHOTO:
        return "photo";
    case WP_CONTENT_TYPE_V1_TYPE_VIDEO:
        return "video";
    case WP_CONTENT_TYPE_V1_TYPE_GAME:
        return "game";
    default:
        return "none";
    }
}

static bool window_tearing(struct hypr_ipc *ipc, struct view *view)
{
    return view->surface && ky_scene_surface_is_tearing_allowed(ipc->server->scene, view->surface);
}

static bool window_listed(struct hypr_window *win, bool all)
{
    if (all) {
        return win->view->base.role == KYWC_VIEW_ROLE_NORMAL || window_eligible(win);
    }
    return window_eligible(win);
}

/* ------------------------------------------------------------------------- */
/* CCommandFormatter */

static void format_window(struct hypr_ipc *ipc, struct hypr_window *win, bool json,
                          struct sbuf *out)
{
    struct view *view = win->view;
    struct kywc_view *kv = &view->base;
    struct hypr_monitor *mon = monitor_from_output(ipc, view->output);
    struct workspace *ws = window_workspace(win);
    char ws_buf[16], addr[16] = "";
    const char *ws_display = ws ? ws_name(ws, ws_buf) : "";
    if (ws) {
        snprintf(addr, sizeof(addr), "%" PRIu32, ws_id(ws));
    }
    bool accepts = kv->focusable;
    bool visible = kv->mapped && accepts && !kv->minimized;
    int fs = window_fullscreen_mode(view);
    int pid = view->pid > 0 ? view->pid : -1;

    if (json) {
        struct strs s = { 0 };
        char id_line[48] = "";
        if (ws) {
            snprintf(id_line, sizeof(id_line), "\n        \"id\": %" PRIu32 ",", ws_id(ws));
        }
        sbuf_printf(out,
                    "{\n"
                    "    \"address\": \"0x%" PRIx64 "\",\n"
                    "    \"mapped\": %s,\n"
                    "    \"hidden\": %s,\n"
                    "    \"visible\": %s,\n"
                    "    \"acceptsInput\": %s,\n"
                    "    \"at\": [%d, %d],\n"
                    "    \"size\": [%d, %d],\n"
                    "    \"workspace\": {\n"
                    "        \"address\": \"%s\",%s\n"
                    "        \"type\": \"%s\",\n"
                    "        \"name\": \"%s\"\n"
                    "    },\n"
                    "    \"floating\": %s,\n"
                    "    \"monitor\": %d,\n"
                    "    \"class\": \"%s\",\n"
                    "    \"title\": \"%s\",\n"
                    "    \"initialClass\": \"%s\",\n"
                    "    \"initialTitle\": \"%s\",\n"
                    "    \"pid\": %d,\n"
                    "    \"xwayland\": %s,\n"
                    "    \"pinned\": %s,\n"
                    "    \"pinFullscreened\": false,\n"
                    "    \"fullscreen\": %d,\n"
                    "    \"fullscreenClient\": %d,\n"
                    "    \"fullscreenHandler\": \"default\",\n"
                    "    \"allowedOverFullscreen\": %s,\n"
                    "    \"grouped\": [],\n"
                    "    \"tags\": [],\n"
                    "    \"swallowing\": \"0x0\",\n"
                    "    \"focusHistoryID\": %d,\n"
                    "    \"inhibitingIdle\": %s,\n"
                    "    \"xdgTag\": \"%s\",\n"
                    "    \"xdgDescription\": \"%s\",\n"
                    "    \"contentType\": \"%s\",\n"
                    "    \"tearingHint\": %s,\n"
                    "    \"stableId\": \"%" PRIx64 "\"\n"
                    "},",
                    win->address, kv->mapped ? "true" : "false",
                    kv->minimized ? "true" : "false", visible ? "true" : "false",
                    accepts ? "true" : "false", kv->geometry.x, kv->geometry.y,
                    kv->geometry.width, kv->geometry.height, J(&s, addr), id_line,
                    ws ? "normal" : "", J(&s, ws_display),
                    kv->tiled == KYWC_TILE_NONE ? "true" : "false", mon ? mon->id : -1,
                    J(&s, kv->app_id), J(&s, kv->title), J(&s, win->initial_class),
                    J(&s, win->initial_title), pid, xwayland_check_view(view) ? "true" : "false",
                    kv->sticky ? "true" : "false", fs, fs, kv->kept_above ? "true" : "false",
                    window_focus_history_id(ipc, win),
                    idle_inhibit_view_is_inhibiting(view) ? "true" : "false",
                    J(&s, window_tag(ipc, view, false)), J(&s, window_tag(ipc, view, true)),
                    J(&s, window_content_type(ipc, view)),
                    window_tearing(ipc, view) ? "true" : "false", win->stable_id);
        strs_free(&s);
        return;
    }

    sbuf_printf(out,
                "Window %" PRIx64 " -> %s:\n\tmapped: %d\n\thidden: %d\n\tvisible: %d\n"
                "\tacceptsInput: %d\n\tat: %d,%d\n\tsize: %d,%d\n\tworkspace: %s (%s)\n"
                "\tfloating: %d\n\tmonitor: %d\n\tclass: %s\n\ttitle: %s\n\tinitialClass: %s\n"
                "\tinitialTitle: %s\n\tpid: %d\n\txwayland: %d\n\tpinned: %d\n"
                "\tpinFullscreened: 0\n\tfullscreen: %d\n\tfullscreenClient: %d\n"
                "\tfullscreenHandler: default\n\tallowedOverFullscreen: %d\n\tgrouped: 0\n"
                "\ttags: \n\tswallowing: 0\n\tfocusHistoryID: %d\n\tinhibitingIdle: %d\n"
                "\txdgTag: %s\n\txdgDescription: %s\n\tcontentType: %s\n\ttearingHint: %d\n"
                "\tstableID: %" PRIx64 "\n\n",
                win->address, str_or_empty(kv->title), kv->mapped, kv->minimized, visible,
                accepts, kv->geometry.x, kv->geometry.y, kv->geometry.width,
                kv->geometry.height, addr, ws_display, kv->tiled == KYWC_TILE_NONE,
                mon ? mon->id : -1, str_or_empty(kv->app_id), str_or_empty(kv->title),
                str_or_empty(win->initial_class), str_or_empty(win->initial_title), pid,
                xwayland_check_view(view), kv->sticky, fs, fs, kv->kept_above,
                window_focus_history_id(ipc, win), idle_inhibit_view_is_inhibiting(view),
                window_tag(ipc, view, false), window_tag(ipc, view, true),
                window_content_type(ipc, view), window_tearing(ipc, view), win->stable_id);
}

static void format_workspace(struct hypr_ipc *ipc, struct workspace *ws, bool json,
                             struct sbuf *out)
{
    struct hypr_monitor *mon = workspace_monitor(ipc);
    int windows;
    bool fullscreen;
    struct hypr_window *last;
    workspace_stats(ipc, ws, &windows, &fullscreen, &last);
    char name_buf[16], mon_id[16];
    const char *name = ws_name(ws, name_buf);
    if (mon) {
        snprintf(mon_id, sizeof(mon_id), "%d", mon->id);
    } else {
        strcpy(mon_id, "null");
    }

    if (json) {
        struct strs s = { 0 };
        sbuf_printf(out,
                    "{\n"
                    "    \"address\": \"%" PRIu32 "\",\n"
                    "    \"id\": %" PRIu32 ",\n"
                    "    \"type\": \"normal\",\n"
                    "    \"name\": \"%s\",\n"
                    "    \"monitor\": \"%s\",\n"
                    "    \"monitorID\": %s,\n"
                    "    \"windows\": %d,\n"
                    "    \"hasfullscreen\": %s,\n"
                    "    \"lastwindow\": \"0x%" PRIx64 "\",\n"
                    "    \"lastwindowtitle\": \"%s\",\n"
                    "    \"ispersistent\": false,\n"
                    "    \"tiledLayout\": \"unknown\"\n"
                    "}",
                    ws_id(ws), ws_id(ws), J(&s, name), J(&s, mon ? mon->output->name : "?"),
                    mon_id, windows, fullscreen ? "true" : "false", last ? last->address : 0,
                    last ? J(&s, last->view->base.title) : "");
        strs_free(&s);
        return;
    }
    sbuf_printf(out,
                "workspace %" PRIu32 " (%s) on monitor %s:\n\tmonitorID: %s\n\twindows: %d\n"
                "\thasfullscreen: %d\n\tlastwindow: 0x%" PRIx64 "\n\tlastwindowtitle: %s\n"
                "\tispersistent: 0\n\ttiledLayout: unknown\n\n",
                ws_id(ws), name, mon ? mon->output->name : "?", mon_id, windows, fullscreen,
                last ? last->address : 0, last ? str_or_empty(last->view->base.title) : "");
}

static void monitor_modes(struct kywc_output *o, bool json, struct sbuf *out)
{
    struct kywc_output_mode *mode;
    wl_list_for_each(mode, &o->prop.modes, link) {
        sbuf_printf(out, json ? "\"%dx%d@%.2fHz\"," : "%dx%d@%.2fHz ", mode->width,
                    mode->height, mode->refresh / 1000.0);
    }
}

static void format_monitor(struct hypr_ipc *ipc, struct hypr_monitor *mon,
                           struct hypr_monitor *focused, bool json, struct sbuf *out)
{
    struct kywc_output *o = mon->output;
    struct kywc_output_state *st = &o->state;
    struct wlr_output *wo = monitor_wlr_output(mon);
    bool enabled = monitor_enabled(mon);
    /* every workspace spans all outputs, the current one is active everywhere */
    struct workspace *ws = enabled ? workspace_manager_get_current() : NULL;
    char ws_buf[16], ws_addr[16] = "", scale[32];
    const char *ws_display = ws ? ws_name(ws, ws_buf) : "";
    if (ws) {
        snprintf(ws_addr, sizeof(ws_addr), "%" PRIu32, ws_id(ws));
    }
    int reserved[4];
    monitor_reserved(mon, reserved);
    char *desc = monitor_short_desc(o);
    float refresh = st->refresh / 1000.f;
    bool vrr = wo && wo->adaptive_sync_status == WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED;
    bool vrr_capable = wo && wo->adaptive_sync_supported;
    bool hw_cursor = wo && wo->hardware_cursor;
    const char *format = drm_format_name(wo ? wo->render_format : 0);
    const char *backend = output_backend_name(wo);
    fmt_float(scale, st->scale);

    struct sbuf modes = { 0 };
    monitor_modes(o, json, &modes);
    if (json) {
        sbuf_trim_comma(&modes);
    }
    sbuf_puts(&modes, "");

    if (json) {
        struct strs s = { 0 };
        char id_line[48] = "";
        if (ws) {
            snprintf(id_line, sizeof(id_line), "\n        \"id\": %" PRIu32 ",", ws_id(ws));
        }
        sbuf_printf(out,
                    "{\n"
                    "    \"id\": %d,\n"
                    "    \"name\": \"%s\",\n"
                    "    \"description\": \"%s\",\n"
                    "    \"make\": \"%s\",\n"
                    "    \"model\": \"%s\",\n"
                    "    \"serial\": \"%s\",\n"
                    "    \"width\": %d,\n"
                    "    \"height\": %d,\n"
                    "    \"physicalWidth\": %d,\n"
                    "    \"physicalHeight\": %d,\n"
                    "    \"refreshRate\": %.5f,\n"
                    "    \"x\": %d,\n"
                    "    \"y\": %d,\n"
                    "    \"activeWorkspace\": {\n"
                    "        \"address\": \"%s\",%s\n"
                    "        \"type\": \"%s\",\n"
                    "        \"name\": \"%s\"\n"
                    "    },\n"
                    "    \"specialWorkspace\": {\n"
                    "        \"address\": \"\",\n"
                    "        \"type\": \"\",\n"
                    "        \"name\": \"\"\n"
                    "    },\n"
                    "    \"reserved\": [%d, %d, %d, %d],\n"
                    "    \"scale\": %s,\n"
                    "    \"transform\": %d,\n"
                    "    \"focused\": %s,\n"
                    "    \"dpmsStatus\": %s,\n"
                    "    \"vrr\": %s,\n"
                    "    \"solitary\": \"0\",\n"
                    "    \"solitaryBlockedBy\": null,\n"
                    "    \"activelyTearing\": false,\n"
                    "    \"tearingBlockedBy\": null,\n"
                    "    \"directScanoutTo\": \"0\",\n"
                    "    \"directScanoutBlockedBy\": null,\n"
                    "    \"disabled\": %s,\n"
                    "    \"currentFormat\": \"%s\",\n"
                    "    \"mirrorOf\": \"none\",\n"
                    "    \"availableModes\": [%s],\n"
                    "    \"colorManagementPreset\": \"srgb\",\n"
                    "    \"sdrBrightness\": 1,\n"
                    "    \"sdrSaturation\": 1,\n"
                    "    \"sdrMinLuminance\": 0.2,\n"
                    "    \"sdrMaxLuminance\": 80,\n"
                    "    \"hardwareCursorsInUse\": %s,\n"
                    "    \"hardwareDetails\": {\n"
                    "        \"backend\": \"%s\",\n"
                    "        \"hdr\": false,\n"
                    "        \"chroma\": false,\n"
                    "        \"bt2020\": false,\n"
                    "        \"vrrCapable\": %s\n"
                    "    }\n"
                    "},",
                    mon->id, J(&s, o->name), J(&s, desc), J(&s, o->prop.make),
                    J(&s, o->prop.model), J(&s, o->prop.serial), st->width, st->height,
                    o->prop.phys_width, o->prop.phys_height, refresh, st->lx, st->ly,
                    J(&s, ws_addr), id_line, ws ? "normal" : "", J(&s, ws_display), reserved[0],
                    reserved[1], reserved[2], reserved[3], scale, st->transform,
                    mon == focused ? "true" : "false", st->power ? "true" : "false",
                    vrr ? "true" : "false", enabled ? "false" : "true", format,
                    str_or_empty(modes.data), hw_cursor ? "true" : "false", backend,
                    vrr_capable ? "true" : "false");
        strs_free(&s);
    } else {
        sbuf_printf(out,
                    "Monitor %s (ID %d):\n\t%dx%d@%.5f at %dx%d\n\tdescription: %s\n\tmake: %s\n"
                    "\tmodel: %s\n\tphysical size (mm): %dx%d\n\tserial: %s\n"
                    "\tactive workspace: %s (%s)\n\tspecial workspace:  ()\n"
                    "\treserved: %d %d %d %d\n\tscale: %s\n\ttransform: %d\n\tfocused: %s\n"
                    "\tdpmsStatus: %d\n\tvrr: %s\n\tsolitary: 0\n\tsolitaryBlockedBy: null\n"
                    "\tactivelyTearing: false\n\ttearingBlockedBy: null\n\tdirectScanoutTo: 0\n"
                    "\tdirectScanoutBlockedBy: null\n\tdisabled: %s\n\tcurrentFormat: %s\n"
                    "\tmirrorOf: none\n\tavailableModes: %s\n\tcolorManagementPreset: srgb\n"
                    "\tsdrBrightness: 1\n\tsdrSaturation: 1\n\tsdrMinLuminance: 0.2\n"
                    "\tsdrMaxLuminance: 80\n\thardwareCursorsInUse: %s\n\thardwareDetails:\n"
                    "\t\tbackend: %s\n\t\thdr: no\n\t\tchroma: no\n\t\tbt2020: no\n"
                    "\t\tvrrCapable: %s\n\n",
                    str_or_empty(o->name), mon->id, st->width, st->height, refresh, st->lx,
                    st->ly, str_or_empty(desc), str_or_empty(o->prop.make),
                    str_or_empty(o->prop.model), o->prop.phys_width, o->prop.phys_height,
                    str_or_empty(o->prop.serial), ws_addr, ws_display, reserved[0], reserved[1],
                    reserved[2], reserved[3], scale, st->transform, mon == focused ? "yes" : "no",
                    st->power ? 1 : 0, vrr ? "true" : "false", enabled ? "false" : "true",
                    format, str_or_empty(modes.data), hw_cursor ? "true" : "false", backend,
                    vrr_capable ? "yes" : "no");
    }
    sbuf_finish(&modes);
    free(desc);
}

/* ------------------------------------------------------------------------- */
/* dispatchers */

static bool full_match(const char *pattern, const char *text)
{
    char *anchored = NULL;
    if (asprintf(&anchored, "^(%s)$", pattern) < 0) {
        return false;
    }
    regex_t re;
    int ret = regcomp(&re, anchored, REG_EXTENDED | REG_NOSUB);
    free(anchored);
    if (ret != 0) {
        return false;
    }
    bool match = regexec(&re, str_or_empty(text), 0, NULL, 0) == 0;
    regfree(&re);
    return match;
}

static struct hypr_window *window_by_selector(struct hypr_ipc *ipc, const char *sel)
{
    while (sel && *sel == ' ') {
        sel++;
    }
    if (!sel || !*sel || !strcmp(sel, "activewindow")) {
        return active_window(ipc);
    }

    struct hypr_window *win;
    if (!strncmp(sel, "address:", 8)) {
        uint64_t address = strtoull(sel + 8, NULL, 16);
        wl_list_for_each(win, &ipc->windows, link) {
            if (win->address == address && window_eligible(win)) {
                return win;
            }
        }
        return NULL;
    }
    if (!strncmp(sel, "pid:", 4)) {
        pid_t pid = (pid_t)strtol(sel + 4, NULL, 10);
        wl_list_for_each(win, &ipc->mru, mru_link) {
            if (win->view->pid == pid && window_eligible(win)) {
                return win;
            }
        }
        return NULL;
    }

    enum { BY_CLASS, BY_TITLE, BY_INITIAL_CLASS, BY_INITIAL_TITLE } mode = BY_CLASS;
    const char *pattern = sel;
    if (!strncmp(sel, "class:", 6)) {
        pattern = sel + 6;
    } else if (!strncmp(sel, "title:", 6)) {
        mode = BY_TITLE;
        pattern = sel + 6;
    } else if (!strncmp(sel, "initialclass:", 13)) {
        mode = BY_INITIAL_CLASS;
        pattern = sel + 13;
    } else if (!strncmp(sel, "initialtitle:", 13)) {
        mode = BY_INITIAL_TITLE;
        pattern = sel + 13;
    }

    wl_list_for_each(win, &ipc->mru, mru_link) {
        if (!window_eligible(win)) {
            continue;
        }
        const char *text = mode == BY_CLASS           ? win->view->base.app_id
                           : mode == BY_TITLE         ? win->view->base.title
                           : mode == BY_INITIAL_CLASS ? win->initial_class
                                                      : win->initial_title;
        if (full_match(pattern, text)) {
            return win;
        }
    }
    return NULL;
}

/* flakewm cannot create workspaces, ids past the last one clamp to it */
static struct workspace *workspace_clamped(long id)
{
    long count = workspace_manager_get_count();
    if (count <= 0) {
        return NULL;
    }
    return workspace_by_position(id < 1 ? 0 : id > count ? count - 1 : id - 1);
}

static struct workspace *workspace_by_name(const char *name)
{
    uint32_t count = workspace_manager_get_count();
    for (uint32_t i = 0; i < count; i++) {
        struct workspace *ws = workspace_by_position(i);
        char buf[16];
        if (ws && !strcmp(ws_name(ws, buf), name)) {
            return ws;
        }
    }
    return NULL;
}

/* "+3", "-1", "~2": sign and number, nothing else */
static bool parse_offset(const char *s, long *value)
{
    if ((s[0] != '+' && s[0] != '-' && s[0] != '~') || !isdigit((unsigned char)s[1])) {
        return false;
    }
    char *end = NULL;
    long v = strtol(s + 1, &end, 10);
    if (!end || *end) {
        return false;
    }
    /* clamped, so "current + offset" and "index + offset" cannot overflow */
    if (v > INT_MAX) {
        v = INT_MAX;
    }
    *value = s[0] == '-' ? -v : v;
    return true;
}

/* Hyprland's workspace selectors (src/state/workspace/Resolver.cpp) */
static struct workspace *workspace_by_selector(struct hypr_ipc *ipc, const char *sel)
{
    while (sel && *sel == ' ') {
        sel++;
    }
    if (!sel || !*sel) {
        return NULL;
    }

    struct workspace *current = workspace_manager_get_current();
    long current_id = current ? (long)ws_id(current) : 1;
    long offset;

    if (!strncmp(sel, "name:", 5)) {
        return sel[5] ? workspace_by_name(sel + 5) : NULL;
    }
    if (!strncmp(sel, "empty", 5)) {
        /* "n": the next empty one after the current, "m": same monitor (all are) */
        long count = workspace_manager_get_count();
        for (long id = strchr(sel + 5, 'n') ? current_id + 1 : 1; id <= count; id++) {
            struct workspace *ws = ws_by_id(id);
            int windows;
            bool fs;
            struct hypr_window *last;
            if (ws) {
                workspace_stats(ipc, ws, &windows, &fs, &last);
                if (windows == 0) {
                    return ws;
                }
            }
        }
        return NULL;
    }
    if (!strcmp(sel, "prev") || !strcmp(sel, "previous") ||
        !strcmp(sel, "previous_per_monitor")) {
        return ws_by_id(ipc->previous_ws_id);
    }
    if (!strcmp(sel, "next")) {
        return workspace_clamped(current_id + 1);
    }
    if (sel[0] == 'r' && parse_offset(sel + 1, &offset)) {
        /* every workspace counts, empty ones included */
        return workspace_clamped(sel[1] == '~' ? offset : current_id + offset);
    }
    if ((sel[0] == 'm' || sel[0] == 'e') && parse_offset(sel + 1, &offset)) {
        /* only existing workspaces, sorted by id; relative wraps, absolute clamps */
        struct workspace *list[MAX_WORKSPACES];
        long n = 0, index = 0;
        uint32_t count = workspace_manager_get_count();
        for (uint32_t i = 0; i < count && n < MAX_WORKSPACES; i++) {
            struct workspace *ws = workspace_by_position(i);
            if (ws && ws_exists(ipc, ws)) {
                if (ws == current) {
                    index = n;
                }
                list[n++] = ws;
            }
        }
        if (n == 0) {
            return NULL;
        }
        if (sel[1] == '~') {
            index = offset - 1 < 0 ? 0 : offset - 1 >= n ? n - 1 : offset - 1;
        } else {
            index = ((index + offset) % n + n) % n;
        }
        return list[index];
    }
    if (parse_offset(sel, &offset) && sel[0] != '~') {
        return workspace_clamped(current_id + offset);
    }

    char *end = NULL;
    long id = strtol(sel, &end, 10);
    if (end && end != sel && *end == '\0') {
        return id > 0 ? ws_by_id((uint32_t)id) : NULL;
    }
    /* a bare name addresses a named workspace */
    return workspace_by_name(sel);
}

static void focus_view(struct view *view)
{
    if (view->base.minimized) {
        kywc_view_set_minimized(&view->base, false);
    }
    kywc_view_activate(&view->base);
    view_set_focus(view, input_manager_get_default_seat());
}

static void spawn_shell(const char *command)
{
    pid_t child = fork();
    if (child < 0) {
        kywc_log_errno(KYWC_ERROR, "hyprland ipc: fork failed");
        return;
    }
    if (child == 0) {
        setsid();
        sigset_t set;
        sigfillset(&set);
        sigprocmask(SIG_UNBLOCK, &set, NULL);
        limit_unset_nofile();
        pid_t grandchild = fork();
        if (grandchild == 0) {
            int devnull = open("/dev/null", O_RDWR | O_CLOEXEC);
            if (devnull >= 0) {
                dup2(devnull, STDIN_FILENO);
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
            }
            execl("/bin/sh", "/bin/sh", "-c", command, (char *)NULL);
            _exit(127);
        }
        _exit(grandchild < 0 ? 1 : 0);
    }
    waitpid(child, NULL, 0);
}

/* ------------------------------------------------------------------------- */
/* read commands */

typedef void (*hypr_cmd_func)(struct hypr_ipc *ipc, struct hypr_flags *f, const char *request,
                              struct sbuf *out);

static void cmd_workspaces(struct hypr_ipc *ipc, struct hypr_flags *f, const char *request,
                           struct sbuf *out)
{
    if (f->json) {
        sbuf_puts(out, "[");
    }
    /* existing workspaces in creation order, as announced */
    for (size_t i = 0; i < ipc->ws_snapshot_count; i++) {
        struct workspace *ws = ws_by_id(ipc->ws_snapshot[i].id);
        if (!ws) {
            continue;
        }
        format_workspace(ipc, ws, f->json, out);
        if (f->json) {
            sbuf_puts(out, ",");
        }
    }
    if (f->json) {
        sbuf_trim_comma(out);
        sbuf_puts(out, "]");
    }
}

static void cmd_workspacerules(struct hypr_ipc *ipc, struct hypr_flags *f, const char *request,
                               struct sbuf *out)
{
    /* flakewm has no workspace rules */
    sbuf_puts(out, f->json ? "[]" : "");
}

static void cmd_activeworkspace(struct hypr_ipc *ipc, struct hypr_flags *f, const char *request,
                                struct sbuf *out)
{
    if (!focused_monitor(ipc)) {
        sbuf_puts(out, "unsafe state");
        return;
    }
    struct workspace *ws = workspace_manager_get_current();
    if (!ws) {
        sbuf_puts(out, "internal error");
        return;
    }
    format_workspace(ipc, ws, f->json, out);
}

static void cmd_clients(struct hypr_ipc *ipc, struct hypr_flags *f, const char *request,
                        struct sbuf *out)
{
    size_t start = out->len; // a batch shares the buffer
    if (f->json) {
        sbuf_puts(out, "[");
    }
    struct hypr_window *win;
    /* oldest first, like Hyprland */
    wl_list_for_each_reverse(win, &ipc->windows, link) {
        if (window_listed(win, f->all)) {
            format_window(ipc, win, f->json, out);
        }
    }
    if (f->json) {
        sbuf_trim_comma(out);
        sbuf_puts(out, "]");
    } else if (out->len == start) {
        sbuf_puts(out, "no open windows");
    }
}

static void cmd_activewindow(struct hypr_ipc *ipc, struct hypr_flags *f, const char *request,
                             struct sbuf *out)
{
    struct hypr_window *win = active_window(ipc);
    if (!win) {
        sbuf_puts(out, f->json ? "{}" : "Invalid");
        return;
    }
    format_window(ipc, win, f->json, out);
    if (f->json) {
        sbuf_trim_comma(out);
    }
}

static void cmd_monitors(struct hypr_ipc *ipc, struct hypr_flags *f, const char *request,
                         struct sbuf *out)
{
    struct varlist vars;
    if (!varlist_init(&vars, request)) {
        return;
    }
    if (vars.n > 2) {
        sbuf_puts(out, "too many args");
        varlist_finish(&vars);
        return;
    }
    bool all = vars.n == 2 && !strcmp(var(&vars, 1), "all");
    varlist_finish(&vars);

    struct hypr_monitor *focused = focused_monitor(ipc);
    if (f->json) {
        sbuf_puts(out, "[");
    }
    struct hypr_monitor *mon;
    wl_list_for_each(mon, &ipc->monitors, link) {
        if (mon->output->destroying || (!all && !monitor_enabled(mon))) {
            continue;
        }
        format_monitor(ipc, mon, focused, f->json, out);
    }
    if (f->json) {
        sbuf_trim_comma(out);
        sbuf_puts(out, "]");
    }
}

struct layer_entry {
    struct wlr_layer_surface_v1 *surface;
    struct kywc_output *output;
    int level;
    struct kywc_box box;
};

struct layer_list {
    struct layer_entry *items;
    size_t count, cap;
};

static void collect_layer(struct wlr_layer_surface_v1 *surface, struct kywc_output *output,
                          int level, struct kywc_box *box, void *data)
{
    struct layer_list *list = data;
    if (list->count == list->cap) {
        size_t cap = list->cap ? list->cap * 2 : 16;
        struct layer_entry *items = realloc(list->items, cap * sizeof(*items));
        if (!items) {
            return;
        }
        list->items = items;
        list->cap = cap;
    }
    list->items[list->count++] = (struct layer_entry){ surface, output, level, *box };
}

static pid_t resource_pid(struct wl_resource *resource)
{
    pid_t pid = 0;
    if (resource) {
        wl_client_get_credentials(wl_resource_get_client(resource), &pid, NULL, NULL);
    }
    return pid;
}

static void cmd_layers(struct hypr_ipc *ipc, struct hypr_flags *f, const char *request,
                       struct sbuf *out)
{
    static const char *level_names[4] = { "background", "bottom", "top", "overlay" };
    struct layer_list list = { 0 };
    wlr_layer_shell_for_each(collect_layer, &list);

    if (f->json) {
        sbuf_puts(out, "{\n");
    }
    struct hypr_monitor *mon;
    wl_list_for_each(mon, &ipc->monitors, link) {
        if (!monitor_enabled(mon)) {
            continue;
        }
        struct strs s = { 0 };
        if (f->json) {
            sbuf_printf(out, "\"%s\": {\n    \"levels\": {\n", J(&s, mon->output->name));
        } else {
            sbuf_printf(out, "Monitor %s:\n", mon->output->name);
        }
        for (int level = 0; level < 4; level++) {
            if (f->json) {
                sbuf_printf(out, "\n        \"%d\": [\n", level);
            } else {
                sbuf_printf(out, "\tLayer level %d (%s):\n", level, level_names[level]);
            }
            bool any = false;
            for (size_t i = 0; i < list.count; i++) {
                struct layer_entry *e = &list.items[i];
                if (e->output != mon->output || e->level != level) {
                    continue;
                }
                any = true;
                uint64_t address = (uint64_t)(uintptr_t)e->surface;
                pid_t pid = resource_pid(e->surface->resource);
                if (f->json) {
                    sbuf_printf(out,
                                "                {\n"
                                "                    \"address\": \"0x%" PRIx64 "\",\n"
                                "                    \"x\": %d,\n"
                                "                    \"y\": %d,\n"
                                "                    \"w\": %d,\n"
                                "                    \"h\": %d,\n"
                                "                    \"alpha\": 1,\n"
                                "                    \"namespace\": \"%s\",\n"
                                "                    \"pid\": %d\n"
                                "                },",
                                address, e->box.x, e->box.y, e->box.width, e->box.height,
                                J(&s, e->surface->namespace), (int)pid);
                } else {
                    sbuf_printf(out,
                                "\t\tLayer %" PRIx64 ": xywh: %d %d %d %d, a: 1, namespace: %s, "
                                "pid: %d\n",
                                address, e->box.x, e->box.y, e->box.width, e->box.height,
                                str_or_empty(e->surface->namespace), (int)pid);
                }
            }
            if (f->json) {
                sbuf_trim_comma(out);
                if (any) {
                    sbuf_puts(out, "\n        ");
                }
                sbuf_puts(out, "],");
            }
        }
        if (f->json) {
            sbuf_trim_comma(out);
            sbuf_puts(out, "\n    }\n},");
        } else {
            sbuf_puts(out, "\n\n");
        }
        strs_free(&s);
    }
    if (f->json) {
        sbuf_trim_comma(out);
        sbuf_puts(out, "\n}\n");
    }
    free(list.items);
}

struct input_list {
    struct input *items[128];
    size_t count;
};

static bool collect_input(struct input *input, void *data)
{
    struct input_list *list = data;
    if (list->count < sizeof(list->items) / sizeof(list->items[0])) {
        list->items[list->count++] = input;
    }
    return false;
}

static struct wlr_keyboard *input_keyboard(struct input *input)
{
    if (input->prop.type != WLR_INPUT_DEVICE_KEYBOARD || !input->wlr_input) {
        return NULL;
    }
    return wlr_keyboard_from_input_device(input->wlr_input);
}

/* Hyprland's active keyboard, here the first physical one of the default seat */
static struct input *main_keyboard(struct input_list *list)
{
    struct seat *seat = input_manager_get_default_seat();
    struct input *fallback = NULL;
    for (size_t i = 0; i < list->count; i++) {
        struct input *input = list->items[i];
        if (!input_keyboard(input) || input->seat != seat) {
            continue;
        }
        if (!input->prop.is_virtual) {
            return input;
        }
        if (!fallback) {
            fallback = input;
        }
    }
    return fallback;
}

/* IKeyboard::getActiveLayoutIndex / getActiveLayout */
static void keyboard_layout(struct wlr_keyboard *kb, char index[16], const char **name)
{
    strcpy(index, "none");
    *name = "none";
    if (!kb || !kb->keymap || !kb->xkb_state) {
        return;
    }
    xkb_layout_index_t layouts = xkb_keymap_num_layouts(kb->keymap);
    for (xkb_layout_index_t i = 0; i < layouts; i++) {
        if (xkb_state_layout_index_is_active(kb->xkb_state, i, XKB_STATE_LAYOUT_EFFECTIVE) == 1) {
            snprintf(index, 16, "%u", i);
            const char *layout = xkb_keymap_layout_get_name(kb->keymap, i);
            *name = layout ? layout : "error";
            return;
        }
    }
}

static bool keyboard_mod_locked(struct wlr_keyboard *kb, const char *mod)
{
    if (!kb || !kb->keymap) {
        return false;
    }
    xkb_mod_index_t index = xkb_keymap_mod_get_index(kb->keymap, mod);
    return index != XKB_MOD_INVALID && (kb->modifiers.locked & (1u << index));
}

static void cmd_devices(struct hypr_ipc *ipc, struct hypr_flags *f, const char *request,
                        struct sbuf *out)
{
    struct input_list list = { 0 };
    input_manager_for_each_input(collect_input, &list);
    struct input *main_kb = main_keyboard(&list);
    struct strs s = { 0 };
    char name[256];

    /* mice */
    sbuf_puts(out, f->json ? "{\n\"mice\": [\n" : "mice:\n");
    for (size_t i = 0; i < list.count; i++) {
        struct input *input = list.items[i];
        if (input->prop.type != WLR_INPUT_DEVICE_POINTER) {
            continue;
        }
        device_name(input->name, name, sizeof(name));
        double speed = input->device ? input->default_state.pointer_accel_speed : 0.f;
        if (f->json) {
            sbuf_printf(out,
                        "    {\n        \"address\": \"0x%" PRIxPTR "\",\n"
                        "        \"name\": \"%s\",\n        \"defaultSpeed\": %.5f,\n"
                        "        \"scrollFactor\": %.2f\n    },",
                        (uintptr_t)input, J(&s, name), speed, input->state.scroll_factor);
        } else {
            sbuf_printf(out,
                        "\tMouse at %" PRIxPTR ":\n\t\t%s\n\t\t\tdefault speed: %.5f\n"
                        "\t\t\tscroll factor: %.2f\n",
                        (uintptr_t)input, name, speed, input->state.scroll_factor);
        }
    }

    /* keyboards */
    if (f->json) {
        sbuf_trim_comma(out);
        sbuf_puts(out, "\n],\n\"keyboards\": [\n");
    } else {
        sbuf_puts(out, "\n\nKeyboards:\n");
    }
    for (size_t i = 0; i < list.count; i++) {
        struct input *input = list.items[i];
        struct wlr_keyboard *kb = input_keyboard(input);
        if (!kb) {
            continue;
        }
        device_name(input->name, name, sizeof(name));
        char index[16];
        const char *layout;
        keyboard_layout(kb, index, &layout);
        struct keymap_rules *rules = &input->state.rules;
        bool caps = keyboard_mod_locked(kb, XKB_MOD_NAME_CAPS);
        bool num = keyboard_mod_locked(kb, XKB_MOD_NAME_NUM);
        if (f->json) {
            sbuf_printf(out,
                        "    {\n        \"address\": \"0x%" PRIxPTR "\",\n"
                        "        \"name\": \"%s\",\n        \"rules\": \"%s\",\n"
                        "        \"model\": \"%s\",\n        \"layout\": \"%s\",\n"
                        "        \"variant\": \"%s\",\n        \"options\": \"%s\",\n"
                        "        \"active_layout_index\": %s,\n"
                        "        \"active_keymap\": \"%s\",\n        \"capsLock\": %s,\n"
                        "        \"numLock\": %s,\n        \"main\": %s\n    },",
                        (uintptr_t)input, J(&s, name), J(&s, rules->xkb_rules),
                        J(&s, rules->xkb_model), J(&s, rules->xkb_layout),
                        J(&s, rules->xkb_variant), J(&s, rules->xkb_options), index,
                        J(&s, layout), caps ? "true" : "false", num ? "true" : "false",
                        input == main_kb ? "true" : "false");
            strs_free(&s);
        } else {
            sbuf_printf(out,
                        "\tKeyboard at %" PRIxPTR ":\n\t\t%s\n\t\t\trules: r \"%s\", m \"%s\", "
                        "l \"%s\", v \"%s\", o \"%s\"\n\t\t\tactive layout index: %s\n"
                        "\t\t\tactive keymap: %s\n\t\t\tcapsLock: %s\n\t\t\tnumLock: %s\n"
                        "\t\t\tmain: %s\n",
                        (uintptr_t)input, name, str_or_empty(rules->xkb_rules),
                        str_or_empty(rules->xkb_model), str_or_empty(rules->xkb_layout),
                        str_or_empty(rules->xkb_variant), str_or_empty(rules->xkb_options),
                        index, layout, caps ? "yes" : "no", num ? "yes" : "no",
                        input == main_kb ? "yes" : "no");
        }
    }

    /* tablets: pads, then tablets */
    if (f->json) {
        sbuf_trim_comma(out);
        sbuf_puts(out, "\n],\n\"tablets\": [\n");
    } else {
        sbuf_puts(out, "\n\nTablets:\n");
    }
    for (size_t i = 0; i < list.count; i++) {
        struct input *input = list.items[i];
        if (input->prop.type != WLR_INPUT_DEVICE_TABLET_PAD) {
            continue;
        }
        if (f->json) {
            sbuf_printf(out,
                        "    {\n        \"address\": \"0x%" PRIxPTR "\",\n"
                        "        \"type\": \"tabletPad\",\n        \"belongsTo\": {\n"
                        "            \"address\": \"0x0\",\n            \"name\": \"\"\n"
                        "        }\n    },",
                        (uintptr_t)input);
        } else {
            sbuf_printf(out, "\tTablet Pad at %" PRIxPTR " (belongs to 0 -> )\n",
                        (uintptr_t)input);
        }
    }
    for (size_t i = 0; i < list.count; i++) {
        struct input *input = list.items[i];
        if (input->prop.type != WLR_INPUT_DEVICE_TABLET) {
            continue;
        }
        device_name(input->name, name, sizeof(name));
        if (f->json) {
            sbuf_printf(out,
                        "    {\n        \"address\": \"0x%" PRIxPTR "\",\n"
                        "        \"name\": \"%s\"\n    },",
                        (uintptr_t)input, J(&s, name));
        } else {
            struct wlr_tablet *tablet = wlr_tablet_from_input_device(input->wlr_input);
            char w[32], h[32];
            sbuf_printf(out, "\tTablet at %" PRIxPTR ":\n\t\t%s\n\t\t\tsize: %sx%smm\n",
                        (uintptr_t)input, name, fmt_double(w, tablet->width_mm),
                        fmt_double(h, tablet->height_mm));
        }
    }

    /* touch */
    if (f->json) {
        sbuf_trim_comma(out);
        sbuf_puts(out, "\n],\n\"touch\": [\n");
    } else {
        sbuf_puts(out, "\n\nTouch:\n");
    }
    for (size_t i = 0; i < list.count; i++) {
        struct input *input = list.items[i];
        if (input->prop.type != WLR_INPUT_DEVICE_TOUCH) {
            continue;
        }
        device_name(input->name, name, sizeof(name));
        if (f->json) {
            sbuf_printf(out,
                        "    {\n        \"address\": \"0x%" PRIxPTR "\",\n"
                        "        \"name\": \"%s\"\n    },",
                        (uintptr_t)input, J(&s, name));
        } else {
            sbuf_printf(out, "\tTouch Device at %" PRIxPTR ":\n\t\t%s\n", (uintptr_t)input, name);
        }
    }

    /* switches, under their device name */
    if (f->json) {
        sbuf_trim_comma(out);
        sbuf_puts(out, "\n],\n\"switches\": [\n");
    } else {
        sbuf_puts(out, "\n\nSwitches:\n");
    }
    for (size_t i = 0; i < list.count; i++) {
        struct input *input = list.items[i];
        if (input->prop.type != WLR_INPUT_DEVICE_SWITCH) {
            continue;
        }
        if (f->json) {
            sbuf_printf(out,
                        "    {\n        \"address\": \"0x%" PRIxPTR "\",\n"
                        "        \"name\": \"%s\"\n    },",
                        (uintptr_t)input, J(&s, input->name));
        } else {
            sbuf_printf(out, "\tSwitch Device at %" PRIxPTR ":\n\t\t%s\n", (uintptr_t)input,
                        str_or_empty(input->name));
        }
    }
    if (f->json) {
        sbuf_trim_comma(out);
        sbuf_puts(out, "\n]\n}\n");
    }
    strs_free(&s);
}

static void cmd_cursorpos(struct hypr_ipc *ipc, struct hypr_flags *f, const char *request,
                          struct sbuf *out)
{
    struct seat *seat = input_manager_get_default_seat();
    int x = 0, y = 0;
    if (seat && seat->cursor) {
        x = (int)floor(seat->cursor->lx);
        y = (int)floor(seat->cursor->ly);
    }
    if (f->json) {
        sbuf_printf(out, "\n{\n    \"x\": %d,\n    \"y\": %d\n}\n", x, y);
    } else {
        sbuf_printf(out, "%d, %d", x, y);
    }
}

struct binds_ctx {
    struct sbuf *out;
    bool json;
};

static bool format_bind(struct key_binding *binding, enum key_binding_type type, char *keybind,
                        char *desc, int32_t modifiers, int32_t key, void *data)
{
    struct binds_ctx *ctx = data;
    char key_name[64] = "";
    if (key) {
        xkb_keysym_get_name((xkb_keysym_t)key, key_name, sizeof(key_name));
    }
    if (!ctx->json) {
        sbuf_printf(ctx->out,
                    "bind\n\tflags: \n\tmodmask: %u\n\tsubmap: \n\tkey: %s\n\tkeycode: 0\n"
                    "\tcatchall: false\n\tdescription: %s\n\tdispatcher: \n\targ: \n\n",
                    (uint32_t)modifiers, key_name, str_or_empty(desc));
        return false;
    }
    struct strs s = { 0 };
    sbuf_printf(ctx->out,
                "\n{\n    \"locked\": false,\n    \"mouse\": false,\n    \"release\": false,\n"
                "    \"repeat\": false,\n    \"longPress\": false,\n"
                "    \"non_consuming\": false,\n    \"auto_consuming\": false,\n"
                "    \"has_description\": %s,\n    \"modmask\": %u,\n    \"submap\": \"\",\n"
                "    \"submap_universal\": \"false\",\n    \"key\": \"%s\",\n"
                "    \"keycode\": 0,\n    \"catch_all\": false,\n    \"description\": \"%s\",\n"
                "    \"allow_input_capture\": false,\n    \"dispatcher\": \"\",\n"
                "    \"arg\": \"\"\n},",
                desc && *desc ? "true" : "false", (uint32_t)modifiers, J(&s, key_name),
                J(&s, desc));
    strs_free(&s);
    return false;
}

static void cmd_binds(struct hypr_ipc *ipc, struct hypr_flags *f, const char *request,
                      struct sbuf *out)
{
    struct binds_ctx ctx = { out, f->json };
    if (f->json) {
        sbuf_puts(out, "[");
    }
    kywc_key_binding_for_each_typed(format_bind, &ctx);
    if (f->json) {
        sbuf_trim_comma(out);
        sbuf_puts(out, "]");
    }
}

struct shortcuts_ctx {
    struct sbuf *out;
    bool json, any;
};

static bool format_shortcut(const char *app_id, const char *id, const char *description,
                            const char *trigger_description, void *data)
{
    struct shortcuts_ctx *ctx = data;
    ctx->any = true;
    if (!ctx->json) {
        sbuf_printf(ctx->out, "%s:%s -> %s\n", app_id, id, description);
        return false;
    }
    char *name = NULL;
    if (asprintf(&name, "%s:%s", app_id, id) < 0) {
        return false;
    }
    struct strs s = { 0 };
    sbuf_printf(ctx->out, "\n{\n    \"name\": \"%s\",\n    \"description\": \"%s\"\n},",
                J(&s, name), J(&s, description));
    strs_free(&s);
    free(name);
    return false;
}

static void cmd_globalshortcuts(struct hypr_ipc *ipc, struct hypr_flags *f, const char *request,
                                struct sbuf *out)
{
    struct shortcuts_ctx ctx = { out, f->json, false };
    if (f->json) {
        sbuf_puts(out, "[");
    }
    hyprland_global_shortcuts_for_each(format_shortcut, &ctx);
    if (f->json) {
        sbuf_trim_comma(out);
        sbuf_puts(out, "]\n");
    } else if (!ctx.any) {
        sbuf_puts(out, "none");
    }
}

/* getSystemLibraryVersion */
static void library_version(const char *name, char *out, size_t size)
{
    static const char *paths[] = { "/usr/lib/pkgconfig", "/usr/local/lib/pkgconfig",
                                   "/usr/lib64/pkgconfig" };
    snprintf(out, size, "unknown");
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        char path[256];
        snprintf(path, sizeof(path), "%s/%s.pc", paths[i], name);
        FILE *file = fopen(path, "re");
        if (!file) {
            continue;
        }
        char line[256];
        bool found = false;
        while (fgets(line, sizeof(line), file)) {
            if (!strncmp(line, "Version: ", 9)) {
                line[strcspn(line, "\n")] = '\0';
                snprintf(out, size, "%s", line + 9);
                found = true;
                break;
            }
        }
        fclose(file);
        if (found) {
            return;
        }
    }
}

static const char *hypr_libraries[5][2] = {
    { "Hyprgraphics", "hyprgraphics" }, { "Hyprutils", "hyprutils" },
    { "Hyprcursor", "hyprcursor" },     { "Hyprlang", "hyprlang" },
    { "Aquamarine", "aquamarine" },
};

/* getBuiltSystemLibraryNames, nothing of it is built into flakewm */
static void library_names(struct sbuf *out)
{
    sbuf_puts(out, "Libraries:\n");
    for (size_t i = 0; i < 5; i++) {
        char version[64];
        library_version(hypr_libraries[i][1], version, sizeof(version));
        sbuf_printf(out, "%s: built against none, system has %s\n", hypr_libraries[i][0],
                    version);
    }
}

/* SystemInfo::getVersion */
static void format_version(bool json, struct sbuf *out)
{
    if (!json) {
        sbuf_puts(out, "Hyprland " HYPR_VERSION " built from branch " HYPR_GIT_BRANCH
                       " at commit  (" HYPR_GIT_COMMIT_MESSAGE ").\n"
                       "Date: \nTag: v" HYPR_VERSION ", commits: 0\n");
        sbuf_puts(out, "\n");
        library_names(out);
        sbuf_puts(out, "\nVersion ABI string: \nno flags were set\n");
        return;
    }
    char versions[5][64];
    for (size_t i = 0; i < 5; i++) {
        library_version(hypr_libraries[i][1], versions[i], sizeof(versions[i]));
    }
    sbuf_printf(out,
                "{\n    \"branch\": \"" HYPR_GIT_BRANCH "\",\n    \"commit\": \"\",\n"
                "    \"version\": \"" HYPR_VERSION "\",\n    \"dirty\": false,\n"
                "    \"commit_message\": \"" HYPR_GIT_COMMIT_MESSAGE "\",\n"
                "    \"commit_date\": \"\",\n    \"tag\": \"v" HYPR_VERSION "\",\n"
                "    \"commits\": \"0\",\n    \"buildAquamarine\": \"none\",\n"
                "    \"buildHyprlang\": \"none\",\n    \"buildHyprutils\": \"none\",\n"
                "    \"buildHyprcursor\": \"none\",\n    \"buildHyprgraphics\": \"none\",\n"
                "    \"systemAquamarine\": \"%s\",\n    \"systemHyprlang\": \"%s\",\n"
                "    \"systemHyprutils\": \"%s\",\n    \"systemHyprcursor\": \"%s\",\n"
                "    \"systemHyprgraphics\": \"%s\",\n    \"abiHash\": \"\",\n"
                "    \"flags\": []\n}",
                versions[4], versions[3], versions[1], versions[2], versions[0]);
}

static void cmd_version(struct hypr_ipc *ipc, struct hypr_flags *f, const char *request,
                        struct sbuf *out)
{
    format_version(f->json, out);
}

static void find_session_backend(struct wlr_backend *backend, void *data)
{
    const char **name = data;
    if (strcmp(*name, "error")) {
        return;
    }
    if (wlr_backend_is_drm(backend)) {
        *name = "drm";
    } else if (wlr_backend_is_wl(backend)) {
        *name = "wayland";
    }
}

/* SystemInfo::getStatus, flakewm acts as the lua config provider of 0.57 */
static void format_status(struct hypr_ipc *ipc, bool json, struct sbuf *out)
{
    const char *backend = "error";
    struct wlr_backend *wlr_backend = ipc->server->backend;
    if (wlr_backend && wlr_backend_is_multi(wlr_backend)) {
        wlr_multi_for_each_backend(wlr_backend, find_session_backend, &backend);
    } else if (wlr_backend) {
        find_session_backend(wlr_backend, &backend);
    }
    if (json) {
        sbuf_printf(out, "\n{\n    \"configProvider\": \"lua\",\n    \"backend\": \"%s\"\n}\n",
                    backend);
    } else {
        sbuf_printf(out, "\nconfigProvider: lua\nbackend: %s\n", backend);
    }
}

static void cmd_status(struct hypr_ipc *ipc, struct hypr_flags *f, const char *request,
                       struct sbuf *out)
{
    format_status(ipc, f->json, out);
}

/* the "lspci | grep VGA" of Hyprland, from sysfs instead of a subprocess */
static void gpu_info(struct sbuf *out)
{
    DIR *dir = opendir("/sys/bus/pci/devices");
    if (!dir) {
        return;
    }
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        char path[512], value[64];
        unsigned long class = 0, vendor = 0, device = 0;
        const char *fields[3] = { "class", "vendor", "device" };
        unsigned long *values[3] = { &class, &vendor, &device };
        bool ok = true;
        for (int i = 0; i < 3 && ok; i++) {
            snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/%s", entry->d_name, fields[i]);
            FILE *file = fopen(path, "re");
            ok = file && fgets(value, sizeof(value), file);
            if (file) {
                fclose(file);
            }
            if (ok) {
                *values[i] = strtoul(value, NULL, 16);
            }
        }
        /* display controllers: VGA, XGA, 3D, other */
        if (!ok || (class >> 16) != 0x03) {
            continue;
        }
        const char *kind = (class >> 8) == 0x0300   ? "VGA compatible controller"
                           : (class >> 8) == 0x0302 ? "3D controller"
                                                    : "Display controller";
        sbuf_printf(out, "%s %s [%04lx]: [%04lx:%04lx]\n", entry->d_name, kind, class >> 8,
                    vendor, device);
    }
    closedir(dir);
}

/* SystemInfo::getSystemInfo */
static void cmd_systeminfo(struct hypr_ipc *ipc, struct hypr_flags *f, const char *request,
                           struct sbuf *out)
{
    format_version(false, out);
    sbuf_puts(out, "\n\nSystem Information:\n");
    struct utsname uts;
    if (uname(&uts) == 0) {
        sbuf_printf(out, "System name: %s\nNode name: %s\nRelease: %s\nVersion: %s\n",
                    uts.sysname, uts.nodename, uts.release, uts.version);
    }
    sbuf_puts(out, "\n");
    library_names(out);
    sbuf_puts(out, "\n\n\nGPU information: \n");
    gpu_info(out);
    sbuf_puts(out, "\n\n");

    FILE *file = fopen("/etc/os-release", "re");
    if (file) {
        sbuf_puts(out, "os-release: ");
        char buf[1024];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), file)) > 0) {
            sbuf_append(out, buf, n);
        }
        fclose(file);
        sbuf_puts(out, "\n\n");
    } else {
        sbuf_puts(out, "os-release: error\n\n");
    }
    sbuf_puts(out, "plugins:\n");
    sbuf_printf(out, "\nBackend: %s", ipc->server->session ? "drm" : "sessionless");
    sbuf_puts(out, "\n\nMonitor info:");
    struct hypr_monitor *mon;
    wl_list_for_each(mon, &ipc->monitors, link) {
        if (!monitor_enabled(mon)) {
            continue;
        }
        struct kywc_output *o = mon->output;
        struct wlr_output *wo = monitor_wlr_output(mon);
        sbuf_printf(out,
                    "\n\tPanel %s: %dx%d, %s %s %s %s -> backend %s\n\t\texplicit %s\n"
                    "\t\tedid:\n\t\t\thdr %s\n\t\t\tchroma %s\n\t\t\tbt2020 %s\n"
                    "\t\tvrr capable %s\n\t\tnon-desktop %s\n\t\t",
                    o->name, o->state.width, o->state.height, o->name,
                    str_or_empty(o->prop.make), str_or_empty(o->prop.model),
                    str_or_empty(o->prop.serial), output_backend_name(wo), "❌", "❌", "❌", "❌",
                    wo && wo->adaptive_sync_supported ? "✔️" : "❌",
                    wo && wo->non_desktop ? "✔️" : "❌");
    }
    sbuf_puts(out, "\n\nState:\n");
    format_status(ipc, false, out);
    sbuf_puts(out, "\n\n");

    if (f->config) {
        /* flakewm has no Hyprland config to show */
        sbuf_puts(out, "\n\n======Config-Start======\n\n======Config-End========\n");
    }
}

static void cmd_animations(struct hypr_ipc *ipc, struct hypr_flags *f, const char *request,
                           struct sbuf *out)
{
    /* the root of Hyprland's animation tree and its default bezier */
    if (!f->json) {
        sbuf_puts(out, "animations:\n\n\tname: global\n\t\toverridden: 0\n\t\tbezier: default\n"
                       "\t\tenabled: true\n\t\tspeed: 8.00\n\t\tstyle: \n"
                       "beziers:\n\n\tname: default\n\t\tX0: 0.00\n\t\tY0: 0.75\n\t\tX1: 0.15\n"
                       "\t\tY1: 1.00");
        return;
    }
    sbuf_puts(out, "[[\n{\n    \"name\": \"global\",\n    \"overridden\": false,\n"
                   "    \"bezier\": \"default\",\n    \"enabled\": true,\n    \"speed\": 8.00,\n"
                   "    \"style\": \"\"\n}],\n[\n{\n    \"name\": \"default\",\n"
                   "    \"X0\": 0.00,\n    \"Y0\": 0.75,\n    \"X1\": 0.15,\n    \"Y1\": 1.00\n}]]");
}

static void cmd_rollinglog(struct hypr_ipc *ipc, struct hypr_flags *f, const char *request,
                           struct sbuf *out)
{
    char *log = logger_rolling_dup();
    if (f->json) {
        sbuf_puts(out, "[\n\"log\":\"");
        sbuf_escaped(out, log);
        sbuf_puts(out, "\"]");
    } else {
        sbuf_puts(out, str_or_empty(log));
    }
    free(log);
}

static void cmd_configerrors(struct hypr_ipc *ipc, struct hypr_flags *f, const char *request,
                             struct sbuf *out)
{
    /* no errors is one empty line, CVarList("") */
    sbuf_puts(out, f->json ? "[\n\t\"\"\n]\n" : "\n");
}

static void cmd_locked(struct hypr_ipc *ipc, struct hypr_flags *f, const char *request,
                       struct sbuf *out)
{
    const char *locked = session_lock_is_active() ? "true" : "false";
    if (f->json) {
        sbuf_printf(out, "\n{\n    \"locked\": %s\n}\n", locked);
    } else {
        sbuf_puts(out, locked);
    }
}

static void cmd_descriptions(struct hypr_ipc *ipc, struct hypr_flags *f, const char *request,
                             struct sbuf *out)
{
    /* Config::Values::getAsJson(), flakewm has no Hyprland options */
    sbuf_puts(out, "[]");
}

static void cmd_submap(struct hypr_ipc *ipc, struct hypr_flags *f, const char *request,
                       struct sbuf *out)
{
    sbuf_puts(out, f->json ? "\"default\"\n" : "default\n");
}

static void cmd_deprecated_config(struct hypr_ipc *ipc, struct hypr_flags *f, const char *request,
                                  struct sbuf *out)
{
    sbuf_puts(out, f->json ? "[]" : "all good!");
}

static void cmd_splash(struct hypr_ipc *ipc, struct hypr_flags *f, const char *request,
                       struct sbuf *out)
{
    sbuf_puts(out, "flakewm");
}

/* ------------------------------------------------------------------------- */
/* hl.dispatch(...): the Lua dispatchers of 0.57 (config/lua/bindings), parsed
 * from a small subset of Lua: one call of hl.dsp.* with literal arguments */

enum lval_type { LVAL_NIL, LVAL_BOOL, LVAL_NUM, LVAL_STR, LVAL_TABLE };

struct lfield;

struct lval {
    enum lval_type type;
    bool b;
    double num;
    char *str;
    struct lfield *fields;
    size_t nfields;
};

struct lfield {
    char *key; // NULL for positional fields
    struct lval val;
};

struct lparser {
    const char *p;
    char err[128];
};

static void lval_finish(struct lval *v)
{
    free(v->str);
    for (size_t i = 0; i < v->nfields; i++) {
        free(v->fields[i].key);
        lval_finish(&v->fields[i].val);
    }
    free(v->fields);
    *v = (struct lval){ 0 };
}

static void lp_skip(struct lparser *lp)
{
    while (true) {
        while (isspace((unsigned char)*lp->p)) {
            lp->p++;
        }
        if (lp->p[0] == '-' && lp->p[1] == '-') {
            while (*lp->p && *lp->p != '\n') {
                lp->p++;
            }
            continue;
        }
        return;
    }
}

static bool lp_fail(struct lparser *lp, const char *what)
{
    if (!lp->err[0]) {
        snprintf(lp->err, sizeof(lp->err), "%s near '%.12s'", what, *lp->p ? lp->p : "<eof>");
    }
    return false;
}

static bool lp_accept(struct lparser *lp, char c)
{
    lp_skip(lp);
    if (*lp->p == c) {
        lp->p++;
        return true;
    }
    return false;
}

static char *lp_ident(struct lparser *lp)
{
    lp_skip(lp);
    const char *start = lp->p;
    if (!isalpha((unsigned char)*start) && *start != '_') {
        return NULL;
    }
    while (isalnum((unsigned char)*lp->p) || *lp->p == '_') {
        lp->p++;
    }
    return strndup(start, lp->p - start);
}

static bool lp_string(struct lparser *lp, char **out)
{
    char quote = *lp->p++;
    struct sbuf b = { 0 };
    while (*lp->p && *lp->p != quote) {
        char c = *lp->p++;
        if (c == '\\' && *lp->p) {
            c = *lp->p++;
            c = c == 'n' ? '\n' : c == 't' ? '\t' : c == 'r' ? '\r' : c;
        }
        sbuf_append(&b, &c, 1);
    }
    if (*lp->p != quote) {
        sbuf_finish(&b);
        return lp_fail(lp, "unfinished string");
    }
    lp->p++;
    sbuf_puts(&b, "");
    *out = b.data;
    return !b.oom;
}

static bool lp_value(struct lparser *lp, struct lval *v, int depth);

static bool lp_table(struct lparser *lp, struct lval *v, int depth)
{
    v->type = LVAL_TABLE;
    while (true) {
        lp_skip(lp);
        if (lp_accept(lp, '}')) {
            return true;
        }
        struct lfield field = { 0 };
        const char *save = lp->p;
        if (*lp->p == '[') {
            lp->p++;
            lp_skip(lp);
            if ((*lp->p != '"' && *lp->p != '\'') || !lp_string(lp, &field.key) ||
                !lp_accept(lp, ']') || !lp_accept(lp, '=')) {
                free(field.key);
                return lp_fail(lp, "bad table key");
            }
        } else {
            char *ident = lp_ident(lp);
            if (ident && lp_accept(lp, '=')) {
                field.key = ident;
            } else {
                free(ident);
                lp->p = save; // a positional value, e.g. true or a nested call is not allowed
            }
        }
        if (!lp_value(lp, &field.val, depth + 1)) {
            free(field.key);
            lval_finish(&field.val);
            return false;
        }
        struct lfield *fields = realloc(v->fields, (v->nfields + 1) * sizeof(*fields));
        if (!fields) {
            free(field.key);
            lval_finish(&field.val);
            return lp_fail(lp, "out of memory");
        }
        v->fields = fields;
        v->fields[v->nfields++] = field;
        if (!lp_accept(lp, ',') && !lp_accept(lp, ';')) {
            if (lp_accept(lp, '}')) {
                return true;
            }
            return lp_fail(lp, "'}' expected");
        }
    }
}

static bool lp_value(struct lparser *lp, struct lval *v, int depth)
{
    *v = (struct lval){ 0 };
    if (depth > 8) {
        return lp_fail(lp, "nested too deep");
    }
    lp_skip(lp);
    char c = *lp->p;
    if (c == '"' || c == '\'') {
        v->type = LVAL_STR;
        return lp_string(lp, &v->str);
    }
    if (c == '{') {
        lp->p++;
        return lp_table(lp, v, depth);
    }
    if (isdigit((unsigned char)c) || c == '-' || c == '.') {
        char *end = NULL;
        v->num = strtod(lp->p, &end);
        if (!end || end == lp->p) {
            return lp_fail(lp, "unexpected symbol");
        }
        lp->p = end;
        v->type = LVAL_NUM;
        return true;
    }
    char *ident = lp_ident(lp);
    if (ident && (!strcmp(ident, "true") || !strcmp(ident, "false"))) {
        v->type = LVAL_BOOL;
        v->b = ident[0] == 't';
        free(ident);
        return true;
    }
    if (ident && !strcmp(ident, "nil")) {
        free(ident);
        return true;
    }
    free(ident);
    return lp_fail(lp, "unexpected symbol");
}

/* "hl.dsp.a.b(args)" -> path "a.b" */
static bool lp_call(struct lparser *lp, const char *prefix, char **path, struct lval *args)
{
    *args = (struct lval){ .type = LVAL_TABLE };
    lp_skip(lp);
    size_t n = strlen(prefix);
    if (strncmp(lp->p, prefix, n)) {
        return lp_fail(lp, "unexpected symbol");
    }
    lp->p += n;
    struct sbuf name = { 0 };
    while (lp_accept(lp, '.')) {
        char *ident = lp_ident(lp);
        if (!ident) {
            sbuf_finish(&name);
            return lp_fail(lp, "<name> expected");
        }
        if (name.len) {
            sbuf_puts(&name, ".");
        }
        sbuf_puts(&name, ident);
        free(ident);
    }
    sbuf_puts(&name, "");
    *path = name.data;
    if (!lp_accept(lp, '(')) {
        return lp_fail(lp, "'(' expected");
    }
    if (lp_accept(lp, ')')) {
        return true;
    }
    while (true) {
        struct lval value;
        if (!lp_value(lp, &value, 0)) {
            lval_finish(&value);
            return false;
        }
        struct lfield *fields = realloc(args->fields, (args->nfields + 1) * sizeof(*fields));
        if (!fields) {
            lval_finish(&value);
            return lp_fail(lp, "out of memory");
        }
        args->fields = fields;
        args->fields[args->nfields++] = (struct lfield){ NULL, value };
        if (lp_accept(lp, ')')) {
            return true;
        }
        if (!lp_accept(lp, ',')) {
            return lp_fail(lp, "')' expected");
        }
    }
}

static struct lval *larg(struct lval *args, size_t index)
{
    return index < args->nfields ? &args->fields[index].val : NULL;
}

static struct lval *lfield(struct lval *table, const char *key)
{
    if (!table || table->type != LVAL_TABLE) {
        return NULL;
    }
    for (size_t i = 0; i < table->nfields; i++) {
        if (table->fields[i].key && !strcmp(table->fields[i].key, key)) {
            return &table->fields[i].val;
        }
    }
    return NULL;
}

/* Lua numbers come from strtod: nan/inf are rejected, the rest truncated and clamped */
static bool lnum_int(const struct lval *v, int lo, int hi, int *out)
{
    if (!v || v->type != LVAL_NUM || !isfinite(v->num)) {
        return false;
    }
    double d = trunc(v->num);
    *out = d < lo ? lo : d > hi ? hi : (int)d;
    return true;
}

/* argStr: strings as they are, numbers like Lua prints them */
static const char *lval_str(struct lval *v, char buf[32])
{
    if (!v) {
        return NULL;
    }
    if (v->type == LVAL_STR) {
        return v->str;
    }
    if (v->type == LVAL_NUM) {
        if (v->num == floor(v->num) && fabs(v->num) < 1e15) {
            snprintf(buf, 32, "%lld", (long long)v->num);
        } else {
            snprintf(buf, 32, "%.14g", v->num);
        }
        return buf;
    }
    return NULL;
}

static bool lval_bool(struct lval *v, bool fallback)
{
    return v && v->type == LVAL_BOOL ? v->b : fallback;
}

/* tableToggleAction: toggle (default), enable/on, disable/off */
static int lval_toggle(struct lval *table, bool current)
{
    struct lval *action = lfield(table, "action");
    const char *s = action && action->type == LVAL_STR ? action->str : "";
    if (!strcmp(s, "enable") || !strcmp(s, "on")) {
        return true;
    }
    if (!strcmp(s, "disable") || !strcmp(s, "off")) {
        return false;
    }
    return !current;
}

/* ------------------------------------------------------------------------- */
/* write commands */

static void cmd_reload(struct hypr_ipc *ipc, struct hypr_flags *f, const char *request,
                       struct sbuf *out)
{
    /* nothing to reload, but listeners expect the event */
    emit_event(ipc, "configreloaded", "%s", "");
    sbuf_puts(out, "ok");
}

static void cmd_reloadshaders(struct hypr_ipc *ipc, struct hypr_flags *f, const char *request,
                              struct sbuf *out)
{
    struct varlist vars;
    if (!varlist_init(&vars, request)) {
        return;
    }
    if (vars.n > 2) {
        sbuf_puts(out, "too many args");
    } else {
        sbuf_puts(out, f->json ? "{\"ok\": true}" : "ok");
    }
    varlist_finish(&vars);
}

/* "kill": the next click SIGKILLs the window under the cursor, Escape cancels */

static void kill_mode_end(struct hypr_ipc *ipc)
{
    if (!ipc->kill.active) {
        return;
    }
    ipc->kill.active = false;
    seat_end_pointer_grab(ipc->kill.seat, &ipc->kill.pointer);
    seat_end_keyboard_grab(ipc->kill.seat, &ipc->kill.keyboard);
    ipc->kill.seat = NULL;
}

static struct view *view_at(struct seat *seat, double lx, double ly)
{
    double sx, sy;
    struct ky_scene_node *node = ky_scene_node_at(&seat->scene->tree.node, lx, ly, &sx, &sy);
    if (!node) {
        return NULL;
    }
    struct input_event_node *inode = input_event_node_from_node(node);
    struct wlr_surface *toplevel = inode ? input_event_node_toplevel(inode) : NULL;
    struct view *view = toplevel ? view_try_from_wlr_surface(toplevel) : NULL;
    return view && view->base.mapped ? view : NULL;
}

static bool kill_pointer_motion(struct seat_pointer_grab *grab, uint32_t time, double lx,
                                double ly)
{
    return false;
}

static bool kill_pointer_button(struct seat_pointer_grab *grab, uint32_t time, uint32_t button,
                                bool pressed)
{
    struct hypr_ipc *ipc = grab->data;
    if (!pressed) {
        return true;
    }
    struct seat *seat = ipc->kill.seat;
    struct view *view = view_at(seat, seat->cursor->lx, seat->cursor->ly);
    kill_mode_end(ipc);
    if (!view) {
        return true;
    }
    reconcile(ipc);
    emit_event(ipc, "kill", "%" PRIx64, (uint64_t)(uintptr_t)view);
    if (view->pid > 0) {
        kill(view->pid, SIGKILL);
    }
    return true;
}

static bool kill_pointer_axis(struct seat_pointer_grab *grab, uint32_t time, bool vertical,
                              double value)
{
    return true;
}

static void kill_pointer_cancel(struct seat_pointer_grab *grab)
{
    struct hypr_ipc *ipc = grab->data;
    /* replaced by another grab, which must not be ended */
    ipc->kill.pointer.seat = NULL;
    kill_mode_end(ipc);
}

static const struct seat_pointer_grab_interface kill_pointer_impl = {
    .motion = kill_pointer_motion,
    .button = kill_pointer_button,
    .axis = kill_pointer_axis,
    .cancel = kill_pointer_cancel,
};

static bool kill_keyboard_key(struct seat_keyboard_grab *grab, struct keyboard *keyboard,
                              uint32_t time, uint32_t key, bool pressed, uint32_t modifiers)
{
    struct hypr_ipc *ipc = grab->data;
    if (key == KEY_ESC) {
        if (pressed) {
            kill_mode_end(ipc);
        }
        return true;
    }
    return false;
}

static void kill_keyboard_cancel(struct seat_keyboard_grab *grab)
{
    struct hypr_ipc *ipc = grab->data;
    kill_mode_end(ipc);
}

static const struct seat_keyboard_grab_interface kill_keyboard_impl = {
    .key = kill_keyboard_key,
    .cancel = kill_keyboard_cancel,
};

static void cmd_kill(struct hypr_ipc *ipc, struct hypr_flags *f, const char *request,
                     struct sbuf *out)
{
    struct seat *seat = input_manager_get_default_seat();
    if (seat && seat->cursor && !ipc->kill.active) {
        ipc->kill.active = true;
        ipc->kill.seat = seat;
        ipc->kill.pointer.interface = &kill_pointer_impl;
        ipc->kill.pointer.data = ipc;
        ipc->kill.keyboard.interface = &kill_keyboard_impl;
        ipc->kill.keyboard.data = ipc;
        seat_start_pointer_grab(seat, &ipc->kill.pointer);
        if (ipc->kill.active) {
            seat_start_keyboard_grab(seat, &ipc->kill.keyboard);
        }
    }
    sbuf_puts(out, "ok");
}

/* notify, dismissnotify and seterror, shown by the notification daemon */

static void notification_shown(uint32_t id, void *data)
{
    struct hypr_ipc *ipc = hypr_ipc;
    if (!ipc || id == 0) {
        return;
    }
    if (ipc->notification_count == HYPR_MAX_NOTIFICATIONS) {
        memmove(ipc->notifications, ipc->notifications + 1,
                (HYPR_MAX_NOTIFICATIONS - 1) * sizeof(ipc->notifications[0]));
        ipc->notification_count--;
    }
    ipc->notifications[ipc->notification_count++] = id;
}

static void error_shown(uint32_t id, void *data)
{
    if (hypr_ipc && id) {
        hypr_ipc->error_notification = id;
    }
}

/* Config::ParserUtils::parseColor: rgb(...), rgba(...), 0xAARRGGBB or a number */
static bool parse_color(const char *s)
{
    size_t len = strlen(s);
    if ((!strncmp(s, "rgb(", 4) || !strncmp(s, "rgba(", 5)) && len > 5 && s[len - 1] == ')') {
        return true;
    }
    if (!strncmp(s, "0x", 2) && len > 2) {
        for (const char *p = s + 2; *p; p++) {
            if (!isxdigit((unsigned char)*p)) {
                return false;
            }
        }
        return true;
    }
    return is_number(s, false);
}

static void cmd_notify(struct hypr_ipc *ipc, struct hypr_flags *f, const char *request,
                       struct sbuf *out)
{
    static const char *icons[] = { "dialog-warning", "dialog-information", "dialog-question",
                                   "dialog-error",   "dialog-question",    "emblem-ok",
                                   "" };
    struct varlist vars;
    if (!varlist_init(&vars, request)) {
        return;
    }
    int icon, time;
    if (vars.n < 5) {
        sbuf_puts(out, "not enough args");
    } else if (!is_number(var(&vars, 1), false) || !parse_stoi(var(&vars, 1), &icon)) {
        sbuf_puts(out, "invalid arg 1");
    } else if (!parse_stoi(var(&vars, 2), &time)) {
        sbuf_puts(out, "invalid arg 2");
    } else if (!parse_color(var(&vars, 3))) {
        sbuf_puts(out, "invalid arg 3");
    } else {
        if (icon < 0 || icon > 6) {
            icon = 6;
        }
        size_t msg = 4;
        const char *fontsize = var(&vars, msg);
        if (strlen(fontsize) > 9 && !strncmp(fontsize, "fontsize:", 9)) {
            if (!is_number(fontsize + 9, true)) {
                sbuf_puts(out, "invalid fontsize kwarg");
                varlist_finish(&vars);
                return;
            }
            msg++;
        }
        if (vars.n <= msg) {
            sbuf_puts(out, "not enough args");
        } else {
            struct sbuf text = { 0 };
            var_join(&vars, msg, &text);
            dbus_notify_full("flakewm", str_or_empty(text.data), "", icons[icon],
                             time > 0 ? time : 0, 1, 0, notification_shown, NULL);
            sbuf_finish(&text);
            sbuf_puts(out, "ok");
        }
    }
    varlist_finish(&vars);
}

static void cmd_dismissnotify(struct hypr_ipc *ipc, struct hypr_flags *f, const char *request,
                              struct sbuf *out)
{
    struct varlist vars;
    if (!varlist_init(&vars, request)) {
        return;
    }
    int amount = -1;
    if (vars.n > 1 && (!is_number(var(&vars, 1), false) || !parse_stoi(var(&vars, 1), &amount))) {
        sbuf_puts(out, "invalid arg 1");
        varlist_finish(&vars);
        return;
    }
    varlist_finish(&vars);

    /* oldest first */
    size_t count = amount < 0 || (size_t)amount > ipc->notification_count ? ipc->notification_count
                                                                          : (size_t)amount;
    for (size_t i = 0; i < count; i++) {
        dbus_close_notification(ipc->notifications[i]);
    }
    memmove(ipc->notifications, ipc->notifications + count,
            (ipc->notification_count - count) * sizeof(ipc->notifications[0]));
    ipc->notification_count -= count;
    sbuf_puts(out, "ok");
}

static void cmd_seterror(struct hypr_ipc *ipc, struct hypr_flags *f, const char *request,
                         struct sbuf *out)
{
    struct varlist vars;
    if (!varlist_init(&vars, request)) {
        return;
    }
    if (vars.n < 3) {
        dbus_close_notification(ipc->error_notification);
        ipc->error_notification = 0;
        sbuf_puts(out, vars.n == 2 && !strstr(var(&vars, 1), "dis") ? "var 1 not color or disable"
                                                                     : "ok");
        varlist_finish(&vars);
        return;
    }
    struct sbuf text = { 0 };
    var_join(&vars, 2, &text);
    varlist_finish(&vars);
    if (!text.data || !*text.data) {
        dbus_close_notification(ipc->error_notification);
        ipc->error_notification = 0;
    } else {
        /* stays until replaced or disabled, like the error bar */
        dbus_notify_full("flakewm", text.data, "", "dialog-error", 0, 2, ipc->error_notification,
                         error_shown, NULL);
    }
    sbuf_finish(&text);
    sbuf_puts(out, "ok");
}

/* switchxkblayout <device|main|active|current|all> <next|prev|index> */
static const char *switch_layout(struct input *input, const char *cmd, char err[64])
{
    struct wlr_keyboard *kb = input_keyboard(input);
    if (!kb || !kb->keymap || !kb->xkb_state) {
        return NULL;
    }
    xkb_layout_index_t layouts = xkb_keymap_num_layouts(kb->keymap);
    xkb_layout_index_t active = 0;
    while (active < layouts &&
           xkb_state_layout_index_is_active(kb->xkb_state, active, XKB_STATE_LAYOUT_EFFECTIVE) != 1) {
        active++;
    }
    struct wlr_keyboard_modifiers *mods = &kb->modifiers;
    uint32_t group;
    if (!strcmp(cmd, "next")) {
        group = active > layouts ? 0 : active + 1;
    } else if (!strcmp(cmd, "prev")) {
        group = active == 0 ? layouts - 1 : active - 1;
    } else {
        int requested;
        if (!parse_stoi(cmd, &requested)) {
            return "invalid arg 2";
        }
        if (requested < 0 || (uint64_t)requested > (uint64_t)layouts - 1) {
            snprintf(err, 64, "layout idx out of range of %u", layouts);
            return err;
        }
        group = (uint32_t)requested;
    }
    wlr_keyboard_notify_modifiers(kb, mods->depressed, mods->latched, mods->locked, group);
    return NULL;
}

static void cmd_switchxkblayout(struct hypr_ipc *ipc, struct hypr_flags *f, const char *request,
                                struct sbuf *out)
{
    struct varlist vars;
    if (!varlist_init(&vars, request)) {
        return;
    }
    const char *target = var(&vars, 1), *cmd = var(&vars, 2);
    struct input_list list = { 0 };
    input_manager_for_each_input(collect_input, &list);
    char err[64];

    struct input *keyboard = NULL;
    if (!strcmp(target, "main") || !strcmp(target, "active") || !strcmp(target, "current")) {
        keyboard = main_keyboard(&list);
    } else if (!strcmp(target, "all")) {
        size_t start = out->len;
        for (size_t i = 0; i < list.count; i++) {
            const char *res = switch_layout(list.items[i], cmd, err);
            if (res) {
                sbuf_printf(out, "%s\n", res);
            }
        }
        if (out->len == start) {
            sbuf_puts(out, "ok");
        }
        varlist_finish(&vars);
        return;
    } else {
        char wanted[256], name[256];
        device_name(target, wanted, sizeof(wanted));
        for (size_t i = 0; i < list.count && !keyboard; i++) {
            device_name(list.items[i]->name, name, sizeof(name));
            if (input_keyboard(list.items[i]) && !strcmp(name, wanted)) {
                keyboard = list.items[i];
            }
        }
        if (!keyboard) {
            sbuf_puts(out, "device not found");
            varlist_finish(&vars);
            return;
        }
    }
    if (!keyboard) {
        sbuf_puts(out, "no device");
    } else {
        const char *res = switch_layout(keyboard, cmd, err);
        sbuf_puts(out, res ? res : "ok");
    }
    varlist_finish(&vars);
}

static void cmd_setcursor(struct hypr_ipc *ipc, struct hypr_flags *f, const char *request,
                          struct sbuf *out)
{
    struct varlist vars;
    if (!varlist_init(&vars, request)) {
        return;
    }
    int size;
    if (!parse_stoi(var(&vars, vars.n - 1), &size)) {
        sbuf_puts(out, "size not int");
    } else if (size <= 0) {
        sbuf_puts(out, "size not positive");
    } else {
        struct sbuf theme = { 0 };
        for (size_t i = 1; i + 1 < vars.n; i++) {
            sbuf_printf(&theme, "%s%s", i > 1 ? " " : "", vars.v[i]);
        }
        sbuf_puts(&theme, "");
        input_set_all_cursor(theme.data, (uint32_t)size);
        sbuf_finish(&theme);
        sbuf_puts(out, "ok");
    }
    varlist_finish(&vars);
}

static void cmd_getoption(struct hypr_ipc *ipc, struct hypr_flags *f, const char *request,
                          struct sbuf *out)
{
    /* flakewm is not configured through Hyprland options */
    sbuf_puts(out, "no such option");
}

static void cmd_getprop(struct hypr_ipc *ipc, struct hypr_flags *f, const char *request,
                        struct sbuf *out)
{
    struct varlist vars;
    if (!varlist_init(&vars, request)) {
        return;
    }
    if (vars.n < 3) {
        sbuf_puts(out, "not enough args");
        varlist_finish(&vars);
        return;
    }
    struct hypr_window *win = window_by_selector(ipc, var(&vars, 1));
    const char *prop = var(&vars, 2);
    if (!win) {
        sbuf_puts(out, "window not found");
        varlist_finish(&vars);
        return;
    }
    struct kywc_view *kv = &win->view->base;
    bool norm = !f->json;
    char a[32], b[32];

    if (!strcmp(prop, "animation")) {
        if (norm) {
            sbuf_puts(out, "(unset)");
        } else {
            sbuf_printf(out, "{\"%s\": \"\"}", prop);
        }
    } else if (!strcmp(prop, "max_size") || !strcmp(prop, "min_size")) {
        bool max = prop[1] == 'a';
        /* MIN_WINDOW_SIZE, and no maximum as infinity */
        double w = max ? (kv->max_width > 0 ? kv->max_width : INFINITY)
                       : (kv->min_width > 0 ? kv->min_width : 20);
        double h = max ? (kv->max_height > 0 ? kv->max_height : INFINITY)
                       : (kv->min_height > 0 ? kv->min_height : 20);
        if (norm) {
            sbuf_printf(out, "%s %s", fmt_double(a, w), fmt_double(b, h));
        } else {
            char xs[32] = "null", ys[32] = "null";
            if (!isinf(w)) {
                snprintf(xs, sizeof(xs), "%f", w); // std::to_string
            }
            if (!isinf(h)) {
                snprintf(ys, sizeof(ys), "%f", h);
            }
            sbuf_printf(out, "{\"%s\": [%s,%s]}", prop, xs, ys);
        }
    } else if (!strncmp(prop, "opacity", 7)) {
        bool override = strstr(prop, "_override") != NULL;
        bool known = !strcmp(prop, "opacity") || !strcmp(prop, "opacity_inactive") ||
                     !strcmp(prop, "opacity_fullscreen") || !strcmp(prop, "opacity_override") ||
                     !strcmp(prop, "opacity_inactive_override") ||
                     !strcmp(prop, "opacity_fullscreen_override");
        if (!known) {
            sbuf_puts(out, "prop not found");
        } else if (norm) {
            sbuf_puts(out, override ? "false" : "1");
        } else {
            sbuf_printf(out, "{\"%s\": %s}", prop, override ? "false" : "1");
        }
    } else if (!strcmp(prop, "active_border_color") || !strcmp(prop, "inactive_border_color")) {
        const char *color = "ffffffff 0deg";
        if (norm) {
            sbuf_puts(out, color);
        } else {
            sbuf_printf(out, "{\"%s\": \"%s\"}", prop, color);
        }
    } else {
        /* window rule properties, flakewm's real state where it has one */
        static const char *false_props[] = {
            "allows_input",     "focus_on_activate", "keep_aspect_ratio", "nearest_neighbor",
            "no_anim",          "no_blur",           "no_dim",            "no_max_size",
            "no_shadow",        "no_glow",           "no_wobble",         "no_shortcuts_inhibit",
            "opaque",           "dim_around",        "force_rgbx",        "sync_fullscreen",
            "xray",             "render_unfocused",  "no_follow_mouse",   "no_screen_share",
            "no_vrr",           "no_auto_hdr",       "persistent_size",   "stay_focused",
        };
        char value[32] = "";
        if (!strcmp(prop, "decorate")) {
            snprintf(value, sizeof(value), "%s", kv->ssd != KYWC_SSD_NONE ? "true" : "false");
        } else if (!strcmp(prop, "no_focus")) {
            snprintf(value, sizeof(value), "%s", kv->focusable ? "false" : "true");
        } else if (!strcmp(prop, "immediate")) {
            snprintf(value, sizeof(value), "%s",
                     window_tearing(ipc, win->view) ? "true" : "false");
        } else if (!strcmp(prop, "idle_inhibit")) {
            snprintf(value, sizeof(value), "%d",
                     idle_inhibit_view_is_inhibiting(win->view) ? 1 : 0);
        } else if (!strcmp(prop, "border_size")) {
            snprintf(value, sizeof(value), "%d", kv->ssd & KYWC_SSD_BORDER ? 1 : 0);
        } else if (!strcmp(prop, "rounding")) {
            snprintf(value, sizeof(value), "0");
        } else if (!strcmp(prop, "rounding_power")) {
            snprintf(value, sizeof(value), "2");
        } else if (!strcmp(prop, "scroll_mouse") || !strcmp(prop, "scroll_touchpad")) {
            snprintf(value, sizeof(value), "1");
        } else {
            for (size_t i = 0; i < sizeof(false_props) / sizeof(false_props[0]); i++) {
                if (!strcmp(prop, false_props[i])) {
                    snprintf(value, sizeof(value), "false");
                }
            }
        }
        if (!*value) {
            sbuf_puts(out, "prop not found");
        } else if (norm) {
            sbuf_puts(out, value);
        } else {
            sbuf_printf(out, "{\"%s\": %s}", prop, value);
        }
    }
    varlist_finish(&vars);
}

static void cmd_decorations(struct hypr_ipc *ipc, struct hypr_flags *f, const char *request,
                            struct sbuf *out)
{
    struct varlist vars;
    if (!varlist_init(&vars, request)) {
        return;
    }
    /* an empty regex matches no window */
    struct hypr_window *win = *var(&vars, 1) ? window_by_selector(ipc, var(&vars, 1)) : NULL;
    varlist_finish(&vars);
    if (!win) {
        sbuf_puts(out, "none");
        return;
    }
    /* flakewm's server side decorations */
    struct {
        const char *name;
        bool present;
    } decos[] = {
        { "Title bar", win->view->base.ssd & KYWC_SSD_TITLE },
        { "Border", win->view->base.ssd & KYWC_SSD_BORDER },
    };
    if (f->json) {
        sbuf_puts(out, "[");
    } else {
        sbuf_puts(out, "Decoration\tPriority\n");
    }
    for (size_t i = 0; i < sizeof(decos) / sizeof(decos[0]); i++) {
        if (!decos[i].present) {
            continue;
        }
        if (f->json) {
            sbuf_printf(out, "{\n\"decorationName\": \"%s\",\n\"priority\": 0\n},", decos[i].name);
        } else {
            sbuf_printf(out, "%s\t0\n", decos[i].name);
        }
    }
    if (f->json) {
        sbuf_trim_comma(out);
        sbuf_puts(out, "]");
    }
}

static struct hypr_monitor *monitor_by_name(struct hypr_ipc *ipc, const char *name)
{
    struct hypr_monitor *mon;
    wl_list_for_each(mon, &ipc->monitors, link) {
        if (!mon->output->destroying && !strcmp(str_or_empty(mon->output->name), name)) {
            return mon;
        }
    }
    return NULL;
}

struct wl_backend_search {
    struct wlr_backend *found;
};

static void find_wl_backend(struct wlr_backend *backend, void *data)
{
    struct wl_backend_search *search = data;
    if (!search->found && wlr_backend_is_wl(backend)) {
        search->found = backend;
    }
}

struct output_namer {
    struct wl_listener new_output;
    const char *name;
};

static void handle_named_output(struct wl_listener *listener, void *data)
{
    struct output_namer *namer = wl_container_of(listener, namer, new_output);
    wlr_output_set_name(data, namer->name);
}

/* the name must be set before flakewm sees the output and creates its global, so the
 * listener goes in front of the multi backend's on the creating backend */
static struct wlr_output *create_named_output(struct wlr_backend *backend, bool headless,
                                              const char *name)
{
    struct output_namer namer = { .new_output.notify = handle_named_output, .name = name };
    if (*name) {
        wl_list_insert(&backend->events.new_output.listener_list, &namer.new_output.link);
    }
    struct wlr_output *output =
        headless ? wlr_headless_add_output(backend, 1920, 1080) : wlr_wl_output_create(backend);
    if (*name) {
        wl_list_remove(&namer.new_output.link);
    }
    return output;
}

/* output <create|add> <headless|wayland|auto> [name] / output <destroy|remove> <name> */
static void cmd_output(struct hypr_ipc *ipc, struct hypr_flags *f, const char *request,
                       struct sbuf *out)
{
    struct varlist vars;
    if (!varlist_init(&vars, request)) {
        return;
    }
    const char *mode = var(&vars, 1), *name = var(&vars, 3);
    if (vars.n < 2) {
        sbuf_puts(out, "not enough args");
    } else if (*name && monitor_by_name(ipc, name)) {
        sbuf_puts(out, "Name already taken");
    } else if (!strcmp(mode, "create") || !strcmp(mode, "add")) {
        const char *backend = var(&vars, 2);
        struct wlr_output *output = NULL;
        /* backends are tried headless first, as Hyprland walks them in reverse */
        if (!strcmp(backend, "headless") || !strcmp(backend, "auto")) {
            output = create_named_output(ipc->server->headless_backend, true, name);
        } else if (!strcmp(backend, "wayland")) {
            struct wl_backend_search search = { 0 };
            if (wlr_backend_is_multi(ipc->server->backend)) {
                wlr_multi_for_each_backend(ipc->server->backend, find_wl_backend, &search);
            }
            output = search.found ? create_named_output(search.found, false, name) : NULL;
        }
        if (!output) {
            sbuf_puts(out, "no backend replied to the request");
        } else {
            if (ipc->user_output_count < sizeof(ipc->user_outputs) / sizeof(ipc->user_outputs[0])) {
                ipc->user_outputs[ipc->user_output_count++] = output;
            }
            sbuf_puts(out, "ok");
        }
    } else if (!strcmp(mode, "destroy") || !strcmp(mode, "remove")) {
        struct hypr_monitor *mon = monitor_by_name(ipc, var(&vars, 2));
        struct wlr_output *output = mon ? monitor_wlr_output(mon) : NULL;
        bool by_user = false;
        for (size_t i = 0; output && i < ipc->user_output_count; i++) {
            by_user |= ipc->user_outputs[i] == output;
        }
        if (!mon) {
            sbuf_puts(out, "output not found");
        } else if (!by_user) {
            sbuf_puts(out, "cannot remove a real display. Use the monitor keyword.");
        } else {
            wlr_output_destroy(output);
            sbuf_puts(out, "ok");
        }
    } else {
        sbuf_puts(out, "ok");
    }
    varlist_finish(&vars);
}

static void cmd_plugin(struct hypr_ipc *ipc, struct hypr_flags *f, const char *request,
                       struct sbuf *out)
{
    struct varlist vars;
    if (!varlist_init(&vars, request)) {
        return;
    }
    const char *op = var(&vars, 1);
    if (vars.n < 2) {
        sbuf_puts(out, "not enough args");
    } else if (!strcmp(op, "load")) {
        sbuf_puts(out, vars.n < 3 ? "not enough args" : "flakewm cannot load Hyprland plugins");
    } else if (!strcmp(op, "unload")) {
        sbuf_puts(out, vars.n < 3 ? "not enough args" : "plugin not loaded");
    } else if (!strcmp(op, "list")) {
        sbuf_puts(out, f->json ? "[]" : "no plugins loaded");
    } else {
        sbuf_puts(out, "unknown opt");
    }
    varlist_finish(&vars);
}

/* ------------------------------------------------------------------------- */
/* actions shared by hl.dsp.* and the legacy dispatchers */

static void window_set_floating(struct view *view, int floating)
{
    struct kywc_view *kv = &view->base;
    bool is_floating = kv->tiled == KYWC_TILE_NONE;
    if (floating < 0) {
        floating = !is_floating;
    }
    if (floating && !is_floating) {
        kywc_view_set_tiled(kv, KYWC_TILE_NONE, NULL);
    } else if (!floating && is_floating) {
        /* flakewm does not tile, a tiled window fills its output */
        kywc_view_set_tiled(kv, KYWC_TILE_ALL, view->output);
    }
}

static void window_set_pinned(struct view *view, int pinned)
{
    if (pinned < 0) {
        pinned = !view->base.sticky;
    }
    if (pinned && !view->base.sticky) {
        view_add_all_workspace(view);
    } else if (!pinned && view->base.sticky) {
        struct workspace *ws = workspace_manager_get_current();
        if (ws) {
            view_set_workspace(view, ws);
        }
    }
}

/* mode 0 none, 1 maximized, 2 fullscreen; set < 0 toggles */
static void window_set_fullscreen(struct view *view, int mode, int set)
{
    struct kywc_view *kv = &view->base;
    bool current = mode == 1 ? kv->maximized : kv->fullscreen;
    if (set < 0) {
        set = !current;
    }
    if (mode == 1) {
        kywc_view_set_maximized(kv, set, NULL);
    } else {
        kywc_view_set_fullscreen(kv, set, NULL);
    }
}

static const char *window_signal(struct view *view, int signal)
{
    if (signal < 1 || signal > 31) {
        return "Invalid signal";
    }
    if (view->pid <= 0) {
        return "window has no pid";
    }
    kill(view->pid, signal);
    return "ok";
}

static void window_force_kill(struct hypr_ipc *ipc, struct view *view)
{
    emit_event(ipc, "kill", "%" PRIx64, (uint64_t)(uintptr_t)view);
    if (view->pid > 0) {
        kill(view->pid, SIGKILL);
    }
}

static void window_move_to(struct hypr_ipc *ipc, struct view *view, struct workspace *ws,
                           bool follow)
{
    view_set_workspace(view, ws);
    if (follow) {
        if (ws != workspace_manager_get_current()) {
            workspace_activate_with_effect(ws);
        }
        focus_view(view);
    }
}

static void move_cursor(double x, double y)
{
    struct seat *seat = input_manager_get_default_seat();
    if (seat && seat->cursor) {
        cursor_move(seat->cursor, NULL, x, y, false, false);
        cursor_rebase(seat->cursor);
    }
}

/* the most recent urgent window, else the previously focused one */
static struct hypr_window *urgent_or_last(struct hypr_ipc *ipc)
{
    struct hypr_window *win, *active = active_window(ipc), *last = NULL;
    wl_list_for_each(win, &ipc->mru, mru_link) {
        if (!window_eligible(win) || win == active) {
            continue;
        }
        if (win->view->base.demands_attention) {
            return win;
        }
        if (!last) {
            last = win;
        }
    }
    return last;
}

/* ------------------------------------------------------------------------- */
/* hl.dsp.* */

static struct hypr_window *lua_window(struct hypr_ipc *ipc, struct lval *opts)
{
    char buf[32];
    struct lval *sel = lfield(opts, "window");
    return window_by_selector(ipc, sel ? lval_str(sel, buf) : NULL);
}

/* the 'a' of "hl.dsp.window.close(a)": nil, a selector string or an option table */
static struct lval *lua_opts(struct lval *args, struct lval *storage, struct lfield *field)
{
    static char key[] = "window";
    struct lval *first = larg(args, 0);
    if (first && first->type == LVAL_STR) {
        /* hl.dsp.window.close("class:foo") is shorthand for { window = "class:foo" },
         * storage borrows the string from args and is not finished */
        *field = (struct lfield){ key, *first };
        *storage = (struct lval){ .type = LVAL_TABLE, .fields = field, .nfields = 1 };
        return storage;
    }
    return first;
}

static const char *lua_window_dispatch(struct hypr_ipc *ipc, const char *name, struct lval *opts,
                                       char err[128])
{
    struct hypr_window *win = lua_window(ipc, opts);
    if (!win) {
        return "No such window found";
    }
    struct view *view = win->view;
    struct kywc_view *kv = &view->base;
    char buf[32];

    if (!strcmp(name, "close")) {
        kywc_view_close(kv);
    } else if (!strcmp(name, "kill")) {
        window_force_kill(ipc, view);
    } else if (!strcmp(name, "signal")) {
        int sig = 0;
        lnum_int(lfield(opts, "signal"), INT_MIN, INT_MAX, &sig);
        return window_signal(view, sig);
    } else if (!strcmp(name, "float")) {
        int set = lval_toggle(opts, kv->tiled == KYWC_TILE_NONE);
        window_set_floating(view, set);
    } else if (!strcmp(name, "fullscreen") || !strcmp(name, "fullscreen_state")) {
        struct lval *mode = lfield(opts, "mode");
        const char *m = mode ? lval_str(mode, buf) : "fullscreen";
        int which = m && (!strcmp(m, "maximized") || !strcmp(m, "1")) ? 1
                    : m && !strcmp(m, "0")                           ? 0
                                                                     : 2;
        if (which == 0) {
            kywc_view_set_fullscreen(kv, false, NULL);
            kywc_view_set_maximized(kv, false, NULL);
        } else {
            struct lval *action = lfield(opts, "action");
            const char *a = action && action->type == LVAL_STR ? action->str : "toggle";
            int set = !strcmp(a, "set") ? 1 : !strcmp(a, "unset") ? 0 : -1;
            window_set_fullscreen(view, which, set);
        }
    } else if (!strcmp(name, "pin")) {
        window_set_pinned(view, lval_toggle(opts, kv->sticky));
    } else if (!strcmp(name, "center")) {
        view_move_to_center(view);
    } else if (!strcmp(name, "bring_to_top")) {
        view_raise_to_top(view, false);
    } else if (!strcmp(name, "alter_zorder")) {
        struct lval *mode = lfield(opts, "mode");
        const char *m = mode ? lval_str(mode, buf) : "top";
        if (m && !strcmp(m, "bottom")) {
            kywc_view_set_kept_below(kv, true);
            kywc_view_set_kept_below(kv, false);
        } else {
            view_raise_to_top(view, false);
        }
    } else if (!strcmp(name, "move")) {
        struct lval *ws_sel = lfield(opts, "workspace");
        struct lval *x = lfield(opts, "x"), *y = lfield(opts, "y");
        int ix, iy;
        if (ws_sel) {
            struct workspace *ws = workspace_by_selector(ipc, lval_str(ws_sel, buf));
            if (!ws) {
                return "Invalid workspace";
            }
            window_move_to(ipc, view, ws, lval_bool(lfield(opts, "follow"), true));
        } else if (lnum_int(x, -HYPR_LUA_COORD_MAX, HYPR_LUA_COORD_MAX, &ix) &&
                   lnum_int(y, -HYPR_LUA_COORD_MAX, HYPR_LUA_COORD_MAX, &iy)) {
            bool relative = lval_bool(lfield(opts, "relative"), false);
            int nx = ix + (relative ? kv->geometry.x : 0);
            int ny = iy + (relative ? kv->geometry.y : 0);
            kywc_view_move(kv, nx, ny);
        } else {
            return "flakewm does not tile, cannot move in a direction";
        }
    } else if (!strcmp(name, "resize")) {
        int ix, iy;
        if (!lnum_int(lfield(opts, "x"), -HYPR_LUA_COORD_MAX, HYPR_LUA_COORD_MAX, &ix) ||
            !lnum_int(lfield(opts, "y"), -HYPR_LUA_COORD_MAX, HYPR_LUA_COORD_MAX, &iy)) {
            return "resize needs x and y";
        }
        bool relative = lval_bool(lfield(opts, "relative"), false);
        struct kywc_box box = kv->geometry;
        box.width = ix + (relative ? box.width : 0);
        box.height = iy + (relative ? box.height : 0);
        if (box.width <= 0 || box.height <= 0) {
            return "Invalid size";
        }
        kywc_view_resize(kv, &box);
    } else if (!strcmp(name, "cycle_next")) {
        /* the next window after the active one, oldest first */
        struct hypr_window *it = win, *next = NULL;
        wl_list_for_each_reverse(it, &ipc->windows, link) {
            if (it != win && window_eligible(it) && !next) {
                next = it;
            }
        }
        if (next) {
            focus_view(next->view);
        }
    } else if (!strcmp(name, "pseudo") || !strcmp(name, "swap") || !strcmp(name, "tag") ||
               !strcmp(name, "clear_tags") || !strcmp(name, "toggle_swallow") ||
               !strcmp(name, "set_prop") || !strcmp(name, "deny_from_group") ||
               !strcmp(name, "drag")) {
        snprintf(err, 128, "hl.dsp.window.%s is not supported by flakewm", name);
        return err;
    } else {
        snprintf(err, 128, "attempt to call a nil value (field '%s')", name);
        return err;
    }
    return "ok";
}

static const char *lua_focus(struct hypr_ipc *ipc, struct lval *opts, char err[128])
{
    char buf[32];
    struct lval *v;
    if ((v = lfield(opts, "workspace"))) {
        struct workspace *ws = workspace_by_selector(ipc, lval_str(v, buf));
        if (!ws) {
            return "Invalid workspace";
        }
        if (ws != workspace_manager_get_current()) {
            workspace_activate_with_effect(ws);
        }
        return "ok";
    }
    if ((v = lfield(opts, "window"))) {
        struct hypr_window *win = window_by_selector(ipc, lval_str(v, buf));
        if (!win) {
            return "No such window found";
        }
        focus_view(win->view);
        return "ok";
    }
    if (lfield(opts, "urgent_or_last")) {
        struct hypr_window *win = urgent_or_last(ipc);
        if (!win) {
            return "No such window found";
        }
        focus_view(win->view);
        return "ok";
    }
    if ((v = lfield(opts, "monitor"))) {
        const char *sel = lval_str(v, buf);
        struct hypr_monitor *mon = NULL;
        if (sel && is_number(sel, false)) {
            mon = monitor_by_id(ipc, atoi(sel));
        } else if (sel) {
            mon = monitor_by_name(ipc, sel);
        }
        if (!mon || !monitor_enabled(mon)) {
            return "Invalid monitor";
        }
        /* focus follows the cursor, so move it to the middle of the monitor */
        struct kywc_output_state *st = &mon->output->state;
        move_cursor(st->lx + st->width / 2.0, st->ly + st->height / 2.0);
        return "ok";
    }
    if (lfield(opts, "direction")) {
        return "flakewm does not tile, cannot focus in a direction";
    }
    snprintf(err, 128, "focus: no target given");
    return err;
}

static const char *lua_dispatch(struct hypr_ipc *ipc, const char *path, struct lval *args,
                                char err[128])
{
    char buf[32];
    struct lval storage;
    struct lfield field;
    struct lval *opts = lua_opts(args, &storage, &field);
    const char *s0 = lval_str(larg(args, 0), buf);

    if (!strncmp(path, "window.", 7)) {
        return lua_window_dispatch(ipc, path + 7, opts, err);
    }
    if (!strncmp(path, "group.", 6)) {
        return "flakewm has no window groups";
    }
    if (!strncmp(path, "workspace.", 10)) {
        snprintf(err, 128, "hl.dsp.%s is not supported by flakewm", path);
        return err;
    }
    if (!strcmp(path, "cursor.move")) {
        struct lval *x = lfield(opts, "x"), *y = lfield(opts, "y");
        if (!x || !y || x->type != LVAL_NUM || y->type != LVAL_NUM || !isfinite(x->num) ||
            !isfinite(y->num)) {
            return "cursor.move needs x and y";
        }
        move_cursor(x->num, y->num);
        return "ok";
    }
    if (!strcmp(path, "cursor.move_to_corner")) {
        struct hypr_window *win = lua_window(ipc, opts);
        struct lval *corner = lfield(opts, "corner");
        if (!win) {
            return "No such window found";
        }
        /* 0 bottom left, 1 bottom right, 2 top right, 3 top left */
        int c = 0;
        lnum_int(corner, INT_MIN, INT_MAX, &c);
        struct kywc_box *g = &win->view->base.geometry;
        move_cursor(g->x + (c == 1 || c == 2 ? g->width - 1 : 0),
                    g->y + (c == 0 || c == 1 ? g->height - 1 : 0));
        return "ok";
    }
    if (!strcmp(path, "exec_cmd") || !strcmp(path, "exec_raw")) {
        if (!s0 || !*s0) {
            return "exec: missing command";
        }
        spawn_shell(s0);
        return "ok";
    }
    if (!strcmp(path, "exit")) {
        wl_display_terminate(ipc->server->display);
        return "ok";
    }
    if (!strcmp(path, "reload_config")) {
        emit_event(ipc, "configreloaded", "%s", "");
        return "ok";
    }
    if (!strcmp(path, "event")) {
        emit_event(ipc, "custom", "%s", str_or_empty(s0));
        return "ok";
    }
    if (!strcmp(path, "global")) {
        if (!s0 || !hyprland_global_shortcut_trigger(s0, false)) {
            return "global shortcut not found";
        }
        return "ok";
    }
    if (!strcmp(path, "submap")) {
        return s0 && (!strcmp(s0, "reset") || !strcmp(s0, "default")) ? "ok"
                                                                       : "flakewm has no submaps";
    }
    if (!strcmp(path, "focus")) {
        return lua_focus(ipc, opts, err);
    }
    if (!strcmp(path, "dpms")) {
        struct lval *mon_sel = lfield(opts, "monitor");
        const char *name = mon_sel ? lval_str(mon_sel, buf) : NULL;
        struct hypr_monitor *mon;
        wl_list_for_each(mon, &ipc->monitors, link) {
            if (!monitor_enabled(mon) || (name && strcmp(mon->output->name, name))) {
                continue;
            }
            struct kywc_output_state state = mon->output->state;
            state.power = lval_toggle(opts, state.power);
            kywc_output_set_state(mon->output, &state);
        }
        return "ok";
    }
    if (!strcmp(path, "no_op") || !strcmp(path, "force_renderer_reload") ||
        !strcmp(path, "force_idle") || !strcmp(path, "release_input_capture")) {
        return "ok";
    }
    if (!strcmp(path, "pass") || !strcmp(path, "send_shortcut") ||
        !strcmp(path, "send_key_state") || !strcmp(path, "layout")) {
        snprintf(err, 128, "hl.dsp.%s is not supported by flakewm", path);
        return err;
    }
    snprintf(err, 128, "attempt to call a nil value (field '%s')", path);
    return err;
}

/* one "hl.dispatch(hl.dsp.x(...))" or "hl.dsp.x(...)" expression */
struct lcall {
    char *path;
    struct lval args;
};

static void lcall_finish(struct lcall *call)
{
    free(call->path);
    lval_finish(&call->args);
    *call = (struct lcall){ 0 };
}

static bool lua_parse_dispatch(struct lparser *lp, struct lcall *call)
{
    *call = (struct lcall){ .args = { .type = LVAL_TABLE } };
    lp_skip(lp);
    bool wrapped = !strncmp(lp->p, "hl.dispatch", 11);
    if (wrapped) {
        lp->p += 11;
        if (!lp_accept(lp, '(')) {
            return lp_fail(lp, "'(' expected");
        }
    }
    if (!lp_call(lp, "hl.dsp", &call->path, &call->args) || (wrapped && !lp_accept(lp, ')'))) {
        lcall_finish(call);
        return lp_fail(lp, "')' expected");
    }
    return true;
}

/* a chunk of such statements, "return" and ';' allowed, all parsed before any runs */
static bool lua_parse_chunk(struct lparser *lp, struct lcall *calls, size_t max, size_t *count)
{
    *count = 0;
    while (true) {
        lp_skip(lp);
        while (lp_accept(lp, ';')) {
        }
        if (!*lp->p) {
            return true;
        }
        if (!strncmp(lp->p, "return", 6) && !isalnum((unsigned char)lp->p[6]) &&
            lp->p[6] != '_') {
            lp->p += 6;
        }
        if (*count == max) {
            return lp_fail(lp, "too many statements");
        }
        if (!lua_parse_dispatch(lp, &calls[*count])) {
            return false;
        }
        (*count)++;
    }
}

/* runs the chunk, the first result that is not "ok" ends it */
static void lua_run(struct hypr_ipc *ipc, const char *code, bool single, struct sbuf *out)
{
    struct lparser lp = { .p = code };
    struct lcall calls[16];
    size_t count = 0;
    char err[128];
    bool parsed = lua_parse_chunk(&lp, calls, single ? 1 : 16, &count);
    if (parsed && count == 0) {
        parsed = lp_fail(&lp, "unexpected symbol");
    }
    if (!parsed) {
        sbuf_printf(out, "[string \"%.40s\"]:1: %s", code, lp.err);
    } else {
        const char *res = "ok";
        for (size_t i = 0; i < count && !strcmp(res, "ok"); i++) {
            res = lua_dispatch(ipc, calls[i].path, &calls[i].args, err);
        }
        sbuf_puts(out, res);
    }
    for (size_t i = 0; i < count; i++) {
        lcall_finish(&calls[i]);
    }
}

/* ------------------------------------------------------------------------- */
/* legacy dispatchers ("dispatch workspace 2"), kept as an extension */

static const char *dispatch(struct hypr_ipc *ipc, const char *name, const char *arg)
{
    if (!strcmp(name, "exec") || !strcmp(name, "execr")) {
        /* skip exec rules: "exec [rules] command" */
        if (*arg == '[') {
            const char *close = strchr(arg, ']');
            arg = close ? close + 1 : arg;
            while (*arg == ' ') {
                arg++;
            }
        }
        if (!*arg) {
            return "exec: missing command";
        }
        spawn_shell(arg);
        return "ok";
    }

    if (!strcmp(name, "workspace")) {
        struct workspace *ws = workspace_by_selector(ipc, arg);
        if (!ws) {
            return "Invalid workspace";
        }
        if (ws != workspace_manager_get_current()) {
            workspace_activate_with_effect(ws);
        }
        return "ok";
    }

    if (!strcmp(name, "exit")) {
        wl_display_terminate(ipc->server->display);
        return "ok";
    }

    if (!strcmp(name, "event")) {
        emit_event(ipc, "custom", "%s", arg);
        return "ok";
    }

    if (!strcmp(name, "global")) {
        return hyprland_global_shortcut_trigger(arg, false) ? "ok" : "global shortcut not found";
    }

    if (!strcmp(name, "focusurgentorlast")) {
        struct hypr_window *win = urgent_or_last(ipc);
        if (!win) {
            return "No such window found";
        }
        focus_view(win->view);
        return "ok";
    }

    /* the rest act on a window, the active one by default */
    bool active_only = !strcmp(name, "killactive") || !strcmp(name, "forcekillactive") ||
                       !strcmp(name, "fullscreen") || !strcmp(name, "bringactivetotop");
    const char *win_sel = arg;
    char *ws_sel = NULL;
    if (!strcmp(name, "movetoworkspace") || !strcmp(name, "movetoworkspacesilent") ||
        !strcmp(name, "signalwindow")) {
        /* "first,window" */
        ws_sel = strdup(arg);
        if (!ws_sel) {
            return "Out of memory";
        }
        char *comma = strchr(ws_sel, ',');
        win_sel = NULL;
        if (comma) {
            *comma = '\0';
            win_sel = arg + (comma - ws_sel) + 1;
        }
    } else if (!strcmp(name, "togglefloating") || !strcmp(name, "setfloating") ||
               !strcmp(name, "settiled") || !strcmp(name, "pin") ||
               !strcmp(name, "centerwindow") || !strcmp(name, "signal")) {
        /* centerwindow takes "1", signal its number, these act on the active window */
        if (!strcmp(name, "centerwindow") || !strcmp(name, "signal")) {
            win_sel = NULL;
        }
    } else if (active_only) {
        win_sel = NULL;
    } else if (strcmp(name, "focuswindow") && strcmp(name, "closewindow") &&
               strcmp(name, "killwindow")) {
        return "Invalid dispatcher";
    }

    struct hypr_window *win = window_by_selector(ipc, win_sel);
    const char *res = "ok";
    if (!win) {
        res = "No such window found";
    } else if (!strcmp(name, "focuswindow")) {
        focus_view(win->view);
    } else if (!strcmp(name, "closewindow") || !strcmp(name, "killactive")) {
        kywc_view_close(&win->view->base);
    } else if (!strcmp(name, "killwindow") || !strcmp(name, "forcekillactive")) {
        window_force_kill(ipc, win->view);
    } else if (!strcmp(name, "signal")) {
        res = window_signal(win->view, atoi(arg));
    } else if (!strcmp(name, "signalwindow")) {
        res = window_signal(win->view, atoi(ws_sel));
    } else if (!strcmp(name, "fullscreen")) {
        window_set_fullscreen(win->view, atoi(arg) == 1 ? 1 : 2, -1);
    } else if (!strcmp(name, "togglefloating")) {
        window_set_floating(win->view, -1);
    } else if (!strcmp(name, "setfloating") || !strcmp(name, "settiled")) {
        window_set_floating(win->view, name[3] == 'f');
    } else if (!strcmp(name, "pin")) {
        window_set_pinned(win->view, -1);
    } else if (!strcmp(name, "centerwindow")) {
        view_move_to_center(win->view);
    } else if (!strcmp(name, "bringactivetotop")) {
        view_raise_to_top(win->view, false);
    } else {
        struct workspace *ws = workspace_by_selector(ipc, ws_sel);
        if (!ws) {
            res = "Invalid workspace";
        } else {
            window_move_to(ipc, win->view, ws, !strcmp(name, "movetoworkspace"));
        }
    }
    free(ws_sel);
    return res;
}

static void cmd_dispatch(struct hypr_ipc *ipc, struct hypr_flags *f, const char *request,
                         struct sbuf *out)
{
    /* Hyprland 0.57 runs "return hl.dispatch(<in>)" */
    const char *space = strchr(request, ' ');
    const char *in = space ? space + 1 : request;

    const char *p = in;
    while (isspace((unsigned char)*p)) {
        p++;
    }
    size_t start = out->len;
    if (!strncmp(p, "hl.", 3)) {
        lua_run(ipc, p, true, out);
    } else {
        /* the pre 0.57 form, "dispatch <name> <arg>" */
        char name[64] = "";
        const char *arg_space = strchr(p, ' ');
        size_t len = arg_space ? (size_t)(arg_space - p) : strlen(p);
        if (len < sizeof(name)) {
            memcpy(name, p, len);
            name[len] = '\0';
        }
        const char *arg = arg_space ? arg_space + 1 : "";
        while (*arg == ' ') {
            arg++;
        }
        sbuf_puts(out, *name ? dispatch(ipc, name, arg) : "Invalid dispatcher");
    }

    const char *res = out->data ? out->data + start : "";
    if (strncmp(res, "ok", 2) && !strchr(in, '(')) {
        sbuf_puts(out, "\n\n → Note: dispatch in lua is a shorthand for hl.dispatch(...), your "
                       "syntax might need to be updated.");
    }
}

/* eval/repl: statements of hl.dispatch(hl.dsp.*(...)), the only Lua flakewm speaks */
static void cmd_eval(struct hypr_ipc *ipc, struct hypr_flags *f, const char *request,
                     struct sbuf *out)
{
    const char *space = strchr(request, ' ');
    lua_run(ipc, space ? space + 1 : "", false, out);
}

/* ------------------------------------------------------------------------- */
/* request socket */

/* registration order of CHyprCtl, exact commands are tried before prefixes */
static const struct {
    const char *name;
    bool exact;
    hypr_cmd_func func;
} hypr_commands[] = {
    { "workspaces", true, cmd_workspaces },
    { "workspacerules", true, cmd_workspacerules },
    { "activeworkspace", true, cmd_activeworkspace },
    { "clients", true, cmd_clients },
    { "kill", true, cmd_kill },
    { "activewindow", true, cmd_activewindow },
    { "layers", true, cmd_layers },
    { "version", true, cmd_version },
    { "devices", true, cmd_devices },
    { "splash", true, cmd_splash },
    { "cursorpos", true, cmd_cursorpos },
    { "binds", true, cmd_binds },
    { "globalshortcuts", true, cmd_globalshortcuts },
    { "systeminfo", true, cmd_systeminfo },
    { "animations", true, cmd_animations },
    { "rollinglog", true, cmd_rollinglog },
    { "configerrors", true, cmd_configerrors },
    { "locked", true, cmd_locked },
    { "descriptions", true, cmd_descriptions },
    { "submap", true, cmd_submap },
    { "status", true, cmd_status },
    { "deprecated-config", true, cmd_deprecated_config },

    { "reloadshaders", false, cmd_reloadshaders },
    { "monitors", false, cmd_monitors },
    { "reload", false, cmd_reload },
    { "plugin", false, cmd_plugin },
    { "notify", false, cmd_notify },
    { "dismissnotify", false, cmd_dismissnotify },
    { "getprop", false, cmd_getprop },
    { "seterror", false, cmd_seterror },
    { "switchxkblayout", false, cmd_switchxkblayout },
    { "output", false, cmd_output },
    { "dispatch", false, cmd_dispatch },
    { "setcursor", false, cmd_setcursor },
    { "getoption", false, cmd_getoption },
    { "decorations", false, cmd_decorations },
    { "eval", false, cmd_eval },
    { "repl", false, cmd_eval },
};

/* parseRequest: "[flags]/command", a space before any '/' means there are no flags */
static const char *parse_request(const char *request, struct hypr_flags *flags)
{
    if (!strchr(request, '/')) {
        return request;
    }
    size_t len = strlen(request), sep = 0;
    struct hypr_flags parsed = { 0 };
    for (const char *c = request; *c; c++) {
        if (*c == '/') {
            break;
        }
        if (*c == ' ') {
            sep = len;
            break;
        }
        sep++;
        switch (*c) {
        case 'j':
            parsed.json = true;
            break;
        case 'r':
            parsed.refresh = true;
            break;
        case 'a':
            parsed.all = true;
            break;
        case 'c':
            parsed.config = true;
            break;
        case 'f':
            parsed.follow = true;
            break;
        }
    }
    *flags = parsed;
    return sep < len ? request + sep + 1 : request;
}

/* getReply: one request, appended to out */
static void handle_one_request(struct hypr_ipc *ipc, const char *request, struct hypr_flags *flags,
                               struct sbuf *out)
{
    request = parse_request(request, flags);
    /* replies must agree with the event stream, also within a batch */
    reconcile(ipc);

    struct sbuf reply = { 0 };
    size_t count = sizeof(hypr_commands) / sizeof(hypr_commands[0]);
    for (int pass = 0; pass < 2 && !reply.len; pass++) {
        for (size_t i = 0; i < count; i++) {
            const char *name = hypr_commands[i].name;
            if (hypr_commands[i].exact != (pass == 0)) {
                continue;
            }
            bool match = pass == 0 ? !strcmp(request, name)
                                   : !strncmp(request, name, strlen(name));
            if (match) {
                hypr_commands[i].func(ipc, flags, request, &reply);
                break;
            }
        }
    }

    if (!reply.len) {
        sbuf_puts(out, reply.oom ? "internal error" : "unknown request");
    } else {
        sbuf_append(out, reply.data, reply.len);
        if (flags->refresh) {
            emit_event(ipc, "configreloaded", "%s", "");
        }
    }
    sbuf_finish(&reply);
}

/* returns true when the connection stays open for f/rollinglog */
static bool handle_request(struct hypr_ipc *ipc, const char *request, struct sbuf *out)
{
    static const char batch_prefix[] = "[[BATCH]]";
    struct hypr_flags flags = { 0 };
    if (strncmp(request, batch_prefix, sizeof(batch_prefix) - 1)) {
        handle_one_request(ipc, request, &flags, out);
        return flags.follow && !strcmp(parse_request(request, &flags), "rollinglog");
    }

    /* split on ';', "\;" and "\\" are escapes, every other backslash is kept */
    struct sbuf item = { 0 }, reply = { 0 };
    bool first = true, follow = false;
    for (const char *p = request + sizeof(batch_prefix) - 1;; p++) {
        if (*p == '\\' && (p[1] == ';' || p[1] == '\\')) {
            sbuf_append(&item, ++p, 1);
            continue;
        }
        if (*p != ';' && *p != '\0') {
            sbuf_append(&item, p, 1);
            continue;
        }
        sbuf_puts(&item, "");
        char *trimmed = item.data ? trim_in_place(item.data) : "";
        if (*trimmed || *p == ';') {
            if (!first) {
                sbuf_puts(&reply, "\n\n\n");
            }
            first = false;
            struct hypr_flags item_flags = { 0 };
            handle_one_request(ipc, trimmed, &item_flags, &reply);
            follow |= item_flags.follow;
        }
        item.len = 0;
        if (*p == '\0') {
            break;
        }
    }
    sbuf_finish(&item);

    if (follow) {
        sbuf_puts(out, "follow mode is unavailable in batch requests");
    } else if (reply.len) {
        sbuf_append(out, reply.data, reply.len);
    }
    sbuf_finish(&reply);
    return false;
}

static void log_followers_update(struct hypr_ipc *ipc);

static void request_destroy(struct hypr_request *req)
{
    wl_list_remove(&req->link);
    if (req->follow) {
        log_followers_update(req->ipc);
    } else {
        req->ipc->request_count--;
    }
    wl_event_source_remove(req->source);
    if (req->timer) {
        wl_event_source_remove(req->timer);
    }
    close(req->fd);
    sbuf_finish(&req->in);
    sbuf_finish(&req->out);
    free(req);
}

static void request_write(struct hypr_request *req)
{
    while (req->out.len > 0) {
        ssize_t n = send(req->fd, req->out.data, req->out.len, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n > 0) {
            sbuf_consume(&req->out, n);
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            wl_event_source_fd_update(req->source, WL_EVENT_WRITABLE);
            return;
        }
        request_destroy(req);
        return;
    }
    if (req->follow) {
        /* drained, wait for more log lines or the client to hang up */
        wl_event_source_fd_update(req->source, WL_EVENT_READABLE);
        return;
    }
    request_destroy(req);
}

/* f/rollinglog: every new log line goes to the followers */
static void handle_log_line(const char *line, void *data)
{
    struct hypr_ipc *ipc = data;
    struct hypr_request *req, *tmp;
    wl_list_for_each_safe(req, tmp, &ipc->log_followers, link) {
        if (req->out.len > HYPR_MAX_EVENT_BACKLOG) {
            request_destroy(req);
            continue;
        }
        sbuf_puts(&req->out, line);
        sbuf_puts(&req->out, "\n");
        if (req->out.oom) {
            request_destroy(req);
            continue;
        }
        request_write(req);
    }
}

static void log_followers_update(struct hypr_ipc *ipc)
{
    if (wl_list_empty(&ipc->log_followers)) {
        logger_set_tap(NULL, NULL);
    } else {
        logger_set_tap(handle_log_line, ipc);
    }
}

static void request_follow(struct hypr_request *req)
{
    struct hypr_ipc *ipc = req->ipc;
    if (req->timer) {
        wl_event_source_remove(req->timer);
        req->timer = NULL;
    }
    /* followers do not count against the clients being served */
    wl_list_remove(&req->link);
    ipc->request_count--;
    wl_list_insert(&ipc->log_followers, &req->link);
    req->follow = true;
    log_followers_update(ipc);
}

static void request_process(struct hypr_request *req)
{
    req->replying = true;
    sbuf_puts(&req->in, "");
    bool follow = req->in.data && handle_request(req->ipc, req->in.data, &req->out);
    if (req->out.oom) {
        request_destroy(req);
        return;
    }
    /* past the limit the client gets the snapshot only, like a plain rollinglog */
    if (follow && wl_list_length(&req->ipc->log_followers) < HYPR_MAX_LOG_FOLLOWERS) {
        request_follow(req);
    }
    request_write(req);
}

static int handle_request_fd(int fd, uint32_t mask, void *data)
{
    struct hypr_request *req = data;

    if (req->replying) {
        if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
            request_destroy(req);
        } else if (mask & WL_EVENT_WRITABLE) {
            request_write(req);
        } else if (req->follow && (mask & WL_EVENT_READABLE)) {
            /* a follower only ever sends its request, anything more is drained */
            char buf[256];
            ssize_t n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
            if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                request_destroy(req);
            }
        }
        return 0;
    }

    bool eof = false;
    while (true) {
        char buf[4096];
        ssize_t n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (n > 0) {
            if (req->in.len + n > HYPR_MAX_REQUEST) {
                request_destroy(req);
                return 0;
            }
            sbuf_append(&req->in, buf, n);
            continue;
        }
        if (n == 0) {
            eof = true;
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        request_destroy(req);
        return 0;
    }

    /* the protocol has no terminator: the request is what one write delivered */
    if (req->in.len > 0) {
        request_process(req);
    } else if (eof || (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR))) {
        request_destroy(req);
    }
    return 0;
}

static int handle_request_timeout(void *data)
{
    struct hypr_request *req = data;
    /* request_destroy removes the timer, which is fine from its own callback */
    request_destroy(req);
    return 0;
}

static int handle_request_accept(int fd, uint32_t mask, void *data)
{
    struct hypr_ipc *ipc = data;

    int client = accept4(fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (client < 0) {
        return 0;
    }
    if (ipc->request_count >= HYPR_MAX_REQUEST_CLIENTS) {
        close(client);
        return 0;
    }

    struct hypr_request *req = calloc(1, sizeof(*req));
    if (!req) {
        close(client);
        return 0;
    }
    req->ipc = ipc;
    req->fd = client;
    req->source = wl_event_loop_add_fd(ipc->server->event_loop, client, WL_EVENT_READABLE,
                                       handle_request_fd, req);
    if (!req->source) {
        close(client);
        free(req);
        return 0;
    }
    req->timer = wl_event_loop_add_timer(ipc->server->event_loop, handle_request_timeout, req);
    if (req->timer) {
        wl_event_source_timer_update(req->timer, HYPR_REQUEST_TIMEOUT_MS);
    }
    wl_list_insert(&ipc->requests, &req->link);
    ipc->request_count++;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* hooks for state flakewm has no signal for (view/hyprland_ipc.h) */

void hyprland_ipc_screencast(bool active, enum hyprland_ipc_share_type type, const char *name)
{
    struct hypr_ipc *ipc = hypr_ipc;
    if (!ipc || ipc->terminating) {
        return;
    }
    /* screencast's owner is 0 for a monitor and 1 for a window */
    emit_event(ipc, "screencast", "%d,%d", active, type == HYPRLAND_IPC_SHARE_WINDOW);
    char *line = sanitize_line(name);
    emit_event(ipc, "screencastv2", "%d,%d,%s", active, (int)type, str_or_empty(line));
    free(line);
}

void hyprland_ipc_bell(struct wlr_surface *surface)
{
    struct hypr_ipc *ipc = hypr_ipc;
    if (!ipc || ipc->terminating) {
        return;
    }
    struct view *view = surface ? view_try_from_wlr_surface(surface) : NULL;
    struct hypr_window *win = view ? window_from_view(ipc, view) : NULL;
    if (win && win->announced) {
        emit_event(ipc, "bell", "%" PRIx64, win->address);
    } else {
        emit_event(ipc, "bell", "%s", "");
    }
}

static void toplevel_tag_free(struct hypr_toplevel_tag *tag)
{
    wl_list_remove(&tag->link);
    wl_list_remove(&tag->destroy.link);
    free(tag->tag);
    free(tag->description);
    free(tag);
}

static void handle_toplevel_tag_destroy(struct wl_listener *listener, void *data)
{
    struct hypr_toplevel_tag *tag = wl_container_of(listener, tag, destroy);
    toplevel_tag_free(tag);
}

void hyprland_ipc_toplevel_tag(struct wlr_surface *surface, const char *tag,
                               const char *description)
{
    struct hypr_ipc *ipc = hypr_ipc;
    if (!ipc || !surface) {
        return;
    }
    struct hypr_toplevel_tag *entry, *found = NULL;
    wl_list_for_each(entry, &ipc->toplevel_tags, link) {
        if (entry->surface == surface) {
            found = entry;
            break;
        }
    }
    if (!found) {
        found = calloc(1, sizeof(*found));
        if (!found) {
            return;
        }
        found->surface = surface;
        found->destroy.notify = handle_toplevel_tag_destroy;
        wl_signal_add(&surface->events.destroy, &found->destroy);
        wl_list_insert(&ipc->toplevel_tags, &found->link);
    }
    if (tag) {
        replace_str(&found->tag, tag);
    }
    if (description) {
        replace_str(&found->description, description);
    }
}

void hyprland_ipc_keyboard_layout(struct input *input, uint32_t group)
{
    struct hypr_ipc *ipc = hypr_ipc;
    if (!ipc || ipc->terminating || !input) {
        return;
    }
    struct wlr_keyboard *kb = input_keyboard(input);
    if (!kb || !kb->keymap) {
        return;
    }
    char name[256];
    device_name(input->name, name, sizeof(name));
    const char *layout = group < xkb_keymap_num_layouts(kb->keymap)
                             ? xkb_keymap_layout_get_name(kb->keymap, group)
                             : NULL;
    char *line = sanitize_line(layout ? layout : "error");
    emit_event(ipc, "activelayout", "%s,%s", name, str_or_empty(line));
    free(line);
}

static void handle_layer_map(struct wl_listener *listener, void *data)
{
    struct hypr_ipc *ipc = wl_container_of(listener, ipc, layer_map);
    struct wlr_layer_surface_v1 *surface = data;
    if (ipc->terminating) {
        return;
    }
    reconcile(ipc);
    char *ns = sanitize_line(surface->namespace);
    emit_event(ipc, "openlayer", "%s", str_or_empty(ns));
    free(ns);
}

static void handle_layer_unmap(struct wl_listener *listener, void *data)
{
    struct hypr_ipc *ipc = wl_container_of(listener, ipc, layer_unmap);
    struct wlr_layer_surface_v1 *surface = data;
    if (ipc->terminating) {
        return;
    }
    reconcile(ipc);
    char *ns = sanitize_line(surface->namespace);
    emit_event(ipc, "closelayer", "%s", str_or_empty(ns));
    free(ns);
}

/* ------------------------------------------------------------------------- */
/* setup and teardown */

static int listen_unix(const char *path)
{
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    if (strlen(path) >= sizeof(addr.sun_path)) {
        kywc_log(KYWC_ERROR, "hyprland ipc: socket path too long: %s", path);
        return -1;
    }
    strcpy(addr.sun_path, path);

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        kywc_log_errno(KYWC_ERROR, "hyprland ipc: socket");
        return -1;
    }
    unlink(path);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, 16) < 0) {
        kywc_log_errno(KYWC_ERROR, "hyprland ipc: unable to listen on %s", path);
        close(fd);
        return -1;
    }
    return fd;
}

static void close_sockets(struct hypr_ipc *ipc)
{
    struct hypr_request *req, *rtmp;
    wl_list_for_each_safe(req, rtmp, &ipc->requests, link) {
        request_destroy(req);
    }
    wl_list_for_each_safe(req, rtmp, &ipc->log_followers, link) {
        request_destroy(req);
    }
    struct hypr_subscriber *sub, *stmp;
    wl_list_for_each_safe(sub, stmp, &ipc->subscribers, link) {
        subscriber_destroy(sub);
    }

    if (ipc->request_source) {
        wl_event_source_remove(ipc->request_source);
        ipc->request_source = NULL;
    }
    if (ipc->event_source) {
        wl_event_source_remove(ipc->event_source);
        ipc->event_source = NULL;
    }
    if (ipc->request_fd >= 0) {
        close(ipc->request_fd);
        ipc->request_fd = -1;
        unlink(ipc->request_path);
    }
    if (ipc->event_fd >= 0) {
        close(ipc->event_fd);
        ipc->event_fd = -1;
        unlink(ipc->event_path);
    }
    if (ipc->lock_path) {
        unlink(ipc->lock_path);
    }
    if (ipc->dir) {
        rmdir(ipc->dir);
    }
}

static bool open_sockets(struct hypr_ipc *ipc)
{
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (!runtime || !*runtime) {
        kywc_log(KYWC_WARN, "hyprland ipc: XDG_RUNTIME_DIR is not set");
        return false;
    }

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    snprintf(ipc->signature, sizeof(ipc->signature), "flakewm_%lld_%ld",
             (long long)ts.tv_sec, (long)(ts.tv_nsec / 1000) ^ (long)getpid());

    char *hypr_dir = NULL;
    if (asprintf(&hypr_dir, "%s/hypr", runtime) < 0) {
        return false;
    }
    if (mkdir(hypr_dir, 0700) < 0 && errno != EEXIST) {
        kywc_log_errno(KYWC_ERROR, "hyprland ipc: unable to create %s", hypr_dir);
        free(hypr_dir);
        return false;
    }
    if (asprintf(&ipc->dir, "%s/%s", hypr_dir, ipc->signature) < 0) {
        ipc->dir = NULL;
        free(hypr_dir);
        return false;
    }
    free(hypr_dir);
    if (mkdir(ipc->dir, 0700) < 0 && errno != EEXIST) {
        kywc_log_errno(KYWC_ERROR, "hyprland ipc: unable to create %s", ipc->dir);
        return false;
    }

    if (asprintf(&ipc->request_path, "%s/.socket.sock", ipc->dir) < 0) {
        ipc->request_path = NULL;
        return false;
    }
    if (asprintf(&ipc->event_path, "%s/.socket2.sock", ipc->dir) < 0) {
        ipc->event_path = NULL;
        return false;
    }

    ipc->request_fd = listen_unix(ipc->request_path);
    ipc->event_fd = listen_unix(ipc->event_path);
    if (ipc->request_fd < 0 || ipc->event_fd < 0) {
        return false;
    }

    struct wl_event_loop *loop = ipc->server->event_loop;
    ipc->request_source = wl_event_loop_add_fd(loop, ipc->request_fd, WL_EVENT_READABLE,
                                               handle_request_accept, ipc);
    ipc->event_source =
        wl_event_loop_add_fd(loop, ipc->event_fd, WL_EVENT_READABLE, handle_event_accept, ipc);
    return ipc->request_source && ipc->event_source;
}

/* "<pid>\n<wayland display>\n", read by `hyprctl instances` and -i */
static void write_lock(struct hypr_ipc *ipc)
{
    const char *display = getenv("WAYLAND_DISPLAY");
    if (!ipc->dir || !display || asprintf(&ipc->lock_path, "%s/hyprland.lock", ipc->dir) < 0) {
        ipc->lock_path = NULL;
        return;
    }
    FILE *file = fopen(ipc->lock_path, "we");
    if (!file) {
        kywc_log_errno(KYWC_WARN, "hyprland ipc: unable to write %s", ipc->lock_path);
        free(ipc->lock_path);
        ipc->lock_path = NULL;
        return;
    }
    fprintf(file, "%d\n%s\n", (int)getpid(), display);
    fclose(file);
}

static void handle_server_start(struct wl_listener *listener, void *data)
{
    struct hypr_ipc *ipc = wl_container_of(listener, ipc, server_start);
    wl_list_remove(&ipc->server_start.link);
    wl_list_init(&ipc->server_start.link);

    /* WAYLAND_DISPLAY is only known once the server has started */
    write_lock(ipc);

    /* for applications started through D-Bus or systemd --user */
    dbus_update_activation_environment("HYPRLAND_INSTANCE_SIGNATURE", ipc->signature);
    schedule_reconcile(ipc);
}

static void handle_server_terminate(struct wl_listener *listener, void *data)
{
    struct hypr_ipc *ipc = wl_container_of(listener, ipc, server_terminate);
    ipc->terminating = true;
    if (ipc->idle) {
        wl_event_source_remove(ipc->idle);
        ipc->idle = NULL;
    }
    close_sockets(ipc);
}

static void handle_server_destroy(struct wl_listener *listener, void *data)
{
    struct hypr_ipc *ipc = wl_container_of(listener, ipc, server_destroy);
    ipc->terminating = true;
    if (ipc->idle) {
        wl_event_source_remove(ipc->idle);
        ipc->idle = NULL;
    }
    close_sockets(ipc);

    struct hypr_window *win, *wtmp;
    wl_list_for_each_safe(win, wtmp, &ipc->windows, link) {
        window_destroy(win);
    }
    struct hypr_workspace *hws, *htmp;
    wl_list_for_each_safe(hws, htmp, &ipc->workspaces, link) {
        hypr_workspace_free(hws);
    }
    struct hypr_monitor *mon, *mtmp;
    wl_list_for_each_safe(mon, mtmp, &ipc->monitors, link) {
        wl_list_remove(&mon->link);
        wl_list_remove(&mon->on.link);
        wl_list_remove(&mon->off.link);
        wl_list_remove(&mon->destroy.link);
        free(mon);
    }
    /* window_destroy may have scheduled a reconcile before terminating was set */
    if (ipc->idle) {
        wl_event_source_remove(ipc->idle);
        ipc->idle = NULL;
    }

    for (size_t i = 0; i < ipc->ws_snapshot_count; i++) {
        free(ipc->ws_snapshot[i].name);
    }
    for (size_t i = 0; i < ipc->closed_monitor_count; i++) {
        free(ipc->closed_monitors[i].name);
        free(ipc->closed_monitors[i].desc);
    }
    free(ipc->closed_monitors);
    free(ipc->closed_windows);
    free(ipc->announced_class);
    free(ipc->announced_title);

    kill_mode_end(ipc);
    struct hypr_toplevel_tag *tag, *ttmp;
    wl_list_for_each_safe(tag, ttmp, &ipc->toplevel_tags, link) {
        toplevel_tag_free(tag);
    }

    wl_list_remove(&ipc->new_view.link);
    wl_list_remove(&ipc->layer_map.link);
    wl_list_remove(&ipc->layer_unmap.link);
    wl_list_remove(&ipc->new_mapped_view.link);
    wl_list_remove(&ipc->activate_view.link);
    wl_list_remove(&ipc->new_workspace.link);
    wl_list_remove(&ipc->new_output.link);
    wl_list_remove(&ipc->primary_output.link);
    wl_list_remove(&ipc->server_start.link);
    wl_list_remove(&ipc->server_terminate.link);
    wl_list_remove(&ipc->server_destroy.link);

    free(ipc->request_path);
    free(ipc->event_path);
    free(ipc->lock_path);
    free(ipc->dir);
    free(ipc);
    hypr_ipc = NULL;
}

static bool track_existing_output(struct kywc_output *output, int index, void *data)
{
    track_output(data, output);
    return false;
}

bool hyprland_ipc_create(struct view_manager *view_manager)
{
    if (!view_manager->server->options.enable_hypr_unix_socket_sim) {
        return false;
    }

    struct hypr_ipc *ipc = calloc(1, sizeof(*ipc));
    if (!ipc) {
        return false;
    }
    ipc->view_manager = view_manager;
    ipc->server = view_manager->server;
    ipc->request_fd = ipc->event_fd = -1;
    ipc->announced_mon_id = -1;
    wl_list_init(&ipc->requests);
    wl_list_init(&ipc->subscribers);
    wl_list_init(&ipc->windows);
    wl_list_init(&ipc->mru);
    wl_list_init(&ipc->monitors);
    wl_list_init(&ipc->workspaces);
    wl_list_init(&ipc->toplevel_tags);
    wl_list_init(&ipc->log_followers);
    ipc->next_stable_id = HYPR_STABLE_ID_BASE;

    if (!open_sockets(ipc)) {
        close_sockets(ipc);
        free(ipc->request_path);
        free(ipc->event_path);
        free(ipc->dir);
        free(ipc);
        return false;
    }
    hypr_ipc = ipc;

    /* inherited by the session and everything it spawns */
    setenv("HYPRLAND_INSTANCE_SIGNATURE", ipc->signature, true);

    ipc->new_view.notify = handle_new_view;
    kywc_view_add_new_listener(&ipc->new_view);
    ipc->layer_map.notify = handle_layer_map;
    ipc->layer_unmap.notify = handle_layer_unmap;
    wlr_layer_shell_add_map_listener(&ipc->layer_map, &ipc->layer_unmap);
    ipc->new_mapped_view.notify = handle_new_mapped_view;
    kywc_view_add_new_mapped_listener(&ipc->new_mapped_view);
    ipc->activate_view.notify = handle_activate_view;
    view_manager_add_activate_view_listener(&ipc->activate_view);
    ipc->new_workspace.notify = handle_new_workspace;
    workspace_manager_add_new_listener(&ipc->new_workspace);
    ipc->new_output.notify = handle_new_output;
    kywc_output_add_new_listener(&ipc->new_output);
    ipc->primary_output.notify = handle_primary_output;
    kywc_output_add_primary_listener(&ipc->primary_output);

    ipc->server_start.notify = handle_server_start;
    wl_signal_add(&ipc->server->events.start, &ipc->server_start);
    ipc->server_terminate.notify = handle_server_terminate;
    wl_signal_add(&ipc->server->events.terminate, &ipc->server_terminate);
    ipc->server_destroy.notify = handle_server_destroy;
    server_add_destroy_listener(ipc->server, &ipc->server_destroy);

    output_manager_for_each_output(track_existing_output, true, ipc);
    output_manager_for_each_output(track_existing_output, false, ipc);
    uint32_t count = workspace_manager_get_count();
    for (uint32_t i = 0; i < count; i++) {
        struct workspace *ws = workspace_by_position(i);
        if (ws) {
            track_workspace(ipc, ws);
        }
    }

    kywc_log(KYWC_INFO, "hyprland ipc: listening on %s", ipc->dir);
    return true;
}
