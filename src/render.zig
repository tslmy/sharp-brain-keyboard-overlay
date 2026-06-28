// SPDX-License-Identifier: MIT
//
// render.zig - render backend interface.
//
// A `Backend` is a small fat pointer (instance pointer + vtable) so that
// backends can be compiled conditionally (the X11 backend is only referenced
// when built with -Dx11=true, keeping its libX11 dependency optional).  The
// core input loop in main.zig is backend-agnostic.

const Layout = @import("layout.zig").Layout;

pub const Backend = struct {
    ptr: *anyopaque,
    vtable: *const VTable,

    pub const VTable = struct {
        /// Show (or switch to) a layout.  On the first call the backend saves
        /// whatever is beneath the panel (or maps/raises a window); subsequent
        /// calls while visible simply repaint with the new layout.
        show: *const fn (ptr: *anyopaque, layout: *const Layout) void,
        /// Hide the overlay and restore the previous screen state.
        hide: *const fn (ptr: *anyopaque) void,
        /// Release all resources held by the backend.  The caller must not use
        /// the backend afterwards.
        deinit: *const fn (ptr: *anyopaque) void,
    };

    pub fn show(self: Backend, layout: *const Layout) void {
        self.vtable.show(self.ptr, layout);
    }
    pub fn hide(self: Backend) void {
        self.vtable.hide(self.ptr);
    }
    pub fn deinit(self: Backend) void {
        self.vtable.deinit(self.ptr);
    }
};
