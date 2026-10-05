// SPDX-License-Identifier: MIT
/*
 * keyoverlay - on-screen keymap cheat-sheet overlay for SHARP Brain devices.
 *
 * Listens to a Linux evdev input device and, while a modifier key is held,
 * asks the active render backend to display the corresponding key legend.
 * The overlay disappears when all modifiers are released.
 *
 *   Shift held              -> Shift layout
 *   Symbol (記号) held      -> Symbol layout
 *   Symbol + Shift held     -> Symbol+Shift layout
 *   "normal" trigger held   -> Normal layout (optional, configurable key)
 *
 * Backend selection at startup:
 *   - If DISPLAY is set and WITH_X11 was compiled in → X11 backend
 *   - Otherwise                                      → framebuffer backend
 *
 * The daemon is a passive observer: it never grabs the device, so keystrokes
 * still reach the console/applications as usual.
 */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

#include "keyoverlay.h"
#include "render_fb.h"
#include "render_x11.h"

/* ------------------------------------------------------------------ */
/* Input device helpers                                               */
/* ------------------------------------------------------------------ */

static int input_device_name(const char *path, char *name, size_t size)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	memset(name, 0, size);
	int result = ioctl(fd, EVIOCGNAME(size - 1), name);
	close(fd);
	return result;
}

static int find_input_device(const char *want, char *out, size_t outlen)
{
	for (int i = 0; i < 32; i++) {
		char path[64], name[256];
		snprintf(path, sizeof(path), "/dev/input/event%d", i);
		if (input_device_name(path, name, sizeof(name)) >= 0 && strstr(name, want)) {
			snprintf(out, outlen, "%s", path);
			return 0;
		}
	}
	return -1;
}

static void list_input_devices(void)
{
	for (int i = 0; i < 32; i++) {
		char path[64], name[256];
		snprintf(path, sizeof(path), "/dev/input/event%d", i);
		if (input_device_name(path, name, sizeof(name)) >= 0)
			printf("%s: %s\n", path, name);
	}
}

/* ------------------------------------------------------------------ */
/* Main                                                               */
/* ------------------------------------------------------------------ */

static volatile sig_atomic_t g_stop;

static int read_symbol_state(int fd, bool *symbol)
{
	char state[3];
	ssize_t count = pread(fd, state, sizeof(state), 0);
	if (count < 0)
		return -1;
	if (count != 2 || (state[0] != '0' && state[0] != '1') || state[1] != '\n') {
		errno = EINVAL;
		return -1;
	}
	*symbol = state[0] == '1';
	return 0;
}

static int open_symbol_state(int input_fd, const char *path, bool *symbol, int verbose)
{
	char sysfs_path[128];
	if (!path) {
		struct stat input_stat;
		if (fstat(input_fd, &input_stat) < 0) {
			perror("keyoverlay: stat input device");
			return -1;
		}
		snprintf(sysfs_path, sizeof(sysfs_path),
			 "/sys/dev/char/%u:%u/device/symbol_activated",
			 major(input_stat.st_rdev), minor(input_stat.st_rdev));
		path = sysfs_path;
	}
	int fd = open(path, O_RDONLY);
	if (fd < 0 || read_symbol_state(fd, symbol) < 0) {
		fprintf(stderr, "keyoverlay: read %s: %s\n", path, strerror(errno));
		if (fd >= 0)
			close(fd);
		return -1;
	}
	if (verbose)
		fprintf(stderr, "keyoverlay: Symbol state from %s\n", path);
	return fd;
}

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
		"  -s PATH   Symbol state sysfs file (default: selected input device's symbol_activated)\n"
		"  -n CODE   key code that triggers the Normal layout (default: 0 = disabled)\n"
		"  -l        list input devices and exit\n"
		"  -v        verbose\n"
		"  -h        this help\n",
		argv0);
}

int main(int argc, char **argv)
{
	const char *dev         = NULL;
	const char *match       = "brain-kbd";
	const char *fbpath      = "/dev/fb0";
	const char *symbol_path = NULL;
	int         normal_code = 0;        /* KEY_RESERVED = disabled */
	int         verbose     = 0;
	char        devbuf[64];
	int         opt;

	while ((opt = getopt(argc, argv, "d:m:f:s:n:lvh")) != -1) {
		switch (opt) {
		case 'd': dev         = optarg;       break;
		case 'm': match       = optarg;       break;
		case 'f': fbpath      = optarg;       break;
		case 's': symbol_path = optarg;       break;
		case 'n': normal_code = atoi(optarg); break;
		case 'l': list_input_devices(); return 0;
		case 'v': verbose     = 1;            break;
		case 'h': usage(argv[0]); return 0;
		default:  usage(argv[0]); return 2;
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

	int result = 1;
	int sfd = -1;
	render_backend *backend = NULL;
	int ifd = open(dev, O_RDONLY);
	if (ifd < 0) {
		fprintf(stderr, "keyoverlay: open %s: %s\n", dev, strerror(errno));
		return 1;
	}
	if (verbose)
		fprintf(stderr, "keyoverlay: listening on %s\n", dev);

	bool shift = false, symbol = false, normal = false;
	sfd = open_symbol_state(ifd, symbol_path, &symbol, verbose);
	if (sfd < 0)
		goto cleanup;

	/* Select render backend: prefer X11 when DISPLAY is available. */

#ifdef WITH_X11
	const char *display = getenv("DISPLAY");
	if (display && display[0]) {
		if (verbose)
			fprintf(stderr, "keyoverlay: DISPLAY=%s, trying X11 backend\n",
				display);
		backend = render_x11_create(verbose);
		if (!backend)
			fprintf(stderr, "keyoverlay: X11 backend failed, "
				"falling back to framebuffer\n");
	}
#endif

	if (!backend)
		backend = render_fb_create(fbpath, verbose);

	if (!backend)
		goto cleanup;

	struct sigaction sa = {0};
	sa.sa_handler = on_signal;
	sigaction(SIGINT,  &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	enum layout_id shown = L_NONE;
	struct pollfd inputs[] = {
		{ .fd = ifd, .events = POLLIN },
		{ .fd = sfd, .events = POLLPRI },
	};
	result = 0;

	while (!g_stop) {
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

		if (want != shown) {
			if (want == L_NONE)
				backend->hide(backend);
			else
				backend->show(backend, &layouts[want]);
			shown = want;
			if (verbose)
				fprintf(stderr, "keyoverlay: layout %d\n", want);
		}

		if (poll(inputs, 2, -1) < 0) {
			if (errno == EINTR)
				continue;
			perror("keyoverlay: poll");
			result = 1;
			break;
		}
		if ((inputs[0].revents & (POLLERR | POLLHUP | POLLNVAL)) ||
		    (inputs[1].revents & (POLLHUP | POLLNVAL))) {
			fprintf(stderr, "keyoverlay: input device disconnected\n");
			result = 1;
			break;
		}
		if (inputs[1].revents & (POLLPRI | POLLERR)) {
			if (read_symbol_state(sfd, &symbol) < 0) {
				perror("keyoverlay: read Symbol state");
				result = 1;
				break;
			}
		}
		if (inputs[0].revents & POLLIN) {
			struct input_event ev;
			ssize_t count = read(ifd, &ev, sizeof(ev));
			if (count < 0 && errno == EINTR)
				continue;
			if (count != (ssize_t)sizeof(ev)) {
				fprintf(stderr, "keyoverlay: failed to read input event\n");
				result = 1;
				break;
			}
			if (ev.type != EV_KEY || ev.value == 2)
				continue;
			bool down = ev.value == 1;
			if (ev.code == KEY_LEFTSHIFT || ev.code == KEY_RIGHTSHIFT)
				shift = down;
			else if (normal_code && ev.code == (unsigned)normal_code)
				normal = down;
		}
	}

	if (shown != L_NONE)
		backend->hide(backend);

cleanup:
	if (backend)
		backend->close(backend);
	if (sfd >= 0)
		close(sfd);
	close(ifd);
	return result;
}
