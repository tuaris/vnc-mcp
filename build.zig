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

    // stb_image_write needs C compilation
    exe_mod.addCSourceFiles(.{
        .files = &.{"src/c/stb_impl.c"},
    });
    exe_mod.addIncludePath(b.path("src/c"));

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
