/* SPDX-License-Identifier: MIT */
/*
 * keyoverlay.h - shared layout data and rendering interfaces.
 *
 * All rendering backends (framebuffer, X11, …) implement the render_backend
 * vtable.  The core input loop in keyoverlay.c is backend-agnostic.
 */
#ifndef KEYOVERLAY_H
#define KEYOVERLAY_H

#include <stdint.h>

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

enum layout_id { L_NONE = -1, L_NORMAL, L_SHIFT, L_SYMBOL, L_SYMSHIFT };

extern const Layout layouts[];
extern const int    N_LAYOUTS;

/* ------------------------------------------------------------------ */
/* Color                                                               */
/* ------------------------------------------------------------------ */

struct rgb { uint8_t r, g, b; };
enum render_color { COL_PANEL, COL_BORDER, COL_CELL, COL_CELL_EMPTY,
		    COL_TEXT, COL_TITLE, COLOR_COUNT };
extern const struct rgb render_colors[COLOR_COUNT];

struct panel { int x, y, w, h; };

struct render_canvas {
	void *context;
	void (*fill_rect)(void *context, int x, int y, int w, int h, unsigned long color);
	const unsigned long *colors;
};

void render_layout(const struct render_canvas *canvas, const struct panel *panel,
		   const Layout *layout);
struct panel compute_panel(int screen_w, int screen_h);

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
