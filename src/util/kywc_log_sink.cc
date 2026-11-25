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

#include "src/util/kywc_log_sink.h"

#include <algorithm>
#include <climits>
#include <cstddef>
#include <string_view>

#include "absl/base/log_severity.h"
#include "absl/log/globals.h"
#include "absl/log/log_entry.h"
#include "absl/log/log_sink_registry.h"
#include "src/c_compact.h"

namespace flakewm {
namespace {

kywc_log_level ToKywcLevel(const absl::LogEntry& entry) {
  if (entry.verbosity() > 0) {
    return KYWC_DEBUG;
  }
  switch (entry.log_severity()) {
    case absl::LogSeverity::kInfo:
      return KYWC_INFO;
    case absl::LogSeverity::kWarning:
      return KYWC_WARN;
    case absl::LogSeverity::kError:
      return KYWC_ERROR;
    case absl::LogSeverity::kFatal:
      return KYWC_FATAL;
  }
  return KYWC_ERROR;
}

// The text is passed to printf as "%.*s", never as the format, and need not be
// NUL-terminated. Precision is an int, and a null pointer is not allowed even
// for zero length.
int PrintfLength(std::string_view text) {
  return static_cast<int>(std::min<size_t>(text.size(), INT_MAX));
}

const char* PrintfData(std::string_view text) {
  return text.data() != nullptr ? text.data() : "";
}

}  // namespace

void KywcLogSink::Send(const absl::LogEntry& entry) {
  const kywc_log_level level = ToKywcLevel(entry);
  if (level > kywc_log_get_level()) {
    return;
  }
  // Both views point into `entry` and are only used before this returns.
  const std::string_view file = entry.source_basename();
  const std::string_view message = entry.text_message();
  kywc_log(level, "[%.*s:%d] %.*s", PrintfLength(file), PrintfData(file),
           entry.source_line(), PrintfLength(message), PrintfData(message));
}

ScopedKywcLogger::ScopedKywcLogger(kywc_log_level level, bool log_to_file,
                                   bool log_in_realtime)
    : saved_stderr_threshold_(absl::StderrThreshold()) {
  logger_init(level, log_to_file, log_in_realtime);
  // Read the level back: KYWC_LOG_LEVEL may override the requested one.
  if (kywc_log_get_level() >= KYWC_DEBUG) {
    absl::SetGlobalVLogLevel(1);
  }
  absl::AddLogSink(&sink_);
  absl::SetStderrThreshold(absl::LogSeverityAtLeast::kInfinity);
}

ScopedKywcLogger::~ScopedKywcLogger() {
  // Restore stderr before detaching the sink, so no entry is lost in between.
  absl::SetStderrThreshold(saved_stderr_threshold_);
  absl::RemoveLogSink(&sink_);
  logger_finish();
}

}  // namespace flakewm
