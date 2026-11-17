// SPDX-FileCopyrightText: 2026 CharOfString <root@charofstring.cc>
// SPDX-License-Identifier: GPL-3.0-or-later

/* This feat simulates Hyprland's UNIX IPC socket.
 * Hyprland's IPC socket exposes lots of WM's internal info to your system.
 * It also exposes WM control to programs like hyprctl.
 * You'll have to start with --hypr-unix-socket-sim-enable and please only start it when you HAVE TO.
 * The arg is intentionally designed to be super long to force you think "Should I do so?"
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <regex.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <json-c/json.h>
#include <wlr/types/wlr_keyboard.h>
#include <xkbcommon/xkbcommon.h>

#include <kywc/log.h>

#include "input/cursor.h"
#include "input/input.h"
#include "input/keyboard.h"
#include "input/seat.h"
#include "output.h"
#include "util/dbus.h"
#include "util/limit.h"
#include "view/workspace.h"
#include "view_p.h"
#include "xwayland.h"

#define HYPR_VERSION "0.45.0"
#define HYPR_MAX_REQUEST (64 * 1024)
#define HYPR_MAX_REQUEST_CLIENTS 32
#define HYPR_MAX_SUBSCRIBERS 64
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
    char *request_path, *event_path;
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
    uint64_t next_address;

    struct wl_event_source *idle;
    bool terminating;

    /* what has been announced on the event socket */
    struct hypr_ws_snapshot ws_snapshot[MAX_WORKSPACES];
    size_t ws_snapshot_count;
    uint64_t *closed_windows;
    size_t closed_window_count, closed_window_cap;
    struct hypr_closed_monitor *closed_monitors;
    size_t closed_monitor_count, closed_monitor_cap;

    uint32_t announced_ws_id;
    char *announced_ws_name;
    int announced_mon_id;
    uint64_t announced_window;
    char *announced_class, *announced_title;
    int announced_fullscreen;

    uint32_t previous_ws_id;

    struct wl_listener new_mapped_view;
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
    char *initial_class, *initial_title;

    bool announced;
    uint32_t announced_ws_id;
    char *announced_title;

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
        char *desc = sanitize_line(mon->output->prop.desc);
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

static void reconcile_workspaces(struct hypr_ipc *ipc)
{
    struct hypr_ws_snapshot live[MAX_WORKSPACES];
    size_t live_count = 0;
    uint32_t count = workspace_manager_get_count();
    for (uint32_t i = 0; i < count && live_count < MAX_WORKSPACES; i++) {
        struct workspace *ws = workspace_by_position(i);
        if (!ws) {
            continue;
        }
        char buf[16];
        live[live_count].id = ws_id(ws);
        live[live_count].name = sanitize_line(ws_name(ws, buf));
        live_count++;
    }

    /* destroyed */
    for (size_t i = 0; i < ipc->ws_snapshot_count; i++) {
        struct hypr_ws_snapshot *old = &ipc->ws_snapshot[i];
        bool found = false;
        for (size_t j = 0; j < live_count; j++) {
            found |= live[j].id == old->id;
        }
        if (!found) {
            emit_event(ipc, "destroyworkspace", "%s", str_or_empty(old->name));
            emit_event(ipc, "destroyworkspacev2", "%" PRIu32 ",%s", old->id,
                       str_or_empty(old->name));
        }
    }

    /* created and renamed */
    for (size_t j = 0; j < live_count; j++) {
        struct hypr_ws_snapshot *old = NULL;
        for (size_t i = 0; i < ipc->ws_snapshot_count; i++) {
            if (ipc->ws_snapshot[i].id == live[j].id) {
                old = &ipc->ws_snapshot[i];
            }
        }
        const char *name = str_or_empty(live[j].name);
        if (!old) {
            emit_event(ipc, "createworkspace", "%s", name);
            emit_event(ipc, "createworkspacev2", "%" PRIu32 ",%s", live[j].id, name);
        } else if (str_changed(old->name, name)) {
            emit_event(ipc, "renameworkspace", "%" PRIu32 ",%s", live[j].id, name);
        }
    }

    for (size_t i = 0; i < ipc->ws_snapshot_count; i++) {
        free(ipc->ws_snapshot[i].name);
    }
    memcpy(ipc->ws_snapshot, live, sizeof(live[0]) * live_count);
    ipc->ws_snapshot_count = live_count;
}

static void reconcile_windows(struct hypr_ipc *ipc)
{
    for (size_t i = 0; i < ipc->closed_window_count; i++) {
        emit_event(ipc, "closewindow", "%" PRIx64, ipc->closed_windows[i]);
    }
    ipc->closed_window_count = 0;

    struct hypr_window *win;
    wl_list_for_each_reverse(win, &ipc->windows, link) {
        bool eligible = window_eligible(win);
        if (!eligible) {
            if (win->announced) {
                emit_event(ipc, "closewindow", "%" PRIx64, win->address);
                win->announced = false;
            }
            continue;
        }

        struct workspace *ws = window_workspace(win);
        char buf[16];
        uint32_t id = ws ? ws_id(ws) : 0;
        char *ws_label = sanitize_line(ws ? ws_name(ws, buf) : "");
        char *title = sanitize_line(win->view->base.title);

        if (!win->announced) {
            char *cls = sanitize_line(win->view->base.app_id);
            emit_event(ipc, "openwindow", "%" PRIx64 ",%s,%s,%s", win->address,
                       str_or_empty(ws_label), str_or_empty(cls), str_or_empty(title));
            free(cls);
            win->announced = true;
            win->announced_ws_id = id;
            replace_str(&win->announced_title, title);
        } else {
            if (win->announced_ws_id != id && !win->view->base.sticky) {
                emit_event(ipc, "movewindow", "%" PRIx64 ",%s", win->address,
                           str_or_empty(ws_label));
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
        }
        free(ws_label);
        free(title);
    }
}

static void reconcile_focus(struct hypr_ipc *ipc)
{
    struct workspace *ws = workspace_manager_get_current();
    char buf[16];
    uint32_t id = ws ? ws_id(ws) : 0;
    char *name = sanitize_line(ws ? ws_name(ws, buf) : "");

    if (ws && (id != ipc->announced_ws_id || str_changed(ipc->announced_ws_name, name))) {
        if (id != ipc->announced_ws_id && ipc->announced_ws_id) {
            ipc->previous_ws_id = ipc->announced_ws_id;
        }
        emit_event(ipc, "workspace", "%s", str_or_empty(name));
        emit_event(ipc, "workspacev2", "%" PRIu32 ",%s", id, str_or_empty(name));
    }

    struct hypr_monitor *mon = focused_monitor(ipc);
    int mon_id = mon ? mon->id : -1;
    if (mon && mon_id != ipc->announced_mon_id) {
        emit_event(ipc, "focusedmon", "%s,%s", mon->output->name, str_or_empty(name));
        emit_event(ipc, "focusedmonv2", "%s,%" PRIu32, mon->output->name, id);
    }
    ipc->announced_mon_id = mon_id;
    ipc->announced_ws_id = id;
    replace_str(&ipc->announced_ws_name, name);
    free(name);

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

    /* fullscreen state of the current workspace */
    int fullscreen = 0;
    wl_list_for_each(win, &ipc->windows, link) {
        if (window_eligible(win) && window_workspace(win) == ws && !win->view->base.minimized &&
            win->view->base.fullscreen) {
            fullscreen = 1;
            break;
        }
    }
    if (fullscreen != ipc->announced_fullscreen) {
        /* the first state is the baseline, not a change */
        if (ipc->announced_fullscreen >= 0 || fullscreen) {
            emit_event(ipc, "fullscreen", "%d", fullscreen);
        }
        ipc->announced_fullscreen = fullscreen;
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
    reconcile_workspaces(ipc);
    reconcile_windows(ipc);
    reconcile_focus(ipc);
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

static void window_destroy(struct hypr_window *win)
{
    struct hypr_ipc *ipc = win->ipc;

    if (win->announced) {
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

    wl_list_remove(&win->link);
    wl_list_remove(&win->mru_link);
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
    free(win->initial_class);
    free(win->initial_title);
    free(win->announced_title);
    free(win);
}

static void handle_window_unmap(struct wl_listener *listener, void *data)
{
    struct hypr_window *win = wl_container_of(listener, win, unmap);
    window_destroy(win);
}

static void handle_window_destroy(struct wl_listener *listener, void *data)
{
    struct hypr_window *win = wl_container_of(listener, win, destroy);
    window_destroy(win);
}

static void handle_new_mapped_view(struct wl_listener *listener, void *data)
{
    struct hypr_ipc *ipc = wl_container_of(listener, ipc, new_mapped_view);
    struct view *view = view_from_kywc_view(data);
    if (!view || window_from_view(ipc, view)) {
        return;
    }

    struct hypr_window *win = calloc(1, sizeof(*win));
    if (!win) {
        return;
    }
    win->ipc = ipc;
    win->view = view;
    win->address = ipc->next_address++;
    win->initial_class = view->base.app_id ? strdup(view->base.app_id) : NULL;
    win->initial_title = view->base.title ? strdup(view->base.title) : NULL;
    wl_list_insert(&ipc->windows, &win->link);
    if (view->base.activated) {
        wl_list_insert(&ipc->mru, &win->mru_link);
    } else {
        wl_list_insert(ipc->mru.prev, &win->mru_link);
    }

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

    schedule_reconcile(ipc);
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
            m->desc = sanitize_line(mon->output->prop.desc);
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
/* JSON / text serialization */

static json_object *json_ws_ref(struct workspace *ws)
{
    char buf[16];
    json_object *obj = json_object_new_object();
    json_object_object_add(obj, "id", json_object_new_int64(ws ? ws_id(ws) : -1));
    json_object_object_add(obj, "name", json_object_new_string(ws ? ws_name(ws, buf) : ""));
    return obj;
}

static void format_address(char buf[32], uint64_t address)
{
    snprintf(buf, 32, "0x%" PRIx64, address);
}

static json_object *json_window(struct hypr_ipc *ipc, struct hypr_window *win)
{
    struct view *view = win->view;
    struct kywc_view *kv = &view->base;
    struct hypr_monitor *mon = monitor_from_output(ipc, view->output);
    char addr[32];
    format_address(addr, win->address);

    json_object *obj = json_object_new_object();
    json_object_object_add(obj, "address", json_object_new_string(addr));
    json_object_object_add(obj, "mapped", json_object_new_boolean(kv->mapped));
    json_object_object_add(obj, "hidden", json_object_new_boolean(kv->minimized));
    json_object *at = json_object_new_array();
    json_object_array_add(at, json_object_new_int(kv->geometry.x));
    json_object_array_add(at, json_object_new_int(kv->geometry.y));
    json_object_object_add(obj, "at", at);
    json_object *size = json_object_new_array();
    json_object_array_add(size, json_object_new_int(kv->geometry.width));
    json_object_array_add(size, json_object_new_int(kv->geometry.height));
    json_object_object_add(obj, "size", size);
    json_object_object_add(obj, "workspace", json_ws_ref(window_workspace(win)));
    json_object_object_add(obj, "floating",
                           json_object_new_boolean(kv->tiled == KYWC_TILE_NONE));
    json_object_object_add(obj, "pseudo", json_object_new_boolean(false));
    json_object_object_add(obj, "monitor", json_object_new_int(mon ? mon->id : -1));
    json_object_object_add(obj, "class", json_object_new_string(str_or_empty(kv->app_id)));
    json_object_object_add(obj, "title", json_object_new_string(str_or_empty(kv->title)));
    json_object_object_add(obj, "initialClass",
                           json_object_new_string(str_or_empty(win->initial_class)));
    json_object_object_add(obj, "initialTitle",
                           json_object_new_string(str_or_empty(win->initial_title)));
    json_object_object_add(obj, "pid", json_object_new_int(view->pid > 0 ? view->pid : -1));
    json_object_object_add(obj, "xwayland", json_object_new_boolean(xwayland_check_view(view)));
    json_object_object_add(obj, "pinned", json_object_new_boolean(kv->sticky));
    int fs = window_fullscreen_mode(view);
    json_object_object_add(obj, "fullscreen", json_object_new_int(fs));
    json_object_object_add(obj, "fullscreenClient", json_object_new_int(fs));
    json_object_object_add(obj, "grouped", json_object_new_array());
    json_object_object_add(obj, "tags", json_object_new_array());
    json_object_object_add(obj, "swallowing", json_object_new_string("0x0"));
    json_object_object_add(obj, "focusHistoryID",
                           json_object_new_int(window_focus_history_id(ipc, win)));
    json_object_object_add(obj, "inhibitingIdle", json_object_new_boolean(false));
    json_object_object_add(obj, "xdgTag", json_object_new_string(""));
    json_object_object_add(obj, "xdgDescription", json_object_new_string(""));
    return obj;
}

static void text_window(struct hypr_ipc *ipc, struct hypr_window *win, struct sbuf *out)
{
    struct view *view = win->view;
    struct kywc_view *kv = &view->base;
    struct hypr_monitor *mon = monitor_from_output(ipc, view->output);
    struct workspace *ws = window_workspace(win);
    char buf[16];
    int fs = window_fullscreen_mode(view);

    sbuf_printf(out, "Window %" PRIx64 " -> %s:\n", win->address, str_or_empty(kv->title));
    sbuf_printf(out, "\tmapped: %d\n\thidden: %d\n", kv->mapped, kv->minimized);
    sbuf_printf(out, "\tat: %d,%d\n\tsize: %d,%d\n", kv->geometry.x, kv->geometry.y,
                kv->geometry.width, kv->geometry.height);
    sbuf_printf(out, "\tworkspace: %d (%s)\n", ws ? (int)ws_id(ws) : -1,
                ws ? ws_name(ws, buf) : "");
    sbuf_printf(out, "\tfloating: %d\n\tpseudo: 0\n\tmonitor: %d\n",
                kv->tiled == KYWC_TILE_NONE, mon ? mon->id : -1);
    sbuf_printf(out, "\tclass: %s\n\ttitle: %s\n", str_or_empty(kv->app_id),
                str_or_empty(kv->title));
    sbuf_printf(out, "\tinitialClass: %s\n\tinitialTitle: %s\n",
                str_or_empty(win->initial_class), str_or_empty(win->initial_title));
    sbuf_printf(out, "\tpid: %d\n\txwayland: %d\n\tpinned: %d\n", view->pid > 0 ? view->pid : -1,
                xwayland_check_view(view), kv->sticky);
    sbuf_printf(out, "\tfullscreen: %d\n\tfullscreenClient: %d\n", fs, fs);
    sbuf_printf(out, "\tgrouped: 0\n\ttags: \n\tswallowing: 0\n");
    sbuf_printf(out, "\tfocusHistoryID: %d\n\tinhibitingIdle: 0\n",
                window_focus_history_id(ipc, win));
    sbuf_printf(out, "\txdgTag: \n\txdgDescription: \n\n");
}

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

static json_object *json_workspace(struct hypr_ipc *ipc, struct workspace *ws)
{
    struct hypr_monitor *mon = workspace_monitor(ipc);
    int windows;
    bool fullscreen;
    struct hypr_window *last;
    workspace_stats(ipc, ws, &windows, &fullscreen, &last);
    char buf[16], addr[32];
    format_address(addr, last ? last->address : 0);

    json_object *obj = json_object_new_object();
    json_object_object_add(obj, "id", json_object_new_int64(ws_id(ws)));
    json_object_object_add(obj, "name", json_object_new_string(ws_name(ws, buf)));
    json_object_object_add(obj, "monitor", json_object_new_string(mon ? mon->output->name : "?"));
    json_object_object_add(obj, "monitorID", json_object_new_int(mon ? mon->id : -1));
    json_object_object_add(obj, "windows", json_object_new_int(windows));
    json_object_object_add(obj, "hasfullscreen", json_object_new_boolean(fullscreen));
    json_object_object_add(obj, "lastwindow", json_object_new_string(addr));
    json_object_object_add(
        obj, "lastwindowtitle",
        json_object_new_string(last ? str_or_empty(last->view->base.title) : ""));
    json_object_object_add(obj, "ispersistent", json_object_new_boolean(true));
    return obj;
}

static void text_workspace(struct hypr_ipc *ipc, struct workspace *ws, struct sbuf *out)
{
    struct hypr_monitor *mon = workspace_monitor(ipc);
    int windows;
    bool fullscreen;
    struct hypr_window *last;
    workspace_stats(ipc, ws, &windows, &fullscreen, &last);
    char buf[16];

    sbuf_printf(out, "workspace ID %" PRIu32 " (%s) on monitor %s:\n", ws_id(ws),
                ws_name(ws, buf), mon ? mon->output->name : "?");
    sbuf_printf(out, "\tmonitorID: %d\n\twindows: %d\n\thasfullscreen: %d\n",
                mon ? mon->id : -1, windows, fullscreen);
    sbuf_printf(out, "\tlastwindow: 0x%" PRIx64 "\n\tlastwindowtitle: %s\n",
                last ? last->address : 0, last ? str_or_empty(last->view->base.title) : "");
    sbuf_printf(out, "\tispersistent: 1\n\n");
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

static json_object *json_monitor(struct hypr_ipc *ipc, struct hypr_monitor *mon,
                                 struct hypr_monitor *focused)
{
    struct kywc_output *o = mon->output;
    struct kywc_output_state *st = &o->state;
    struct workspace *ws = workspace_manager_get_current();
    int reserved[4];
    monitor_reserved(mon, reserved);

    json_object *obj = json_object_new_object();
    json_object_object_add(obj, "id", json_object_new_int(mon->id));
    json_object_object_add(obj, "name", json_object_new_string(str_or_empty(o->name)));
    json_object_object_add(obj, "description",
                           json_object_new_string(str_or_empty(o->prop.desc)));
    json_object_object_add(obj, "make", json_object_new_string(str_or_empty(o->prop.make)));
    json_object_object_add(obj, "model", json_object_new_string(str_or_empty(o->prop.model)));
    json_object_object_add(obj, "serial", json_object_new_string(str_or_empty(o->prop.serial)));
    json_object_object_add(obj, "width", json_object_new_int(st->width));
    json_object_object_add(obj, "height", json_object_new_int(st->height));
    json_object_object_add(obj, "physicalWidth", json_object_new_int(o->prop.phys_width));
    json_object_object_add(obj, "physicalHeight", json_object_new_int(o->prop.phys_height));
    json_object_object_add(obj, "refreshRate", json_object_new_double(st->refresh / 1000.0));
    json_object_object_add(obj, "x", json_object_new_int(st->lx));
    json_object_object_add(obj, "y", json_object_new_int(st->ly));
    json_object_object_add(obj, "activeWorkspace", json_ws_ref(ws));
    json_object *special = json_object_new_object();
    json_object_object_add(special, "id", json_object_new_int(0));
    json_object_object_add(special, "name", json_object_new_string(""));
    json_object_object_add(obj, "specialWorkspace", special);
    json_object *res = json_object_new_array();
    for (int i = 0; i < 4; i++) {
        json_object_array_add(res, json_object_new_int(reserved[i]));
    }
    json_object_object_add(obj, "reserved", res);
    json_object_object_add(obj, "scale", json_object_new_double(st->scale));
    json_object_object_add(obj, "transform", json_object_new_int(st->transform));
    json_object_object_add(obj, "focused", json_object_new_boolean(mon == focused));
    json_object_object_add(obj, "dpmsStatus", json_object_new_boolean(st->power));
    json_object_object_add(obj, "vrr",
                           json_object_new_boolean(st->vrr_policy != KYWC_OUTPUT_VRR_POLICY_NEVER));
    json_object_object_add(obj, "solitary", json_object_new_string("0"));
    json_object_object_add(obj, "activelyTearing", json_object_new_boolean(false));
    json_object_object_add(obj, "directScanoutTo", json_object_new_string("0"));
    json_object_object_add(obj, "disabled", json_object_new_boolean(!monitor_enabled(mon)));
    json_object_object_add(obj, "currentFormat", json_object_new_string("XRGB8888"));
    json_object_object_add(obj, "mirrorOf", json_object_new_string("none"));
    json_object *modes = json_object_new_array();
    struct kywc_output_mode *mode;
    wl_list_for_each(mode, &o->prop.modes, link) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%dx%d@%.2fHz", mode->width, mode->height,
                 mode->refresh / 1000.0);
        json_object_array_add(modes, json_object_new_string(buf));
    }
    json_object_object_add(obj, "availableModes", modes);
    return obj;
}

static void text_monitor(struct hypr_ipc *ipc, struct hypr_monitor *mon,
                         struct hypr_monitor *focused, struct sbuf *out)
{
    struct kywc_output *o = mon->output;
    struct kywc_output_state *st = &o->state;
    struct workspace *ws = workspace_manager_get_current();
    int reserved[4];
    monitor_reserved(mon, reserved);
    char buf[16];

    sbuf_printf(out, "Monitor %s (ID %d):\n", str_or_empty(o->name), mon->id);
    sbuf_printf(out, "\t%dx%d@%.5f at %dx%d\n", st->width, st->height, st->refresh / 1000.0,
                st->lx, st->ly);
    sbuf_printf(out, "\tdescription: %s\n\tmake: %s\n\tmodel: %s\n\tserial: %s\n",
                str_or_empty(o->prop.desc), str_or_empty(o->prop.make),
                str_or_empty(o->prop.model), str_or_empty(o->prop.serial));
    sbuf_printf(out, "\tactive workspace: %d (%s)\n", ws ? (int)ws_id(ws) : -1,
                ws ? ws_name(ws, buf) : "");
    sbuf_printf(out, "\tspecial workspace: 0 ()\n");
    sbuf_printf(out, "\treserved: %d %d %d %d\n", reserved[0], reserved[1], reserved[2],
                reserved[3]);
    sbuf_printf(out, "\tscale: %.2f\n\ttransform: %d\n\tfocused: %s\n", st->scale,
                st->transform, mon == focused ? "yes" : "no");
    sbuf_printf(out, "\tdpmsStatus: %d\n\tvrr: %s\n", st->power,
                st->vrr_policy != KYWC_OUTPUT_VRR_POLICY_NEVER ? "true" : "false");
    sbuf_printf(out, "\tsolitary: 0\n\tactivelyTearing: false\n\tdirectScanoutTo: 0\n");
    sbuf_printf(out, "\tdisabled: %s\n\tcurrentFormat: XRGB8888\n\tmirrorOf: none\n",
                monitor_enabled(mon) ? "false" : "true");
    sbuf_puts(out, "\tavailableModes:");
    struct kywc_output_mode *mode;
    wl_list_for_each(mode, &o->prop.modes, link) {
        sbuf_printf(out, " %dx%d@%.2fHz", mode->width, mode->height, mode->refresh / 1000.0);
    }
    sbuf_puts(out, "\n\n");
}

static void json_append(struct sbuf *out, json_object *obj)
{
    sbuf_puts(out, json_object_to_json_string_ext(
                       obj, JSON_C_TO_STRING_PRETTY | JSON_C_TO_STRING_SPACED | JSON_C_TO_STRING_NOSLASHESCAPE));
    json_object_put(obj);
}

/* ------------------------------------------------------------------------- */
/* read commands */

struct hypr_flags {
    bool json, all;
};

static void cmd_version(struct hypr_ipc *ipc, struct hypr_flags *f, const char *args,
                        struct sbuf *out)
{
    if (f->json) {
        json_object *obj = json_object_new_object();
        json_object_object_add(obj, "branch", json_object_new_string("flakewm"));
        json_object_object_add(obj, "commit", json_object_new_string(""));
        json_object_object_add(obj, "version", json_object_new_string(HYPR_VERSION));
        json_object_object_add(obj, "dirty", json_object_new_boolean(false));
        json_object_object_add(obj, "commit_message",
                               json_object_new_string("flakewm hyprland ipc compatibility"));
        json_object_object_add(obj, "commit_date", json_object_new_string(""));
        json_object_object_add(obj, "tag", json_object_new_string("v" HYPR_VERSION));
        json_object_object_add(obj, "commits", json_object_new_string("0"));
        json_object_object_add(obj, "buildAquamarine", json_object_new_string(""));
        json_object_object_add(obj, "systemAquamarine", json_object_new_string(""));
        json_object_object_add(obj, "abiHash", json_object_new_string(""));
        json_object_object_add(obj, "flags", json_object_new_array());
        json_append(out, obj);
        return;
    }
    sbuf_puts(out, "Hyprland " HYPR_VERSION " built from branch flakewm at commit  ().\n"
                   "Date: \nTag: v" HYPR_VERSION ", commits: 0\n\n"
                   "flakewm Hyprland IPC compatibility layer.\n");
}

static void cmd_monitors(struct hypr_ipc *ipc, struct hypr_flags *f, const char *args,
                         struct sbuf *out)
{
    bool all = f->all || (args && strstr(args, "all"));
    struct hypr_monitor *focused = focused_monitor(ipc);
    json_object *arr = f->json ? json_object_new_array() : NULL;
    struct hypr_monitor *mon;
    wl_list_for_each(mon, &ipc->monitors, link) {
        if (mon->output->destroying || (!all && !monitor_enabled(mon))) {
            continue;
        }
        if (arr) {
            json_object_array_add(arr, json_monitor(ipc, mon, focused));
        } else {
            text_monitor(ipc, mon, focused, out);
        }
    }
    if (arr) {
        json_append(out, arr);
    }
}

static void cmd_workspaces(struct hypr_ipc *ipc, struct hypr_flags *f, const char *args,
                           struct sbuf *out)
{
    json_object *arr = f->json ? json_object_new_array() : NULL;
    uint32_t count = workspace_manager_get_count();
    for (uint32_t i = 0; i < count; i++) {
        struct workspace *ws = workspace_by_position(i);
        if (!ws) {
            continue;
        }
        if (arr) {
            json_object_array_add(arr, json_workspace(ipc, ws));
        } else {
            text_workspace(ipc, ws, out);
        }
    }
    if (arr) {
        json_append(out, arr);
    }
}

static void cmd_activeworkspace(struct hypr_ipc *ipc, struct hypr_flags *f, const char *args,
                                struct sbuf *out)
{
    struct workspace *ws = workspace_manager_get_current();
    if (!ws) {
        sbuf_puts(out, f->json ? "{}" : "Invalid\n");
        return;
    }
    if (f->json) {
        json_append(out, json_workspace(ipc, ws));
    } else {
        text_workspace(ipc, ws, out);
    }
}

static void cmd_clients(struct hypr_ipc *ipc, struct hypr_flags *f, const char *args,
                        struct sbuf *out)
{
    json_object *arr = f->json ? json_object_new_array() : NULL;
    struct hypr_window *win;
    /* oldest first, like Hyprland */
    wl_list_for_each_reverse(win, &ipc->windows, link) {
        if (!window_eligible(win)) {
            continue;
        }
        if (arr) {
            json_object_array_add(arr, json_window(ipc, win));
        } else {
            text_window(ipc, win, out);
        }
    }
    if (arr) {
        json_append(out, arr);
    }
}

static void cmd_activewindow(struct hypr_ipc *ipc, struct hypr_flags *f, const char *args,
                             struct sbuf *out)
{
    struct hypr_window *win = active_window(ipc);
    if (!win) {
        sbuf_puts(out, f->json ? "{}" : "Invalid\n");
        return;
    }
    if (f->json) {
        json_append(out, json_window(ipc, win));
    } else {
        text_window(ipc, win, out);
    }
}

static void cmd_cursorpos(struct hypr_ipc *ipc, struct hypr_flags *f, const char *args,
                          struct sbuf *out)
{
    struct seat *seat = input_manager_get_default_seat();
    int x = 0, y = 0;
    if (seat && seat->cursor) {
        x = (int)seat->cursor->lx;
        y = (int)seat->cursor->ly;
    }
    if (f->json) {
        sbuf_printf(out, "{\n  \"x\": %d,\n  \"y\": %d\n}", x, y);
    } else {
        sbuf_printf(out, "%d, %d", x, y);
    }
}

static void cmd_layers(struct hypr_ipc *ipc, struct hypr_flags *f, const char *args,
                       struct sbuf *out)
{
    /* layer surfaces are not exposed, report empty levels per monitor */
    json_object *obj = f->json ? json_object_new_object() : NULL;
    struct hypr_monitor *mon;
    wl_list_for_each(mon, &ipc->monitors, link) {
        if (!monitor_enabled(mon)) {
            continue;
        }
        if (obj) {
            json_object *levels = json_object_new_object();
            for (int i = 0; i < 4; i++) {
                char key[2] = { (char)('0' + i), 0 };
                json_object_object_add(levels, key, json_object_new_array());
            }
            json_object *m = json_object_new_object();
            json_object_object_add(m, "levels", levels);
            json_object_object_add(obj, mon->output->name, m);
        } else {
            sbuf_printf(out, "Monitor %s:\n\tLayer level 0:\n\n\tLayer level 1:\n\n"
                             "\tLayer level 2:\n\n\tLayer level 3:\n\n",
                        mon->output->name);
        }
    }
    if (obj) {
        json_append(out, obj);
    }
}

static void keyboard_info(const char **keymap, bool *caps, bool *num)
{
    *keymap = "";
    *caps = *num = false;
    struct seat *seat = input_manager_get_default_seat();
    if (!seat || !seat->keyboard || !seat->keyboard->wlr_keyboard) {
        return;
    }
    struct wlr_keyboard *kb = seat->keyboard->wlr_keyboard;
    if (kb->keymap && kb->xkb_state) {
        xkb_layout_index_t layout =
            xkb_state_serialize_layout(kb->xkb_state, XKB_STATE_LAYOUT_EFFECTIVE);
        const char *name = xkb_keymap_layout_get_name(kb->keymap, layout);
        *keymap = name ? name : "";
        *caps = xkb_state_led_name_is_active(kb->xkb_state, XKB_LED_NAME_CAPS) > 0;
        *num = xkb_state_led_name_is_active(kb->xkb_state, XKB_LED_NAME_NUM) > 0;
    }
}

static void cmd_devices(struct hypr_ipc *ipc, struct hypr_flags *f, const char *args,
                        struct sbuf *out)
{
    const char *keymap;
    bool caps, num;
    keyboard_info(&keymap, &caps, &num);

    if (!f->json) {
        sbuf_printf(out, "mice:\n\nKeyboards:\n\tKeyboard at 0x1:\n\t\tflakewm-keyboard\n"
                         "\t\t\tactive keymap: %s\n\t\t\tcapsLock: %s\n\t\t\tnumLock: %s\n"
                         "\t\t\tmain: yes\n\n",
                    keymap, caps ? "yes" : "no", num ? "yes" : "no");
        return;
    }

    json_object *obj = json_object_new_object();
    json_object_object_add(obj, "mice", json_object_new_array());
    json_object *kbs = json_object_new_array();
    json_object *kb = json_object_new_object();
    json_object_object_add(kb, "address", json_object_new_string("0x1"));
    json_object_object_add(kb, "name", json_object_new_string("flakewm-keyboard"));
    json_object_object_add(kb, "rules", json_object_new_string(""));
    json_object_object_add(kb, "model", json_object_new_string(""));
    json_object_object_add(kb, "layout", json_object_new_string(""));
    json_object_object_add(kb, "variant", json_object_new_string(""));
    json_object_object_add(kb, "options", json_object_new_string(""));
    json_object_object_add(kb, "active_keymap", json_object_new_string(keymap));
    json_object_object_add(kb, "capsLock", json_object_new_boolean(caps));
    json_object_object_add(kb, "numLock", json_object_new_boolean(num));
    json_object_object_add(kb, "main", json_object_new_boolean(true));
    json_object_array_add(kbs, kb);
    json_object_object_add(obj, "keyboards", kbs);
    json_object_object_add(obj, "tablets", json_object_new_array());
    json_object_object_add(obj, "touch", json_object_new_array());
    json_object_object_add(obj, "switches", json_object_new_array());
    json_append(out, obj);
}

static void cmd_empty_list(struct hypr_ipc *ipc, struct hypr_flags *f, const char *args,
                           struct sbuf *out)
{
    sbuf_puts(out, f->json ? "[]" : "");
}

static void cmd_animations(struct hypr_ipc *ipc, struct hypr_flags *f, const char *args,
                           struct sbuf *out)
{
    sbuf_puts(out, f->json ? "[[],[]]" : "");
}

static void cmd_splash(struct hypr_ipc *ipc, struct hypr_flags *f, const char *args,
                       struct sbuf *out)
{
    sbuf_puts(out, "flakewm");
}

static void cmd_submap(struct hypr_ipc *ipc, struct hypr_flags *f, const char *args,
                       struct sbuf *out)
{
    sbuf_puts(out, "default");
}

static void cmd_ok(struct hypr_ipc *ipc, struct hypr_flags *f, const char *args,
                   struct sbuf *out)
{
    sbuf_puts(out, "ok");
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

static struct workspace *workspace_relative(struct workspace *current, long delta, bool wrap)
{
    long count = workspace_manager_get_count();
    if (!current || count <= 0) {
        return NULL;
    }
    long pos = (long)current->position + delta;
    if (wrap) {
        pos = ((pos % count) + count) % count;
    } else if (pos < 0) {
        pos = 0;
    } else if (pos >= count) {
        pos = count - 1;
    }
    return workspace_by_position(pos);
}

static struct workspace *workspace_by_selector(struct hypr_ipc *ipc, const char *sel)
{
    while (sel && *sel == ' ') {
        sel++;
    }
    if (!sel || !*sel) {
        return NULL;
    }

    struct workspace *current = workspace_manager_get_current();
    char *end = NULL;

    if (!strcmp(sel, "previous") || !strcmp(sel, "previous_per_monitor")) {
        return ws_by_id(ipc->previous_ws_id);
    }
    if (!strncmp(sel, "name:", 5)) {
        uint32_t count = workspace_manager_get_count();
        for (uint32_t i = 0; i < count; i++) {
            struct workspace *ws = workspace_by_position(i);
            char buf[16];
            if (ws && !strcmp(ws_name(ws, buf), sel + 5)) {
                return ws;
            }
        }
        return NULL;
    }
    if (!strncmp(sel, "empty", 5)) {
        uint32_t count = workspace_manager_get_count();
        for (uint32_t i = 0; i < count; i++) {
            struct workspace *ws = workspace_by_position(i);
            int windows;
            bool fs;
            struct hypr_window *last;
            if (!ws) {
                continue;
            }
            workspace_stats(ipc, ws, &windows, &fs, &last);
            if (windows == 0) {
                return ws;
            }
        }
        return NULL;
    }
    if (sel[0] == 'e' || sel[0] == 'm' || sel[0] == 'r') {
        if (sel[1] == '+' || sel[1] == '-') {
            long delta = strtol(sel + 1, &end, 10);
            if (end && *end == '\0') {
                return workspace_relative(current, delta, sel[0] != 'r');
            }
        }
        return NULL;
    }
    if (sel[0] == '+' || sel[0] == '-') {
        long delta = strtol(sel, &end, 10);
        return end && *end == '\0' ? workspace_relative(current, delta, false) : NULL;
    }

    long id = strtol(sel, &end, 10);
    if (end && end != sel && *end == '\0') {
        return id > 0 ? ws_by_id((uint32_t)id) : NULL;
    }
    return NULL;
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

    if (!strcmp(name, "focuswindow")) {
        struct hypr_window *win = window_by_selector(ipc, arg);
        if (!win) {
            return "No such window found";
        }
        focus_view(win->view);
        return "ok";
    }

    if (!strcmp(name, "closewindow") || !strcmp(name, "killactive")) {
        struct hypr_window *win =
            window_by_selector(ipc, !strcmp(name, "killactive") ? NULL : arg);
        if (!win) {
            return "No such window found";
        }
        kywc_view_close(&win->view->base);
        return "ok";
    }

    if (!strcmp(name, "fullscreen")) {
        struct hypr_window *win = active_window(ipc);
        if (!win) {
            return "No active window";
        }
        if (atoi(arg) == 1) {
            kywc_view_toggle_maximized(&win->view->base);
        } else {
            kywc_view_toggle_fullscreen(&win->view->base);
        }
        return "ok";
    }

    if (!strcmp(name, "movetoworkspace") || !strcmp(name, "movetoworkspacesilent")) {
        char *ws_sel = strdup(arg);
        if (!ws_sel) {
            return "Out of memory";
        }
        char *comma = strchr(ws_sel, ',');
        const char *win_sel = NULL;
        if (comma) {
            *comma = '\0';
            win_sel = arg + (comma - ws_sel) + 1;
        }
        struct workspace *ws = workspace_by_selector(ipc, ws_sel);
        free(ws_sel);
        struct hypr_window *win = window_by_selector(ipc, win_sel);
        if (!ws) {
            return "Invalid workspace";
        }
        if (!win) {
            return "No such window found";
        }
        view_set_workspace(win->view, ws);
        if (!strcmp(name, "movetoworkspace")) {
            if (ws != workspace_manager_get_current()) {
                workspace_activate_with_effect(ws);
            }
            focus_view(win->view);
        }
        return "ok";
    }

    if (!strcmp(name, "pin")) {
        struct hypr_window *win = window_by_selector(ipc, arg);
        if (!win) {
            return "No such window found";
        }
        if (win->view->base.sticky) {
            struct workspace *ws = workspace_manager_get_current();
            if (ws) {
                view_set_workspace(win->view, ws);
            }
        } else {
            view_add_all_workspace(win->view);
        }
        return "ok";
    }

    return "Invalid dispatcher";
}

static void cmd_dispatch(struct hypr_ipc *ipc, struct hypr_flags *f, const char *args,
                         struct sbuf *out)
{
    while (args && *args == ' ') {
        args++;
    }
    if (!args || !*args) {
        sbuf_puts(out, "Invalid dispatcher");
        return;
    }

    const char *space = strchr(args, ' ');
    size_t len = space ? (size_t)(space - args) : strlen(args);
    char name[64];
    if (len >= sizeof(name)) {
        sbuf_puts(out, "Invalid dispatcher");
        return;
    }
    memcpy(name, args, len);
    name[len] = '\0';
    const char *arg = space ? space + 1 : "";
    while (*arg == ' ') {
        arg++;
    }

    sbuf_puts(out, dispatch(ipc, name, arg));
}

/* ------------------------------------------------------------------------- */
/* request socket */

typedef void (*hypr_cmd_func)(struct hypr_ipc *ipc, struct hypr_flags *f, const char *args,
                              struct sbuf *out);

static const struct {
    const char *name;
    bool exact;
    hypr_cmd_func func;
} hypr_commands[] = {
    { "version", true, cmd_version },
    { "monitors", false, cmd_monitors },
    { "workspaces", true, cmd_workspaces },
    { "activeworkspace", true, cmd_activeworkspace },
    { "workspacerules", true, cmd_empty_list },
    { "clients", true, cmd_clients },
    { "activewindow", true, cmd_activewindow },
    { "layers", true, cmd_layers },
    { "devices", true, cmd_devices },
    { "cursorpos", true, cmd_cursorpos },
    { "binds", true, cmd_empty_list },
    { "globalshortcuts", true, cmd_empty_list },
    { "layouts", true, cmd_empty_list },
    { "configerrors", true, cmd_empty_list },
    { "animations", true, cmd_animations },
    { "splash", true, cmd_splash },
    { "submap", true, cmd_submap },
    { "reload", false, cmd_ok },
    { "dispatch", false, cmd_dispatch },
};

/* strips "[flags]/" off the request, a space before any '/' means no flags */
static char *parse_flags(char *request, struct hypr_flags *flags)
{
    while (*request == ' ' || *request == '\n' || *request == '\t') {
        request++;
    }
    size_t len = strlen(request);
    while (len && (request[len - 1] == '\n' || request[len - 1] == ' ')) {
        request[--len] = '\0';
    }

    size_t sep = 0;
    for (; request[sep]; sep++) {
        if (request[sep] == '/') {
            break;
        }
        if (request[sep] == ' ') {
            sep = len;
            break;
        }
    }
    if (sep < len) {
        for (size_t i = 0; i < sep; i++) {
            if (request[i] == 'j') {
                flags->json = true;
            } else if (request[i] == 'a') {
                flags->all = true;
            }
        }
        request += sep + 1;
    }
    return request;
}

static void handle_one_request(struct hypr_ipc *ipc, char *request, struct hypr_flags flags,
                               struct sbuf *out)
{
    request = parse_flags(request, &flags);

    for (size_t i = 0; i < sizeof(hypr_commands) / sizeof(hypr_commands[0]); i++) {
        const char *name = hypr_commands[i].name;
        size_t n = strlen(name);
        bool match = hypr_commands[i].exact ? !strcmp(request, name)
                                            : !strncmp(request, name, n) &&
                                                  (request[n] == '\0' || request[n] == ' ');
        if (match) {
            hypr_commands[i].func(ipc, &flags, request[n] ? request + n + 1 : "", out);
            return;
        }
    }
    sbuf_puts(out, "unknown request");
}

static void handle_request(struct hypr_ipc *ipc, char *request, struct sbuf *out)
{
    static const char batch_prefix[] = "[[BATCH]]";
    struct hypr_flags flags = { 0 };
    /* hyprctl sends "[[BATCH]]j/a;j/b", also accept "j/[[BATCH]]a;b" */
    if (strncmp(request, batch_prefix, sizeof(batch_prefix) - 1) != 0) {
        request = parse_flags(request, &flags);
        if (strncmp(request, batch_prefix, sizeof(batch_prefix) - 1) != 0) {
            handle_one_request(ipc, request, flags, out);
            return;
        }
    }

    char *saveptr = NULL;
    bool first = true;
    for (char *item = strtok_r(request + sizeof(batch_prefix) - 1, ";", &saveptr); item;
         item = strtok_r(NULL, ";", &saveptr)) {
        if (!first) {
            sbuf_puts(out, "\n\n\n");
        }
        first = false;
        handle_one_request(ipc, item, flags, out);
    }
}

static void request_destroy(struct hypr_request *req)
{
    wl_list_remove(&req->link);
    req->ipc->request_count--;
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
        break;
    }
    request_destroy(req);
}

static void request_process(struct hypr_request *req)
{
    req->replying = true;
    sbuf_puts(&req->in, "");
    if (req->in.data) {
        handle_request(req->ipc, req->in.data, &req->out);
    }
    if (req->out.oom) {
        request_destroy(req);
        return;
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
    req->timer = NULL;
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

static void handle_server_start(struct wl_listener *listener, void *data)
{
    struct hypr_ipc *ipc = wl_container_of(listener, ipc, server_start);
    wl_list_remove(&ipc->server_start.link);
    wl_list_init(&ipc->server_start.link);

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
    free(ipc->announced_ws_name);
    free(ipc->announced_class);
    free(ipc->announced_title);

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
    ipc->next_address = 0x1000;
    ipc->announced_mon_id = -1;
    ipc->announced_fullscreen = -1;
    wl_list_init(&ipc->requests);
    wl_list_init(&ipc->subscribers);
    wl_list_init(&ipc->windows);
    wl_list_init(&ipc->mru);
    wl_list_init(&ipc->monitors);
    wl_list_init(&ipc->workspaces);

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
