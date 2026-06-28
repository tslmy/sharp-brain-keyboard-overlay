# SPDX-License-Identifier: MIT
#
# Thin convenience wrapper around the Zig build system (see build.zig).
# `zig build` is the source of truth; these targets just shorten common
# invocations and drive the Docker-based armhf .deb packaging.

ZIG       ?= zig
OPTIMIZE  ?= ReleaseFast

# Native/host build (framebuffer backend only by default).
#   make X11=1   to also build the X11 backend (requires libX11).
ifeq ($(X11),1)
  X11FLAG := -Dx11=true
endif

all:
	$(ZIG) build -Doptimize=$(OPTIMIZE) $(X11FLAG)

# Cross-build for the Brain (armhf, framebuffer only).
arm:
	$(ZIG) build -Dtarget=arm-linux-gnueabihf -Doptimize=$(OPTIMIZE)

clean:
	rm -rf .zig-cache zig-out

# ------------ Debian package (armhf cross-build via Docker) ----------
# Produces dist/keyoverlay_armhf.deb for distribution via an apt repository.
# Requires Docker with linux/amd64 emulation (Docker Desktop on macOS is fine).
#
# Intended consumers:
#   - An apt repo / PPA for Debian/Brainux installs
#   - buildbrain's `make docker-brainux-keyoverlay` (copies the deb into the
#     os-brainux/override dir so it gets injected into the rootfs image)

DEB_IMAGE := keyoverlay-deb-builder:local

deb-image:
	docker build -t $(DEB_IMAGE) -f Dockerfile.deb .

deb: deb-image
	mkdir -p dist
	docker run --rm \
		-v "$$PWD/dist":/out \
		$(DEB_IMAGE)

.PHONY: all arm clean deb-image deb
