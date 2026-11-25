// Copyright (C) 2026 CharOfString <root@charofstring.cc>
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version.
//
// This program is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
// details.
//
// You should have received a copy of the GNU General Public License along with
// this program. If not, see <https://www.gnu.org/licenses/>.
// -----------------------------------------------------------------------------
// Bridges absl/log, used by the C++ sources, to the kywc logger that the C
// sources use (src/util/logger.c).

#ifndef SRC_UTIL_KYWC_LOG_SINK_H_
#define SRC_UTIL_KYWC_LOG_SINK_H_

#include "absl/base/log_severity.h"
#include "absl/log/log_entry.h"
#include "absl/log/log_sink.h"
#include "src/c_compact.h"

namespace flakewm {

// Forwards every absl/log entry to kywc_log(), so C++ and C messages share one
// output (stdout or ~/.log/flakewm.log), one level filter (including
// KYWC_LOG_LEVEL and logger_set_level()), the Hyprland IPC rolling log and the
// logger tap. VLOG(n) with n >= 1 maps to KYWC_DEBUG.
//
// Thread-safe as far as kywc_log() is. Not async-signal-safe: signal handlers
// must use ABSL_RAW_LOG, which bypasses all sinks.
class KywcLogSink final : public absl::LogSink {
 public:
  KywcLogSink() = default;
  KywcLogSink(const KywcLogSink&) = delete;
  KywcLogSink& operator=(const KywcLogSink&) = delete;
  ~KywcLogSink() override = default;

  void Send(const absl::LogEntry& entry) override;
};

// Owns the kywc logger: logger_init() on construction, logger_finish() on
// destruction, with a KywcLogSink registered in between. While it lives,
// absl/log does not write to stderr itself, so nothing is printed twice.
//
// logger_finish() closes the log file, so the sink is removed first; entries
// logged afterwards go to stderr again.
//
// Create exactly one, in main(), once the command line has been parsed.
class ScopedKywcLogger final {
 public:
  ScopedKywcLogger(kywc_log_level level, bool log_to_file,
                   bool log_in_realtime);
  ScopedKywcLogger(const ScopedKywcLogger&) = delete;
  ScopedKywcLogger& operator=(const ScopedKywcLogger&) = delete;
  ~ScopedKywcLogger();

 private:
  KywcLogSink sink_;
  const absl::LogSeverityAtLeast saved_stderr_threshold_;
};

}  // namespace flakewm

#endif  // SRC_UTIL_KYWC_LOG_SINK_H_
