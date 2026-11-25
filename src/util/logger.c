// SPDX-FileCopyrightText: 2023 KylinSoft Co., Ltd.
//
// SPDX-License-Identifier: GPL-1.0-or-later

#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "util/file.h"
#include "util/logger.h"
#include "util/string.h"

/* NULL before logger_init() and after logger_finish(): log to stderr then */
static FILE *log_fp = NULL;
/* fileno() of the current output, for crash reports from signal handlers */
static volatile sig_atomic_t log_fd = STDERR_FILENO;
static enum kywc_log_level log_level = KYWC_WARN;
static bool log_in_realtime = true;

static const char *level_colors[] = {
    [KYWC_SILENT] = "",         [KYWC_FATAL] = "\x1B[7;31m", [KYWC_ERROR] = "\x1B[1;31m",
    [KYWC_WARN] = "\x1B[1;35m", [KYWC_INFO] = "\x1B[1;34m",  [KYWC_DEBUG] = "\x1B[1;90m",
};

static const char *level_headers[] = {
    [KYWC_SILENT] = "",     [KYWC_FATAL] = "[FATAL]", [KYWC_ERROR] = "[ERROR]",
    [KYWC_WARN] = "[WARN]", [KYWC_INFO] = "[INFO]",   [KYWC_DEBUG] = "[DEBUG]",
};

#define ROLLING_LOG_SIZE 4096

static struct {
    pthread_mutex_t lock;
    char data[ROLLING_LOG_SIZE];
    size_t len;

    pthread_t main_thread;
    bool main_thread_known;
    logger_tap_func_t tap;
    void *tap_data;
    bool in_tap;
} rolling = { .lock = PTHREAD_MUTEX_INITIALIZER };

static void rolling_append(const char *line, size_t len)
{
    pthread_mutex_lock(&rolling.lock);
    if (len + 1 >= ROLLING_LOG_SIZE) {
        line += len + 2 - ROLLING_LOG_SIZE;
        len = ROLLING_LOG_SIZE - 2;
    }
    size_t need = len + 1;
    if (rolling.len + need > ROLLING_LOG_SIZE - 1) {
        /* drop whole lines from the front */
        size_t drop = rolling.len + need - (ROLLING_LOG_SIZE - 1);
        char *nl = memchr(rolling.data + drop, '\n', rolling.len - drop);
        drop = nl ? (size_t)(nl - rolling.data) + 1 : rolling.len;
        memmove(rolling.data, rolling.data + drop, rolling.len - drop);
        rolling.len -= drop;
    }
    memcpy(rolling.data + rolling.len, line, len);
    rolling.len += len;
    rolling.data[rolling.len++] = '\n';
    rolling.data[rolling.len] = '\0';
    pthread_mutex_unlock(&rolling.lock);
}

static void log_rolling(enum kywc_log_level level, const char *fmt, va_list args)
{
    char line[1024];
    int n = snprintf(line, sizeof(line), "%s ", level_headers[level]);
    if (n < 0) {
        return;
    }
    int m = vsnprintf(line + n, sizeof(line) - n, fmt, args);
    if (m < 0) {
        return;
    }
    size_t len = (size_t)n + (size_t)m;
    if (len >= sizeof(line)) {
        len = sizeof(line) - 1;
    }
    rolling_append(line, len);

    if (rolling.tap && !rolling.in_tap && rolling.main_thread_known &&
        pthread_equal(rolling.main_thread, pthread_self())) {
        rolling.in_tap = true;
        rolling.tap(line, rolling.tap_data);
        rolling.in_tap = false;
    }
}

char *logger_rolling_dup(void)
{
    pthread_mutex_lock(&rolling.lock);
    char *copy = strndup(rolling.data, rolling.len);
    pthread_mutex_unlock(&rolling.lock);
    return copy;
}

void logger_set_tap(logger_tap_func_t tap, void *data)
{
    rolling.tap = tap;
    rolling.tap_data = data;
}

static void set_output(FILE *fp)
{
    log_fp = fp;
    log_fd = fp ? fileno(fp) : STDERR_FILENO;
}

int logger_get_fd(void)
{
    return log_fd;
}

static void log_file(enum kywc_log_level level, const char *fmt, va_list args)
{
    if (level > log_level) {
        return;
    }

    va_list copy;
    va_copy(copy, args);
    log_rolling(level, fmt, copy);
    va_end(copy);

    // FIXME: thread-safe
    FILE *fp = log_fp ? log_fp : stderr;
    if (log_in_realtime) {
        time_t time_log = time(NULL);
        /* localtime() returns a buffer shared by all threads */
        struct tm tm_log = { 0 };
        localtime_r(&time_log, &tm_log);
        fprintf(fp, "[%04d-%02d-%02d %02d:%02d:%02d] %s: ", tm_log.tm_year + 1900,
                tm_log.tm_mon + 1, tm_log.tm_mday, tm_log.tm_hour, tm_log.tm_min, tm_log.tm_sec,
                fp == stdout ? level_colors[level] : level_headers[level]);
    } else {
        struct timespec ts = { 0 };
        clock_gettime(CLOCK_MONOTONIC, &ts);
        fprintf(fp, "[%02d:%02d:%02d.%06ld] %s: ", (int)(ts.tv_sec / 60 / 60),
                (int)(ts.tv_sec / 60 % 60), (int)(ts.tv_sec % 60), ts.tv_nsec / 1000,
                fp == stdout ? level_colors[level] : level_headers[level]);
    }

    vfprintf(fp, fmt, args);
    if (fp == stdout) {
        fprintf(fp, "\x1B[0m");
    }
    fprintf(fp, "\n");

    fflush(fp);
}

void kywc_log(enum kywc_log_level level, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    log_file(level, format, args);
    va_end(args);
}

void kywc_vlog(enum kywc_log_level level, const char *format, va_list args)
{
    log_file(level, format, args);
}

static enum kywc_log_level detect_log_level(enum kywc_log_level level)
{
    /* set logger log_level from env */
    const char *level_str = getenv("KYWC_LOG_LEVEL");
    if (!level_str) {
        return level;
    }

    if (strcmp(level_str, "DEBUG") == 0) {
        return KYWC_DEBUG;
    } else if (strcmp(level_str, "INFO") == 0) {
        return KYWC_INFO;
    } else if (strcmp(level_str, "WARN") == 0) {
        return KYWC_WARN;
    } else if (strcmp(level_str, "ERROR") == 0) {
        return KYWC_ERROR;
    } else if (strcmp(level_str, "FATAL") == 0) {
        return KYWC_FATAL;
    } else if (strcmp(level_str, "SILENT") == 0) {
        return KYWC_SILENT;
    } else {
        fprintf(stderr, "logger: env error, support (DEBUG|INFO|WARN|ERROR|FATAL|SILENT)\n");
        return level;
    }
}

void logger_init(enum kywc_log_level level, bool log_to_file, bool realtime)
{
    rolling.main_thread = pthread_self();
    rolling.main_thread_known = true;
    log_in_realtime = realtime;
    log_level = detect_log_level(level);
    fprintf(stdout, "logger: current log level is %s\n", level_headers[log_level]);

    /* stdout unless the log file below opens */
    set_output(stdout);
    if (!log_to_file) {
        return;
    }

    char *log_dir = string_expand_path("~/.log");
    if (!log_dir) {
        fprintf(stderr, "get log dir failed\n");
        return;
    }

    if (!file_exists(log_dir)) {
        fprintf(stdout, "logger: %s not exist, create it\n", log_dir);
        int ret = mkdir(log_dir, S_IRWXU | S_IRWXG);
        if (ret) {
            fprintf(stderr, "create log dir failed: %s\n", strerror(errno));
            free(log_dir);
            return;
        }
    }

    char *log_path = string_join_path(log_dir, NULL, "flakewm.log");
    free(log_dir);
    fprintf(stdout, "logger: path is %s\n", log_path);

    FILE *fp = fopen(log_path, "a"); // appending mode
    free(log_path);
    if (!fp) {
        fprintf(stderr, "logger: create log file failed: %s\n", strerror(errno));
        return;
    }
    set_output(fp);
}

enum kywc_log_level kywc_log_get_level(void)
{
    return log_level;
}

void logger_set_level(enum kywc_log_level level)
{
    if (level == log_level) {
        return;
    }

    log_level = level;
    kywc_log(KYWC_SILENT, "logger: current log level is %s", level_headers[level]);
}

void logger_finish(void)
{
    FILE *fp = log_fp;
    /* switch to stderr before closing, for later kywc_log() and crash reports */
    set_output(NULL);
    if (!fp) {
        return;
    }

    fflush(fp);
    if (fp != stdout && fp != stderr) {
        fclose(fp);
    }
}
