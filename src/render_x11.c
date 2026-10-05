// SPDX-License-Identifier: MIT
/*
 * render_x11.c - X11 render backend.
 *
 * Creates an override-redirect, always-on-top window positioned at the same
 * panel rectangle used by the framebuffer backend.  The window is unmapped
 * when no overlay is shown, so it is invisible and does not interfere with
 * other applications.
 *
 * Glyph rendering reuses the same font8x16 bitmap data as the framebuffer
 * backend (via XFillRectangle), so both backends produce identical-looking
 * output.
 *
 * Color allocation uses XAllocColor so the code is correct for all X visual
 * types (PseudoColor, TrueColor, …).
 *
 * Only compiled when WITH_X11 is defined (set by the Makefile).
 */

#ifdef WITH_X11

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>

#include "keyoverlay.h"
#include "render_x11.h"

/* ------------------------------------------------------------------ */
/* Internal state                                                      */
/* ------------------------------------------------------------------ */

struct x11_backend {
	render_backend  base;   /* MUST be first */
	Display        *dpy;
	int             screen;
	Window          win;
	GC              gc;
	struct panel    panel;
	bool            shown;

	unsigned long   colors[COLOR_COUNT];
	unsigned long   foreground;
};

/* ------------------------------------------------------------------ */
/* Color helpers                                                       */
/* ------------------------------------------------------------------ */

static unsigned long alloc_color(Display *dpy, int screen, struct rgb c)
{
	XColor xc;
	xc.red   = (unsigned short)c.r << 8;
	xc.green = (unsigned short)c.g << 8;
	xc.blue  = (unsigned short)c.b << 8;
	xc.flags = DoRed | DoGreen | DoBlue;
	if (XAllocColor(dpy, DefaultColormap(dpy, screen), &xc))
		return xc.pixel;
	/* Fallback: compose a 24-bit value for TrueColor displays. */
	return ((unsigned long)c.r << 16) |
	       ((unsigned long)c.g <<  8) |
	        (unsigned long)c.b;
}

/* ------------------------------------------------------------------ */
/* Primitive drawing (onto the overlay window)                         */
/* ------------------------------------------------------------------ */

static void x11_fill_rect(void *context, int x, int y, int w, int h,
			   unsigned long px)
{
	struct x11_backend *b = context;
	if (px != b->foreground) {
		XSetForeground(b->dpy, b->gc, px);
		b->foreground = px;
	}
	XFillRectangle(b->dpy, b->win, b->gc, x, y,
		       (unsigned)w, (unsigned)h);
}

/* ------------------------------------------------------------------ */
/* Backend vtable                                                      */
/* ------------------------------------------------------------------ */

static void x11_show(render_backend *base, const Layout *L)
{
	struct x11_backend *b = (struct x11_backend *)base;
	if (!b->shown) {
		XMapRaised(b->dpy, b->win);
		b->shown = true;
	}
	struct render_canvas canvas = {b, x11_fill_rect, b->colors};
	struct panel panel = {0, 0, b->panel.w, b->panel.h};
	render_layout(&canvas, &panel, L);
	XFlush(b->dpy);
}

static void x11_hide(render_backend *base)
{
	struct x11_backend *b = (struct x11_backend *)base;
	if (!b->shown)
		return;
	XUnmapWindow(b->dpy, b->win);
	XFlush(b->dpy);
	b->shown = false;
}

static void x11_close(render_backend *base)
{
	struct x11_backend *b = (struct x11_backend *)base;
	if (b->gc)
		XFreeGC(b->dpy, b->gc);
	if (b->win)
		XDestroyWindow(b->dpy, b->win);
	if (b->dpy)
		XCloseDisplay(b->dpy);
	free(b);
}

/* ------------------------------------------------------------------ */
/* Constructor                                                         */
/* ------------------------------------------------------------------ */

render_backend *render_x11_create(int verbose)
{
	struct x11_backend *b = calloc(1, sizeof(*b));
	if (!b)
		return NULL;

	b->base.show  = x11_show;
	b->base.hide  = x11_hide;
	b->base.close = x11_close;

	b->dpy = XOpenDisplay(NULL);
	if (!b->dpy) {
		fprintf(stderr, "keyoverlay: cannot open X display\n");
		goto error;
	}
	b->screen = DefaultScreen(b->dpy);

	int screen_w = DisplayWidth(b->dpy,  b->screen);
	int screen_h = DisplayHeight(b->dpy, b->screen);

	if (verbose)
		fprintf(stderr, "keyoverlay: X11 display %dx%d\n",
			screen_w, screen_h);

	b->panel = compute_panel(screen_w, screen_h);

	/* Pre-allocate colors. */
	for (int color = 0; color < COLOR_COUNT; color++)
		b->colors[color] = alloc_color(b->dpy, b->screen, render_colors[color]);

	/* Create an override-redirect window (bypasses the window manager). */
	XSetWindowAttributes attr = {0};
	attr.override_redirect = True;
	attr.background_pixel  = b->colors[COL_PANEL];

	b->win = XCreateWindow(
		b->dpy,
		RootWindow(b->dpy, b->screen),
		b->panel.x, b->panel.y,
		(unsigned)b->panel.w, (unsigned)b->panel.h,
		0,                          /* border width */
		DefaultDepth(b->dpy, b->screen),
		InputOutput,
		DefaultVisual(b->dpy, b->screen),
		CWOverrideRedirect | CWBackPixel,
		&attr);

	/* Hint to EWMH compositors that this is a notification-style window. */
	Atom wm_type      = XInternAtom(b->dpy, "_NET_WM_WINDOW_TYPE",        False);
	Atom type_notif   = XInternAtom(b->dpy, "_NET_WM_WINDOW_TYPE_NOTIFICATION", False);
	XChangeProperty(b->dpy, b->win, wm_type, XA_ATOM, 32,
			PropModeReplace, (unsigned char *)&type_notif, 1);

	/* Set a descriptive title for debugging. */
	XStoreName(b->dpy, b->win, "keyoverlay");

	b->gc = XCreateGC(b->dpy, b->win, 0, NULL);
	if (!b->gc) {
		fprintf(stderr, "keyoverlay: XCreateGC failed\n");
		goto error;
	}

	/* Window starts unmapped (invisible) until show() is called. */
	XFlush(b->dpy);
	return &b->base;

error:
	x11_close(&b->base);
	return NULL;
}

#endif /* WITH_X11 */
