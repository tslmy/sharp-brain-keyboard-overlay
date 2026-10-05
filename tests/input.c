#include <assert.h>
#include <poll.h>
#include <unistd.h>

static int test_poll(struct pollfd *fds, nfds_t count, int timeout);
static ssize_t test_read(int fd, void *buffer, size_t count);

#define main keyoverlay_main
#define poll test_poll
#define read test_read
#include "../src/keyoverlay.c"
#undef main
#undef poll
#undef read

struct step {
	int symbol;
	unsigned short code;
	int value;
	enum layout_id expected;
};

static const struct step steps[] = {
	{1, 0, 0, L_SYMBOL},
	{-1, KEY_LEFTSHIFT, 1, L_SYMSHIFT},
	{-1, KEY_LEFTSHIFT, 2, L_SYMSHIFT},
	{-1, KEY_LEFTSHIFT, 0, L_SYMBOL},
	{0, 0, 0, L_NONE},
	{-1, KEY_LEFTSHIFT, 1, L_SHIFT},
	{1, 0, 0, L_SYMSHIFT},
	{0, 0, 0, L_SHIFT},
	{-1, KEY_LEFTSHIFT, 0, L_NONE},
	{-1, KEY_F16, 1, L_NONE},
	{-1, KEY_F16, 0, L_NONE},
	{-1, KEY_F17, 1, L_NORMAL},
	{1, KEY_RIGHTSHIFT, 1, L_NORMAL},
	{-1, KEY_F17, 0, L_SYMSHIFT},
	{0, KEY_RIGHTSHIFT, 0, L_NONE},
};

static int state_fd;
static size_t next_step;
static enum layout_id current_layout;
static enum layout_id initial_layout;
static struct input_event pending;
static bool closed;

static int test_poll(struct pollfd *fds, nfds_t count, int timeout)
{
	assert(count == 2 && timeout == -1);
	assert(fds[0].events == POLLIN && fds[1].events == POLLPRI);
	assert(current_layout == (next_step ? steps[next_step - 1].expected : initial_layout));
	if (next_step == sizeof(steps) / sizeof(steps[0])) {
		g_stop = 1;
		fds[0].revents = fds[1].revents = 0;
		return 0;
	}
	const struct step *step = &steps[next_step++];
	fds[0].revents = step->code ? POLLIN : 0;
	fds[1].revents = step->symbol >= 0 ? POLLPRI | POLLERR : 0;
	if (step->symbol >= 0) {
		const char *state = step->symbol ? "1\n" : "0\n";
		assert(pwrite(state_fd, state, 2, 0) == 2);
	}
	pending = (struct input_event){ .type = EV_KEY, .code = step->code,
				       .value = step->value };
	return 1;
}

static ssize_t test_read(int fd, void *buffer, size_t count)
{
	(void)fd;
	assert(count == sizeof(pending));
	memcpy(buffer, &pending, count);
	return count;
}

static void test_show(render_backend *backend, const Layout *layout)
{
	(void)backend;
	current_layout = (enum layout_id)(layout - layouts);
}

static void test_hide(render_backend *backend)
{
	(void)backend;
	current_layout = L_NONE;
}

static void test_close(render_backend *backend)
{
	(void)backend;
	closed = true;
}

render_backend *render_fb_create(const char *path, int verbose)
{
	(void)path;
	(void)verbose;
	static render_backend backend = {test_show, test_hide, test_close};
	return &backend;
}

int main(void)
{
	char path[] = "/tmp/keyoverlay-state-XXXXXX";
	state_fd = mkstemp(path);
	assert(state_fd >= 0);
	for (int initial = 0; initial <= 1; initial++) {
		assert(pwrite(state_fd, initial ? "1\n" : "0\n", 2, 0) == 2);
		next_step = 0;
		current_layout = L_NONE;
		initial_layout = initial ? L_SYMBOL : L_NONE;
		closed = false;
		g_stop = 0;
		optind = 0;
		char *argv[] = {"keyoverlay", "-d", "/dev/null", "-s", path, "-n", "187", NULL};
		assert(keyoverlay_main(7, argv) == 0);
		assert(closed && current_layout == L_NONE);
	}
	bool symbol = false;
	const char *invalid[] = {"", "1", "2\n", "x\n", "1\nx"};
	for (size_t index = 0; index < sizeof(invalid) / sizeof(invalid[0]); index++) {
		assert(ftruncate(state_fd, 0) == 0);
		assert(pwrite(state_fd, invalid[index], strlen(invalid[index]), 0) ==
		       (ssize_t)strlen(invalid[index]));
		assert(read_symbol_state(state_fd, &symbol) == -1 && errno == EINVAL);
	}
	assert(read_symbol_state(-1, &symbol) == -1 && errno == EBADF);
	close(state_fd);
	unlink(path);
	puts("Symbol state and modifier transitions passed");
	return 0;
}