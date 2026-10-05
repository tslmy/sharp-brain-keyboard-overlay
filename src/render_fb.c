// SPDX-License-Identifier: MIT
/*
 * render_fb.c - framebuffer render backend.
 *
 * Renders the overlay directly onto /dev/fb0 (or a user-supplied path).
 * Before the first draw the panel region is saved to a heap buffer; it is
 * restored when the overlay is hidden.  Pixel format (RGB565 / XRGB8888 /
 * …) is detected at runtime via FBIOGET_VSCREENINFO.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "keyoverlay.h"
#include "render_fb.h"

/* ------------------------------------------------------------------ */
/* Internal framebuffer state                                         */
/* ------------------------------------------------------------------ */

struct fb {
	int      fd;
	uint8_t *mem;
	size_t   map_size;
	uint32_t xres, yres;
	uint32_t line_length;
	uint32_t bpp;           /* bytes per pixel */
	struct fb_var_screeninfo var;
};

struct fb_backend {
	render_backend  base;   /* MUST be first */
	struct fb       fb;
	struct panel    panel;
	uint8_t        *backup; /* saved pixels under the panel rectangle */
	bool            shown;
};

/* ------------------------------------------------------------------ */
/* Color packing                                                       */
/* ------------------------------------------------------------------ */

static uint32_t pack_color(const struct fb *fb, struct rgb c)
{
	uint32_t r = c.r >> (8 - fb->var.red.length);
	uint32_t g = c.g >> (8 - fb->var.green.length);
	uint32_t b = c.b >> (8 - fb->var.blue.length);
	return (r << fb->var.red.offset)   |
	       (g << fb->var.green.offset) |
	       (b << fb->var.blue.offset);
}

/* ------------------------------------------------------------------ */
/* Pixel / rectangle primitives                                        */
/* ------------------------------------------------------------------ */

static inline void put_pixel(struct fb *fb, int x, int y, uint32_t v)
{
	if (x < 0 || y < 0 || (uint32_t)x >= fb->xres || (uint32_t)y >= fb->yres)
		return;
	uint8_t *p = fb->mem + (size_t)y * fb->line_length + (size_t)x * fb->bpp;
	if (fb->bpp == 2)
		*(uint16_t *)p = (uint16_t)v;
	else
		*(uint32_t *)p = v;
}

static void fill_rect(void *context, int x, int y, int w, int h, unsigned long color)
{
	struct fb *fb = context;
	for (int yy = y; yy < y + h; yy++)
		for (int xx = x; xx < x + w; xx++)
			put_pixel(fb, xx, yy, (uint32_t)color);
}

/* ------------------------------------------------------------------ */
/* Region save / restore                                               */
/* ------------------------------------------------------------------ */

static void region_save(struct fb *fb, const struct panel *p, uint8_t *buf)
{
	for (int yy = 0; yy < p->h; yy++) {
		const uint8_t *src = fb->mem +
			(size_t)(p->y + yy) * fb->line_length +
			(size_t)p->x * fb->bpp;
		uint8_t *dst = buf + (size_t)yy * p->w * fb->bpp;
		memcpy(dst, src, (size_t)p->w * fb->bpp);
	}
}

static void region_restore(struct fb *fb, const struct panel *p,
			    const uint8_t *buf)
{
	for (int yy = 0; yy < p->h; yy++) {
		uint8_t *dst = fb->mem +
			(size_t)(p->y + yy) * fb->line_length +
			(size_t)p->x * fb->bpp;
		const uint8_t *src = buf + (size_t)yy * p->w * fb->bpp;
		memcpy(dst, src, (size_t)p->w * fb->bpp);
	}
}

/* ------------------------------------------------------------------ */
/* Backend vtable                                                      */
/* ------------------------------------------------------------------ */

static void fb_show(render_backend *base, const Layout *L)
{
	struct fb_backend *b = (struct fb_backend *)base;
	if (!b->shown)
		region_save(&b->fb, &b->panel, b->backup);
	unsigned long colors[COLOR_COUNT];
	for (int color = 0; color < COLOR_COUNT; color++)
		colors[color] = pack_color(&b->fb, render_colors[color]);
	struct render_canvas canvas = {&b->fb, fill_rect, colors};
	render_layout(&canvas, &b->panel, L);
	b->shown = true;
}

static void fb_hide(render_backend *base)
{
	struct fb_backend *b = (struct fb_backend *)base;
	if (!b->shown)
		return;
	region_restore(&b->fb, &b->panel, b->backup);
	b->shown = false;
}

static void fb_close(render_backend *base)
{
	struct fb_backend *b = (struct fb_backend *)base;
	struct fb *fb = &b->fb;
	free(b->backup);
	if (fb->mem && fb->mem != MAP_FAILED)
		munmap(fb->mem, fb->map_size);
	if (fb->fd >= 0)
		close(fb->fd);
	free(b);
}

/* ------------------------------------------------------------------ */
/* Constructor                                                         */
/* ------------------------------------------------------------------ */

render_backend *render_fb_create(const char *fbpath, int verbose)
{
	struct fb_backend *b = calloc(1, sizeof(*b));
	if (!b)
		return NULL;

	b->base.show  = fb_show;
	b->base.hide  = fb_hide;
	b->base.close = fb_close;

	struct fb *fb = &b->fb;
	struct fb_fix_screeninfo fix;

	fb->fd = open(fbpath, O_RDWR);
	if (fb->fd < 0) {
		fprintf(stderr, "keyoverlay: open %s: %s\n", fbpath, strerror(errno));
		goto error;
	}
	if (ioctl(fb->fd, FBIOGET_VSCREENINFO, &fb->var) < 0 ||
	    ioctl(fb->fd, FBIOGET_FSCREENINFO, &fix) < 0) {
		fprintf(stderr, "keyoverlay: FBIOGET_*SCREENINFO: %s\n",
			strerror(errno));
		goto error;
	}

	fb->xres        = fb->var.xres;
	fb->yres        = fb->var.yres;
	fb->line_length = fix.line_length;
	fb->bpp         = fb->var.bits_per_pixel / 8;
	if (fb->bpp != 2 && fb->bpp != 4) {
		fprintf(stderr, "keyoverlay: unsupported bpp %u\n",
			fb->var.bits_per_pixel);
		goto error;
	}

	fb->map_size = fix.smem_len
		? fix.smem_len
		: (size_t)fb->line_length * fb->yres;
	fb->mem = mmap(NULL, fb->map_size, PROT_READ | PROT_WRITE, MAP_SHARED,
		       fb->fd, 0);
	if (fb->mem == MAP_FAILED) {
		fprintf(stderr, "keyoverlay: mmap: %s\n", strerror(errno));
		goto error;
	}

	if (verbose)
		fprintf(stderr, "keyoverlay: fb %ux%u %ubpp\n",
			fb->xres, fb->yres, fb->var.bits_per_pixel);

	b->panel = compute_panel((int)fb->xres, (int)fb->yres);

	size_t region_sz = (size_t)b->panel.w * b->panel.h * fb->bpp;
	b->backup = malloc(region_sz);
	if (!b->backup) {
		fprintf(stderr, "keyoverlay: out of memory for region backup\n");
		goto error;
	}

	return &b->base;

error:
	fb_close(&b->base);
	return NULL;
}
