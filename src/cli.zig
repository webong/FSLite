const std = @import("std");
const c = @cImport({
    @cInclude("fslite.h");
});

const chunk_size = 64 * 1024;

fn out(bytes: []const u8) !void {
    try std.fs.File.stdout().writeAll(bytes);
}

fn print(comptime fmt: []const u8, args: anytype) !void {
    var buffer: [4096]u8 = undefined;
    const text = try std.fmt.bufPrint(&buffer, fmt, args);
    try out(text);
}

fn fail(status: c.fslite_status) noreturn {
    const message = c.fslite_status_string(status);
    std.debug.print("fslite: {s}\n", .{std.mem.sliceTo(message, 0)});
    std.process.exit(1);
}

fn expect(ok: bool) void {
    if (!ok) std.process.exit(2);
}

fn openVolume(path: [:0]const u8, flags: u32) ?*c.fslite_volume {
    var volume: ?*c.fslite_volume = null;
    const status = c.fslite_open(path.ptr, flags, &volume);
    if (status != c.FSLITE_OK) fail(status);
    return volume;
}

fn usage() noreturn {
    out(
        \\usage: fslite <command> [arguments]
        \\
        \\Commands:
        \\  create <volume>                    Create or open a volume
        \\  ls <volume> [path]                 List a directory (default /)
        \\  stat <volume> <path>               Show file or directory metadata
        \\  cat <volume> <path>                Write file content to stdout
        \\  put <volume> <path> <host-file>    Replace a volume file from a host file
        \\  get <volume> <path> <host-file>    Extract a volume file to a host file
        \\  mkdir <volume> <path>              Create a directory
        \\  rm [-r] <volume> <path>            Remove a file or directory
        \\  mv <volume> <source> <destination> Rename within the volume
        \\  check <volume>                     Run the integrity checker
        \\  backup <volume> <destination>      Export a consistent snapshot
        \\  stats <volume>                     Print volume diagnostics
        \\  version                            Print the ABI revision
        \\
    ) catch {};
    std.process.exit(2);
}

fn commandCreate(args: []const [:0]u8) !void {
    expect(args.len == 1);
    const volume = openVolume(args[0], c.FSLITE_OPEN_CREATE);
    c.fslite_close(volume);
}

fn listEntry(_: ?*anyopaque, name: [*c]const u8, name_size: usize, info: [*c]const c.fslite_info) callconv(.c) i32 {
    const name_ptr: [*]const u8 = @ptrCast(name);
    const entry = info[0];
    const kind: u8 = if (entry.kind == c.FSLITE_DIRECTORY) 'd' else '-';
    var buffer: [1024]u8 = undefined;
    const line = std.fmt.bufPrint(&buffer, "{c} {d:>12} {s}\n", .{
        kind, entry.size, name_ptr[0..name_size],
    }) catch return 1;
    std.fs.File.stdout().writeAll(line) catch return 1;
    return 0;
}

fn commandList(args: []const [:0]u8) !void {
    expect(args.len == 1 or args.len == 2);
    const volume = openVolume(args[0], 0);
    defer c.fslite_close(volume);
    const path: [:0]const u8 = if (args.len == 2) args[1] else "/";
    const status = c.fslite_list(volume, path.ptr, listEntry, null);
    if (status != c.FSLITE_OK) fail(status);
}

fn commandStat(args: []const [:0]u8) !void {
    expect(args.len == 2);
    const volume = openVolume(args[0], 0);
    defer c.fslite_close(volume);
    var info: c.fslite_file_info = undefined;
    const status = c.fslite_inspect(volume, args[1].ptr, &info);
    if (status != c.FSLITE_OK) fail(status);
    try print("path: {s}\n", .{args[1]});
    try print("kind: {s}\n", .{if (info.kind == c.FSLITE_DIRECTORY) "directory" else "file"});
    try print("id: {d}\n", .{info.id});
    try print("size: {d}\n", .{info.size});
    try print("chunks: {d}\n", .{info.chunks});
    try print("allocated: {d}\n", .{info.allocated});
    try print("depth: {d}\n", .{info.depth});
}

fn commandCat(args: []const [:0]u8) !void {
    expect(args.len == 2);
    const volume = openVolume(args[0], 0);
    defer c.fslite_close(volume);
    var buffer: [chunk_size]u8 = undefined;
    var offset: u64 = 0;
    while (true) {
        var count: usize = 0;
        const status = c.fslite_read(volume, args[1].ptr, offset, &buffer, buffer.len, &count);
        if (status != c.FSLITE_OK) fail(status);
        if (count == 0) return;
        try out(buffer[0..count]);
        offset += count;
    }
}

fn commandPut(args: []const [:0]u8) !void {
    expect(args.len == 3);
    const volume = openVolume(args[0], 0);
    defer c.fslite_close(volume);
    var host = try std.fs.cwd().openFile(args[2], .{});
    defer host.close();
    var writer: ?*c.fslite_writer = null;
    const open_status = c.fslite_writer_open(volume, args[1].ptr, &writer);
    if (open_status != c.FSLITE_OK) fail(open_status);
    defer c.fslite_writer_close(writer);
    var buffer: [chunk_size]u8 = undefined;
    while (true) {
        const count = try host.read(&buffer);
        if (count == 0) break;
        const status = c.fslite_writer_write(writer, &buffer, count);
        if (status != c.FSLITE_OK) fail(status);
    }
    const finish_status = c.fslite_writer_finish(writer);
    if (finish_status != c.FSLITE_OK) fail(finish_status);
}

fn commandGet(args: []const [:0]u8) !void {
    expect(args.len == 3);
    const volume = openVolume(args[0], 0);
    defer c.fslite_close(volume);
    var host = try std.fs.cwd().createFile(args[2], .{ .truncate = true });
    defer host.close();
    var buffer: [chunk_size]u8 = undefined;
    var offset: u64 = 0;
    while (true) {
        var count: usize = 0;
        const status = c.fslite_read(volume, args[1].ptr, offset, &buffer, buffer.len, &count);
        if (status != c.FSLITE_OK) fail(status);
        if (count == 0) return;
        try host.writeAll(buffer[0..count]);
        offset += count;
    }
}

fn commandMkdir(args: []const [:0]u8) !void {
    expect(args.len == 2);
    const volume = openVolume(args[0], 0);
    defer c.fslite_close(volume);
    const status = c.fslite_mkdir(volume, args[1].ptr);
    if (status != c.FSLITE_OK) fail(status);
}

fn commandRemove(args: []const [:0]u8) !void {
    if (args.len == 3 and std.mem.eql(u8, args[0], "-r")) {
        const volume = openVolume(args[1], 0);
        defer c.fslite_close(volume);
        const status = c.fslite_remove_recursive(volume, args[2].ptr);
        if (status != c.FSLITE_OK) fail(status);
        return;
    }
    expect(args.len == 2);
    const volume = openVolume(args[0], 0);
    defer c.fslite_close(volume);
    const status = c.fslite_remove(volume, args[1].ptr);
    if (status != c.FSLITE_OK) fail(status);
}

fn commandRename(args: []const [:0]u8) !void {
    expect(args.len == 3);
    const volume = openVolume(args[0], 0);
    defer c.fslite_close(volume);
    const status = c.fslite_rename(volume, args[1].ptr, args[2].ptr);
    if (status != c.FSLITE_OK) fail(status);
}

fn commandCheck(args: []const [:0]u8) !void {
    expect(args.len == 1);
    const volume = openVolume(args[0], 0);
    defer c.fslite_close(volume);
    const status = c.fslite_check(volume);
    if (status != c.FSLITE_OK) fail(status);
    try out("ok\n");
}

fn commandBackup(args: []const [:0]u8) !void {
    expect(args.len == 2);
    const volume = openVolume(args[0], 0);
    defer c.fslite_close(volume);
    const status = c.fslite_export(volume, args[1].ptr);
    if (status != c.FSLITE_OK) fail(status);
}

fn commandStats(args: []const [:0]u8) !void {
    expect(args.len == 1);
    const volume = openVolume(args[0], 0);
    defer c.fslite_close(volume);
    var stats: c.fslite_volume_stats = undefined;
    const status = c.fslite_get_volume_stats(volume, &stats);
    if (status != c.FSLITE_OK) fail(status);
    try print("files: {d}\n", .{stats.file_count});
    try print("directories: {d}\n", .{stats.directory_count});
    try print("chunks: {d}\n", .{stats.chunk_count});
    try print("chunk_bytes: {d}\n", .{stats.chunk_bytes});
    try print("logical_bytes: {d}\n", .{stats.logical_bytes});
    try print("page_size: {d}\n", .{stats.page_size});
    try print("page_count: {d}\n", .{stats.page_count});
    try print("freelist_pages: {d}\n", .{stats.freelist_pages});
    try print("database_bytes: {d}\n", .{stats.database_bytes});
    try print("in_transaction: {s}\n", .{if (stats.in_transaction != 0) "yes" else "no"});
    try print("sparse: {s}\n", .{if (stats.sparse != 0) "yes" else "no"});
}

fn run() !void {
    var arena = std.heap.ArenaAllocator.init(std.heap.page_allocator);
    defer arena.deinit();
    const allocator = arena.allocator();
    const args = try std.process.argsAlloc(allocator);
    if (args.len < 2) usage();
    const command = args[1];
    const rest = args[2..];

    if (std.mem.eql(u8, command, "version")) {
        expect(rest.len == 0);
        try print("FSLite ABI revision {d}\n", .{c.fslite_abi_version()});
    } else if (std.mem.eql(u8, command, "create")) {
        try commandCreate(rest);
    } else if (std.mem.eql(u8, command, "ls")) {
        try commandList(rest);
    } else if (std.mem.eql(u8, command, "stat")) {
        try commandStat(rest);
    } else if (std.mem.eql(u8, command, "cat")) {
        try commandCat(rest);
    } else if (std.mem.eql(u8, command, "put")) {
        try commandPut(rest);
    } else if (std.mem.eql(u8, command, "get")) {
        try commandGet(rest);
    } else if (std.mem.eql(u8, command, "mkdir")) {
        try commandMkdir(rest);
    } else if (std.mem.eql(u8, command, "rm")) {
        try commandRemove(rest);
    } else if (std.mem.eql(u8, command, "mv")) {
        try commandRename(rest);
    } else if (std.mem.eql(u8, command, "check")) {
        try commandCheck(rest);
    } else if (std.mem.eql(u8, command, "backup")) {
        try commandBackup(rest);
    } else if (std.mem.eql(u8, command, "stats")) {
        try commandStats(rest);
    } else if (std.mem.eql(u8, command, "help") or std.mem.eql(u8, command, "--help")) {
        usage();
    } else {
        usage();
    }
}

pub fn main() void {
    run() catch |err| {
        std.debug.print("fslite: {s}\n", .{@errorName(err)});
        std.process.exit(1);
    };
}
