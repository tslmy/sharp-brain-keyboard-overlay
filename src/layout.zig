// SPDX-License-Identifier: MIT
//
// layout.zig - shared layout types, color palette, geometry helpers, and the
// concrete keymap layout tables.  All rendering backends consume these; the
// geometry helpers compute identical results to the original C inline helpers.

const font = @import("font8x16.zig");

// ------------------------------------------------------------------ //
// Layout types                                                        //
// ------------------------------------------------------------------ //

pub const Row = struct {
    label: []const []const u8,
    /// Indent in half cell-widths, mirrors the physical key stagger.
    indent_half: i32 = 0,
};

pub const Layout = struct {
    title: []const u8,
    rows: []const Row,
};

// ------------------------------------------------------------------ //
// Color                                                               //
// ------------------------------------------------------------------ //

pub const Rgb = struct { r: u8, g: u8, b: u8 };

pub const COL_PANEL = Rgb{ .r = 20, .g = 20, .b = 24 };
pub const COL_BORDER = Rgb{ .r = 210, .g = 210, .b = 210 };
pub const COL_CELL = Rgb{ .r = 120, .g = 120, .b = 120 };
pub const COL_CELL_EMPTY = Rgb{ .r = 70, .g = 70, .b = 74 };
pub const COL_TEXT = Rgb{ .r = 255, .g = 255, .b = 255 };
pub const COL_TITLE = Rgb{ .r = 255, .g = 255, .b = 255 };

// ------------------------------------------------------------------ //
// Rendering geometry constants (match font8x16: FONT_W=8, FONT_H=16)  //
// ------------------------------------------------------------------ //

pub const FONT_SCALE = 2;
pub const GLYPH_W = font.FONT_W * FONT_SCALE; // 16 px
pub const GLYPH_H = font.FONT_H * FONT_SCALE; // 32 px
pub const HPAD = 6;
pub const VPAD = 6;
pub const MIN_CELL_W = GLYPH_W + 2 * HPAD;
pub const CELL_H = GLYPH_H + 2 * VPAD;
pub const HGAP = 6;
pub const VGAP = 6;
pub const PANEL_PAD = 16;
pub const TITLE_GAP = 10;

// ------------------------------------------------------------------ //
// Panel geometry helpers                                              //
// ------------------------------------------------------------------ //

pub const Panel = struct { x: i32, y: i32, w: i32, h: i32 };

pub fn cellWidth(label: []const u8) i32 {
    const len: i32 = @intCast(label.len);
    const w = len * GLYPH_W + 2 * HPAD;
    return if (w < MIN_CELL_W) MIN_CELL_W else w;
}

pub fn rowWidth(r: *const Row) i32 {
    var w = @divTrunc(r.indent_half * MIN_CELL_W, 2);
    for (r.label, 0..) |label, i| {
        w += cellWidth(label);
        if (i + 1 < r.label.len)
            w += HGAP;
    }
    return w;
}

pub fn layoutSize(l: *const Layout) struct { w: i32, h: i32 } {
    var maxw: i32 = 0;
    for (l.rows) |*r| {
        const w = rowWidth(r);
        if (w > maxw) maxw = w;
    }
    const nrows: i32 = @intCast(l.rows.len);
    return .{
        .w = maxw,
        .h = GLYPH_H + TITLE_GAP + nrows * CELL_H + (nrows - 1) * VGAP,
    };
}

/// Compute the panel rectangle centered on a screen of the given dimensions,
/// sized to fit the largest layout across all entries in `layouts`.
pub fn computePanel(screen_w: i32, screen_h: i32) Panel {
    var maxw: i32 = 0;
    var maxh: i32 = 0;
    for (&layouts) |*l| {
        const s = layoutSize(l);
        if (s.w > maxw) maxw = s.w;
        if (s.h > maxh) maxh = s.h;
    }
    var p: Panel = undefined;
    p.w = maxw + 2 * PANEL_PAD;
    p.h = maxh + 2 * PANEL_PAD;
    if (p.w > screen_w) p.w = screen_w;
    if (p.h > screen_h) p.h = screen_h;
    p.x = @divTrunc(screen_w - p.w, 2);
    p.y = @divTrunc(screen_h - p.h, 2);
    return p;
}

// ------------------------------------------------------------------ //
// Layout definitions                                                  //
// ------------------------------------------------------------------ //

/// Function/top row is identical across all layouts.
const func_row = Row{ .label = &.{ "Pwr", "Esc", "Tab", "PgU", "PgD", "Ins", "Del" } };
/// Ctrl/Alt bottom row is identical across all layouts.
const ctrl_row = Row{ .label = &.{ "Ctrl", "Alt" } };

const normal_rows = [_]Row{
    func_row,
    .{ .label = &.{ "q", "w", "e", "r", "t", "y", "u", "i", "o", "p" } },
    .{ .label = &.{ "a", "s", "d", "f", "g", "h", "j", "k", "l" }, .indent_half = 1 },
    .{ .label = &.{ "Shift", "z", "x", "c", "v", "b", "n", "m", "-", "BS" } },
    ctrl_row,
};

const shift_rows = [_]Row{
    func_row,
    .{ .label = &.{ "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P" } },
    .{ .label = &.{ "A", "S", "D", "F", "G", "H", "J", "K", "L" }, .indent_half = 1 },
    .{ .label = &.{ "Shift", "Z", "X", "C", "V", "B", "N", "M", "_", "BS" } },
    ctrl_row,
};

const symbol_rows = [_]Row{
    func_row,
    .{ .label = &.{ "1", "2", "3", "4", "5", "6", "7", "8", "9", "0" } },
    .{ .label = &.{ "", "", "`", "=", "\\", ";", "'", "[", "]" }, .indent_half = 1 },
    .{ .label = &.{ "Shift", "", "", "", "", "", ",", ".", "/", "BS" } },
    ctrl_row,
};

const symshift_rows = [_]Row{
    func_row,
    .{ .label = &.{ "!", "@", "#", "$", "%", "^", "&", "*", "(", ")" } },
    .{ .label = &.{ "", "", "~", "+", "|", ":", "\"", "{", "}" }, .indent_half = 1 },
    .{ .label = &.{ "Shift", "", "", "", "", "", "<", ">", "?", "BS" } },
    ctrl_row,
};

pub const Id = enum(i32) {
    none = -1,
    normal = 0,
    shift = 1,
    symbol = 2,
    symshift = 3,
};

pub const layouts = [_]Layout{
    .{ .title = "Normal", .rows = &normal_rows },
    .{ .title = "Shift", .rows = &shift_rows },
    .{ .title = "Symbol", .rows = &symbol_rows },
    .{ .title = "Symbol + Shift", .rows = &symshift_rows },
};

pub fn byId(id: Id) *const Layout {
    return &layouts[@intCast(@intFromEnum(id))];
}
