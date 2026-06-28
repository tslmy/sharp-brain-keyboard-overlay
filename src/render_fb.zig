// SPDX-License-Identifier: MIT
//
// render_fb.zig - framebuffer render backend.
//
// Renders the overlay directly onto /dev/fb0 (or a user-supplied path).
// Before the first draw the panel region is saved to a heap buffer; it is
// restored when the overlay is hidden.  Pixel format (RGB565 / XRGB8888 / …)
// is detected at runtime via FBIOGET_VSCREENINFO.

const std = @import("std");
const lx = @import("linux.zig");
const c = lx.c;
const font = @import("font8x16.zig");
const lo = @import("layout.zig");

const Layout = lo.Layout;
const Panel = lo.Panel;
const Rgb = lo.Rgb;
const Backend = @import("render.zig").Backend;

fn errno() c_int {
    return std.c._errno().*;
}

pub const FbBackend = struct {
    alloc: std.mem.Allocator,
    fd: c_int,
    mem: []u8,
    map_size: usize,
    xres: u32,
    yres: u32,
    line_length: u32,
    bpp: u32, // bytes per pixel
    var_info: c.struct_fb_var_screeninfo,
    panel: Panel,
    backup: []u8,
    shown: bool,

    const vtable = Backend.VTable{
        .show = showThunk,
        .hide = hideThunk,
        .deinit = deinitThunk,
    };

    pub fn backend(self: *FbBackend) Backend {
        return .{ .ptr = self, .vtable = &vtable };
    }

    // -------------------------------------------------------------- //
    // Color packing                                                   //
    // -------------------------------------------------------------- //

    fn packColor(self: *const FbBackend, col: Rgb) u32 {
        const v = &self.var_info;
        const r = @as(u32, col.r) >> @intCast(8 - v.red.length);
        const g = @as(u32, col.g) >> @intCast(8 - v.green.length);
        const b = @as(u32, col.b) >> @intCast(8 - v.blue.length);
        return (r << @intCast(v.red.offset)) |
            (g << @intCast(v.green.offset)) |
            (b << @intCast(v.blue.offset));
    }

    // -------------------------------------------------------------- //
    // Pixel / rectangle primitives                                    //
    // -------------------------------------------------------------- //

    fn putPixel(self: *FbBackend, x: i32, y: i32, v: u32) void {
        if (x < 0 or y < 0 or
            x >= @as(i32, @intCast(self.xres)) or
            y >= @as(i32, @intCast(self.yres)))
            return;
        const off = @as(usize, @intCast(y)) * self.line_length +
            @as(usize, @intCast(x)) * self.bpp;
        const p = self.mem.ptr + off;
        if (self.bpp == 2) {
            @as(*align(1) u16, @ptrCast(p)).* = @truncate(v);
        } else {
            @as(*align(1) u32, @ptrCast(p)).* = v;
        }
    }

    fn fillRect(self: *FbBackend, x: i32, y: i32, w: i32, h: i32, v: u32) void {
        var yy = y;
        while (yy < y + h) : (yy += 1) {
            var xx = x;
            while (xx < x + w) : (xx += 1)
                self.putPixel(xx, yy, v);
        }
    }

    fn drawRectOutline(self: *FbBackend, x: i32, y: i32, w: i32, h: i32, v: u32) void {
        var xx = x;
        while (xx < x + w) : (xx += 1) {
            self.putPixel(xx, y, v);
            self.putPixel(xx, y + h - 1, v);
        }
        var yy = y;
        while (yy < y + h) : (yy += 1) {
            self.putPixel(x, yy, v);
            self.putPixel(x + w - 1, yy, v);
        }
    }

    // -------------------------------------------------------------- //
    // Region save / restore                                           //
    // -------------------------------------------------------------- //

    fn regionSave(self: *FbBackend) void {
        const p = self.panel;
        const pw: usize = @intCast(p.w);
        const ph: usize = @intCast(p.h);
        const px: usize = @intCast(p.x);
        const py: usize = @intCast(p.y);
        const row_bytes = pw * self.bpp;
        var yy: usize = 0;
        while (yy < ph) : (yy += 1) {
            const src = (py + yy) * self.line_length + px * self.bpp;
            const dst = yy * row_bytes;
            @memcpy(self.backup[dst .. dst + row_bytes], self.mem[src .. src + row_bytes]);
        }
    }

    fn regionRestore(self: *FbBackend) void {
        const p = self.panel;
        const pw: usize = @intCast(p.w);
        const ph: usize = @intCast(p.h);
        const px: usize = @intCast(p.x);
        const py: usize = @intCast(p.y);
        const row_bytes = pw * self.bpp;
        var yy: usize = 0;
        while (yy < ph) : (yy += 1) {
            const dst = (py + yy) * self.line_length + px * self.bpp;
            const src = yy * row_bytes;
            @memcpy(self.mem[dst .. dst + row_bytes], self.backup[src .. src + row_bytes]);
        }
    }

    // -------------------------------------------------------------- //
    // Glyph / text rendering                                          //
    // -------------------------------------------------------------- //

    fn drawGlyph(self: *FbBackend, x: i32, y: i32, ch: u8, fg: u32) void {
        var cc = ch;
        if (cc < font.FONT_FIRST or cc > font.FONT_LAST) cc = '?';
        const base = (@as(usize, cc) - font.FONT_FIRST) * font.FONT_H;
        var row: usize = 0;
        while (row < font.FONT_H) : (row += 1) {
            const bits = font.font8x16[base + row];
            var col: usize = 0;
            while (col < font.FONT_W) : (col += 1) {
                if (bits & (@as(u8, 0x80) >> @intCast(col)) == 0) continue;
                self.fillRect(
                    x + @as(i32, @intCast(col)) * lo.FONT_SCALE,
                    y + @as(i32, @intCast(row)) * lo.FONT_SCALE,
                    lo.FONT_SCALE,
                    lo.FONT_SCALE,
                    fg,
                );
            }
        }
    }

    fn drawText(self: *FbBackend, x: i32, y: i32, s: []const u8, fg: u32) void {
        var cx = x;
        for (s) |ch| {
            self.drawGlyph(cx, y, ch, fg);
            cx += lo.GLYPH_W;
        }
    }

    // -------------------------------------------------------------- //
    // Layout rendering                                                //
    // -------------------------------------------------------------- //

    fn drawLayout(self: *FbBackend, l: *const Layout) void {
        const p = self.panel;

        const col_panel = self.packColor(lo.COL_PANEL);
        const col_border = self.packColor(lo.COL_BORDER);
        const col_cell = self.packColor(lo.COL_CELL);
        const col_cell_empty = self.packColor(lo.COL_CELL_EMPTY);
        const col_text = self.packColor(lo.COL_TEXT);
        const col_title = self.packColor(lo.COL_TITLE);

        self.fillRect(p.x, p.y, p.w, p.h, col_panel);
        self.drawRectOutline(p.x, p.y, p.w, p.h, col_border);
        self.drawRectOutline(p.x + 1, p.y + 1, p.w - 2, p.h - 2, col_border);

        const content_x = p.x + lo.PANEL_PAD;
        var y = p.y + lo.PANEL_PAD;

        self.drawText(content_x, y, l.title, col_title);
        y += lo.GLYPH_H + lo.TITLE_GAP;

        for (l.rows) |*r| {
            var x = content_x + @divTrunc(r.indent_half * lo.MIN_CELL_W, 2);
            for (r.label) |label| {
                const cw = lo.cellWidth(label);
                const empty = label.len == 0;
                self.fillRect(x, y, cw, lo.CELL_H, if (empty) col_cell_empty else col_cell);
                if (!empty) {
                    const tw = @as(i32, @intCast(label.len)) * lo.GLYPH_W;
                    const tx = x + @divTrunc(cw - tw, 2);
                    const ty = y + @divTrunc(lo.CELL_H - lo.GLYPH_H, 2);
                    self.drawText(tx, ty, label, col_text);
                }
                x += cw + lo.HGAP;
            }
            y += lo.CELL_H + lo.VGAP;
        }
    }

    // -------------------------------------------------------------- //
    // Backend vtable                                                  //
    // -------------------------------------------------------------- //

    fn show(self: *FbBackend, l: *const Layout) void {
        if (!self.shown) self.regionSave();
        self.drawLayout(l);
        self.shown = true;
    }

    fn hide(self: *FbBackend) void {
        if (!self.shown) return;
        self.regionRestore();
        self.shown = false;
    }

    fn deinit(self: *FbBackend) void {
        const alloc = self.alloc;
        alloc.free(self.backup);
        _ = lx.munmap(@ptrCast(self.mem.ptr), self.map_size);
        _ = lx.close(self.fd);
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
// Constructor                                                         //
// ------------------------------------------------------------------ //

pub fn create(alloc: std.mem.Allocator, fbpath: [*:0]const u8, verbose: bool) ?Backend {
    const self = alloc.create(FbBackend) catch return null;
    errdefer alloc.destroy(self);

    const fd = lx.open(fbpath, lx.O_RDWR);
    if (fd < 0) {
        std.debug.print("keyoverlay: open {s}: errno {d}\n", .{ fbpath, errno() });
        return null;
    }
    errdefer _ = lx.close(fd);

    var var_info: c.struct_fb_var_screeninfo = undefined;
    var fix: c.struct_fb_fix_screeninfo = undefined;
    if (lx.ioctl(fd, c.FBIOGET_VSCREENINFO, &var_info) < 0 or
        lx.ioctl(fd, c.FBIOGET_FSCREENINFO, &fix) < 0)
    {
        std.debug.print("keyoverlay: FBIOGET_*SCREENINFO failed\n", .{});
        return null;
    }

    const xres = var_info.xres;
    const yres = var_info.yres;
    const line_length = fix.line_length;
    const bpp = var_info.bits_per_pixel / 8;
    if (bpp != 2 and bpp != 4) {
        std.debug.print("keyoverlay: unsupported bpp {d}\n", .{var_info.bits_per_pixel});
        return null;
    }

    const map_size: usize = if (fix.smem_len != 0)
        fix.smem_len
    else
        @as(usize, line_length) * yres;

    const raw = lx.mmap(null, map_size, lx.PROT_READ | lx.PROT_WRITE, lx.MAP_SHARED, fd, 0);
    if (raw == lx.MAP_FAILED) {
        std.debug.print("keyoverlay: mmap failed\n", .{});
        return null;
    }
    const mem = @as([*]u8, @ptrCast(raw))[0..map_size];
    errdefer _ = lx.munmap(@ptrCast(mem.ptr), map_size);

    if (verbose)
        std.debug.print("keyoverlay: fb {d}x{d} {d}bpp\n", .{ xres, yres, var_info.bits_per_pixel });

    const panel = lo.computePanel(@intCast(xres), @intCast(yres));

    const region_sz = @as(usize, @intCast(panel.w)) * @as(usize, @intCast(panel.h)) * bpp;
    const backup = alloc.alloc(u8, region_sz) catch {
        std.debug.print("keyoverlay: out of memory for region backup\n", .{});
        return null;
    };

    self.* = .{
        .alloc = alloc,
        .fd = fd,
        .mem = mem,
        .map_size = map_size,
        .xres = xres,
        .yres = yres,
        .line_length = line_length,
        .bpp = bpp,
        .var_info = var_info,
        .panel = panel,
        .backup = backup,
        .shown = false,
    };
    return self.backend();
}
