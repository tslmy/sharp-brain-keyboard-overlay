#include <string.h>

#include "font8x16.h"
#include "keyoverlay.h"

#define FUNC_ROW {{"Pwr", "Esc", "Tab", "PgU", "PgD", "Ins", "Del"}, 7, 0}
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

const Layout layouts[] = {
	[L_NORMAL]  = {"Normal",         normal_rows,   5},
	[L_SHIFT]   = {"Shift",          shift_rows,    5},
	[L_SYMBOL]  = {"Symbol",         symbol_rows,   5},
	[L_SYMSHIFT]= {"Symbol + Shift", symshift_rows, 5},
};
const int N_LAYOUTS = (int)(sizeof(layouts) / sizeof(layouts[0]));

#define FONT_SCALE 2
#define GLYPH_W (FONT_W * FONT_SCALE)
#define GLYPH_H (FONT_H * FONT_SCALE)
#define HPAD 6
#define VPAD 6
#define MIN_CELL_W (GLYPH_W + 2 * HPAD)
#define CELL_H (GLYPH_H + 2 * VPAD)
#define HGAP 6
#define VGAP 6
#define PANEL_PAD 16
#define TITLE_GAP 10

static int cell_width(const char *label)
{
	int width = (int)strlen(label) * GLYPH_W + 2 * HPAD;
	return width < MIN_CELL_W ? MIN_CELL_W : width;
}

struct panel compute_panel(int screen_w, int screen_h)
{
	int max_width = 0, max_height = 0;
	for (int layout = 0; layout < N_LAYOUTS; layout++) {
		for (int row = 0; row < layouts[layout].nrows; row++) {
			const Row *keys = &layouts[layout].rows[row];
			int width = (keys->indent_half * MIN_CELL_W) / 2;
			for (int cell = 0; cell < keys->n; cell++) {
				width += cell_width(keys->label[cell]);
				if (cell + 1 < keys->n)
					width += HGAP;
			}
			if (width > max_width)
				max_width = width;
		}
		int height = GLYPH_H + TITLE_GAP + layouts[layout].nrows * CELL_H +
			     (layouts[layout].nrows - 1) * VGAP;
		if (height > max_height)
			max_height = height;
	}
	int width = max_width + 2 * PANEL_PAD;
	int height = max_height + 2 * PANEL_PAD;
	if (width > screen_w)
		width = screen_w;
	if (height > screen_h)
		height = screen_h;
	return (struct panel){(screen_w - width) / 2, (screen_h - height) / 2, width, height};
}

const struct rgb render_colors[COLOR_COUNT] = {
	[COL_PANEL]      = {20, 20, 24},
	[COL_BORDER]     = {210, 210, 210},
	[COL_CELL]       = {120, 120, 120},
	[COL_CELL_EMPTY] = {70, 70, 74},
	[COL_TEXT]       = {255, 255, 255},
	[COL_TITLE]      = {255, 255, 255},
};

static void fill_rect(const struct render_canvas *canvas, int x, int y, int w,
		      int h, enum render_color color)
{
	canvas->fill_rect(canvas->context, x, y, w, h, canvas->colors[color]);
}

static void draw_border(const struct render_canvas *canvas, int x, int y, int w, int h)
{
	fill_rect(canvas, x, y, w, 1, COL_BORDER);
	fill_rect(canvas, x, y + h - 1, w, 1, COL_BORDER);
	fill_rect(canvas, x, y, 1, h, COL_BORDER);
	fill_rect(canvas, x + w - 1, y, 1, h, COL_BORDER);
}

static void draw_text(const struct render_canvas *canvas, int x, int y,
		      const char *text, enum render_color color)
{
	for (; *text; text++, x += GLYPH_W) {
		unsigned char ch = (unsigned char)*text;
		if (ch < FONT_FIRST || ch > FONT_LAST)
			ch = '?';
		const unsigned char *glyph = &font8x16[(ch - FONT_FIRST) * FONT_H];
		for (int row = 0; row < FONT_H; row++) {
			for (int col = 0; col < FONT_W; col++) {
				if (glyph[row] & (0x80 >> col))
					fill_rect(canvas, x + col * FONT_SCALE,
						  y + row * FONT_SCALE, FONT_SCALE,
						  FONT_SCALE, color);
			}
		}
	}
}

void render_layout(const struct render_canvas *canvas, const struct panel *panel,
		   const Layout *layout)
{
	fill_rect(canvas, panel->x, panel->y, panel->w, panel->h, COL_PANEL);
	draw_border(canvas, panel->x, panel->y, panel->w, panel->h);
	draw_border(canvas, panel->x + 1, panel->y + 1, panel->w - 2, panel->h - 2);

	int content_x = panel->x + PANEL_PAD;
	int y = panel->y + PANEL_PAD;
	draw_text(canvas, content_x, y, layout->title, COL_TITLE);
	y += GLYPH_H + TITLE_GAP;

	for (int row = 0; row < layout->nrows; row++) {
		const Row *keys = &layout->rows[row];
		int x = content_x + (keys->indent_half * MIN_CELL_W) / 2;
		for (int cell = 0; cell < keys->n; cell++) {
			const char *label = keys->label[cell];
			int width = cell_width(label);
			fill_rect(canvas, x, y, width, CELL_H,
				  *label ? COL_CELL : COL_CELL_EMPTY);
			if (*label) {
				int text_x = x + (width - (int)strlen(label) * GLYPH_W) / 2;
				int text_y = y + (CELL_H - GLYPH_H) / 2;
				draw_text(canvas, text_x, text_y, label, COL_TEXT);
			}
			x += width + HGAP;
		}
		y += CELL_H + VGAP;
	}
}