# SPDX-FileCopyrightText: CharOfString <root@charofstring.cc>
#
# SPDX-License-Identifier: GPL-3.0-or-later

# Version/Release/gxde_rev 由 update-pkgver 从 debian/changelog 同步 ・ update-pkgver keeps
# Version, Release and gxde_rev in step with debian/changelog.
%global gxde_rev gxde3
# git 标签与 Debian 版本号相同，例如 3.0.5-gxde1 ・ The git tag is the Debian version, e.g. 3.0.5-gxde1.
%global tag %{version}-%{gxde_rev}

Name:           flakewm
Version:        3.0.6
Release:        3%{?dist}
Summary:        GXDE Wayland compositor

License:        BSD-3-Clause AND GPL-1.0-or-later AND GPL-2.0-or-later AND GPL-3.0-or-later AND LGPL-2.1-or-later AND MIT AND MIT-CMU AND MulanPSL-2.0
URL:            https://github.com/Flake-Shell/flake-wm
# ./build-rpm 从本地源码树生成同名 tarball ・ ./build-rpm makes the same tarball from a checkout.
Source0:        %{url}/archive/%{tag}/flake-wm-%{tag}.tar.gz

# 与其他平台一致，只构建和测试了 64 位 ・ Only built and tested on 64-bit, like the other packages.
ExclusiveArch:  x86_64 aarch64

BuildRequires:  cmake >= 3.21
BuildRequires:  gcc
# flakectl (Hyprland's hyprctl) is C++26 ・ flakectl is C++26.
BuildRequires:  gcc-c++
BuildRequires:  gettext
BuildRequires:  glslang
BuildRequires:  meson >= 1.3.0
BuildRequires:  ninja-build
BuildRequires:  pkgconfig(cairo)
BuildRequires:  pkgconfig(egl)
BuildRequires:  pkgconfig(epoxy) >= 1.5.4
BuildRequires:  pkgconfig(fontconfig)
BuildRequires:  pkgconfig(gbm) >= 21.1
BuildRequires:  pkgconfig(gio-2.0) >= 2.64
BuildRequires:  pkgconfig(glesv2)
BuildRequires:  pkgconfig(hwdata)
BuildRequires:  pkgconfig(json-c) >= 0.13
BuildRequires:  pkgconfig(lcms2)
BuildRequires:  pkgconfig(libdisplay-info) >= 0.2.0
BuildRequires:  pkgconfig(libdrm)
BuildRequires:  pkgconfig(libeis-1.0)
BuildRequires:  pkgconfig(libinput) >= 1.19.0
BuildRequires:  pkgconfig(libjpeg)
BuildRequires:  pkgconfig(libpng)
BuildRequires:  pkgconfig(librsvg-2.0) >= 2.46
BuildRequires:  pkgconfig(libseat) >= 0.2.0
BuildRequires:  pkgconfig(libsystemd)
BuildRequires:  pkgconfig(libudev)
BuildRequires:  pkgconfig(libunwind)
BuildRequires:  pkgconfig(openssl)
BuildRequires:  pkgconfig(pango)
BuildRequires:  pkgconfig(pangocairo)
BuildRequires:  pkgconfig(pixman-1)
BuildRequires:  pkgconfig(re2)
BuildRequires:  pkgconfig(readline)
BuildRequires:  pkgconfig(vulkan) >= 1.2.182
BuildRequires:  pkgconfig(wayland-client)
BuildRequires:  pkgconfig(wayland-protocols) >= 1.41
BuildRequires:  pkgconfig(wayland-scanner)
BuildRequires:  pkgconfig(wayland-server)
BuildRequires:  pkgconfig(xcb)
BuildRequires:  pkgconfig(xcb-composite)
BuildRequires:  pkgconfig(xcb-dri3)
BuildRequires:  pkgconfig(xcb-ewmh)
BuildRequires:  pkgconfig(xcb-icccm)
BuildRequires:  pkgconfig(xcb-present)
BuildRequires:  pkgconfig(xcb-render)
BuildRequires:  pkgconfig(xcb-renderutil)
BuildRequires:  pkgconfig(xcb-res)
BuildRequires:  pkgconfig(xcb-shape)
BuildRequires:  pkgconfig(xcb-shm)
BuildRequires:  pkgconfig(xcb-xfixes) >= 1.15
BuildRequires:  pkgconfig(xcb-xinput)
BuildRequires:  pkgconfig(xkbcommon)
BuildRequires:  pkgconfig(xwayland)

Requires:       xorg-x11-server-Xwayland

# wlroots 以静态库形式编进 flakewm ・ wlroots is built from libs/wlroots and linked statically.
Provides:       bundled(wlroots) = 0.20.2

# 系统库过旧时 CMake 才会构建私有副本，放在 %%{_libdir}/flakewm，不能对外提供
# CMake only builds private copies into %%{_libdir}/flakewm when the system ones
# are too old. They are not for other packages.
%global __provides_exclude_from ^%{_libdir}/flakewm/.*$
# 私有库靠 $ORIGIN 的 RUNPATH 找到，%%cmake 默认的 CMAKE_SKIP_RPATH 会把它去掉
# They are found through an $ORIGIN RUNPATH, which the default
# CMAKE_SKIP_RPATH of %%cmake would drop.
%global _cmake_skip_rpath %{nil}

%description
flakewm is the Wayland compositor of GXDE, based on wlroots and
kylin-wayland-compositor. It supports the KDE, UKUI, wlroots and Hyprland
protocols that GXDE and its applications use.

%package        client
Summary:        Client library of the GXDE Wayland compositor

%description    client
Client library for the private protocols of flakewm.

%package        client-devel
Summary:        Development files of the flakewm client library
Requires:       %{name}-client%{?_isa} = %{version}-%{release}

%description    client-devel
Headers and pkg-config file of the flakewm client library.

%package -n     gxde-flake-session
Summary:        GXDE session on the Flake Wayland compositor
BuildArch:      noarch
Requires:       %{name} = %{version}-%{release}
Requires:       bash

%description -n gxde-flake-session
Display manager session that starts the GXDE desktop on flakewm.

%prep
%autosetup -n flake-wm-%{tag}
# 两份都叫 LICENSE，%license 需要不同的文件名 ・ Both are named LICENSE, %license needs distinct names.
cp -p tools/flakectl/hyprland/LICENSE LICENSE.hyprland
cp -p tools/flakectl/hyprutils/LICENSE LICENSE.hyprutils

%build
# 选项与 debian/rules 相同；依赖固定为 enabled，缺了就报错而不是悄悄关掉功能
# Same options as debian/rules. Optional dependencies are set to "enabled", so a
# missing one fails the build instead of silently disabling the feature.
%cmake -GNinja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_INSTALL_LIBDIR=%{_lib} \
    -DCMAKE_DISABLE_FIND_PACKAGE_Qt6=ON \
    -DWLCOM_VERSION=%{version}-%{release} \
    -DWLCOM_EXAMPLES=OFF \
    -DWLCOM_UKUI_THEME=ON \
    -DWLCOM_WLROOTS_RENDERERS=gles2,vulkan \
    -DWLCOM_NLS=enabled \
    -DWLCOM_LIBEIS=enabled \
    -DWLCOM_LIBUNWIND=enabled
%cmake_build

%install
%cmake_install
# 与 debian/flakewm.install 相同 ・ Same as debian/flakewm.install.
install -Dpm 0644 data/configs/OMG.conf %{buildroot}%{_sysconfdir}/flakewm/OMG.conf

%find_lang %{name}
cp %{name}.lang %{name}.files
if [ -d %{buildroot}%{_libdir}/flakewm ]; then
    echo '%{_libdir}/flakewm/' >> %{name}.files
fi

%files -f %{name}.files
%license LICENSE LICENSES/GPL-1.0-or-later.txt LICENSES/LGPL-2.1-or-later.txt
%license LICENSES/MIT-CMU.txt LICENSES/MulanPSL-2.0.txt
%license LICENSE.hyprland LICENSE.hyprutils
%doc README.md README.zh.md
%{_bindir}/flakectl
%{_bindir}/flakewm
%dir %{_sysconfdir}/flakewm
%config(noreplace) %{_sysconfdir}/flakewm/config.json
%config(noreplace) %{_sysconfdir}/flakewm/OMG.conf
%{_datadir}/flakewm/
%{_datadir}/glib-2.0/schemas/org.flakewm.peripherals-mouse.gschema.xml
%{_prefix}/lib/udev/rules.d/90-flakewm-framebuffer.rules

%files client
%license LICENSE LICENSES/GPL-1.0-or-later.txt
%{_libdir}/libflakewm-client.so.1{,.*}

%files client-devel
%{_includedir}/flakewm/
%{_libdir}/libflakewm-client.so
%{_libdir}/pkgconfig/flakewm-client.pc

%files -n gxde-flake-session
%{_bindir}/startflakewm
%{_datadir}/wayland-sessions/gxde-flakewm.desktop

%changelog
* Sun Nov 22 2026 CharOfString <root@charofstring.cc> - 3.0.6-3
- FreeBSD构建产物自动上传到GitHub Release

* Sun Nov 22 2026 CharOfString <root@charofstring.cc> - 3.0.6-2
- 修复FreeBSD 15构建

* Sat Nov 21 2026 CharOfString <root@charofstring.cc> - 3.0.6-1
- 支持部分HyprCtl控制

* Fri Nov 20 2026 CharOfString <root@charofstring.cc> - 3.0.5-1
- 新增Hyprland协议支持
- 新增FreeBSD 15支持
