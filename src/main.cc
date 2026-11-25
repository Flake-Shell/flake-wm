// Copyright (C) 2026 CharOfString <root@charofstring.cc>
// Original C source copyright (C) 2023 KylinSoft Co., Ltd.
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
// The flakewm entry point: parses the command line, sets up logging and signal
// handling, runs the compositor and supervises the session process.

#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-server-core.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>

#include "absl/base/internal/raw_logging.h"
#include "absl/base/log_severity.h"
#include "absl/log/globals.h"
#include "absl/log/initialize.h"
#include "absl/log/log.h"
#include "src/c_compact.h"
#include "src/util/kywc_log_sink.h"

#if HAVE_NLS
#include <libintl.h>
#include <locale.h>
#endif

namespace flakewm {
namespace {

// Everything a signal handler touches must be lock-free.
static_assert(std::atomic<bool>::is_always_lock_free);
static_assert(std::atomic<int>::is_always_lock_free);
static_assert(std::atomic<pid_t>::is_always_lock_free);
static_assert(std::atomic<uid_t>::is_always_lock_free);

constexpr int kSessionXwaylandReadyTimeoutMs = 5000;

// Long-only options, outside the char range.
enum LongOption : int {
  kNoDefaultKeybind = 0x100,
  kEmergencyTermKeybind,
  kHyprUnixSocketSim,
};

constexpr option kLongOptions[] = {
    {"help", no_argument, nullptr, 'h'},
    {"debug", no_argument, nullptr, 'd'},
    {"version", no_argument, nullptr, 'v'},
    {"session", required_argument, nullptr, 's'},
    {"verbose", no_argument, nullptr, 'V'},
    {"no-default-keybind", no_argument, nullptr, kNoDefaultKeybind},
    {"emergency-term-keybind", no_argument, nullptr, kEmergencyTermKeybind},
    {"hypr-unix-socket-sim-enable", no_argument, nullptr, kHyprUnixSocketSim},
    {nullptr, 0, nullptr, 0},
};

constexpr char kUsage[] = R"(Usage: flakewm [options] [command]

  -h, --help               Show help message and quit.
  -d, --debug              Enables full logging, including debug information.
  -D, --debug <options>    noxwayland, logtostdout, logtofile or loginmtime.
  -s, --session <process>  Run session on startup.
  -v, --version            Show the version number and quit.
  -V, --verbose            Enables more verbose logging.
      --no-default-keybind Register no key bindings on startup, neither the
                           built-in ones nor those in the InputAction config.
                           Bindings added at runtime (InputAction D-Bus,
                           KGlobalAccel) still work.
      --emergency-term-keybind
                           Bind Win+q to open a terminal.
      --hypr-unix-socket-sim-enable
                           Serve Hyprland-compatible IPC sockets and export
                           HYPRLAND_INSTANCE_SIGNATURE (off by default).

)";

constexpr int kCrashSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};

// Main-thread state. Signal handlers never touch it.
constinit server compositor = {};
constinit int exit_value = 0;
constinit bool session_start_scheduled = false;
constinit wl_listener session_xwayland_ready = {};
constinit wl_event_source* session_xwayland_timeout = nullptr;
constinit bool termination_handled = false;

// Self-pipe for the asynchronous signals (SIGINT, SIGCHLD). The kernel runs
// their handlers on any thread that does not block them, concurrently with the
// main thread, so the handlers only write a byte here. The event loop reads it
// on the main thread (HandleSignalPipe) and does the actual work.
//
// The write end is never closed: a handler on another thread may be about to
// write to it, and once closed the number could be reused by an unrelated fd.
// The process exit closes it.
constinit int signal_pipe_read_fd = -1;
constinit std::atomic<int> signal_pipe_write_fd{-1};
constinit wl_event_source* signal_source = nullptr;

// The first termination signal and its sender. The handler that wins
// `termination_claimed` fills in the fields, then publishes the signal number.
constinit std::atomic<bool> termination_claimed{false};
constinit std::atomic<int> termination_signal{0};
constinit std::atomic<pid_t> termination_sender{0};
constinit std::atomic<uid_t> termination_sender_uid{0};
constinit std::atomic<bool> termination_from_user{false};

void InitializeState() {
  compositor.options.enable_xwayland = true;
  compositor.options.log_to_file = false;
  compositor.options.log_in_realtime = true;
  compositor.options.binding_session = true;
  compositor.session_pid = -1;
  // ScheduleSessionStart() checks this link, whichever path calls it.
  wl_list_init(&session_xwayland_ready.link);
}

bool DetectSuid() {
  if (geteuid() != 0 && getegid() != 0) {
    return false;
  }
  if (getuid() == geteuid() && getgid() == getegid()) {
    return false;
  }
  LOG(ERROR) << "SUID operation is no longer supported, refusing to start.";
  return true;
}

void EnableDebugFlag(std::string_view flag) {
  if (flag == "noxwayland") {
    compositor.options.enable_xwayland = false;
  } else if (flag == "logtostdout") {
    compositor.options.log_to_file = false;
  } else if (flag == "logtofile") {
    compositor.options.log_to_file = true;
  } else if (flag == "loginmtime") {
    compositor.options.log_in_realtime = false;
  } else if (flag == "nosessionbinding") {
    compositor.options.binding_session = false;
  } else {
    LOG(WARNING) << "Unknown debug flag: " << flag;
  }
}

// Leaves at once while there is no display yet, otherwise stops the event loop
// and lets Run() tear the compositor down. Main thread only.
void RequestExit(int exit_code) {
  if (compositor.display == nullptr) {
    std::exit(exit_code);
  }
  if (!compositor.terminate) {
    exit_value = exit_code;
    wl_display_terminate(compositor.display);
  }
}

// SA_RESTART keeps asynchronous signals from failing unrelated syscalls with
// EINTR. The crash signals are synchronous, so it does not matter for them.
void InstallSignalAction(int signo, void (*action)(int, siginfo_t*, void*)) {
  struct sigaction act = {};
  sigemptyset(&act.sa_mask);
  sigaddset(&act.sa_mask, signo);
  act.sa_sigaction = action;
  act.sa_flags = SA_SIGINFO | SA_RESTART;
  sigaction(signo, &act, nullptr);
}

// `disposition` is SIG_DFL or SIG_IGN.
void SetSignalDisposition(int signo, void (*disposition)(int)) {
  struct sigaction act = {};
  sigemptyset(&act.sa_mask);
  act.sa_handler = disposition;
  sigaction(signo, &act, nullptr);
}

// strsignal() may allocate and use the locale, so signal handlers use these
// static names for the signals HandleSignal() is installed for.
const char* SignalName(int signo) {
  switch (signo) {
    case SIGINT:
      return "SIGINT";
    case SIGSEGV:
      return "SIGSEGV";
    case SIGBUS:
      return "SIGBUS";
    case SIGILL:
      return "SIGILL";
    case SIGFPE:
      return "SIGFPE";
    case SIGABRT:
      return "SIGABRT";
    default:
      return "signal";
  }
}

bool IsCrashSignal(int signo) {
  for (const int crash_signal : kCrashSignals) {
    if (signo == crash_signal) {
      return true;
    }
  }
  return false;
}

// Async-signal-safe. Only a wakeup: HandleSignalPipe() re-checks all state, so
// a byte dropped because the pipe is full loses nothing.
void WakeEventLoop() {
  const int saved_errno = errno;
  const int fd = signal_pipe_write_fd.load(std::memory_order_acquire);
  const char byte = 0;
  ssize_t result;
  do {
    result = write(fd, &byte, 1);
  } while (result < 0 && errno == EINTR);
  errno = saved_errno;
}

// Runs in signal context, possibly on another thread: no allocation, no locks,
// no absl/log or kywc_log, no compositor state. ABSL_RAW_LOG writes straight
// to stderr and is safe here.
void HandleSignal(int signo, siginfo_t* info, void* ucontext) {
  const bool from_user = info->si_code == SI_USER;
  if (!from_user && IsCrashSignal(signo)) {
    // Synchronous: this is the faulting thread.
    ABSL_RAW_LOG(ERROR, "Caught %s(%d) at address %p", SignalName(signo), signo,
                 info->si_addr);
    debug_backtrace_from(ucontext);
    // Re-raise the original signal so the core dump keeps the faulting stack
    // instead of the handler's abort() stack.
    SetSignalDisposition(signo, SIG_DFL);
    raise(signo);
    return;
  }

  if (!termination_claimed.exchange(true, std::memory_order_acq_rel)) {
    termination_sender.store(from_user ? info->si_pid : 0,
                             std::memory_order_relaxed);
    termination_sender_uid.store(from_user ? info->si_uid : 0,
                                 std::memory_order_relaxed);
    termination_from_user.store(from_user, std::memory_order_relaxed);
    termination_signal.store(signo, std::memory_order_release);
  }
  WakeEventLoop();
}

void HandleChildSignal(int /*signo*/, siginfo_t* /*info*/, void* /*ucontext*/) {
  WakeEventLoop();
}

void HandleTerminationSignal() {
  const int signo = termination_signal.load(std::memory_order_acquire);
  // Zero while the winning handler is still filling in the sender.
  if (signo == 0 || termination_handled) {
    return;
  }
  termination_handled = true;

  if (termination_from_user.load(std::memory_order_relaxed)) {
    LOG(WARNING) << "Received signal " << strsignal(signo) << "(" << signo
                 << ") sent by process "
                 << termination_sender.load(std::memory_order_relaxed)
                 << ", uid "
                 << termination_sender_uid.load(std::memory_order_relaxed);
  } else {
    LOG(WARNING) << "Received signal " << strsignal(signo) << "(" << signo
                 << ") by Ctrl+C";
  }
  RequestExit(EXIT_SUCCESS);
}

void ReapSession() {
  if (compositor.session_pid <= 0) {
    return;
  }

  int status = 0;
  pid_t pid;
  do {
    pid = waitpid(compositor.session_pid, &status, WNOHANG);
  } while (pid < 0 && errno == EINTR);
  // Never reap wlroots' or other subsystems' children.
  if (pid <= 0) {
    return;
  }

  if (WIFEXITED(status)) {
    LOG(ERROR) << "session " << pid << " exited with " << WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    LOG(ERROR) << "session " << pid << " terminated with signal "
               << WTERMSIG(status) << " (" << strsignal(WTERMSIG(status))
               << ")";
  }
  compositor.session_pid = -1;

  if (compositor.options.binding_session) {
    LOG(ERROR) << "flakewm abort...";
    RequestExit(EXIT_SUCCESS);
  }
}

int HandleSignalPipe(int fd, uint32_t /*mask*/, void* /*data*/) {
  char bytes[64];
  while (read(fd, bytes, sizeof(bytes)) > 0) {
  }
  ReapSession();
  HandleTerminationSignal();
  return 0;
}

// Before any asynchronous handler is installed, so they always have a valid fd.
bool CreateSignalPipe() {
  int fds[2];
  if (pipe2(fds, O_NONBLOCK | O_CLOEXEC) < 0) {
    return false;
  }
  signal_pipe_read_fd = fds[0];
  signal_pipe_write_fd.store(fds[1], std::memory_order_release);
  return true;
}

// Only scheduled when compositor.session_process is set.
void StartSession(void* /*data*/) {
  compositor.session_pid = spawn_session(compositor.session_process);
  if (compositor.session_pid < 0) {
    LOG(ERROR) << "session " << compositor.session_process << " start failed"
               << (compositor.options.binding_session ? ", abort" : "");
    if (compositor.options.binding_session) {
      RequestExit(EXIT_FAILURE);
    }
  } else {
    LOG(INFO) << "session " << compositor.session_process << "("
              << compositor.session_pid << ") started";
  }
}

void ScheduleSessionStart() {
  if (session_start_scheduled) {
    return;
  }
  session_start_scheduled = true;

  if (!wl_list_empty(&session_xwayland_ready.link)) {
    wl_list_remove(&session_xwayland_ready.link);
    wl_list_init(&session_xwayland_ready.link);
  }
  if (session_xwayland_timeout != nullptr) {
    wl_event_source_remove(session_xwayland_timeout);
    session_xwayland_timeout = nullptr;
  }

  wl_event_loop_add_idle(compositor.event_loop, StartSession, nullptr);
}

void HandleXwaylandReadyForSession(wl_listener* /*listener*/, void* /*data*/) {
  ScheduleSessionStart();
}

int HandleXwaylandReadyTimeout(void* /*data*/) {
  LOG(WARNING) << "XWayland did not become ready in time; starting the "
                  "session anyway";
  ScheduleSessionStart();
  return 0;
}

void ScheduleSessionAfterXwayland() {
  session_xwayland_ready.notify = HandleXwaylandReadyForSession;
  if (!xwayland_server_wait_ready(&session_xwayland_ready)) {
    ScheduleSessionStart();
    return;
  }

  session_xwayland_timeout = wl_event_loop_add_timer(
      compositor.event_loop, HandleXwaylandReadyTimeout, nullptr);
  if (session_xwayland_timeout == nullptr ||
      wl_event_source_timer_update(session_xwayland_timeout,
                                   kSessionXwaylandReadyTimeoutMs) < 0) {
    LOG(WARNING) << "failed to arm XWayland startup timeout";
    ScheduleSessionStart();
  }
}

void StartVirtualBoxWaylandHelper() {
  if (access("/dev/vboxguest", F_OK) < 0 ||
      access("/usr/bin/VBoxClient", X_OK) < 0) {
    return;
  }

  // For VirtualBox, make sure VBoxClient is running so that screen resizing
  // works.
  if (!spawn_invoke("/usr/bin/VBoxClient --vmsvga")) {
    LOG(ERROR) << "failed to start VirtualBox display resize daemon";
  }
  if (!spawn_invoke("/usr/bin/VBoxClient --vmsvga-session")) {
    LOG(ERROR) << "failed to start VirtualBox Wayland display assistant";
  }
}

// Brings the compositor up to the point where server_run() takes over.
bool StartCompositor() {
  if (!server_init(&compositor)) {
    return false;
  }
  // wl_event_loop_add_fd() keeps its own duplicate of the fd. A signal that
  // arrived during server_init() is already waiting in the pipe.
  signal_source =
      wl_event_loop_add_fd(compositor.event_loop, signal_pipe_read_fd,
                           WL_EVENT_READABLE, HandleSignalPipe, nullptr);
  if (signal_source == nullptr || !server_start(&compositor)) {
    return false;
  }
  // WAYLAND_DISPLAY is set by server_start().
  StartVirtualBoxWaylandHelper();
  InstallSignalAction(SIGCHLD, HandleChildSignal);
  if (compositor.session_process != nullptr) {
    ScheduleSessionAfterXwayland();
  }
  return true;
}

void StopSession() {
  if (compositor.session_pid > 0 && kill(compositor.session_pid, SIGTERM) < 0) {
    kill(compositor.session_pid, SIGKILL);
  }
}

void Shutdown() {
  SetSignalDisposition(SIGCHLD, SIG_DFL);
  if (signal_source != nullptr) {
    wl_event_source_remove(signal_source);
    signal_source = nullptr;
  }
  // Later wakeups fail with EPIPE; SIGPIPE is ignored. The write end stays
  // open, see signal_pipe_write_fd.
  if (signal_pipe_read_fd >= 0) {
    close(signal_pipe_read_fd);
    signal_pipe_read_fd = -1;
  }
  server_finish(&compositor);
}

kywc_log_level LogLevelFor(bool enable_debug, bool enable_verbose) {
  if (enable_debug) {
    return KYWC_DEBUG;
  }
  if (enable_verbose) {
    return KYWC_INFO;
  }
  return KYWC_WARN;
}

int Run(int argc, char* argv[]) {
  // Until ScopedKywcLogger takes over, absl/log writes warnings to stderr.
  absl::InitializeLog();
  absl::SetStderrThreshold(absl::LogSeverityAtLeast::kWarning);

#if HAVE_NLS
  setlocale(LC_ALL, "");
  bindtextdomain(GETTEXT_PACKAGE, LOCALEDIR);
  textdomain(GETTEXT_PACKAGE);
#endif

  InitializeState();
  bool enable_debug = false;
  bool enable_verbose = false;
  while (true) {
    const int c = getopt_long(argc, argv, "hdD:s:vV", kLongOptions, nullptr);
    if (c == -1) {
      break;
    }
    switch (c) {
      case 'h':
        std::fputs(kUsage, stdout);
        return EXIT_SUCCESS;
      case 'd':
        enable_debug = true;
        break;
      case 'D':  // Extended debug options.
        EnableDebugFlag(optarg);
        break;
      case 's':
        // Points into argv, which outlives the compositor.
        compositor.session_process = optarg;
        break;
      case 'v':
        std::printf("flakewm version %s\n", KYWC_VERSION);
        return EXIT_SUCCESS;
      case 'V':
        enable_verbose = true;
        break;
      case kNoDefaultKeybind:
        compositor.options.no_default_keybind = true;
        break;
      case kEmergencyTermKeybind:
        compositor.options.emergency_term_keybind = true;
        break;
      case kHyprUnixSocketSim:
        compositor.options.enable_hypr_unix_socket_sim = true;
        break;
      default:
        std::fputs(kUsage, stderr);
        return EXIT_FAILURE;
    }
  }

  // SUID operation is deprecated, so block it for now.
  if (DetectSuid()) {
    return EXIT_FAILURE;
  }

  // Declared before anything that logs during shutdown, so it is destroyed,
  // and the log file closed, last.
  const ScopedKywcLogger logger(LogLevelFor(enable_debug, enable_verbose),
                                compositor.options.log_to_file,
                                compositor.options.log_in_realtime);
  LOG(INFO) << "flakewm " << KYWC_VERSION << " starting...";

  // Raise the open files limit to the maximum.
  limit_set_nofile();

  SetSignalDisposition(SIGPIPE, SIG_IGN);
  if (!CreateSignalPipe()) {
    PLOG(ERROR) << "failed to create the signal pipe";
    return EXIT_FAILURE;
  }
  InstallSignalAction(SIGINT, HandleSignal);
  for (const int signo : kCrashSignals) {
    InstallSignalAction(signo, HandleSignal);
  }

  if (StartCompositor()) {
    server_run(&compositor);
    SetSignalDisposition(SIGCHLD, SIG_DFL);
    StopSession();
  } else {
    RequestExit(EXIT_FAILURE);
  }

  Shutdown();
  return exit_value;
}

}  // namespace
}  // namespace flakewm

int main(int argc, char* argv[]) { return flakewm::Run(argc, argv); }
