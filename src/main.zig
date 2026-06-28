// SPDX-License-Identifier: MIT
//
// keyoverlay - on-screen keymap cheat-sheet overlay for SHARP Brain devices.
//
// Listens to a Linux evdev input device and, while a modifier key is held,
// asks the active render backend to display the corresponding key legend.
// The overlay disappears when all modifiers are released.
//
//   Shift held              -> Shift layout
//   Symbol (記号) held      -> Symbol layout
//   Symbol + Shift held     -> Symbol+Shift layout
//   "normal" trigger held   -> Normal layout (optional, configurable key)
//
// Backend selection at startup:
//   - If DISPLAY is set and built with -Dx11=true → X11 backend
//   - Otherwise                                    → framebuffer backend
//
// The daemon is a passive observer: it never grabs the device, so keystrokes
// still reach the console/applications as usual.

const std = @import("std");
const build_options = @import("build_options");

const lx = @import("linux.zig");
const c = lx.c;
const lo = @import("layout.zig");
const Backend = @import("render.zig").Backend;
const render_fb = @import("render_fb.zig");
const render_x11 = @import("render_x11.zig");

const with_x11 = build_options.with_x11;

// ------------------------------------------------------------------ //
// ioctl request number (computed locally to stay correct on every     //
// target word size; translate-c's EVIOCGNAME can sign-extend on 64-bit)//
// ------------------------------------------------------------------ //

fn EVIOCGNAME(len: usize) c_ulong {
    // _IOC(_IOC_READ=2, 'E', 0x06, len)
    return (2 << 30) | (@as(c_ulong, 'E') << 8) | 0x06 | (@as(c_ulong, len) << 16);
}

// ------------------------------------------------------------------ //
// Input device helpers                                                //
// ------------------------------------------------------------------ //

fn devNameMatches(path: [:0]const u8, want: []const u8) bool {
    const fd = lx.open(path.ptr, lx.O_RDONLY);
    if (fd < 0) return false;
    defer _ = lx.close(fd);
    var name: [256]u8 = [_]u8{0} ** 256;
    if (lx.ioctl(fd, EVIOCGNAME(name.len - 1), &name) < 0)
        return false;
    const n = std.mem.sliceTo(&name, 0);
    return std.mem.indexOf(u8, n, want) != null;
}

fn findInputDevice(want: []const u8, out: []u8) ?[:0]const u8 {
    var i: u32 = 0;
    while (i < 32) : (i += 1) {
        var pathbuf: [64]u8 = undefined;
        const path = std.fmt.bufPrintZ(&pathbuf, "/dev/input/event{d}", .{i}) catch continue;
        if (devNameMatches(path, want))
            return std.fmt.bufPrintZ(out, "{s}", .{path}) catch null;
    }
    return null;
}

fn listInputDevices() void {
    var i: u32 = 0;
    while (i < 32) : (i += 1) {
        var pathbuf: [64]u8 = undefined;
        const path = std.fmt.bufPrintZ(&pathbuf, "/dev/input/event{d}", .{i}) catch continue;
        const fd = lx.open(path.ptr, lx.O_RDONLY);
        if (fd < 0) continue;
        defer _ = lx.close(fd);
        var name: [256]u8 = [_]u8{0} ** 256;
        if (lx.ioctl(fd, EVIOCGNAME(name.len - 1), &name) >= 0) {
            const n = std.mem.sliceTo(&name, 0);
            std.debug.print("{s}: {s}\n", .{ path, n });
        }
    }
}

// ------------------------------------------------------------------ //
// Signal handling                                                     //
// ------------------------------------------------------------------ //

var g_stop = std.atomic.Value(bool).init(false);

fn onSignal(_: c_int) callconv(.c) void {
    g_stop.store(true, .seq_cst);
}

// ------------------------------------------------------------------ //
// Usage                                                               //
// ------------------------------------------------------------------ //

fn usage(argv0: []const u8) void {
    std.debug.print(
        \\Usage: {s} [options]
        \\  -d DEV    input device (default: auto-detect by name)
        \\  -m NAME   input device name substring for auto-detect (default: brain-kbd)
        \\  -f FB     framebuffer device (default: /dev/fb0)
        \\  -s CODE   key code emitted while Symbol (記号) is held (default: 186 = KEY_F16)
        \\  -n CODE   key code that triggers the Normal layout (default: 0 = disabled)
        \\  -l        list input devices and exit
        \\  -v        verbose
        \\  -h        this help
        \\
    , .{argv0});
}

// ------------------------------------------------------------------ //
// Main                                                                //
// ------------------------------------------------------------------ //

pub fn main(init: std.process.Init.Minimal) u8 {
    const alloc = std.heap.c_allocator;

    // Collect command-line arguments into a small fixed buffer so the parser
    // below can index them.
    var argv_buf: [64][:0]const u8 = undefined;
    var argc: usize = 0;
    var arg_it = std.process.Args.Iterator.init(init.args);
    while (arg_it.next()) |a| {
        if (argc >= argv_buf.len) break;
        argv_buf[argc] = a;
        argc += 1;
    }
    const args = argv_buf[0..argc];
    const argv0 = if (args.len > 0) args[0] else "keyoverlay";

    var dev: ?[:0]const u8 = null;
    var match: [:0]const u8 = "brain-kbd";
    var fbpath: [:0]const u8 = "/dev/fb0";
    var symbol_code: i32 = c.KEY_F16;
    var normal_code: i32 = 0; // KEY_RESERVED = disabled
    var verbose = false;

    // Minimal getopt-style parser for the single-letter options above.
    var ai: usize = 1;
    while (ai < args.len) : (ai += 1) {
        const a = args[ai];
        if (a.len < 2 or a[0] != '-') {
            usage(argv0);
            return 2;
        }
        const opt = a[1];
        switch (opt) {
            'l' => {
                listInputDevices();
                return 0;
            },
            'v' => verbose = true,
            'h' => {
                usage(argv0);
                return 0;
            },
            'd', 'm', 'f', 's', 'n' => {
                ai += 1;
                if (ai >= args.len) {
                    usage(argv0);
                    return 2;
                }
                const val = args[ai];
                switch (opt) {
                    'd' => dev = val,
                    'm' => match = val,
                    'f' => fbpath = val,
                    's' => symbol_code = std.fmt.parseInt(i32, val, 10) catch 0,
                    'n' => normal_code = std.fmt.parseInt(i32, val, 10) catch 0,
                    else => unreachable,
                }
            },
            else => {
                usage(argv0);
                return 2;
            },
        }
    }

    var devbuf: [64]u8 = undefined;
    if (dev == null) {
        if (findInputDevice(match, &devbuf)) |found| {
            dev = found;
        } else {
            std.debug.print(
                "keyoverlay: no input device matching \"{s}\" found; use -d or -l\n",
                .{match},
            );
            return 1;
        }
    }

    const ifd = lx.open(dev.?.ptr, lx.O_RDONLY);
    if (ifd < 0) {
        std.debug.print("keyoverlay: open {s}: errno {d}\n", .{ dev.?, std.c._errno().* });
        return 1;
    }
    defer _ = lx.close(ifd);

    if (verbose)
        std.debug.print("keyoverlay: listening on {s}\n", .{dev.?});

    // Select render backend: prefer X11 when DISPLAY is available.
    var backend: ?Backend = null;

    if (comptime with_x11) {
        if (std.c.getenv("DISPLAY")) |display_z| {
            const display = std.mem.span(display_z);
            if (display.len > 0) {
                if (verbose)
                    std.debug.print("keyoverlay: DISPLAY={s}, trying X11 backend\n", .{display});
                backend = render_x11.create(alloc, verbose);
                if (backend == null)
                    std.debug.print("keyoverlay: X11 backend failed, falling back to framebuffer\n", .{});
            }
        }
    }

    if (backend == null)
        backend = render_fb.create(alloc, fbpath.ptr, verbose);

    if (backend == null)
        return 1;
    const be = backend.?;

    _ = lx.signal(lx.SIGINT, onSignal);
    _ = lx.signal(lx.SIGTERM, onSignal);

    var shift = false;
    var symbol = false;
    var normal = false;
    var shown: lo.Id = .none;

    while (!g_stop.load(.seq_cst)) {
        var ev: c.struct_input_event = undefined;
        const nread = lx.read(ifd, &ev, @sizeOf(@TypeOf(ev)));
        if (nread < 0)
            break;
        if (nread != @as(isize, @sizeOf(@TypeOf(ev))))
            continue;
        if (ev.type != c.EV_KEY)
            continue;
        if (ev.value == 2) // autorepeat
            continue;

        const down = ev.value == 1;
        const code: i32 = @intCast(ev.code);
        if (code == c.KEY_LEFTSHIFT or code == c.KEY_RIGHTSHIFT)
            shift = down
        else if (code == symbol_code)
            symbol = down
        else if (normal_code != 0 and code == normal_code)
            normal = down
        else
            continue; // not a trigger key

        const want: lo.Id = if (normal)
            .normal
        else if (symbol and shift)
            .symshift
        else if (symbol)
            .symbol
        else if (shift)
            .shift
        else
            .none;

        if (want == shown)
            continue;

        if (want == .none)
            be.hide()
        else
            be.show(lo.byId(want));

        shown = want;
        if (verbose)
            std.debug.print("keyoverlay: layout {d}\n", .{@intFromEnum(want)});
    }

    if (shown != .none)
        be.hide();

    be.deinit();
    return 0;
}
