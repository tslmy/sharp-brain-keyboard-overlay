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

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <linux/fb.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>

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
	uint8_t *backup;
	size_t map_size;     /* bytes mmapped */
	size_t buf_size;     /* yres * line_length */
	uint32_t xres, yres;
	uint32_t line_length;
	uint32_t bpp;        /* bytes per pixel (2 or 4) */
	struct fb_var_screeninfo var;
};

static uint32_t pack_color(const struct fb *fb, struct rgb c)
{
	uint32_t r = c.r >> (8 - fb->var.red.length);
	uint32_t g = c.g >> (8 - fb->var.green.length);
	uint32_t b = c.b >> (8 - fb->var.blue.length);
	return (r << fb->var.red.offset) | (g << fb->var.green.offset) |
	       (b << fb->var.blue.offset);
}

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
	fb->buf_size = (size_t)fb->line_length * fb->yres;
	fb->mem = mmap(NULL, fb->map_size, PROT_READ | PROT_WRITE, MAP_SHARED,
		       fb->fd, 0);
	if (fb->mem == MAP_FAILED) {
		fprintf(stderr, "keyoverlay: mmap: %s\n", strerror(errno));
		close(fb->fd);
		return -1;
	}

	fb->backup = NULL;  /* allocated later after panel size is known */
	return 0;
}

static void fb_close(struct fb *fb)
{
	if (fb->backup)
		free(fb->backup);
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

/*
 * Copy a rectangular region from framebuffer to a linear buffer.
 * Handles the fact that framebuffer memory is laid out with line_length
 * bytes per row, not necessarily w*bpp.
 */
static void fb_region_to_buf(struct fb *fb, int x, int y, int w, int h,
			       uint8_t *buf)
{
	for (int yy = 0; yy < h; yy++) {
		uint8_t *src = fb->mem + (size_t)(y + yy) * fb->line_length +
			       (size_t)x * fb->bpp;
		uint8_t *dst = buf + (size_t)yy * w * fb->bpp;
		memcpy(dst, src, (size_t)w * fb->bpp);
	}
}

/*
 * Copy a rectangular region from a linear buffer to framebuffer.
 * Inverse of fb_region_to_buf().
 */
static void buf_to_fb_region(struct fb *fb, int x, int y, int w, int h,
			       const uint8_t *buf)
{
	for (int yy = 0; yy < h; yy++) {
		uint8_t *dst = fb->mem + (size_t)(y + yy) * fb->line_length +
			       (size_t)x * fb->bpp;
		const uint8_t *src = buf + (size_t)yy * w * fb->bpp;
		memcpy(dst, src, (size_t)w * fb->bpp);
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
/* Layout rendering                                                   */
/* ------------------------------------------------------------------ */

static int cell_width(const char *label)
{
	int len = (int)strlen(label);
	int w = len * GLYPH_W + 2 * HPAD;
	return w < MIN_CELL_W ? MIN_CELL_W : w;
}

static int row_width(const Row *r)
{
	int w = (r->indent_half * MIN_CELL_W) / 2;
	for (int i = 0; i < r->n; i++) {
		w += cell_width(r->label[i]);
		if (i + 1 < r->n)
			w += HGAP;
	}
	return w;
}

static void layout_size(const Layout *L, int *out_w, int *out_h)
{
	int maxw = 0;
	for (int i = 0; i < L->nrows; i++) {
		int w = row_width(&L->rows[i]);
		if (w > maxw)
			maxw = w;
	}
	*out_w = maxw;
	*out_h = GLYPH_H + TITLE_GAP + L->nrows * CELL_H +
		 (L->nrows - 1) * VGAP;
}

/*
 * The panel is sized once to fit the largest layout so that every layout is
 * drawn within a fixed rectangle. Each draw repaints the whole panel, which
 * wipes any previous overlay content without needing a restore in between.
 */
struct panel {
	int x, y, w, h;
};

static void compute_panel(struct fb *fb, struct panel *p)
{
	int maxw = 0, maxh = 0;
	for (size_t i = 0; i < sizeof(layouts) / sizeof(layouts[0]); i++) {
		int w, h;
		layout_size(&layouts[i], &w, &h);
		if (w > maxw)
			maxw = w;
		if (h > maxh)
			maxh = h;
	}
	p->w = maxw + 2 * PANEL_PAD;
	p->h = maxh + 2 * PANEL_PAD;
	if (p->w > (int)fb->xres)
		p->w = fb->xres;
	if (p->h > (int)fb->yres)
		p->h = fb->yres;
	p->x = ((int)fb->xres - p->w) / 2;
	p->y = ((int)fb->yres - p->h) / 2;

	/* Allocate region-sized backup buffer now that we know panel size */
	if (!fb->backup) {
		size_t region_size = (size_t)p->w * p->h * fb->bpp;
		fb->backup = malloc(region_size);
		if (!fb->backup) {
			fprintf(stderr, "keyoverlay: out of memory for region backup\n");
			exit(1);
		}
	}
}

static void draw_layout(struct fb *fb, const struct panel *p, const Layout *L)
{
	uint32_t panel = pack_color(fb, COL_PANEL);
	uint32_t border = pack_color(fb, COL_BORDER);
	uint32_t cell = pack_color(fb, COL_CELL);
	uint32_t cell_empty = pack_color(fb, COL_CELL_EMPTY);
	uint32_t text = pack_color(fb, COL_TEXT);
	uint32_t title = pack_color(fb, COL_TITLE);

	fill_rect(fb, p->x, p->y, p->w, p->h, panel);
	draw_rect_outline(fb, p->x, p->y, p->w, p->h, border);
	draw_rect_outline(fb, p->x + 1, p->y + 1, p->w - 2, p->h - 2, border);

	int content_x = p->x + PANEL_PAD;
	int y = p->y + PANEL_PAD;

	draw_text(fb, content_x, y, L->title, title);
	y += GLYPH_H + TITLE_GAP;

	for (int ri = 0; ri < L->nrows; ri++) {
		const Row *r = &L->rows[ri];
		int x = content_x + (r->indent_half * MIN_CELL_W) / 2;
		for (int ci = 0; ci < r->n; ci++) {
			const char *label = r->label[ci];
			int cw = cell_width(label);
			bool empty = (label[0] == '\0');
			fill_rect(fb, x, y, cw, CELL_H,
				  empty ? cell_empty : cell);
			if (!empty) {
				int tw = (int)strlen(label) * GLYPH_W;
				int tx = x + (cw - tw) / 2;
				int ty = y + (CELL_H - GLYPH_H) / 2;
				draw_text(fb, tx, ty, label, text);
			}
			x += cw + HGAP;
		}
		y += CELL_H + VGAP;
	}
}

/* ------------------------------------------------------------------ */
/* Input device                                                       */
/* ------------------------------------------------------------------ */

static int dev_name_matches(const char *path, const char *want)
{
	char name[256] = {0};
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return 0;
	int ok = 0;
	if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) >= 0)
		ok = (strstr(name, want) != NULL);
	close(fd);
	return ok;
}

static int find_input_device(const char *want, char *out, size_t outlen)
{
	for (int i = 0; i < 32; i++) {
		char path[64];
		snprintf(path, sizeof(path), "/dev/input/event%d", i);
		if (dev_name_matches(path, want)) {
			snprintf(out, outlen, "%s", path);
			return 0;
		}
	}
	return -1;
}

static void list_input_devices(void)
{
	for (int i = 0; i < 32; i++) {
		char path[64], name[256] = {0};
		snprintf(path, sizeof(path), "/dev/input/event%d", i);
		int fd = open(path, O_RDONLY);
		if (fd < 0)
			continue;
		if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) >= 0)
			printf("%s: %s\n", path, name);
		close(fd);
	}
}

/* ------------------------------------------------------------------ */
/* Main                                                               */
/* ------------------------------------------------------------------ */

static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"Usage: %s [options]\n"
		"  -d DEV    input device (default: auto-detect by name)\n"
		"  -m NAME   input device name substring for auto-detect (default: brain-kbd)\n"
		"  -f FB     framebuffer device (default: /dev/fb0)\n"
		"  -s CODE   key code emitted while Symbol (記号) is held (default: 186 = KEY_F16)\n"
		"  -n CODE   key code that triggers the Normal layout (default: 0 = disabled)\n"
		"  -l        list input devices and exit\n"
		"  -v        verbose\n"
		"  -h        this help\n",
		argv0);
}

int main(int argc, char **argv)
{
	const char *dev = NULL;
	const char *match = "brain-kbd"; /* Name of the input device to search for as a substring. */
	const char *fbpath = "/dev/fb0";
	int symbol_code = KEY_F16;
	int normal_code = 0; /* KEY_RESERVED = disabled */
	int verbose = 0;
	char devbuf[64];
	int opt;

	while ((opt = getopt(argc, argv, "d:m:f:s:n:lvh")) != -1) {
		switch (opt) {
		case 'd': dev = optarg; break;
		case 'm': match = optarg; break;
		case 'f': fbpath = optarg; break;
		case 's': symbol_code = atoi(optarg); break;
		case 'n': normal_code = atoi(optarg); break;
		case 'l': list_input_devices(); return 0;
		case 'v': verbose = 1; break;
		case 'h': usage(argv[0]); return 0;
		default: usage(argv[0]); return 2;
		}
	}

	if (!dev) {
		if (find_input_device(match, devbuf, sizeof(devbuf)) == 0) {
			dev = devbuf;
		} else {
			fprintf(stderr,
				"keyoverlay: no input device matching \"%s\" found; "
				"use -d or -l\n", match);
			return 1;
		}
	}

	int ifd = open(dev, O_RDONLY);
	if (ifd < 0) {
		fprintf(stderr, "keyoverlay: open %s: %s\n", dev, strerror(errno));
		return 1;
	}
	if (verbose)
		fprintf(stderr, "keyoverlay: listening on %s\n", dev);

	struct fb fb;
	if (fb_open(&fb, fbpath) < 0) {
		close(ifd);
		return 1;
	}
	if (verbose)
		fprintf(stderr, "keyoverlay: fb %ux%u %ubpp\n", fb.xres, fb.yres,
			fb.var.bits_per_pixel);

	struct panel panel;
	compute_panel(&fb, &panel);

	struct sigaction sa = {0};
	sa.sa_handler = on_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	bool shift = false, symbol = false, normal = false;
	enum layout_id shown = L_NONE;

	while (!g_stop) {
		struct input_event ev;
		ssize_t n = read(ifd, &ev, sizeof(ev));
		if (n < 0) {
			if (errno == EINTR)
				break;
			fprintf(stderr, "keyoverlay: read: %s\n",
				strerror(errno));
			break;
		}
		if (n != (ssize_t)sizeof(ev))
			continue;
		if (ev.type != EV_KEY)
			continue;
		if (ev.value == 2) /* autorepeat */
			continue;

		bool down = (ev.value == 1);
		if (ev.code == KEY_LEFTSHIFT || ev.code == KEY_RIGHTSHIFT)
			shift = down;
		else if (ev.code == symbol_code)
			symbol = down;
		else if (normal_code && ev.code == normal_code)
			normal = down;
		else
			continue; /* not a trigger key */

		enum layout_id want;
		if (normal)
			want = L_NORMAL;
		else if (symbol && shift)
			want = L_SYMSHIFT;
		else if (symbol)
			want = L_SYMBOL;
		else if (shift)
			want = L_SHIFT;
		else
			want = L_NONE;

		if (want == shown)
			continue;

		if (want == L_NONE) {
			/* restore the panel region from backup */
			buf_to_fb_region(&fb, panel.x, panel.y, panel.w, panel.h,
					       fb.backup);
		} else {
			if (shown == L_NONE) /* first overlay: backup panel region */
				fb_region_to_buf(&fb, panel.x, panel.y, panel.w, panel.h,
						       fb.backup);
			draw_layout(&fb, &panel, &layouts[want]);
		}
		shown = want;
		if (verbose)
			fprintf(stderr, "keyoverlay: layout %d\n", want);
	}

	if (shown != L_NONE)
		buf_to_fb_region(&fb, panel.x, panel.y, panel.w, panel.h,
				       fb.backup);
	fb_close(&fb);
	close(ifd);
	return 0;
}
