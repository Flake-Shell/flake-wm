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
// The requests, all of the existing dde_shell protocol of the host:
//  - set_property(NoTitleBar) and request_window_effect(NoBorder, NoRadius,
//    NoShadow). The host keeps them and applies them when the window maps.
//  - set_state(fullscreen). The host only honours it on a mapped window, so
//    it is sent after the first commit with a buffer. The host handles
//    requests in order, so the window is mapped by the time it reads it.
//
// This is a Wayland client of the host display that the wlroots wayland
// backend owns. The backend destroys its outputs before it disconnects that
// display, so every proxy here is gone before the display is.

#include "src/output/lsg_immersive_client/lsg_immersive_client.h"

#include <wayland-client.h>
#include <wayland-util.h>

#include <cstdint>
#include <memory>
#include <new>
#include <string_view>
#include <utility>

#include "absl/log/log.h"
#include "protocols/dde-shell-client-protocol.h"

// Exported by wlroots. <wlr/backend/wayland.h> declares them but pulls in
// <wlr/render/color.h>, whose [static N] array parameters are not C++.
extern "C" {
// NOLINTNEXTLINE(readability-identifier-naming)
bool wlr_output_is_wl(wlr_output* output);
// NOLINTNEXTLINE(readability-identifier-naming)
wl_surface* wlr_wl_output_get_surface(wlr_output* output);
// NOLINTNEXTLINE(readability-identifier-naming)
wl_display* wlr_wl_backend_get_remote_display(wlr_backend* backend);
}

namespace flakewm {
namespace output {
namespace {

/// The dde_shell version that has set_property and request_window_effect.
constexpr uint32_t kDdeShellVersion = 2;

/// Destroys a dde_shell_surface proxy. The protocol has no destructor request,
/// so this only drops the proxy; libwayland ignores later events for it.
struct SurfaceDeleter {
  void operator()(dde_shell_surface* surface) const {
    dde_shell_surface_destroy(surface);
  }
};

/// Destroys a private event queue.
struct EventQueueDeleter {
  void operator()(wl_event_queue* queue) const {
    wl_event_queue_destroy(queue);
  }
};

using DdeShellSurfacePtr = std::unique_ptr<dde_shell_surface, SurfaceDeleter>;
using EventQueuePtr = std::unique_ptr<wl_event_queue, EventQueueDeleter>;

/// The dde_shell global of the host, shared by all nested outputs. Trivially
/// destructible, so it may have static storage duration.
struct HostDdeShell {
  /// The host display the proxy belongs to.
  wl_display* remote = nullptr;
  /// The bound global, nullptr when not bound.
  dde_shell* shell = nullptr;
  /// The number of outputs using it.
  int users = 0;
};

HostDdeShell host_dde_shell;

void HandleGlobal(void* data, wl_registry* registry, uint32_t name,
                  const char* interface, uint32_t version) {
  auto* shell = static_cast<dde_shell**>(data);
  if (*shell != nullptr || version < kDdeShellVersion ||
      std::string_view(interface) != dde_shell_interface.name) {
    return;
  }
  *shell = static_cast<dde_shell*>(
      wl_registry_bind(registry, name, &dde_shell_interface, kDdeShellVersion));
}

void HandleGlobalRemove(void* /*data*/, wl_registry* /*registry*/,
                        uint32_t /*name*/) {}

constexpr wl_registry_listener kRegistryListener = {
    .global = HandleGlobal,
    .global_remove = HandleGlobalRemove,
};

/**
 * @brief Binds the dde_shell global of the host.
 *
 * The roundtrip runs on a private queue: one on the default queue would
 * dispatch the backend's own events from inside output creation.
 *
 * @param remote The host display.
 * @return The bound global on the default queue, or nullptr.
 */
dde_shell* BindDdeShell(wl_display* remote) {
  const EventQueuePtr queue(wl_display_create_queue(remote));
  if (queue == nullptr) {
    return nullptr;
  }
  auto* wrapper = static_cast<wl_display*>(wl_proxy_create_wrapper(remote));
  if (wrapper == nullptr) {
    return nullptr;
  }
  wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(wrapper), queue.get());
  wl_registry* registry = wl_display_get_registry(wrapper);
  wl_proxy_wrapper_destroy(wrapper);
  if (registry == nullptr) {
    return nullptr;
  }

  dde_shell* shell = nullptr;
  wl_registry_add_listener(registry, &kRegistryListener,
                           static_cast<void*>(&shell));
  const int result = wl_display_roundtrip_queue(remote, queue.get());
  wl_registry_destroy(registry);
  if (result < 0) {
    LOG(ERROR) << "lsg immersive client: roundtrip to the host failed";
    if (shell != nullptr) {
      dde_shell_destroy(shell);
    }
    return nullptr;
  }
  if (shell != nullptr) {
    // Before the queue goes: events of the global and of its surfaces are
    // then dispatched by the backend.
    wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(shell), nullptr);
  }
  return shell;
}

/**
 * @brief Takes a reference on the dde_shell global of the host.
 * @param remote The host display.
 * @return The global, or nullptr if the host has none. Release it with
 * ReleaseDdeShell() when not nullptr.
 */
dde_shell* AcquireDdeShell(wl_display* remote) {
  if (host_dde_shell.shell == nullptr) {
    host_dde_shell.shell = BindDdeShell(remote);
    host_dde_shell.remote = remote;
  } else if (host_dde_shell.remote != remote) {
    // The wayland backend connects to one host only.
    LOG(WARNING) << "lsg immersive client: second host display ignored";
    return nullptr;
  }
  if (host_dde_shell.shell != nullptr) {
    ++host_dde_shell.users;
  }
  return host_dde_shell.shell;
}

/// Drops a reference taken by AcquireDdeShell(), the last one destroys it.
void ReleaseDdeShell() {
  if (--host_dde_shell.users > 0) {
    return;
  }
  dde_shell_destroy(host_dde_shell.shell);
  host_dde_shell = HostDdeShell();
}

/**
 * @brief The dde_shell state of one nested output.
 *
 * Holds a reference on the host's dde_shell global from creation to
 * destruction.
 */
class LsgImmersiveClient {
 public:
  /**
   * @brief Takes over a dde_shell_surface of a host window.
   * @param remote The host display.
   * @param shell_surface The surface, created from an acquired global.
   */
  LsgImmersiveClient(wl_display* remote, DdeShellSurfacePtr shell_surface)
      : remote_(remote), shell_surface_(std::move(shell_surface)) {}
  ~LsgImmersiveClient() {
    // The surface proxy goes before the global it was created from.
    shell_surface_.reset();
    ReleaseDdeShell();
  }
  LsgImmersiveClient(const LsgImmersiveClient&) = delete;
  LsgImmersiveClient& operator=(const LsgImmersiveClient&) = delete;
  LsgImmersiveClient(LsgImmersiveClient&&) = delete;
  LsgImmersiveClient& operator=(LsgImmersiveClient&&) = delete;

  /// Asks for the titlebar, border, corners and shadow to go.
  void Undecorate() {
    // NoTitleBar takes an int32: 1 says the client draws its own titlebar,
    // which here means none at all. The array only borrows the value.
    int32_t no_titlebar = 1;
    wl_array value = {
        .size = sizeof(no_titlebar),
        .alloc = sizeof(no_titlebar),
        .data = &no_titlebar,
    };
    dde_shell_surface_set_property(shell_surface_.get(),
                                   DDE_SHELL_PROPERTY_NOTITLEBAR, &value);
    // Border, rounded corners and shadow. The enum values are int.
    const uint32_t disabled_effects =
        static_cast<uint32_t>(DDE_SHELL_EFFECTSCENE_EFFECTNOBORDER) |
        static_cast<uint32_t>(DDE_SHELL_EFFECTSCENE_EFFECTNORADIUS) |
        static_cast<uint32_t>(DDE_SHELL_EFFECTSCENE_EFFECTNOSHADOW);
    dde_shell_surface_request_window_effect(shell_surface_.get(),
                                            disabled_effects);
    Flush();
  }

  /// Asks for fullscreen once, after the first buffer mapped the window.
  void BufferCommitted() {
    if (fullscreen_requested_) {
      return;
    }
    fullscreen_requested_ = true;
    dde_shell_surface_set_state(shell_surface_.get(),
                                DDE_SHELL_STATE_FULLSCREEN,
                                DDE_SHELL_STATE_FULLSCREEN);
    Flush();
  }

 private:
  /// Sends the requests now, the backend flushes only when it commits.
  void Flush() {
    if (wl_display_flush(remote_) < 0) {
      // The backend sees the broken connection and tears the outputs down.
      LOG(WARNING) << "lsg immersive client: flushing to the host failed";
    }
  }

  wl_display* remote_;
  DdeShellSurfacePtr shell_surface_;
  bool fullscreen_requested_ = false;
};

/**
 * @brief Creates the state of a new nested output.
 * @param wlr_output The output.
 * @param backend The backend of the output.
 * @param name The name of the output, may be NULL.
 * @return The state, or nullptr when there is nothing to do.
 */
std::unique_ptr<LsgImmersiveClient> CreateLsgImmersiveClient(
    wlr_output* wlr_output, wlr_backend* backend, const char* name) {
  if (!wlr_output_is_wl(wlr_output)) {
    return nullptr;
  }
  wl_surface* surface = wlr_wl_output_get_surface(wlr_output);
  wl_display* remote = wlr_wl_backend_get_remote_display(backend);
  if (surface == nullptr || remote == nullptr) {
    return nullptr;
  }
  dde_shell* shell = AcquireDdeShell(remote);
  if (shell == nullptr) {
    LOG(WARNING) << "lsg immersive client: the host has no dde_shell v"
                 << kDdeShellVersion << ", keeping its decorations";
    return nullptr;
  }
  DdeShellSurfacePtr shell_surface(dde_shell_get_shell_surface(shell, surface));
  if (shell_surface == nullptr) {
    ReleaseDdeShell();
    return nullptr;
  }
  // nothrow keeps the C contract: report allocation failure, do not throw
  // through the C caller.
  std::unique_ptr<LsgImmersiveClient> client(
      new (std::nothrow) LsgImmersiveClient(remote, std::move(shell_surface)));
  if (client == nullptr) {
    // The constructor did not run, so shell_surface still owns the proxy.
    shell_surface.reset();
    ReleaseDdeShell();
    return nullptr;
  }
  client->Undecorate();
  LOG(INFO) << "lsg immersive client: asked the host to undecorate "
            << (name != nullptr ? name : "an output");
  return client;
}

}  // namespace
}  // namespace output
}  // namespace flakewm

// The opaque C type: the state itself.
// NOLINTNEXTLINE(readability-identifier-naming)
struct lsg_immersive_client {
  std::unique_ptr<flakewm::output::LsgImmersiveClient> client;
};

// The C entry points declared in the header, named like the C code.
// NOLINTNEXTLINE(readability-identifier-naming)
lsg_immersive_client* lsg_immersive_client_create(wlr_output* wlr_output,
                                                  wlr_backend* backend,
                                                  const char* name) {
  std::unique_ptr<flakewm::output::LsgImmersiveClient> client =
      flakewm::output::CreateLsgImmersiveClient(wlr_output, backend, name);
  if (client == nullptr) {
    return nullptr;
  }
  auto* handle = new (std::nothrow) lsg_immersive_client{std::move(client)};
  if (handle == nullptr) {
    // Aggregate initialization did not run, client still owns the state.
    LOG(ERROR) << "lsg immersive client: out of memory";
  }
  return handle;
}

// NOLINTNEXTLINE(readability-identifier-naming)
void lsg_immersive_client_buffer_committed(lsg_immersive_client* client) {
  if (client != nullptr) {
    client->client->BufferCommitted();
  }
}

// NOLINTNEXTLINE(readability-identifier-naming)
void lsg_immersive_client_destroy(lsg_immersive_client* client) {
  delete client;
}
