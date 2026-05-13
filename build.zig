const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const exe_mod = b.createModule(.{
        .root_source_file = b.path("src/main.zig"),
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });

    // stb_image_write needs C compilation (disable UB sanitizer — stb uses intentional overflow)
    exe_mod.addCSourceFiles(.{
        .files = &.{"src/c/stb_impl.c"},
        .flags = &.{"-fno-sanitize=undefined"},
    });
    exe_mod.addIncludePath(b.path("src/c"));

    exe_mod.linkSystemLibrary("crypto", .{});

    const exe = b.addExecutable(.{
        .name = "vnc-mcp-server",
        .root_module = exe_mod,
    });

    b.installArtifact(exe);

    // Run step
    const run_cmd = b.addRunArtifact(exe);
    run_cmd.step.dependOn(b.getInstallStep());
    if (b.args) |args| {
        run_cmd.addArgs(args);
    }

    const run_step = b.step("run", "Run vnc-mcp-server");
    run_step.dependOn(&run_cmd.step);

    // Helper agent (Windows cross-compile)
    // Step 1: Compile resource file (.rc → .res.o)
    const helper_step = b.step("helper", "Cross-compile vnc-helper.exe for Windows");
    const compile_rc = b.addSystemCommand(&.{
        "zig",    "rc",
        "/i",     "helper/resources",
        "/fo",    "zig-out/bin/vnc-helper.res.o",
        "helper/resources/vnc-helper.rc",
    });
    compile_rc.step.dependOn(b.getInstallStep());

    // Step 2: Compile and link with resource object
    const build_helper = b.addSystemCommand(&.{
        "zig",       "cc",
        "helper/vnc-helper.c",
        "helper/vnc-helper-auth.c",
        "helper/vnc-helper-commands.c",
        "helper/vnc-helper-registry.c",
        "helper/vnc-helper-process.c",
        "helper/vnc-helper-ocr.c",
        "helper/vnc-helper-uia.c",
        "zig-out/bin/vnc-helper.res.o",
        "-target",   "x86_64-windows-gnu",
        "-Ihelper",
        "-Ihelper/resources",
        "-O2",
        "-Wl,--subsystem,windows",
        "-o",        "zig-out/bin/vnc-helper.exe",
        "-lws2_32",
        "-lshell32",
        "-luser32",
        "-lgdi32",
        "-ladvapi32",
    });
    build_helper.step.dependOn(&compile_rc.step);
    helper_step.dependOn(&build_helper.step);

    // Unit tests
    const test_mod = b.createModule(.{
        .root_source_file = b.path("src/main.zig"),
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });

    test_mod.addCSourceFiles(.{
        .files = &.{"src/c/stb_impl.c"},
    });
    test_mod.addIncludePath(b.path("src/c"));

    const unit_tests = b.addTest(.{
        .name = "vnc-mcp-server-tests",
        .root_module = test_mod,
    });

    const run_unit_tests = b.addRunArtifact(unit_tests);
    const test_step = b.step("test", "Run unit tests");
    test_step.dependOn(&run_unit_tests.step);
}
