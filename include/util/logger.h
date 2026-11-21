// SPDX-FileCopyrightText: 2023 KylinSoft Co., Ltd.
//
// SPDX-License-Identifier: GPL-1.0-or-later

#ifndef _UTIL_LOGGER_H_
#define _UTIL_LOGGER_H_

#include <stdbool.h>

#include <kywc/log.h>

void logger_init(enum kywc_log_level level, bool log_to_file, bool in_realtime);

void logger_set_level(enum kywc_log_level level);

void logger_finish(void);

/* the tail of the log output, for the Hyprland IPC rollinglog, free() the result */
char *logger_rolling_dup(void);

/**
 * called with every logged line (no newline), on the main thread only;
 * NULL removes it. Lines logged from inside the tap are not passed to it.
 */
typedef void (*logger_tap_func_t)(const char *line, void *data);
void logger_set_tap(logger_tap_func_t tap, void *data);

#endif /* _LOGGER_H_ */
