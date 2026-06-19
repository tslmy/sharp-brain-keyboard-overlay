/* SPDX-License-Identifier: MIT */
/*
 * keyoverlay.h - shared types, geometry helpers, and render backend interface.
 *
 * All rendering backends (framebuffer, X11, …) implement the render_backend
 * vtable.  The core input loop in keyoverlay.c is backend-agnostic.
 */
#ifndef KEYOVERLAY_H
#define KEYOVERLAY_H

#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Layout types                                                        */
/* ------------------------------------------------------------------ */

#define MAXCELLS 12

typedef struct {
	const char *label[MAXCELLS];
	int n;
	int indent_half; /* indent in half cell-widths, mirrors physical stagger */
} Row;

typedef struct {
	const char *title;
	const Row *rows;
	int nrows;
} Layout;

/* Defined in keyoverlay.c, referenced by the shared geometry helpers below. */
extern const Layout layouts[];
extern const int    N_LAYOUTS;

/* ------------------------------------------------------------------ */
/* Color                                                               */
/* ------------------------------------------------------------------ */

struct rgb { uint8_t r, g, b; };
#define RGB(r, g, b) ((struct rgb){(r), (g), (b)})

static const struct rgb COL_PANEL      = {20,  20,  24 };
static const struct rgb COL_BORDER     = {210, 210, 210};
static const struct rgb COL_CELL       = {120, 120, 120};
static const struct rgb COL_CELL_EMPTY = {70,  70,  74 };
static const struct rgb COL_TEXT       = {255, 255, 255};
static const struct rgb COL_TITLE      = {255, 255, 255};

/* ------------------------------------------------------------------ */
/* Rendering geometry constants                                        */
/* ------------------------------------------------------------------ */

/* Font dimensions match font8x16.h (FONT_W=8, FONT_H=16). */
#define FONT_SCALE   2
#define GLYPH_W      (8  * FONT_SCALE)  /* 16 px */
#define GLYPH_H      (16 * FONT_SCALE)  /* 32 px */
#define HPAD         6
#define VPAD         6
#define MIN_CELL_W   (GLYPH_W + 2 * HPAD)
#define CELL_H       (GLYPH_H + 2 * VPAD)
#define HGAP         6
#define VGAP         6
#define PANEL_PAD    16
#define TITLE_GAP    10

/* ------------------------------------------------------------------ */
/* Panel geometry helpers (used by both backends and main)            */
/* ------------------------------------------------------------------ */

struct panel { int x, y, w, h; };

static inline int cell_width(const char *label)
{
	int len = (int)strlen(label);
	int w   = len * GLYPH_W + 2 * HPAD;
	return w < MIN_CELL_W ? MIN_CELL_W : w;
}

static inline int row_width(const Row *r)
{
	int w = (r->indent_half * MIN_CELL_W) / 2;
	for (int i = 0; i < r->n; i++) {
		w += cell_width(r->label[i]);
		if (i + 1 < r->n)
			w += HGAP;
	}
	return w;
}

static inline void layout_size(const Layout *L, int *out_w, int *out_h)
{
	int maxw = 0;
	for (int i = 0; i < L->nrows; i++) {
		int w = row_width(&L->rows[i]);
		if (w > maxw)
			maxw = w;
	}
	*out_w = maxw;
	*out_h = GLYPH_H + TITLE_GAP + L->nrows * CELL_H + (L->nrows - 1) * VGAP;
}

/*
 * Compute the panel rectangle centered on a screen of the given dimensions,
 * sized to fit the largest layout across all entries in layouts[].
 */
static inline struct panel compute_panel(int screen_w, int screen_h)
{
	struct panel p;
	int maxw = 0, maxh = 0;
	for (int i = 0; i < N_LAYOUTS; i++) {
		int w, h;
		layout_size(&layouts[i], &w, &h);
		if (w > maxw) maxw = w;
		if (h > maxh) maxh = h;
	}
	p.w = maxw + 2 * PANEL_PAD;
	p.h = maxh + 2 * PANEL_PAD;
	if (p.w > screen_w) p.w = screen_w;
	if (p.h > screen_h) p.h = screen_h;
	p.x = (screen_w - p.w) / 2;
	p.y = (screen_h - p.h) / 2;
	return p;
}

/* ------------------------------------------------------------------ */
/* Render backend interface                                            */
/* ------------------------------------------------------------------ */

typedef struct render_backend render_backend;

struct render_backend {
	/*
	 * Show (or switch to) a layout.
	 * On the first call the backend saves whatever is beneath the panel
	 * (or maps/raises a window).  Subsequent calls while the overlay is
	 * already visible simply repaint with the new layout.
	 */
	void (*show)(render_backend *b, const Layout *L);

	/*
	 * Hide the overlay and restore the previous screen state.
	 */
	void (*hide)(render_backend *b);

	/*
	 * Release all resources held by the backend.
	 * The caller must not use the pointer afterwards.
	 */
	void (*close)(render_backend *b);
};

#endif /* KEYOVERLAY_H */
