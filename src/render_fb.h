/* SPDX-License-Identifier: MIT */
#ifndef RENDER_FB_H
#define RENDER_FB_H

#include "keyoverlay.h"

/*
 * Create a framebuffer render backend.
 *
 * Opens the framebuffer device at @fbpath, detects pixel format, and
 * pre-computes the overlay panel rectangle.  Returns NULL on failure.
 *
 * If @verbose is non-zero, diagnostic messages are printed to stderr.
 */
render_backend *render_fb_create(const char *fbpath, int verbose);

#endif /* RENDER_FB_H */
