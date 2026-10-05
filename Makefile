# SPDX-License-Identifier: MIT
CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
PREFIX  ?= /usr
BINDIR  ?= $(PREFIX)/bin

BIN  := keyoverlay
SRCS := src/keyoverlay.c src/render.c src/render_fb.c

# X11 backend — opt out with:  make WITHOUT_X11=1
WITHOUT_X11 ?= 0
ifeq ($(WITHOUT_X11),0)
  SRCS    += src/render_x11.c
  CFLAGS  += -DWITH_X11
  LDFLAGS += -lX11
endif

HDRS := src/font8x16.h src/keyoverlay.h src/render_fb.h src/render_x11.h

all: $(BIN)

$(BIN): $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)

install: $(BIN)
	install -D -m 0755 $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)

clean:
	rm -f $(BIN)

test:
	@set -eu; \
	test_bin=$$(mktemp /tmp/keyoverlay-test.XXXXXX); \
	trap 'rm -f "$$test_bin"' EXIT; \
	for source in tests/input.c tests/render.c; do \
		$(CC) $(filter-out -DWITH_X11,$(CFLAGS)) -o "$$test_bin" "$$source" src/render.c; \
		"$$test_bin"; \
	done

test-x11:
	@set -eu; \
	test_bin=$$(mktemp /tmp/keyoverlay-x11-test.XXXXXX); \
	trap 'rm -f "$$test_bin"' EXIT; \
	$(CC) $(filter-out -DWITH_X11,$(CFLAGS)) -DWITH_X11 -o "$$test_bin" tests/render.c src/render.c -lX11; \
	xvfb-run -a -s '-screen 0 800x480x24' "$$test_bin"

# ------------ Debian package (armel cross-build via Docker) ----------
# Produces dist/keyoverlay_armel.deb for distribution via an apt repository.
# Requires Docker with linux/amd64 emulation (Docker Desktop on macOS is fine).
#
# Intended consumers:
#   - An apt repo / PPA for Debian/Brainux installs
#   - buildbrain's `make docker-brainux-keyoverlay` (copies the deb into the
#     os-brainux/override dir so it gets injected into the rootfs image)

DEB_IMAGE := keyoverlay-deb-builder:local

deb-image:
	docker build --platform linux/amd64 -t $(DEB_IMAGE) -f Dockerfile.deb .

deb: deb-image
	mkdir -p dist
	docker run --rm --platform linux/amd64 \
		-v "$$PWD/dist":/out \
		$(DEB_IMAGE)

.PHONY: all install clean test test-x11 deb-image deb
