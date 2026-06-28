// SPDX-License-Identifier: MIT
//
// build.zig - replaces the old Makefile.
//
// Examples:
//   zig build -Dtarget=arm-linux-gnueabihf -Doptimize=ReleaseFast
//       Framebuffer-only build for the Brain (Buildroot/Brainux console).
//       No external dependencies; Zig bundles the Linux UAPI headers.
//
//   zig build -Dx11=true
//       Also build the X11 backend (requires libX11 headers + library on the
//       build host / sysroot).
//
// The program only runs on Linux; cross-compile with -Dtarget when building
// from another OS (e.g. macOS).

const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const with_x11 = b.option(
        bool,
        "x11",
        "Build the X11 backend (requires libX11). Default: false.",
    ) orelse false;

    const options = b.addOptions();
    options.addOption(bool, "with_x11", with_x11);

    const exe_mod = b.createModule(.{
        .root_source_file = b.path("src/main.zig"),
        .target = target,
        .optimize = optimize,
        .link_libc = true,
        .strip = optimize != .Debug,
    });
    exe_mod.addImport("build_options", options.createModule());

    const exe = b.addExecutable(.{
        .name = "keyoverlay",
        .root_module = exe_mod,
    });

    if (with_x11)
        exe_mod.linkSystemLibrary("X11", .{});

    b.installArtifact(exe);

    // `zig build run` convenience (only meaningful when building for the host).
    const run_cmd = b.addRunArtifact(exe);
    run_cmd.step.dependOn(b.getInstallStep());
    if (b.args) |args| run_cmd.addArgs(args);

    const run_step = b.step("run", "Run keyoverlay");
    run_step.dependOn(&run_cmd.step);
}
