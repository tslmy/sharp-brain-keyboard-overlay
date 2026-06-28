// SPDX-License-Identifier: MIT
//
// linux.zig - Linux UAPI types/constants plus the handful of libc syscall
// wrappers the program needs.
//
// Only the *kernel* UAPI headers are pulled in via @cImport (Zig bundles
// them, so the framebuffer build cross-compiles from any host).  The libc
// entry points (open/read/close/ioctl/mmap/…) are declared directly instead
// of @cInclude-ing <fcntl.h>/<unistd.h>, because glibc's fortified inline
// wrappers (bits/fcntl2.h) fail to translate on some targets.

pub const c = @cImport({
    @cInclude("linux/input.h");
    @cInclude("linux/fb.h");
});

// -- open flags -------------------------------------------------------- //
pub const O_RDONLY: c_int = 0;
pub const O_RDWR: c_int = 2;

// -- mmap ------------------------------------------------------------- //
pub const PROT_READ: c_int = 0x1;
pub const PROT_WRITE: c_int = 0x2;
pub const MAP_SHARED: c_int = 0x1;
pub const MAP_FAILED: ?*anyopaque = @ptrFromInt(~@as(usize, 0));

// -- signals ---------------------------------------------------------- //
pub const SIGINT: c_int = 2;
pub const SIGTERM: c_int = 15;
pub const SigHandler = *const fn (c_int) callconv(.c) void;

// -- libc entry points ------------------------------------------------ //
pub extern "c" fn open(path: [*:0]const u8, flags: c_int, ...) c_int;
pub extern "c" fn close(fd: c_int) c_int;
pub extern "c" fn read(fd: c_int, buf: *anyopaque, nbytes: usize) isize;
pub extern "c" fn ioctl(fd: c_int, request: c_ulong, ...) c_int;
pub extern "c" fn mmap(addr: ?*anyopaque, len: usize, prot: c_int, flags: c_int, fd: c_int, offset: c_long) ?*anyopaque;
pub extern "c" fn munmap(addr: *anyopaque, len: usize) c_int;
pub extern "c" fn signal(sig: c_int, handler: SigHandler) usize;

