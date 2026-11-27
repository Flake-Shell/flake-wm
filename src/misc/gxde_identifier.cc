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

#include "src/misc/gxde_identifier.h"

#include <wayland-server-core.h>

#include <cstdint>
#include <memory>
#include <new>

#include "absl/log/log.h"
#include "protocols/gxde-identifier-v1-protocol.h"
#include "src/c_compact.h"

namespace flakewm {
namespace {

constexpr int kGlobalVersion = 1;

// Owned by the display: deleted by its destroy listener. Standard layout, so
// that wl_container_of() can get from the listener back to it.
struct GxdeIdentifier {
  wl_global* global = nullptr;
  wl_listener display_destroy = {};
};

void HandleRequestDestroy(wl_client* /*client*/, wl_resource* resource) {
  wl_resource_destroy(resource);
}

// The protocol header also names the wl_interface variable
// gxde_identifier_v1_interface, so the struct needs its tag.
constexpr struct gxde_identifier_v1_interface kIdentifierImpl = {
    .destroy = HandleRequestDestroy,
};

void HandleBind(wl_client* client, void* /*data*/, uint32_t version,
                uint32_t id) {
  // libwayland has already clamped version to kGlobalVersion.
  wl_resource* resource = wl_resource_create(
      client, &gxde_identifier_v1_interface, static_cast<int>(version), id);
  if (resource == nullptr) {
    wl_client_post_no_memory(client);
    return;
  }
  // The resource keeps no state, so it needs no user data or destructor.
  wl_resource_set_implementation(resource, &kIdentifierImpl, nullptr, nullptr);
  gxde_identifier_v1_send_version(resource, KYWC_VERSION);
}

void HandleDisplayDestroy(wl_listener* listener, void* /*data*/) {
  auto* identifier = wl_container_of(
      listener, static_cast<GxdeIdentifier*>(nullptr), display_destroy);
  wl_list_remove(&identifier->display_destroy.link);
  wl_global_destroy(identifier->global);
  delete identifier;
}

bool CreateGxdeIdentifier(wl_display* display) {
  // nothrow keeps the C contract: report allocation failure, do not throw
  // through the C caller.
  std::unique_ptr<GxdeIdentifier> identifier(new (std::nothrow)
                                                 GxdeIdentifier());
  if (identifier == nullptr) {
    LOG(ERROR) << "Cannot allocate the gxde_identifier_v1 global";
    return false;
  }

  identifier->global = wl_global_create(display, &gxde_identifier_v1_interface,
                                        kGlobalVersion, nullptr, HandleBind);
  if (identifier->global == nullptr) {
    LOG(WARNING) << "Cannot create the gxde_identifier_v1 global";
    return false;
  }

  identifier->display_destroy.notify = HandleDisplayDestroy;
  // From here on the display owns it.
  wl_display_add_destroy_listener(display,
                                  &identifier.release()->display_destroy);
  LOG(INFO) << "gxde_identifier_v1 created, version " << KYWC_VERSION;
  return true;
}

}  // namespace
}  // namespace flakewm

// The C entry point declared in the header.
// NOLINTNEXTLINE(readability-identifier-naming)
bool gxde_identifier_create(struct server* server) {
  return flakewm::CreateGxdeIdentifier(server->display);
}
