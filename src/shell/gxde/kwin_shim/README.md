# KWin Shim (Project Occultation · Exoskeleton)
## Introduction
GXDE OS has long since moved its Wayland session from `gxde-kwin` to `GXWM`.
We were forced to do that by a dependency issue, so we still want to keep the
original experience. Re-creating the UIs, especially their visual effects, is
hard, so the idea of Project Occultation is to re-use `gxde-kwin`'s UI
directly.

This directory is the shim that makes that possible: a thin Qt layer that
provides the minimal set of KWin functionality the `gxde-kwin` QML frontends
use. Everything underneath is provided by flakewm, with `libkywc` as the bridge
between the compositor and the shim.

The frontends themselves live next to it, e.g. the Alt+Tab switcher in
[`../window_switcher`](../window_switcher).

## Build
The shim is built as the static library `occultation-exoskeleton` when
`WLCOM_LIBKYWC` is on and Qt 6 Quick (with its private GUI module) is found.
Two extra targets are not built by default:
- `occultation-qml-smoke`: loads the switcher QML against the shim, run by
  `ctest -L occultation`.
- `occultation-switcher-preview`: shows the switcher on fake windows, without
  flakewm.

## License
Licensed under GPLv3 or later. Please refer to the project's main license.
The imported `gxde-kwin` frontends carry their own licenses, see the README in
each of their directories.
