# GEMINI.md

Instructions for working in the `flakewm` repository.

## Project Overview

`flakewm` is a wlroots-based Wayland compositor maintained by the GXDE OS team.
It was forked from `kylin-wayland-compositor` (`kywc`) and is the default
compositor for GXDE OS Wayland sessions.

- **Language:** C11 (a small optional Qt 6/C++ shim exists for the KWin-style
  window switcher).
- **Build system:** CMake (>= 3.21) + Ninja; wlroots itself is built through
  Meson as an external project.
- **Display stack:** wlroots `0.20.2` (vendored pristine under `libs/wlroots`),
  Wayland, EGL/OpenGL ES, DRM/KMS, libinput, xkbcommon.
- **License:** GPL-3.0-or-later overall (see `COPYING`). Many inherited source
  files carry `SPDX-License-Identifier: GPL-1.0-or-later`; preserve existing
  SPDX headers and add an SPDX header to every new source file.
- **Runtime identity:** sets `WAYLAND_DISPLAY` and
  `DDE_CURRENT_COMPOSITOR=flakewm`; advertises GXDE-specific D-Bus interfaces
  and Wayland protocols.

The tree is a distribution fork, not plain upstream kywc. GXDE-specific work
includes the DDE Shell / deepin-chameleon "Yunli" appearance, `dde-shell` and
Treeland protocol support, Deepin-style multitasking view, clipboard
persistence, GXDE theme/decoration D-Bus APIs, and GXDE session packaging.

## Repository Map

| Path | Purpose |
| --- | --- |
| `src/main.c` | CLI parsing, signal handling, logging setup, session spawning, server lifecycle. |
| `src/server.c` | Creates the Wayland display, D-Bus context, wlroots backend/renderer/allocator, scene, and all subsystem managers. |
| `src/backend/` | Backend auto-creation, including the Linux framebuffer (`fbdev`) backend. |
| `src/config/` | JSON config manager, GTK theme, KDE global-accel/input, UKUI gsettings/shortcut/view-mode bridges. |
| `src/effect/` | Window effects, animations, blur, screenshots, capture, shaders (embedded via CMake). |
| `src/input/` | Seats, keyboard, pointer, touch, tablet, gestures, text input, clipboard, hotkeys, idle handling. |
| `src/output/` | Output manager, layout, modes, gamma/night mode, DRM lease, KDE/wlroots/UKUI output protocol adapters. |
| `src/painter/`, `src/render/` | Painting helpers, renderer abstraction, OpenGL/EGL, SHM and dmabuf paths. |
| `src/scene/` | `ky_scene` custom scene graph: nodes, surfaces, buffers, animations, xdg-shell, thumbnails, fifo/commit timing. |
| `src/view/` | Window/view management, workspaces, xdg-shell, decorations, switchers, session lock, layer shell, KDE/UKUI/Hyprland/DDE/Treeland adapters. |
| `src/shell/gxde/` | Compositor-native multitasking view, present-windows effect, and the optional KWin QML shim. |
| `src/security/`, `src/util/`, `src/widget/`, `src/theme/` | Security contexts, utilities, internal widgets, theming/icons. |
| `src/patches/` | Small compatibility patches/backports for wlroots. `src/patches/wlroots/0.17` is an inactive historical reference. |
| `src/vendor/gxde/portal/` | GXDE portal integration. |
| `include/` | Internal headers mirroring the `src/` modules; public plugin headers live in `include/kywc/`. |
| `libkywc/` | Client library (`libflakewm-client`) for outputs, toplevels, workspaces, thumbnails, and flakewm detection. |
| `protocols/` | Vendored/generated Wayland protocol XML and CMake protocol targets. |
| `examples/` | Example clients and `simple_plugin.c`; `simple` is built as a loadable plugin. |
| `data/` | Default config, desktop/session entries, startup script, udev rules, GSettings schema. |
| `cmake/` | Build helpers, data installation, and wlroots dependency management. |
| `tests/` | Standalone resource-writer test; CTest smoke tests are registered from `src/CMakeLists.txt`. |
| `libs/` | Vendored third-party sources (`wlroots`, `wayland`, `wayland-protocols`, `libdrm`, `pixman`, `xkbcommon`, `abseil-cpp`). Treat as read-only. |
| `docs/` | Protocol support matrix, environment variables, GXDE D-Bus manuals. |
| `debian/`, `arch/`, `fedora/`, `freebsd/` | Distribution packaging. |

## Architecture Notes

### Startup flow

1. `main.c` parses options, initializes logging and signal handling, then calls
   `server_init()` and `server_start()`.
2. `server.c` creates `wl_display`, the event loop, D-Bus context, the work
   queue, and core managers (`config`, `security`, `theme`, `gxde_identifier`,
   `app_id_resolver`).
3. wlroots is initialized: backend auto-creation, renderer, allocator,
   compositor, linux-dmabuf, explicit sync, output layout, and the `ky_scene`.
4. Managers are created in order: `output`, `input`, `view`, `xwayland`,
   `effect`, `plugin`.
5. `server_start()` adds the Wayland socket, exports `WAYLAND_DISPLAY` and
   `DDE_CURRENT_COMPOSITOR`, and starts the backend.
6. `main.c` optionally starts a session process (`-s`) after XWayland is ready
   (with a 5-second fallback timeout).

Shutdown order matters: XWayland is stopped before clients are disconnected,
and backends are destroyed before `wl_display_destroy()`.

### Subsystem pattern

Most subsystems follow the same pattern:

- A manager is created with `*_manager_create(server)`.
- Objects are created/destroyed with `*_create()` / `*_destroy()`.
- Events use `wl_listener` and `wl_container_of()`.
- Cleanup is registered either on the object's destroy signal or via
  `server_add_destroy_listener()` (last-in, first-out).
- Public compositor APIs use the `kywc_` prefix; internal scene APIs use `ky_`;
  build options/macros use `WLCOM_`/`HAVE_`.

### Protocols

- `protocols/CMakeLists.txt` generates server and client protocol code.
  Consumers link `wlcom-protocols` (server) or
  `wlcom-client-protocol-<name>` (client).
- To add a protocol, add the XML under `protocols/`, register it in the
  `protocols` list (or a feature-gated `list(APPEND ...)`), and implement the
  server side in the relevant `src/` module.
- `docs/PROTOCOLS.md` is the support matrix. Update it when adding or changing
  protocol support.
- **Do not blindly sync `protocols/treeland-personalization-manager-v1.xml`
  with upstream.** The vendored XML intentionally matches the 0.5.8 layout
  (including wallpaper context). The compositor auto-detects DTK 0.5.8 vs
  0.5.9 at startup. Read the README section on Treeland personalization before
  touching this file; wire layout, request order, and versions must remain
  exact.

### Plugins

- Plugins are shared objects scanned one level deep from `PLUGIN_DIR`
  (default: `${CMAKE_INSTALL_PREFIX}/lib/flakewm/plugins`, overridable with
  `-DWLCOM_PLUGIN_DIRECTORY=...`).
- A plugin exports a `struct kywc_plugin_data` symbol named `<name>_plugin_data`
  and declares an ABI version via `ABI_VERSION(major, minor)`.
- Plugin-visible compositor symbols are controlled by `wlcom.syms`
  (`wlcom.freebsd.syms` on FreeBSD). Add new exported symbols there.
- See `include/kywc/plugin.h` and `examples/simple_plugin.c`.

### Client library

`libkywc` builds `libflakewm-client` with public API in `libkywc/libkywc.h`
(installed as `flakewm/libkywc.h`) and pkg-config `flakewm-client.pc`.
Symbol visibility is controlled by `libkywc/kywc.syms`; it covers outputs,
toplevels, workspaces, thumbnails, and `kywc_context_is_gxde_wlcom()`.

## Building

### Prerequisites

Runtime and build dependencies are listed in `README.md` and
`debian/control`. On Debian/Ubuntu the simplest path is:

```bash
sudo apt build-dep .
# or, if the local helper is present:
./build-deb -d
```

Core build requirements include CMake >= 3.21, Ninja, Meson >= 1.3, a C
compiler, Wayland, wayland-protocols >= 1.41, wlroots dependencies (libdrm,
libxkbcommon, libinput, libseat, libdisplay-info, gbm, EGL/GLES, pixman),
json-c, cairo/pango, librsvg, libjpeg/libpng, libsystemd (or basu on FreeBSD),
XCB, and OpenSSL. XWayland itself is optional; enabling it additionally
requires xcb-shape and xcb-xfixes.

### Standard build

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
sudo cmake --install build
```

- If `CMAKE_BUILD_TYPE` is omitted, the project defaults to `Debug`.
- `CMAKE_EXPORT_COMPILE_COMMANDS` is enabled; for clangd/Flymake:
  `ln -sf build/compile_commands.json compile_commands.json`.
- Warnings are enabled globally (`-Wall -Wextra` plus many extra flags). Keep
  new code warning-clean; do not add blanket warning suppressions.
- `cmake -LH build` lists all `WLCOM_*` cache options.

### Important CMake options

| Option | Default | Meaning |
| --- | --- | --- |
| `WLCOM_EXAMPLES` | `ON` | Build example clients and `simple` plugin. |
| `WLCOM_LIBKYWC` | `ON` | Build the `flakewm-client` library and KWin shim. |
| `WLCOM_FLAKECTL` | `ON` | Build `flakectl` (Hyprland-compatible IPC control). |
| `WLCOM_GXDE_SESSION` | `ON` | Install the GXDE session entry and `startflakewm`. |
| `WLCOM_XWAYLAND` | `ON` | Enable XWayland support. |
| `WLCOM_TRACY` | `OFF` | Tracy profiler integration. |
| `WLCOM_CLIPBOARD_PERSIST` | `ON` | Keep clipboard contents after the source client exits. |
| `WLCOM_ALLOW_VENDORED_WLROOTS_DEPS` | `ON` | Allow vendored fallbacks when system deps are too old. |
| `WLCOM_PLUGIN_DIRECTORY` | empty | Override the plugin install directory. |
| `WLCOM_VERSION` | empty | Override the reported version (used by RPM tarball builds). |

Feature toggles are declared in the `wlcom_feature_options` list at the top of
`CMakeLists.txt` (`WLCOM_KDE_*`, `WLCOM_WLR_*`, `WLCOM_UKUI_*`). Each becomes a
`HAVE_<NAME>` compile definition and gates sources in `src/CMakeLists.txt`.

The reported version comes from `debian/changelog` when available, otherwise
from `project(VERSION)` plus a git hash. Do not treat `project(... VERSION
1.3.0)` as the package version.

### wlroots and vendored libraries

- `libs/wlroots` is pristine wlroots `0.20.2`, built statically through Meson
  (`cmake/wlroots.cmake`). Do not patch it in place.
- System dependencies are preferred. Vendored copies under `libs/` are
  fallbacks for older systems; runtime fallbacks are installed privately.
- `src/patches/` contains the compositor's own compatibility code. Keep new
  wlroots adaptations there or in the relevant subsystem, not in `libs/`.

### Packaging

- **Debian:** packaging lives in `debian/`. `build-deb` is a local helper
  (`./build-deb`, `-d` to install deps, `-c` to clean) but is ignored by
  `.gitignore`; a fresh clone may not contain it.
- **Fedora:** `./build-rpm` (tracked) packs the working tree and runs
  `rpmbuild`; `-d` installs BuildRequires, `-s` builds only the source RPM,
  `-c` cleans. Output goes to `fedora/rpmbuild/`.
- **Arch:** `arch/PKGBUILD`; CI uses the GXDE reusable workflow.
- **FreeBSD:** port in `freebsd/`; build with
  `make FLAKEWM_SRC=/path/to/flake-wm package` from
  `/usr/ports/x11-wm/flakewm`. No fbdev backend or GXDE session launcher.
- **Version bumps:** `debian/changelog` is the source of truth. Run
  `./update-pkgver` to sync `arch/PKGBUILD`, `fedora/flakewm.spec`, and
  `freebsd/Makefile`; `./pre-bump` prints a changelog trailer line.

## Running and Debugging

Nested run inside an existing Wayland or X11 session:

```bash
./build/flakewm
# exposes a new socket, usually wayland-1
WAYLAND_DISPLAY=wayland-1 gxde-terminal
```

Useful options:

```bash
./build/flakewm -d                 # debug logging
./build/flakewm -V                 # info logging
./build/flakewm -Dnoxwayland       # disable XWayland
./build/flakewm -Dlogtofile        # write ~/.log/flakewm.log
./build/flakewm -Dloginmtime       # monotonic timestamps
./build/flakewm -s startdde        # spawn a session process
./build/flakewm --no-default-keybind
./build/flakewm --emergency-term-keybind
./build/flakewm --hypr-unix-socket-sim-enable
```

Logs go to stdout by default. `KYWC_LOG_LEVEL` accepts
`DEBUG|INFO|WARN|ERROR|FATAL|SILENT`.

Common environment variables (full list in `docs/ENV.md`):

| Variable | Purpose |
| --- | --- |
| `KYWC_BACKEND=fbdev` | Force the framebuffer backend. |
| `KYWC_RENDERER=gl\|gles2\|pixman\|vulkan` | Select renderer. |
| `WLR_DRM_DEVICES` | DRM device list, first device is the primary renderer. |
| `KYWC_SYNCOBJ=0` | Disable explicit sync (debugging). |
| `KYWC_RENDERER_ALLOW_SOFTWARE=1` | Allow llvmpipe/software rendering. |
| `XDG_CONFIG_HOME` | Relocates the user config directory. |
| `FLAKEWM_PERSONALIZATION=058\|059` | Force Treeland personalization layout. |
| `FLAKEWM_DONOT_BROADCAST_TLPM=TRUE` | Do not advertise the Treeland personalization global. |

### Configuration

- User config: `$XDG_CONFIG_HOME/flakewm/config.json` or
  `~/.config/flakewm/config.json`.
- System default: `/etc/flakewm/config.json` on Linux; on FreeBSD it is
  `${CMAKE_INSTALL_FULL_SYSCONFDIR}/flakewm/config.json` (normally
  `/usr/local/etc/flakewm/config.json`). It is installed from
  `data/default_preferences/config.json`.
- Top-level sections include `theme`, `outputs`, `layouts`, `Inputs`, `Seats`,
  `InputAction`, and `plugins`.
- Runtime D-Bus interfaces include `top.gxde.Wlcom.Theme`,
  `top.gxde.Wlcom.WindowBtn`, `top.gxde.Wlcom.WindowCorner`,
  `top.gxde.Wlcom.Screenshot`, and `com.deepin.wm`. See
  `docs/gxde/manual/dbus.md` and
  `docs/gxde/manual/multitasking-launcher-interface.md`.

## Testing and Validation

Always build before testing. A normal validation pass is:

```bash
cmake --build build
ctest --test-dir build --output-on-failure
```

CTest currently covers the optional Qt/KWin shim smoke tests
(`occultation-qml-smoke`, built on demand through a fixture). These require
Qt 6 Quick and `Qt6::GuiPrivate`; otherwise they are not registered.

Standalone resource-writer test:

```bash
./tests/run-resource-writer.sh
./tests/run-resource-writer.sh --fixture   # wraps Wayland resource symbols
```

It compiles `tests/resource-writer.c` and `src/util/wayland.c` with
`-Wall -Wextra -Werror` and runs with a timeout.

For compositor behavior, also run nested and exercise the affected client:

```bash
WAYLAND_DISPLAY=wayland-0 ./build/flakewm   # exposes wayland-1
WAYLAND_DISPLAY=wayland-1 <client>
```

A successful build and headless/nested test do **not** prove full parity with
the old wlroots fork; call out remaining validation explicitly.

When changing code:

- Add or update a CTest test when the behavior can be tested automatically.
  Register it in the relevant `CMakeLists.txt` (`enable_testing()` is already
  on at top level).
- For protocol changes, validate the wire layout against the target XML before
  testing a real session. The README contains a Python snippet that compares
  interfaces, request/event order, argument types, `since`, and versions.
- For input/gesture changes, test both nested and a real session if possible;
  gestures and shortcuts depend on compositor-side state.

## Development Conventions

### C style

- C11, no compiler extensions (`CMAKE_C_EXTENSIONS OFF`); define
  `_POSIX_C_SOURCE` where POSIX APIs are needed.
- Match the surrounding file. Existing code uses 4-space indentation,
  Linux-kernel-like brace placement (function braces on their own line,
  control-flow braces on the same line), and a soft limit around 100 columns.
  `.clang-format` is Google-based and `CPPLINT.cfg` says 80 columns, but the
  tree is not uniformly formatted with either; do not mass-reformat unrelated
  code.
- Prefer `bool`, fixed-width integer types, `wl_list`, and
  `wl_container_of()`. Keep declarations close to use and initialize
  allocations with `calloc()`.
- Use `kywc_log()` / `kywc_log_errno()` rather than `printf()` in compositor
  code. `printf()` is appropriate in CLI/example code.
- Keep C++ confined to `src/shell/gxde/kwin_shim/`; the compositor core is C.

### Adding code

- Add new source files to the correct module block in `src/CMakeLists.txt`.
  Headers belong under `include/<module>/` and are included with
  `"module/header.h"`.
- Gate optional features with a `WLCOM_*` option, a `HAVE_*` definition, and
  `if(WLCOM_...)` source blocks. Add the option to `wlcom_feature_options` when
  it is a protocol/feature toggle.
- Embed shaders/assets with `wlcom_embed_files()`; add new shader files to the
  appropriate CMake list.
- If a new symbol must be callable from plugins, add it to `wlcom.syms` (and
  `wlcom.freebsd.syms` if relevant).
- For client-facing APIs, extend `libkywc/` and its public header, and update
  `libkywc/kywc.syms` as needed.
- For new D-Bus methods, update `docs/gxde/manual/dbus.md` and the default
  config if a persistent setting is involved.
- For new config keys, add them to `data/default_preferences/config.json` and
  read/write them through the existing config manager.

### Internationalization

- Add languages to `po/LINGUAS` and translatable sources to
  `po/POTFILES.in`.
- Regenerate the template with:

```bash
cmake --build build --target flakewm-pot
```

### Licensing

- New files need an SPDX header in the same style as neighboring files, e.g.:

```c
// SPDX-FileCopyrightText: 2026 Your Name
// SPDX-License-Identifier: GPL-3.0-or-later
```

- Do not remove or rewrite existing SPDX headers. Some inherited files are
  GPL-1.0-or-later even though the project overall is GPL-3.0-or-later.
- Third-party code must remain license-compatible; keep its original notices.

## Contribution Workflow

- Commit messages and merge requests should be in English, with prefixes such
  as `feat:`, `fix:`, `chore:`, `docs:`, `add:`.
- README asks contributors to target the `gxde/testing` branch; stable changes
  are merged onward by maintainers.
- Open an issue before a merge request if you do not have write access, and
  reference the issue in the MR description.
- `CONTRIBUTING.md` describes the MR checker and AI policy. AI-assisted changes
  are allowed, but you must understand and be able to justify every change; do
  not submit unreviewed generated code.
- Do not commit build artifacts (`build/`, `compile_commands.json`, RPM/DEB
  output, packaging trees) or unrelated reformatting.

## Known Caveats in This Checkout

- `README.md` references `WLR_UPGRADE.md`, `mr-test`, and
  `docs/CONTRIBUTING.md`; these files are absent here. Do not assume they
  exist.
- `build-deb` exists locally but is ignored by `.gitignore` (`*build-*`);
  fresh clones may not have it. `build-rpm` is tracked.
- `src/patches/wlroots/0.17` is historical/inactive. Active compatibility code
  lives in `src/patches/*.c` and subsystem sources.
- `libs/` is vendored third-party code. Changes there are overwritten or
  rejected; put adaptations in `src/`.
- The compositor version reported at runtime is derived from
  `debian/changelog`, not from `project(VERSION)` in `CMakeLists.txt`.
- Treeland personalization compatibility is fragile and intentionally
  version-adaptive. Treat `protocols/treeland-personalization-manager-v1.xml`
  and `src/view/treeland_personalization.c` as wire-sensitive code.
