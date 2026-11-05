const std = @import("std");
const builtin = @import("builtin");

fn engine(b: *std.Build, target: std.Build.ResolvedTarget, optimize: std.builtin.OptimizeMode) *std.Build.Module {
    const module = b.createModule(.{
        .root_source_file = b.path("src/c_api.zig"),
        .target = target,
        .optimize = optimize,
        .link_libc = true,
        .pic = true,
    });
    module.addIncludePath(b.path("vendor/sqlite"));
    module.addIncludePath(b.path("include"));
    module.addIncludePath(b.path("src"));
    module.addCSourceFile(.{ .file = b.path("src/backup_file.c"), .flags = &.{ "-std=c11", "-Wall", "-Wextra", "-Werror", "-fvisibility=hidden" } });
    module.addCSourceFile(.{ .file = b.path("vendor/sqlite/sqlite3.c"), .flags = &.{
        "-std=c99",       "-DSQLITE_THREADSAFE=1",        "-DSQLITE_OMIT_LOAD_EXTENSION",
        "-DSQLITE_DQS=0", "-DSQLITE_DEFAULT_MEMSTATUS=0", "-fvisibility=hidden",
    } });
    return module;
}

pub fn build(b: *std.Build) void {
    // Keep native macOS artifacts linkable by the system C compiler's baseline.
    const target = b.standardTargetOptions(.{
        .default_target = if (builtin.os.tag == .macos)
            .{ .os_version_min = .{ .semver = .{ .major = 14, .minor = 0, .patch = 0 } } }
        else
            .{},
    });
    const optimize = b.standardOptimizeOption(.{});
    const static = b.addLibrary(.{ .name = "fslite", .linkage = .static, .root_module = engine(b, target, optimize) });
    // Include compiler helpers and sanitizer handlers for native C consumers.
    static.bundle_compiler_rt = true;
    static.bundle_ubsan_rt = true;
    const shared = b.addLibrary(.{ .name = "fslite", .linkage = .dynamic, .root_module = engine(b, target, optimize) });
    b.installArtifact(static);
    b.installArtifact(shared);
    b.installFile("include/fslite.h", "include/fslite.h");

    const test_step = b.step("test", "Run the C ABI integration tests with static and shared libraries");
    inline for (.{ static, shared }, .{ "static", "shared" }) |lib, name| {
        const module = b.createModule(.{ .target = target, .optimize = optimize, .link_libc = true });
        module.addIncludePath(b.path("include"));
        module.addIncludePath(b.path("vendor/sqlite"));
        module.addCSourceFile(.{ .file = b.path("tests/abi.c"), .flags = &.{ "-std=c11", "-Wall", "-Wextra", "-Werror" } });
        module.addCSourceFile(.{ .file = b.path("tests/namespace.c"), .flags = &.{ "-std=c11", "-Wall", "-Wextra", "-Werror" } });
        module.addCSourceFile(.{ .file = b.path("tests/backup.c"), .flags = &.{ "-std=c11", "-Wall", "-Wextra", "-Werror" } });
        module.addCSourceFile(.{ .file = b.path("tests/random_access.c"), .flags = &.{ "-std=c11", "-Wall", "-Wextra", "-Werror" } });
        module.addCSourceFile(.{ .file = b.path("tests/integrity.c"), .flags = &.{ "-std=c11", "-Wall", "-Wextra", "-Werror" } });
        module.addCSourceFile(.{ .file = b.path("tests/transactions.c"), .flags = &.{ "-std=c11", "-Wall", "-Wextra", "-Werror" } });
        module.linkLibrary(lib);
        // Only the static test calls SQLite directly to construct foreign-format fixtures.
        if (comptime std.mem.eql(u8, name, "static")) module.addCMacro("FSLITE_TEST_SQLITE", "1");
        const exe = b.addExecutable(.{ .name = "abi-" ++ name, .root_module = module });
        const run = b.addRunArtifact(exe);
        test_step.dependOn(&run.step);
    }

    const bench_build = b.step("build-bench", "Install standalone static/shared benchmark executables");
    bench_build.dependOn(b.getInstallStep());
    const bench_step = b.step("bench", "Run full benchmarks sequentially (use ReleaseSafe)");
    const smoke_step = b.step("bench-smoke", "Run short benchmark correctness checks sequentially");
    var previous_bench: ?*std.Build.Step = null;
    var previous_smoke: ?*std.Build.Step = null;
    inline for (.{ static, shared }, .{ "static", "shared" }) |lib, linkage| {
        inline for (.{ "small_files", "streaming_writes", "random_access", "memory_usage", "full_performance" }) |name| {
            const module = b.createModule(.{ .target = target, .optimize = optimize, .link_libc = true });
            module.addIncludePath(b.path("include"));
            module.addCMacro("FSLITE_BENCH_LINKAGE", "\"" ++ linkage ++ "\"");
            module.addCMacro("FSLITE_BENCH_OPTIMIZE", b.fmt("\"{s}\"", .{@tagName(optimize)}));
            module.addCSourceFile(.{ .file = b.path("tests/bench/" ++ name ++ "_bench.c"), .flags = &.{ "-std=c11", "-Wall", "-Wextra", "-Werror" } });
            module.linkLibrary(lib);
            const exe = b.addExecutable(.{ .name = name ++ "-bench-" ++ linkage, .root_module = module });
            const install = b.addInstallArtifact(exe, .{});
            bench_build.dependOn(&install.step);
            // These benchmarks write durable volumes; avoid competing disk workloads.
            const run = b.addRunArtifact(exe);
            run.has_side_effects = true;
            run.step.dependOn(bench_build);
            if (previous_bench) |previous| run.step.dependOn(previous);
            previous_bench = &run.step;
            bench_step.dependOn(&run.step);
            const smoke = b.addRunArtifact(exe);
            smoke.addArg("--smoke");
            smoke.has_side_effects = true;
            smoke.step.dependOn(bench_build);
            if (previous_smoke) |previous| smoke.step.dependOn(previous);
            previous_smoke = &smoke.step;
            smoke_step.dependOn(&smoke.step);
        }
    }
}
