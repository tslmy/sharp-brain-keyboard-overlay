// SPDX-License-Identifier: MIT
/*
 * keyoverlay - on-screen keymap cheat-sheet overlay for SHARP Brain devices.
 *
 * Listens to a Linux evdev input device and, while a modifier key is held,
 * draws the corresponding key legend layout directly onto the Linux
 * framebuffer (/dev/fb0). The overlay disappears when the modifier is
 * released. All layouts are generated programmatically; no image files are
 * used.
 *
 *   Shift held              -> Shift layout
 *   Symbol (記号) held      -> Symbol layout
 *   Symbol + Shift held     -> Symbol+Shift layout
 *   "normal" trigger held   -> Normal layout (optional, configurable key)
 *
 * The daemon is a passive observer: it never grabs the device, so keystrokes
 * still reach the console/applications as usual.
 */

#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "font8x16.h"

/* ------------------------------------------------------------------ */
/* Layout definitions                                                 */
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

/* Function/top row is identical across all layouts. */
#define FUNC_ROW {{"Pwr", "Esc", "Tab", "PgU", "PgD", "Ins", "Del"}, 7, 0}
/* Ctrl/Alt bottom row is identical across all layouts. */
#define CTRL_ROW {{"Ctrl", "Alt"}, 2, 0}

static const Row normal_rows[] = {
	FUNC_ROW,
	{{"q", "w", "e", "r", "t", "y", "u", "i", "o", "p"}, 10, 0},
	{{"a", "s", "d", "f", "g", "h", "j", "k", "l"}, 9, 1},
	{{"Shift", "z", "x", "c", "v", "b", "n", "m", "-", "BS"}, 10, 0},
	CTRL_ROW,
};

static const Row shift_rows[] = {
	FUNC_ROW,
	{{"Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P"}, 10, 0},
	{{"A", "S", "D", "F", "G", "H", "J", "K", "L"}, 9, 1},
	{{"Shift", "Z", "X", "C", "V", "B", "N", "M", "_", "BS"}, 10, 0},
	CTRL_ROW,
};

static const Row symbol_rows[] = {
	FUNC_ROW,
	{{"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"}, 10, 0},
	{{"", "", "`", "=", "\\", ";", "'", "[", "]"}, 9, 1},
	{{"Shift", "", "", "", "", "", ",", ".", "/", "BS"}, 10, 0},
	CTRL_ROW,
};

static const Row symshift_rows[] = {
	FUNC_ROW,
	{{"!", "@", "#", "$", "%", "^", "&", "*", "(", ")"}, 10, 0},
	{{"", "", "~", "+", "|", ":", "\"", "{", "}"}, 9, 1},
	{{"Shift", "", "", "", "", "", "<", ">", "?", "BS"}, 10, 0},
	CTRL_ROW,
};

enum layout_id { L_NONE = -1, L_NORMAL, L_SHIFT, L_SYMBOL, L_SYMSHIFT };

static const Layout layouts[] = {
	[L_NORMAL] = {"Normal", normal_rows, 5},
	[L_SHIFT] = {"Shift", shift_rows, 5},
	[L_SYMBOL] = {"Symbol", symbol_rows, 5},
	[L_SYMSHIFT] = {"Symbol + Shift", symshift_rows, 5},
};

/* ------------------------------------------------------------------ */
/* Rendering geometry & colors                                        */
/* ------------------------------------------------------------------ */

#define SCALE 2                       /* font scale factor */
#define GLYPH_W (FONT_W * SCALE)      /* 16 */
#define GLYPH_H (FONT_H * SCALE)      /* 32 */
#define HPAD 6                        /* horizontal padding inside a cell */
#define VPAD 6                        /* vertical padding inside a cell */
#define MIN_CELL_W (GLYPH_W + 2 * HPAD)
#define CELL_H (GLYPH_H + 2 * VPAD)
#define HGAP 6                        /* gap between cells */
#define VGAP 6                        /* gap between rows */
#define PANEL_PAD 16                  /* padding inside the panel */
#define TITLE_GAP 10                  /* gap below the title */

/* 8-bit RGB triplets. */
#define RGB(r, g, b) ((struct rgb){r, g, b})
struct rgb {
	uint8_t r, g, b;
};

static const struct rgb COL_PANEL = {20, 20, 24};
static const struct rgb COL_BORDER = {210, 210, 210};
static const struct rgb COL_CELL = {120, 120, 120};
static const struct rgb COL_CELL_EMPTY = {70, 70, 74};
static const struct rgb COL_TEXT = {255, 255, 255};
static const struct rgb COL_TITLE = {255, 255, 255};

/* ------------------------------------------------------------------ */
/* Framebuffer                                                        */
/* ------------------------------------------------------------------ */

struct fb {
	int fd;
	uint8_t *mem;
	size_t map_size;     /* bytes mmapped */
	uint32_t xres, yres;
	uint32_t line_length;
	uint32_t bpp;        /* bytes per pixel (2 or 4) */
	struct fb_var_screeninfo var;
};

static int fb_open(struct fb *fb, const char *path)
{
	struct fb_fix_screeninfo fix;

	memset(fb, 0, sizeof(*fb));
	fb->fd = open(path, O_RDWR);
	if (fb->fd < 0) {
		fprintf(stderr, "keyoverlay: open %s: %s\n", path, strerror(errno));
		return -1;
	}
	if (ioctl(fb->fd, FBIOGET_VSCREENINFO, &fb->var) < 0 ||
	    ioctl(fb->fd, FBIOGET_FSCREENINFO, &fix) < 0) {
		fprintf(stderr, "keyoverlay: FBIOGET_*SCREENINFO: %s\n",
			strerror(errno));
		close(fb->fd);
		return -1;
	}
    // Assumption: the resolution doesn't change.
	fb->xres = fb->var.xres;
	fb->yres = fb->var.yres;
	fb->line_length = fix.line_length;
	fb->bpp = fb->var.bits_per_pixel / 8;
	if (fb->bpp != 2 && fb->bpp != 4) {
		fprintf(stderr, "keyoverlay: unsupported bpp %u\n",
			fb->var.bits_per_pixel);
		close(fb->fd);
		return -1;
	}

	fb->map_size = fix.smem_len ? fix.smem_len
				    : (size_t)fb->line_length * fb->yres;
	fb->mem = mmap(NULL, fb->map_size, PROT_READ | PROT_WRITE, MAP_SHARED,
		       fb->fd, 0);
	if (fb->mem == MAP_FAILED) {
		fprintf(stderr, "keyoverlay: mmap: %s\n", strerror(errno));
		close(fb->fd);
		return -1;
	}

	return 0;
}

static void fb_close(struct fb *fb)
{
	if (fb->mem && fb->mem != MAP_FAILED)
		munmap(fb->mem, fb->map_size);
	if (fb->fd >= 0)
		close(fb->fd);
}

static inline void put_pixel(struct fb *fb, int x, int y, uint32_t v)
{
	if (x < 0 || y < 0 || (uint32_t)x >= fb->xres || (uint32_t)y >= fb->yres)
		return;
	uint8_t *p = fb->mem + (size_t)y * fb->line_length + (size_t)x * fb->bpp;
	if (fb->bpp == 2) {
		*(uint16_t *)p = (uint16_t)v;
	} else {
		*(uint32_t *)p = v;
	}
}

static void fill_rect(struct fb *fb, int x, int y, int w, int h, uint32_t v)
{
	for (int yy = y; yy < y + h; yy++)
		for (int xx = x; xx < x + w; xx++)
			put_pixel(fb, xx, yy, v);
}

static void draw_rect_outline(struct fb *fb, int x, int y, int w, int h,
			      uint32_t v)
{
	for (int xx = x; xx < x + w; xx++) {
		put_pixel(fb, xx, y, v);
		put_pixel(fb, xx, y + h - 1, v);
	}
	for (int yy = y; yy < y + h; yy++) {
		put_pixel(fb, x, yy, v);
		put_pixel(fb, x + w - 1, yy, v);
	}
}

static void draw_glyph(struct fb *fb, int x, int y, char ch, uint32_t fg)
{
	if ((unsigned char)ch < FONT_FIRST || (unsigned char)ch > FONT_LAST)
		ch = '?';
	const unsigned char *g = &font8x16[((unsigned char)ch - FONT_FIRST) * FONT_H];
	for (int row = 0; row < FONT_H; row++) {
		unsigned char bits = g[row];
		for (int col = 0; col < FONT_W; col++) {
			if (!(bits & (0x80 >> col)))
				continue;
			fill_rect(fb, x + col * SCALE, y + row * SCALE, SCALE,
				  SCALE, fg);
		}
	}
}

static void draw_text(struct fb *fb, int x, int y, const char *s, uint32_t fg)
{
	for (; *s; s++, x += GLYPH_W)
		draw_glyph(fb, x, y, *s, fg);
}

/* ------------------------------------------------------------------ */
/* Main                                                               */
/* ------------------------------------------------------------------ */

static void usage(const char *argv0)
{
	fprintf(stderr,
		"Usage: %s [options]\n"
		"  -h        this help\n",
		argv0);
}

int main(int argc, char **argv)
{
	while ((opt = getopt(argc, argv, "h")) != -1) {
	const char *fbpath = "/dev/fb0";
		switch (opt) {
		case 'h': usage(argv[0]); return 0;
		default: usage(argv[0]); return 2;
		}
	}

	struct fb fb;
	if (fb_open(&fb, fbpath) < 0) {
		close(ifd);
		return 1;
	}
	fb_close(&fb);
	return 0;
}
