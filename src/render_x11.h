/* SPDX-License-Identifier: MIT */
#ifndef RENDER_X11_H
#define RENDER_X11_H

#include "keyoverlay.h"

/*
 * Create an X11 render backend.
 *
 * Opens the display identified by the DISPLAY environment variable, creates
 * an override-redirect window sized and positioned to match the overlay panel,
 * and keeps it unmapped until show() is called.  Returns NULL on failure
 * (e.g. DISPLAY is unset or the connection cannot be opened).
 *
 * If @verbose is non-zero, diagnostic messages are printed to stderr.
 *
 * Only compiled when WITH_X11 is defined (set by the Makefile).
 */
#ifdef WITH_X11
render_backend *render_x11_create(int verbose);
#endif

#endif /* RENDER_X11_H */
