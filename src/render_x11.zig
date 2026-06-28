// SPDX-License-Identifier: MIT
//
// render_x11.zig - X11 render backend.
//
// Creates an override-redirect, always-on-top window positioned at the same
// panel rectangle used by the framebuffer backend.  The window is unmapped
// when no overlay is shown, so it is invisible and does not interfere with
// other applications.  Glyph rendering reuses the same font8x16 bitmap data
// (via XFillRectangle), so both backends produce identical-looking output.
//
// This file is only referenced (and therefore only compiled, pulling in the
// libX11 dependency) when the program is built with -Dx11=true.

const std = @import("std");
const font = @import("font8x16.zig");
const lo = @import("layout.zig");

const Layout = lo.Layout;
const Panel = lo.Panel;
const Rgb = lo.Rgb;
const Backend = @import("render.zig").Backend;

const x = @cImport({
    @cInclude("X11/Xlib.h");
    @cInclude("X11/Xutil.h");
    @cInclude("X11/Xatom.h");
});

pub const X11Backend = struct {
    alloc: std.mem.Allocator,
    dpy: *x.Display,
    screen: c_int,
    win: x.Window,
    gc: x.GC,
    panel: Panel,
    shown: bool,

    px_panel: c_ulong,
    px_border: c_ulong,
    px_cell: c_ulong,
    px_cell_empty: c_ulong,
    px_text: c_ulong,
    px_title: c_ulong,

    const vtable = Backend.VTable{
        .show = showThunk,
        .hide = hideThunk,
        .deinit = deinitThunk,
    };

    pub fn backend(self: *X11Backend) Backend {
        return .{ .ptr = self, .vtable = &vtable };
    }

    // -------------------------------------------------------------- //
    // Primitive drawing (onto the overlay window)                     //
    // -------------------------------------------------------------- //

    fn fillRect(self: *X11Backend, px_x: i32, px_y: i32, w: i32, h: i32, px: c_ulong) void {
        _ = x.XSetForeground(self.dpy, self.gc, px);
        _ = x.XFillRectangle(self.dpy, self.win, self.gc, px_x, px_y, @intCast(w), @intCast(h));
    }

    fn drawGlyph(self: *X11Backend, px_x: i32, px_y: i32, ch: u8, fg: c_ulong) void {
        var cc = ch;
        if (cc < font.FONT_FIRST or cc > font.FONT_LAST) cc = '?';
        const base = (@as(usize, cc) - font.FONT_FIRST) * font.FONT_H;
        _ = x.XSetForeground(self.dpy, self.gc, fg);
        var row: usize = 0;
        while (row < font.FONT_H) : (row += 1) {
            const bits = font.font8x16[base + row];
            var col: usize = 0;
            while (col < font.FONT_W) : (col += 1) {
                if (bits & (@as(u8, 0x80) >> @intCast(col)) == 0) continue;
                _ = x.XFillRectangle(
                    self.dpy,
                    self.win,
                    self.gc,
                    px_x + @as(i32, @intCast(col)) * lo.FONT_SCALE,
                    px_y + @as(i32, @intCast(row)) * lo.FONT_SCALE,
                    lo.FONT_SCALE,
                    lo.FONT_SCALE,
                );
            }
        }
    }

    fn drawText(self: *X11Backend, px_x: i32, px_y: i32, s: []const u8, fg: c_ulong) void {
        var cx = px_x;
        for (s) |ch| {
            self.drawGlyph(cx, px_y, ch, fg);
            cx += lo.GLYPH_W;
        }
    }

    // -------------------------------------------------------------- //
    // Layout rendering                                                //
    // -------------------------------------------------------------- //

    fn drawLayout(self: *X11Backend, l: *const Layout) void {
        // The window covers exactly the panel rectangle, so all coordinates
        // here are relative to the window's top-left corner.
        const pw = self.panel.w;
        const ph = self.panel.h;

        self.fillRect(0, 0, pw, ph, self.px_panel);

        // Double border.
        _ = x.XSetForeground(self.dpy, self.gc, self.px_border);
        _ = x.XDrawRectangle(self.dpy, self.win, self.gc, 0, 0, @intCast(pw - 1), @intCast(ph - 1));
        _ = x.XDrawRectangle(self.dpy, self.win, self.gc, 1, 1, @intCast(pw - 3), @intCast(ph - 3));

        const x0 = lo.PANEL_PAD;
        var y: i32 = lo.PANEL_PAD;

        self.drawText(x0, y, l.title, self.px_title);
        y += lo.GLYPH_H + lo.TITLE_GAP;

        for (l.rows) |*r| {
            var cx = x0 + @divTrunc(r.indent_half * lo.MIN_CELL_W, 2);
            for (r.label) |label| {
                const cw = lo.cellWidth(label);
                const empty = label.len == 0;
                self.fillRect(cx, y, cw, lo.CELL_H, if (empty) self.px_cell_empty else self.px_cell);
                if (!empty) {
                    const tw = @as(i32, @intCast(label.len)) * lo.GLYPH_W;
                    const tx = cx + @divTrunc(cw - tw, 2);
                    const ty = y + @divTrunc(lo.CELL_H - lo.GLYPH_H, 2);
                    self.drawText(tx, ty, label, self.px_text);
                }
                cx += cw + lo.HGAP;
            }
            y += lo.CELL_H + lo.VGAP;
        }

        _ = x.XFlush(self.dpy);
    }

    // -------------------------------------------------------------- //
    // Backend vtable                                                  //
    // -------------------------------------------------------------- //

    fn show(self: *X11Backend, l: *const Layout) void {
        if (!self.shown) {
            _ = x.XMapRaised(self.dpy, self.win);
            self.shown = true;
        }
        self.drawLayout(l);
    }

    fn hide(self: *X11Backend) void {
        if (!self.shown) return;
        _ = x.XUnmapWindow(self.dpy, self.win);
        _ = x.XFlush(self.dpy);
        self.shown = false;
    }

    fn deinit(self: *X11Backend) void {
        const alloc = self.alloc;
        if (self.gc != null) _ = x.XFreeGC(self.dpy, self.gc);
        if (self.win != 0) _ = x.XDestroyWindow(self.dpy, self.win);
        _ = x.XCloseDisplay(self.dpy);
        alloc.destroy(self);
    }

    fn showThunk(ptr: *anyopaque, l: *const Layout) void {
        show(@ptrCast(@alignCast(ptr)), l);
    }
    fn hideThunk(ptr: *anyopaque) void {
        hide(@ptrCast(@alignCast(ptr)));
    }
    fn deinitThunk(ptr: *anyopaque) void {
        deinit(@ptrCast(@alignCast(ptr)));
    }
};

// ------------------------------------------------------------------ //
// Color helper                                                        //
// ------------------------------------------------------------------ //

fn allocColor(dpy: *x.Display, screen: c_int, col: Rgb) c_ulong {
    var xc: x.XColor = std.mem.zeroes(x.XColor);
    xc.red = @as(c_ushort, col.r) << 8;
    xc.green = @as(c_ushort, col.g) << 8;
    xc.blue = @as(c_ushort, col.b) << 8;
    xc.flags = @intCast(x.DoRed | x.DoGreen | x.DoBlue);
    if (x.XAllocColor(dpy, x.XDefaultColormap(dpy, screen), &xc) != 0)
        return xc.pixel;
    // Fallback: compose a 24-bit value for TrueColor displays.
    return (@as(c_ulong, col.r) << 16) | (@as(c_ulong, col.g) << 8) | col.b;
}

// ------------------------------------------------------------------ //
// Constructor                                                         //
// ------------------------------------------------------------------ //

pub fn create(alloc: std.mem.Allocator, verbose: bool) ?Backend {
    const self = alloc.create(X11Backend) catch return null;
    errdefer alloc.destroy(self);

    const dpy = x.XOpenDisplay(null) orelse {
        std.debug.print("keyoverlay: cannot open X display\n", .{});
        return null;
    };
    errdefer _ = x.XCloseDisplay(dpy);

    const screen = x.XDefaultScreen(dpy);
    const screen_w = x.XDisplayWidth(dpy, screen);
    const screen_h = x.XDisplayHeight(dpy, screen);

    if (verbose)
        std.debug.print("keyoverlay: X11 display {d}x{d}\n", .{ screen_w, screen_h });

    const panel = lo.computePanel(screen_w, screen_h);

    const px_panel = allocColor(dpy, screen, lo.COL_PANEL);

    // Create an override-redirect window (bypasses the window manager).
    var attr: x.XSetWindowAttributes = std.mem.zeroes(x.XSetWindowAttributes);
    attr.override_redirect = 1; // True
    attr.background_pixel = px_panel;

    const win = x.XCreateWindow(
        dpy,
        x.XRootWindow(dpy, screen),
        panel.x,
        panel.y,
        @intCast(panel.w),
        @intCast(panel.h),
        0, // border width
        x.XDefaultDepth(dpy, screen),
        x.InputOutput,
        x.XDefaultVisual(dpy, screen),
        x.CWOverrideRedirect | x.CWBackPixel,
        &attr,
    );
    errdefer _ = x.XDestroyWindow(dpy, win);

    // Hint to EWMH compositors that this is a notification-style window.
    const wm_type = x.XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", x.False);
    var type_notif = x.XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_NOTIFICATION", x.False);
    _ = x.XChangeProperty(dpy, win, wm_type, x.XA_ATOM, 32, x.PropModeReplace, @ptrCast(&type_notif), 1);

    // Set a descriptive title for debugging.
    _ = x.XStoreName(dpy, win, "keyoverlay");

    const gc = x.XCreateGC(dpy, win, 0, null);
    if (gc == null) {
        std.debug.print("keyoverlay: XCreateGC failed\n", .{});
        return null;
    }

    self.* = .{
        .alloc = alloc,
        .dpy = dpy,
        .screen = screen,
        .win = win,
        .gc = gc,
        .panel = panel,
        .shown = false,
        .px_panel = px_panel,
        .px_border = allocColor(dpy, screen, lo.COL_BORDER),
        .px_cell = allocColor(dpy, screen, lo.COL_CELL),
        .px_cell_empty = allocColor(dpy, screen, lo.COL_CELL_EMPTY),
        .px_text = allocColor(dpy, screen, lo.COL_TEXT),
        .px_title = allocColor(dpy, screen, lo.COL_TITLE),
    };

    // Window starts unmapped (invisible) until show() is called.
    _ = x.XFlush(dpy);
    return self.backend();
}
