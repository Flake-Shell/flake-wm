PORTNAME=	flakewm
DISTVERSION=	3.0.4
DISTVERSIONSUFFIX=	-gxde2
CATEGORIES=	x11-wm wayland
# FLAKEWM_SRC=/path/to/flake-wm builds that checkout (working tree included,
# build/ left out) instead of the release tarball, like arch/PKGBUILD does.
.if defined(FLAKEWM_SRC)
DISTFILES=
.endif

MAINTAINER=	root@charofstring.cc
COMMENT=	GXDE Wayland compositor
WWW=		https://github.com/Flake-Shell/flake-wm

LICENSE=	GPLv1+ GPLv2+ GPLv3+ LGPL21+ MIT MITCMU MULANPSL2
LICENSE_COMB=	multi
LICENSE_NAME_MITCMU=	CMU License
LICENSE_NAME_MULANPSL2=	Mulan Permissive Software License, Version 2
LICENSE_FILE_GPLv3+ =	${WRKSRC}/LICENSE
LICENSE_FILE_MITCMU=	${WRKSRC}/LICENSES/MIT-CMU.txt
LICENSE_FILE_MULANPSL2=	${WRKSRC}/LICENSES/MulanPSL-2.0.txt
LICENSE_PERMS_MITCMU=	dist-mirror dist-sell pkg-mirror pkg-sell auto-accept
LICENSE_PERMS_MULANPSL2=	dist-mirror dist-sell pkg-mirror pkg-sell auto-accept

ONLY_FOR_ARCHS=	aarch64 amd64
ONLY_FOR_ARCHS_REASON=	only built and tested on 64-bit, like the Linux packages

# wlroots 0.20 is vendored in libs/wlroots and built with meson during the build
BUILD_DEPENDS=	evdev-proto>0:devel/evdev-proto \
		glslangValidator:graphics/glslang \
		hwdata>0:misc/hwdata \
		meson:devel/meson \
		vulkan-headers>0:graphics/vulkan-headers \
		wayland-protocols>=1.41:graphics/wayland-protocols
LIB_DEPENDS=	libbasu.so:devel/basu \
		libjson-c.so:devel/json-c \
		libudev.so:devel/libudev-devd \
		libunwind.so:devel/libunwind \
		libdrm.so:graphics/libdrm \
		libepoxy.so:graphics/libepoxy \
		liblcms2.so:graphics/lcms2 \
		libpng16.so:graphics/png \
		libvulkan.so:graphics/vulkan-loader \
		libwayland-server.so:graphics/wayland \
		libdisplay-info.so:sysutils/libdisplay-info \
		libseat.so:sysutils/seatd \
		libfontconfig.so:x11-fonts/fontconfig \
		libeis.so:x11/libei \
		libinput.so:x11/libinput \
		libxkbcommon.so:x11/libxkbcommon

USES=		cmake compiler:c11 gl gnome jpeg pathfix pkgconfig python:build \
		ssl xorg
USE_GL=		egl gbm glesv2
USE_GNOME=	cairo glib20 librsvg2 pango
USE_XORG=	pixman xcb
USE_LDCONFIG=	yes
SUB_FILES=	pkg-message
GLIB_SCHEMAS=	org.flakewm.peripherals-mouse.gschema.xml

# the GXDE session runs startdde, GXDE is not available on FreeBSD
CMAKE_ARGS=	-DWLCOM_EXAMPLES=OFF \
		-DWLCOM_GXDE_SESSION=OFF \
		-DWLCOM_LIBEIS=enabled \
		-DWLCOM_LIBUNWIND=enabled \
		-DWLCOM_WLROOTS_RENDERERS=gles2,vulkan \
		-DCMAKE_DISABLE_FIND_PACKAGE_Qt6=ON

OPTIONS_DEFINE=		NLS XWAYLAND
OPTIONS_DEFAULT=	XWAYLAND
OPTIONS_SUB=		yes

NLS_USES=		gettext
NLS_CMAKE_ON=		-DWLCOM_NLS=enabled
NLS_CMAKE_OFF=		-DWLCOM_NLS=disabled

XWAYLAND_DESC=		X11 applications via Xwayland
XWAYLAND_LIB_DEPENDS=	libxcb-ewmh.so:x11/xcb-util-wm \
			libxcb-render-util.so:x11/xcb-util-renderutil
XWAYLAND_RUN_DEPENDS=	Xwayland:x11-servers/xwayland
XWAYLAND_CMAKE_BOOL=	WLCOM_XWAYLAND

.if defined(FLAKEWM_SRC)
BUILD_DEPENDS+=	git:devel/git
WRKSRC=		${WRKDIR}/flake-wm
.else
USE_GITHUB=	yes
GH_ACCOUNT=	Flake-Shell
GH_PROJECT=	flake-wm
.endif

.if defined(FLAKEWM_SRC)
do-fetch:
	@${DO_NADA}

do-extract:
	@${MKDIR} ${WRKSRC}
	@cd ${FLAKEWM_SRC} && \
		if [ -e .git ]; then git ls-files -co --exclude-standard -z; \
		else ${FIND} . -path ./build -prune -o ! -type d -print0; fi | \
		${TAR} --null -T - -cf - | ${TAR} -xf - -C ${WRKSRC}
	@${TEST} -f ${WRKSRC}/CMakeLists.txt
.endif

# The vendored wlroots is meson-installed into the build tree. ninja gets
# DESTDIR from MAKE_ENV, which would move it into STAGEDIR while building.
DO_MAKE_BUILD=	${SETENVI} ${WRK_ENV} ${MAKE_ENV:N${DESTDIRNAME}=*} ${MAKE_CMD} \
		${MAKE_FLAGS} ${MAKEFILE} ${_MAKE_JOBS} ${MAKE_ARGS:N${DESTDIRNAME}=*}

post-install:
# the install step reruns that meson install under STAGEDIR, keep it out
	@${RM} -r ${STAGEDIR}${WRKDIR}
	@${RMDIR} -p ${STAGEDIR}${WRKDIR:H} 2>/dev/null || ${TRUE}
# let @sample keep local changes to the system config
	@${MV} ${STAGEDIR}${PREFIX}/etc/flakewm/config.json \
		${STAGEDIR}${PREFIX}/etc/flakewm/config.json.sample

.include <bsd.port.mk>
