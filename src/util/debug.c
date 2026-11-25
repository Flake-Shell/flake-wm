// SPDX-FileCopyrightText: 2024 KylinSoft Co., Ltd.
//
// SPDX-License-Identifier: GPL-1.0-or-later

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>

#include "util/debug.h"
#include "util/logger.h"

/*
 * Backtraces are printed from the crash signal handler, where the heap may be
 * corrupted and the crashed code may hold any libc lock. So no kywc_log(): it
 * takes stdio locks and calls localtime(), which may malloc. Each line is
 * formatted into a stack buffer and written with write(2), like ABSL_RAW_LOG.
 */
static void crash_log(const char *fmt, ...) _KYWC_ATTRIB_PRINTF(1, 2);

static void crash_log(const char *fmt, ...)
{
    char line[512];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    if (len < 0) {
        return;
    }
    /* truncated lines lose their tail, the newline replaces the NUL */
    if (len > (int)sizeof(line) - 1) {
        len = sizeof(line) - 1;
    }
    line[len++] = '\n';

    int fd = logger_get_fd();
    const char *p = line;
    while (len > 0) {
        ssize_t written = write(fd, p, len);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }
        p += written;
        len -= written;
    }
}

#if HAVE_LIBUNWIND

#define UNW_LOCAL_ONLY
#include <libunwind.h>
#include <ucontext.h>

static void walk_cursor(unw_cursor_t *cursor)
{
    unw_proc_info_t pip = { .unwind_info = NULL };
    unw_word_t off, ip;
    Dl_info dlinfo;
    char procname[256];
    const char *filename;

    crash_log("Backtrace:");

    int i = 0;
    int ret = unw_step(cursor);
    while (ret > 0) {
        ret = unw_get_proc_info(cursor, &pip);
        if (ret) {
            crash_log("unw_get_proc_info failed: %s [%d]", unw_strerror(ret), ret);
            break;
        }

        off = 0;
        ret = unw_get_proc_name(cursor, procname, 256, &off);
        if (ret && ret != -UNW_ENOMEM) {
            if (ret != -UNW_EUNSPEC) {
                crash_log("unw_get_proc_name failed: %s [%d]", unw_strerror(ret), ret);
            }
            procname[0] = '?';
            procname[1] = 0;
        }

        if (unw_get_reg(cursor, UNW_REG_IP, &ip) < 0) {
            ip = pip.start_ip + off;
        }
        if (dladdr((void *)(uintptr_t)(ip), &dlinfo) && dlinfo.dli_fname && *dlinfo.dli_fname) {
            filename = dlinfo.dli_fname;
        } else {
            filename = "?";
        }

        if (unw_is_signal_frame(cursor)) {
            crash_log("  %u: <signal handler called>", i++);
        } else {
            crash_log("  %u: %s (%s%s+0x%x) [%p]", i++, filename, procname,
                      ret == -UNW_ENOMEM ? "..." : "", (int)off, (void *)(uintptr_t)(ip));
        }

        ret = unw_step(cursor);
        if (ret < 0) {
            crash_log("unw_step failed: %s [%d]", unw_strerror(ret), ret);
        }
    }
}

void debug_backtrace(void)
{
    unw_context_t context;
    if (unw_getcontext(&context)) {
        crash_log("unw_getcontext failed");
        return;
    }
    unw_cursor_t cursor;
    if (unw_init_local(&cursor, &context)) {
        crash_log("unw_init_local failed");
        return;
    }
    walk_cursor(&cursor);
}

void debug_backtrace_from(void *ucontext)
{
    if (!ucontext) {
        debug_backtrace();
        return;
    }
    unw_cursor_t cursor;
    int ret = unw_init_local2(&cursor, (unw_context_t *)ucontext, UNW_INIT_SIGNAL_FRAME);
    if (ret) {
        crash_log("unw_init_local2 failed: %s [%d]", unw_strerror(ret), ret);
        debug_backtrace();
        return;
    }
    walk_cursor(&cursor);
}

#else /* HAVE_LIBUNWIND */

#include <execinfo.h>

#define BACKTRACE_SIZE (256)

/* glibc's backtrace() dlopens libgcc_s on first use, which mallocs; not in a crash */
__attribute__((constructor)) static void prime_backtrace(void)
{
    void *frame;
    backtrace(&frame, 1);
}

void debug_backtrace_from(void *ucontext)
{
    (void)ucontext;
    debug_backtrace();
}

void debug_backtrace(void)
{
    void *array[BACKTRACE_SIZE];
    int size = backtrace(array, BACKTRACE_SIZE);

    crash_log("Backtrace:");

    Dl_info info;
    const char *mod;

    for (int i = 0; i < size; i++) {
        if (dladdr(array[i], &info) == 0) {
            crash_log("  %u: ?? [%p]", i, array[i]);
            continue;
        }
        mod = (info.dli_fname && *info.dli_fname) ? info.dli_fname : "(vdso)";
        if (info.dli_saddr) {
            crash_log("  %u: %s (%s+0x%x) [%p]", i, mod, info.dli_sname,
                      (unsigned int)((char *)array[i] - (char *)info.dli_saddr), array[i]);
        } else {
            crash_log("  %u: %s (%p+0x%x) [%p]", i, mod, info.dli_fbase,
                      (unsigned int)((char *)array[i] - (char *)info.dli_fbase), array[i]);
        }
    }
}

#endif
