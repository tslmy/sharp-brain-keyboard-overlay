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

#include <getopt.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
		switch (opt) {
		case 'h': usage(argv[0]); return 0;
		default: usage(argv[0]); return 2;
		}
	}

	return 0;
}
