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

#include <dirent.h>
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
/* Layout definitions                                                 */
/* ------------------------------------------------------------------ */

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

/* Exported so that keyoverlay.h's inline geometry helpers can reference them. */
const Layout layouts[] = {
	[L_NORMAL]  = {"Normal",         normal_rows,   5},
	[L_SHIFT]   = {"Shift",          shift_rows,    5},
	[L_SYMBOL]  = {"Symbol",         symbol_rows,   5},
	[L_SYMSHIFT]= {"Symbol + Shift", symshift_rows, 5},
};
const int N_LAYOUTS = (int)(sizeof(layouts) / sizeof(layouts[0]));

/* ------------------------------------------------------------------ */
/* Input device helpers                                               */
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
	char        symbol_buf[4096];
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

	int ifd = open(dev, O_RDONLY);
	if (ifd < 0) {
		fprintf(stderr, "keyoverlay: open %s: %s\n", dev, strerror(errno));
		return 1;
	}
	if (verbose)
		fprintf(stderr, "keyoverlay: listening on %s\n", dev);

	if (!symbol_path) {
		struct stat input_stat;
		char input_sysfs[128], input_real[4096];
		if (fstat(ifd, &input_stat) < 0) {
			perror("keyoverlay: stat input device");
			close(ifd);
			return 1;
		}
		snprintf(input_sysfs, sizeof(input_sysfs), "/sys/dev/char/%u:%u/device",
			 major(input_stat.st_rdev), minor(input_stat.st_rdev));
		if (!realpath(input_sysfs, input_real)) {
			perror("keyoverlay: resolve input sysfs device");
			close(ifd);
			return 1;
		}
		if (snprintf(symbol_buf, sizeof(symbol_buf), "%s/symbol_activated",
			     input_real) >= (int)sizeof(symbol_buf)) {
			fprintf(stderr, "keyoverlay: Symbol sysfs path too long\n");
			close(ifd);
			return 1;
		}
		symbol_path = symbol_buf;
	}
	int sfd = open(symbol_path, O_RDONLY);
	bool shift = false, symbol = false, normal = false;
	if (sfd < 0 || read_symbol_state(sfd, &symbol) < 0) {
		fprintf(stderr, "keyoverlay: read %s: %s\n", symbol_path, strerror(errno));
		if (sfd >= 0)
			close(sfd);
		close(ifd);
		return 1;
	}
	if (verbose)
		fprintf(stderr, "keyoverlay: Symbol state from %s\n", symbol_path);

	/* Select render backend: prefer X11 when DISPLAY is available. */
	render_backend *backend = NULL;

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

	if (!backend) {
		close(sfd);
		close(ifd);
		return 1;
	}

	struct sigaction sa = {0};
	sa.sa_handler = on_signal;
	sigaction(SIGINT,  &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	enum layout_id shown = L_NONE;
	struct pollfd inputs[] = {
		{ .fd = ifd, .events = POLLIN },
		{ .fd = sfd, .events = POLLPRI },
	};
	int result = 0;

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

	backend->close(backend);
	close(sfd);
	close(ifd);
	return result;
}
