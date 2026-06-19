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

#include "font8x16.h"
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

	/* Pre-allocated pixel values for each named color. */
	unsigned long   px_panel;
	unsigned long   px_border;
	unsigned long   px_cell;
	unsigned long   px_cell_empty;
	unsigned long   px_text;
	unsigned long   px_title;
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

static void x11_fill_rect(struct x11_backend *b, int x, int y, int w, int h,
			   unsigned long px)
{
	XSetForeground(b->dpy, b->gc, px);
	XFillRectangle(b->dpy, b->win, b->gc, x, y,
		       (unsigned)w, (unsigned)h);
}

static void x11_draw_glyph(struct x11_backend *b, int x, int y, char ch,
			    unsigned long fg)
{
	if ((unsigned char)ch < FONT_FIRST || (unsigned char)ch > FONT_LAST)
		ch = '?';
	const unsigned char *g =
		&font8x16[((unsigned char)ch - FONT_FIRST) * FONT_H];
	XSetForeground(b->dpy, b->gc, fg);
	for (int row = 0; row < FONT_H; row++) {
		unsigned char bits = g[row];
		for (int col = 0; col < FONT_W; col++) {
			if (!(bits & (0x80 >> col)))
				continue;
			XFillRectangle(b->dpy, b->win, b->gc,
				       x + col * FONT_SCALE,
				       y + row * FONT_SCALE,
				       FONT_SCALE, FONT_SCALE);
		}
	}
}

static void x11_draw_text(struct x11_backend *b, int x, int y, const char *s,
			   unsigned long fg)
{
	for (; *s; s++, x += GLYPH_W)
		x11_draw_glyph(b, x, y, *s, fg);
}

/* ------------------------------------------------------------------ */
/* Layout rendering                                                    */
/* ------------------------------------------------------------------ */

static void draw_layout_x11(struct x11_backend *b, const Layout *L)
{
	/*
	 * The window covers exactly the panel rectangle, so all coordinates
	 * below are relative to the window's top-left corner (i.e. subtract
	 * panel.x / panel.y from the absolute positions used by the fb backend).
	 */
	int pw = b->panel.w;
	int ph = b->panel.h;

	x11_fill_rect(b, 0, 0, pw, ph, b->px_panel);

	/* Double border */
	XSetForeground(b->dpy, b->gc, b->px_border);
	XDrawRectangle(b->dpy, b->win, b->gc, 0, 0,
		       (unsigned)(pw - 1), (unsigned)(ph - 1));
	XDrawRectangle(b->dpy, b->win, b->gc, 1, 1,
		       (unsigned)(pw - 3), (unsigned)(ph - 3));

	int x0 = PANEL_PAD;
	int y  = PANEL_PAD;

	x11_draw_text(b, x0, y, L->title, b->px_title);
	y += GLYPH_H + TITLE_GAP;

	for (int ri = 0; ri < L->nrows; ri++) {
		const Row *r = &L->rows[ri];
		int x = x0 + (r->indent_half * MIN_CELL_W) / 2;
		for (int ci = 0; ci < r->n; ci++) {
			const char *label = r->label[ci];
			int   cw    = cell_width(label);
			bool  empty = (label[0] == '\0');
			x11_fill_rect(b, x, y, cw, CELL_H,
				      empty ? b->px_cell_empty : b->px_cell);
			if (!empty) {
				int tw = (int)strlen(label) * GLYPH_W;
				int tx = x + (cw - tw) / 2;
				int ty = y + (CELL_H - GLYPH_H) / 2;
				x11_draw_text(b, tx, ty, label, b->px_text);
			}
			x += cw + HGAP;
		}
		y += CELL_H + VGAP;
	}

	XFlush(b->dpy);
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
	draw_layout_x11(b, L);
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
		goto err_free;
	}
	b->screen = DefaultScreen(b->dpy);

	int screen_w = DisplayWidth(b->dpy,  b->screen);
	int screen_h = DisplayHeight(b->dpy, b->screen);

	if (verbose)
		fprintf(stderr, "keyoverlay: X11 display %dx%d\n",
			screen_w, screen_h);

	b->panel = compute_panel(screen_w, screen_h);

	/* Pre-allocate colors. */
	b->px_panel      = alloc_color(b->dpy, b->screen, COL_PANEL);
	b->px_border     = alloc_color(b->dpy, b->screen, COL_BORDER);
	b->px_cell       = alloc_color(b->dpy, b->screen, COL_CELL);
	b->px_cell_empty = alloc_color(b->dpy, b->screen, COL_CELL_EMPTY);
	b->px_text       = alloc_color(b->dpy, b->screen, COL_TEXT);
	b->px_title      = alloc_color(b->dpy, b->screen, COL_TITLE);

	/* Create an override-redirect window (bypasses the window manager). */
	XSetWindowAttributes attr = {0};
	attr.override_redirect = True;
	attr.background_pixel  = b->px_panel;

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
		goto err_destroy_win;
	}

	/* Window starts unmapped (invisible) until show() is called. */
	XFlush(b->dpy);
	return &b->base;

err_destroy_win:
	XDestroyWindow(b->dpy, b->win);
	XCloseDisplay(b->dpy);
err_free:
	free(b);
	return NULL;
}

#endif /* WITH_X11 */
