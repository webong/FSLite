const std = @import("std");
const engine = @import("engine.zig");
const Volume = engine.Volume;
const Info = engine.Info;
const Writer = engine.Writer;
const Backup = engine.Backup;

fn status(err: engine.Error) i32 {
    return switch (err) {
        error.InvalidArgument => 1,
        error.NotFound => 2,
        error.AlreadyExists => 3,
        error.NotDirectory => 4,
        error.IsDirectory => 5,
        error.Busy => 6,
        error.OutOfMemory => 7,
        error.Io => 8,
        error.InvalidFormat => 9,
        error.InvalidState => 10,
        error.TooLarge => 11,
        error.Cancelled => 12,
        error.Internal => 13,
        error.NotEmpty => 14,
        error.Unsupported => 15,
        error.DurabilityUncertain => 16,
    };
}

export fn fslite_abi_version() u32 {
    return 6;
}

export fn fslite_status_string(code: i32) [*:0]const u8 {
    return switch (code) {
        0 => "ok",
        1 => "invalid argument",
        2 => "not found",
        3 => "already exists",
        4 => "not a directory",
        5 => "is a directory",
        6 => "busy",
        7 => "out of memory",
        8 => "I/O error",
        9 => "invalid volume format",
        10 => "invalid transaction state",
        11 => "too large",
        12 => "cancelled",
        13 => "internal error",
        14 => "directory not empty",
        15 => "unsupported operation",
        16 => "backup published; durability not confirmed",
        else => "unknown status",
    };
}

export fn fslite_open(filename: ?[*:0]const u8, flags: u32, output: ?*?*Volume) i32 {
    const out = output orelse return 1;
    out.* = null;
    if ((flags & ~@as(u32, 1)) != 0) return 1;
    const name = filename orelse return 1;
    out.* = Volume.open(std.mem.span(name), flags == 1) catch |err| return status(err);
    return 0;
}

export fn fslite_close(volume: ?*Volume) void {
    if (volume) |v| v.close();
}

export fn fslite_mkdir(volume: ?*Volume, path: ?[*:0]const u8) i32 {
    const v = volume orelse return 1;
    const p = path orelse return 1;
    var op = engine.Operation.start(v) catch |err| return status(err);
    defer op.deinit();
    v.mkdir(std.mem.span(p)) catch |err| return status(err);
    op.finish() catch |err| return status(err);
    return 0;
}

export fn fslite_rename(volume: ?*Volume, source: ?[*:0]const u8, destination: ?[*:0]const u8) i32 {
    const v = volume orelse return 1;
    const from = source orelse return 1;
    const to = destination orelse return 1;
    var op = engine.Operation.start(v) catch |err| return status(err);
    defer op.deinit();
    v.rename(std.mem.span(from), std.mem.span(to)) catch |err| return status(err);
    op.finish() catch |err| return status(err);
    return 0;
}

export fn fslite_remove(volume: ?*Volume, path: ?[*:0]const u8) i32 {
    const v = volume orelse return 1;
    const p = path orelse return 1;
    var op = engine.Operation.start(v) catch |err| return status(err);
    defer op.deinit();
    v.remove(std.mem.span(p)) catch |err| return status(err);
    op.finish() catch |err| return status(err);
    return 0;
}

export fn fslite_remove_recursive(volume: ?*Volume, path: ?[*:0]const u8) i32 {
    const v = volume orelse return 1;
    const p = path orelse return 1;
    var op = engine.Operation.start(v) catch |err| return status(err);
    defer op.deinit();
    v.removeRecursive(std.mem.span(p)) catch |err| return status(err);
    op.finish() catch |err| return status(err);
    return 0;
}

export fn fslite_write(volume: ?*Volume, path: ?[*:0]const u8, data: ?[*]const u8, size: usize) i32 {
    const v = volume orelse return 1;
    const p = path orelse return 1;
    if (data == null and size != 0) return 1;
    if (size > std.math.maxInt(isize)) return 11;
    const bytes: []const u8 = if (data) |ptr| ptr[0..size] else &.{};
    v.write(std.mem.span(p), bytes) catch |err| return status(err);
    return 0;
}

export fn fslite_read(volume: ?*Volume, path: ?[*:0]const u8, offset: u64, buffer: ?[*]u8, capacity: usize, output: ?*usize) i32 {
    const out = output orelse return 1;
    out.* = 0;
    const v = volume orelse return 1;
    const p = path orelse return 1;
    if (buffer == null and capacity != 0) return 1;
    if (capacity > std.math.maxInt(isize)) return 11;
    const bytes: []u8 = if (buffer) |ptr| ptr[0..capacity] else &.{};
    var op = engine.Operation.start(v) catch |err| return status(err);
    defer op.deinit();
    const count = v.read(std.mem.span(p), offset, bytes) catch |err| return status(err);
    op.finish() catch |err| return status(err);
    out.* = count;
    return 0;
}

export fn fslite_write_at(volume: ?*Volume, path: ?[*:0]const u8, offset: u64, data: ?[*]const u8, size: usize) i32 {
    const v = volume orelse return 1;
    const p = path orelse return 1;
    if (data == null and size != 0) return 1;
    if (size > std.math.maxInt(isize)) return 11;
    const bytes: []const u8 = if (data) |ptr| ptr[0..size] else &.{};
    var op = engine.Operation.start(v) catch |err| return status(err);
    defer op.deinit();
    v.writeAt(std.mem.span(p), offset, bytes) catch |err| return status(err);
    op.finish() catch |err| return status(err);
    return 0;
}

export fn fslite_truncate(volume: ?*Volume, path: ?[*:0]const u8, size: u64) i32 {
    const v = volume orelse return 1;
    const p = path orelse return 1;
    var op = engine.Operation.start(v) catch |err| return status(err);
    defer op.deinit();
    v.truncate(std.mem.span(p), size) catch |err| return status(err);
    op.finish() catch |err| return status(err);
    return 0;
}

export fn fslite_stat(volume: ?*Volume, path: ?[*:0]const u8, output: ?*Info) i32 {
    const out = output orelse return 1;
    out.* = .{ .id = 0, .size = 0, .kind = 0 };
    const v = volume orelse return 1;
    const p = path orelse return 1;
    var op = engine.Operation.start(v) catch |err| return status(err);
    defer op.deinit();
    const info = v.stat(std.mem.span(p)) catch |err| return status(err);
    op.finish() catch |err| return status(err);
    out.* = info;
    return 0;
}

export fn fslite_list(volume: ?*Volume, path: ?[*:0]const u8, callback: ?engine.Callback, context: ?*anyopaque) i32 {
    const v = volume orelse return 1;
    const p = path orelse return 1;
    const cb = callback orelse return 1;
    var op = engine.Operation.start(v) catch |err| return status(err);
    defer op.deinit();
    v.list(std.mem.span(p), cb, context) catch |err| return status(err);
    op.finish() catch |err| return status(err);
    return 0;
}

export fn fslite_check(volume: ?*Volume) i32 {
    const v = volume orelse return 1;
    var op = engine.Operation.start(v) catch |err| return status(err);
    defer op.deinit();
    v.integrityCheck() catch |err| return status(err);
    op.finish() catch |err| return status(err);
    return 0;
}

export fn fslite_begin(volume: ?*Volume) i32 {
    const v = volume orelse return 1;
    v.begin() catch |err| return status(err);
    return 0;
}
export fn fslite_commit(volume: ?*Volume) i32 {
    const v = volume orelse return 1;
    v.commit() catch |err| return status(err);
    return 0;
}
export fn fslite_rollback(volume: ?*Volume) i32 {
    const v = volume orelse return 1;
    v.rollback() catch |err| return status(err);
    return 0;
}
export fn fslite_in_transaction(volume: ?*Volume) u32 {
    const v = volume orelse return 0;
    return @intFromBool(v.inTransaction());
}

export fn fslite_writer_open(volume: ?*Volume, path: ?[*:0]const u8, output: ?*?*Writer) i32 {
    const out = output orelse return 1;
    out.* = null;
    const v = volume orelse return 1;
    const p = path orelse return 1;
    out.* = Writer.open(v, std.mem.span(p)) catch |err| return status(err);
    return 0;
}

export fn fslite_writer_write(writer: ?*Writer, data: ?[*]const u8, size: usize) i32 {
    const w = writer orelse return 1;
    if (data == null and size != 0) return 1;
    if (size > std.math.maxInt(isize)) return 11;
    const bytes: []const u8 = if (data) |ptr| ptr[0..size] else &.{};
    w.append(bytes) catch |err| return status(err);
    return 0;
}

export fn fslite_writer_finish(writer: ?*Writer) i32 {
    const w = writer orelse return 1;
    w.finish() catch |err| return status(err);
    return 0;
}

export fn fslite_writer_close(writer: ?*Writer) void {
    if (writer) |w| w.close();
}

export fn fslite_backup_open(volume: ?*Volume, destination: ?[*:0]const u8, output: ?*?*Backup) i32 {
    const out = output orelse return 1;
    out.* = null;
    const v = volume orelse return 1;
    const path = destination orelse return 1;
    out.* = Backup.open(v, std.mem.span(path)) catch |err| return status(err);
    return 0;
}

export fn fslite_backup_step(backup: ?*Backup, pages: u32, output: ?*engine.BackupProgress) i32 {
    const out = output orelse return 1;
    out.* = .{ .remaining_pages = 0, .total_pages = 0, .done = 0 };
    const b = backup orelse return 1;
    out.* = b.step(pages) catch |err| return status(err);
    return 0;
}

export fn fslite_backup_finish(backup: ?*Backup) i32 {
    const b = backup orelse return 1;
    b.finish() catch |err| return status(err);
    return 0;
}

export fn fslite_backup_close(backup: ?*Backup) void {
    if (backup) |b| b.close();
}

export fn fslite_export(volume: ?*Volume, destination: ?[*:0]const u8) i32 {
    const v = volume orelse return 1;
    const path = destination orelse return 1;
    const backup = Backup.open(v, std.mem.span(path)) catch |err| return status(err);
    defer backup.close();
    while (!backup.done) {
        _ = backup.step(256) catch |err| return status(err);
    }
    backup.finish() catch |err| return status(err);
    return 0;
}

// =============================================================================
// Performance API - C ABI exports
// =============================================================================

export fn fslite_cache_get_hit_ratio(volume: ?*Volume) f64 {
    const v = volume orelse return 0.0;
    return v.cache_stats.hitRatio();
}

export fn fslite_cache_get_hits(volume: ?*Volume) u64 {
    const v = volume orelse return 0;
    return v.cache_stats.hits;
}

export fn fslite_cache_get_misses(volume: ?*Volume) u64 {
    const v = volume orelse return 0;
    return v.cache_stats.misses;
}

export fn fslite_cache_get_evictions(volume: ?*Volume) u64 {
    const v = volume orelse return 0;
    return v.cache_stats.evictions;
}

export fn fslite_cache_get_size(volume: ?*Volume) u64 {
    const v = volume orelse return 0;
    return v.cache_stats.current_size;
}

export fn fslite_cache_get_memory_usage(volume: ?*Volume) u64 {
    const v = volume orelse return 0;
    return v.cache_stats.memoryUsage();
}

export fn fslite_cache_reset_stats(volume: ?*Volume) i32 {
    const v = volume orelse return 1;
    v.cache_stats.hits = 0;
    v.cache_stats.misses = 0;
    v.cache_stats.evictions = 0;
    v.cache_stats.current_size = 0;
    return 0;
}

export fn fslite_cache_get_dashboard_cache_hit_ratio(volume: ?*Volume) f64 {
    const v = volume orelse return 0.0;
    return v.performance_dashboard.cache_hit_ratio;
}

export fn fslite_cache_get_dashboard_memory_usage(volume: ?*Volume) u64 {
    const v = volume orelse return 0;
    return v.performance_dashboard.memory_usage_bytes;
}
