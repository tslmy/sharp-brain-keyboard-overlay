#include <assert.h>

#include "../src/render_fb.c"
#ifdef WITH_X11
#include "../src/render_x11.c"
#endif

static uint64_t image_hash(const uint8_t *pixels, size_t size)
{
	uint64_t hash = UINT64_C(14695981039346656037);
	for (size_t index = 0; index < size; index++) {
		hash ^= pixels[index];
		hash *= UINT64_C(1099511628211);
	}
	return hash;
}

int main(void)
{
	const struct { unsigned width, height, bpp; } screens[] = {
		{800, 480, 2}, {800, 480, 4}, {320, 240, 2},
	};
	const uint64_t expected[][4] = {
		{UINT64_C(0x640384b711379f35), UINT64_C(0x82804159badd99fd),
		 UINT64_C(0x7c327b776ddf2f1d), UINT64_C(0x30603d4c2ff144a5)},
		{UINT64_C(0xa27c950c83fe5505), UINT64_C(0xd6747aaf0404aa45),
		 UINT64_C(0x78c2a2775d8d4c85), UINT64_C(0x5506ff520ed56a45)},
		{UINT64_C(0xe8b93615ed54c7c5), UINT64_C(0xdb75ab57528ea0ad),
		 UINT64_C(0x819e7d1907178b2d), UINT64_C(0xec0308571d29ccf5)},
	};
	for (size_t screen = 0; screen < sizeof(screens) / sizeof(screens[0]); screen++) {
		struct fb_backend backend = {0};
		struct fb *fb = &backend.fb;
		fb->xres = screens[screen].width;
		fb->yres = screens[screen].height;
		fb->bpp = screens[screen].bpp;
		fb->line_length = fb->xres * fb->bpp + 16;
		fb->map_size = (size_t)fb->line_length * fb->yres;
		fb->var.red = (struct fb_bitfield){fb->bpp == 2 ? 11 : 16, fb->bpp == 2 ? 5 : 8, 0};
		fb->var.green = (struct fb_bitfield){fb->bpp == 2 ? 5 : 8, fb->bpp == 2 ? 6 : 8, 0};
		fb->var.blue = (struct fb_bitfield){0, fb->bpp == 2 ? 5 : 8, 0};
		fb->mem = malloc(fb->map_size);
		uint8_t *original = malloc(fb->map_size);
		assert(fb->mem && original);
		for (size_t index = 0; index < fb->map_size; index++)
			original[index] = fb->mem[index] = (uint8_t)(index * 37);
		backend.panel = compute_panel((int)fb->xres, (int)fb->yres);
		backend.backup = malloc((size_t)backend.panel.w * backend.panel.h * fb->bpp);
		assert(backend.backup);
#ifdef WITH_X11
		render_backend *x11 = screen == 1 ? render_x11_create(0) : NULL;
		assert(screen != 1 || x11);
#endif
		for (int layout = 0; layout < N_LAYOUTS; layout++) {
			fb_show(&backend.base, &layouts[layout]);
			assert(image_hash(fb->mem, fb->map_size) == expected[screen][layout]);
#ifdef WITH_X11
			if (x11) {
				x11->show(x11, &layouts[layout]);
				struct x11_backend *window = (struct x11_backend *)x11;
				assert(window->panel.w == backend.panel.w && window->panel.h == backend.panel.h);
				XSync(window->dpy, False);
				XImage *image = XGetImage(window->dpy, window->win, 0, 0,
					(unsigned)window->panel.w, (unsigned)window->panel.h, AllPlanes, ZPixmap);
				assert(image);
				for (int y = 0; y < window->panel.h; y++) {
					for (int x = 0; x < window->panel.w; x++) {
						uint32_t pixel;
						memcpy(&pixel, fb->mem + (size_t)(backend.panel.y + y) * fb->line_length +
						       (size_t)(backend.panel.x + x) * fb->bpp, sizeof(pixel));
						assert(XGetPixel(image, x, y) == pixel);
					}
				}
				XDestroyImage(image);
			}
#endif
		}
#ifdef WITH_X11
		if (x11) {
			x11->hide(x11);
			x11->close(x11);
		}
#endif
		fb_hide(&backend.base);
		assert(memcmp(fb->mem, original, fb->map_size) == 0);
		fb_hide(&backend.base);
		assert(memcmp(fb->mem, original, fb->map_size) == 0);
		free(backend.backup);
		free(fb->mem);
		free(original);
	}
	puts("Rendering pixels and framebuffer restoration passed");
	return 0;
}