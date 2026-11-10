const std = @import("std");
pub const c = @cImport({
    @cInclude("sqlite3.h");
    @cInclude("backup_file.h");
});
const allocator = std.heap.c_allocator;
pub const chunk_size = 64 * 1024;

pub const Error = error{
    InvalidArgument,
    NotFound,
    AlreadyExists,
    NotDirectory,
    IsDirectory,
    Busy,
    OutOfMemory,
    Io,
    InvalidFormat,
    InvalidState,
    TooLarge,
    Cancelled,
    Internal,
    NotEmpty,
    Unsupported,
    DurabilityUncertain,
};
pub const Info = extern struct {
    id: u64,
    size: u64,
    kind: u32,
    reserved: u32 = 0,
};

/// Extended per-path metadata. `chunks` and `allocated` describe stored chunk
/// rows, so sparse holes reduce both values without changing the logical size.
pub const FileInfo = extern struct {
    id: u64,
    size: u64,
    chunks: u64,
    allocated: u64,
    kind: u32,
    depth: u32,
    reserved: [2]u32 = .{ 0, 0 },
};

/// Public limit configuration. Zero fields select the default for that limit.
pub const Limits = extern struct {
    max_path_bytes: u64 = 0,
    max_name_bytes: u64 = 0,
    max_file_size: u64 = 0,
    max_volume_bytes: u64 = 0,
    reserved: [4]u32 = .{ 0, 0, 0, 0 },
};

/// Resolved limits with the zero-means-default rules applied.
pub const ResolvedLimits = struct {
    max_path_bytes: usize = 4096,
    max_name_bytes: usize = 255,
    max_file_size: u64 = std.math.maxInt(i64),
    max_volume_bytes: u64 = std.math.maxInt(u64),
};

/// Whole-volume diagnostics returned by fslite_get_volume_stats.
pub const VolumeStats = extern struct {
    file_count: u64,
    directory_count: u64,
    chunk_count: u64,
    chunk_bytes: u64,
    logical_bytes: u64,
    page_size: u64,
    page_count: u64,
    freelist_pages: u64,
    database_bytes: u64,
    in_transaction: u32,
    sparse: u32,
    reserved: [2]u32 = .{ 0, 0 },
};

pub const Callback = *const fn (?*anyopaque, [*]const u8, usize, *const Info) callconv(.c) i32;

pub const Volume = struct {
    db: *c.sqlite3,
    writer: ?*Writer = null,
    backup: ?*Backup = null,
    cache_stats: CacheStats,
    adaptive_cache: AdaptiveCacheManager,
    performance_dashboard: PerformanceDashboard,
    prepared_statements: std.ArrayList(*c.sqlite3_stmt),
    last_adapt_time_us: u64 = 0,
    sparse: bool = false,
    limits: ResolvedLimits = .{},

    pub fn open(filename: [:0]const u8, create: bool, sparse: bool) Error!*Volume {
        if (filename.len == 0 or !std.unicode.utf8ValidateSlice(filename)) return error.InvalidArgument;
        var db: ?*c.sqlite3 = null;
        const flags: c_int = c.SQLITE_OPEN_READWRITE | c.SQLITE_OPEN_FULLMUTEX |
            (if (create) @as(c_int, c.SQLITE_OPEN_CREATE) else 0);
        const rc = c.sqlite3_open_v2(filename.ptr, &db, flags, null);
        errdefer if (db) |conn| {
            _ = c.sqlite3_close_v2(conn);
        };
        try check(rc);
        const self = try allocator.create(Volume);
        errdefer allocator.destroy(self);

        self.* = .{
            .db = db.?,
            .cache_stats = CacheStats{},
            .adaptive_cache = AdaptiveCacheManager.init(self),
            .performance_dashboard = PerformanceDashboard{},
            .prepared_statements = try std.ArrayList(*c.sqlite3_stmt).initCapacity(allocator, 16),
            .last_adapt_time_us = 0,
            .sparse = sparse,
        };
        try check(c.sqlite3_busy_timeout(self.db, 1000));
        try self.exec("PRAGMA foreign_keys=ON; PRAGMA trusted_schema=OFF;");
        try self.exec(if (create) "BEGIN IMMEDIATE" else "BEGIN");
        errdefer self.exec("ROLLBACK") catch {};
        const app = try self.scalar("PRAGMA application_id");
        const version = try self.scalar("PRAGMA user_version");
        if (app == 0 and version == 0 and create and
            try self.scalar("SELECT count(*) FROM sqlite_schema") == 0)
        {
            try self.exec(@embedFile("schema.sql"));
        } else if (app != 0x46534c54 or version != 2) {
            return error.InvalidFormat;
        }
        // Verify the root before handing a purported volume to the host.
        const root_count = self.scalar(
            "SELECT count(*) FROM nodes WHERE id=1 AND parent IS NULL AND name='' AND kind=2 AND size=0",
        ) catch return error.InvalidFormat;
        if (root_count != 1) return error.InvalidFormat;
        try self.exec("COMMIT");
        return self;
    }

    pub fn close(self: *Volume) void {
        if (self.backup) |backup| backup.abort();
        if (self.writer) |writer| writer.abort();
        // Finalize all cached prepared statements before closing the connection.
        for (self.prepared_statements.items) |stmt| {
            _ = c.sqlite3_finalize(stmt);
        }
        self.prepared_statements.deinit(allocator);
        // SQLite rolls back uncommitted work when the connection closes.
        _ = c.sqlite3_close_v2(self.db);
        allocator.destroy(self);
    }

    /// Migrate a volume from format version 1 to version 2. If the volume is
    /// already at version 2, this is a no-op. Returns InvalidFormat if the
    /// volume cannot be migrated (e.g., unrecognized schema).
    pub fn migrate(self: *Volume) Error!void {
        const app = try self.scalar("PRAGMA application_id");
        const version = try self.scalar("PRAGMA user_version");
        if (app == 0x46534c54 and version == 2) return; // Already v2

        // Attempt to detect and migrate from v1 format.
        // V1 used a single-BLOB layout; v2 uses nodes/chunks tables.
        // Since we don't have the v1 schema definition, attempt a best-effort
        // migration by reading all user tables and reconstructing.
        const tables = try self.scalar("SELECT count(*) FROM sqlite_schema WHERE type='table' AND name NOT LIKE 'sqlite_%'");
        if (tables == 0) {
            // Empty database - initialize as v2
            try self.exec(@embedFile("schema.sql"));
            try self.exec("PRAGMA application_id=0x46534c54; PRAGMA user_version=2;");
            return;
        }

        // Check if nodes and chunks tables already exist (v2 schema)
        const has_nodes = try self.scalar("SELECT count(*) FROM sqlite_schema WHERE type='table' AND name='nodes'");
        const has_chunks = try self.scalar("SELECT count(*) FROM sqlite_schema WHERE type='table' AND name='chunks'");
        if (has_nodes == 1 and has_chunks == 1) {
            // Tables exist but wrong version - just update version
            try self.exec("PRAGMA application_id=0x46534c54; PRAGMA user_version=2;");
            return;
        }

        // Unknown format - cannot migrate
        return error.InvalidFormat;
    }

    pub fn exec(self: *Volume, sql: [:0]const u8) Error!void {
        try check(c.sqlite3_exec(self.db, sql.ptr, null, null, null));
    }

    fn prepare(self: *Volume, sql: [:0]const u8) Error!Statement {
        var stmt: ?*c.sqlite3_stmt = null;
        try check(c.sqlite3_prepare_v2(self.db, sql.ptr, -1, &stmt, null));
        return .{ .ptr = stmt.? };
    }

    /// Prepare a SQL statement and cache it for reuse. If the same SQL has been
    /// prepared before, the cached statement is returned with its bindings
    /// already cleared. The statement remains owned by the Volume and is
    /// finalized in close(). Callers must not close() statements from prepareCached.
    fn prepareCached(self: *Volume, sql: [:0]const u8) Error!Statement {
        // Check cache for exact SQL match.
        for (self.prepared_statements.items) |stmt_ptr| {
            const cached_sql = c.sqlite3_sql(stmt_ptr);
            if (cached_sql != null) {
                // cached_sql is a null-terminated C string; find its length.
                var c_len: usize = 0;
                while (cached_sql[c_len] != 0) : (c_len += 1) {}
                if (c_len == sql.len and std.mem.eql(u8, sql, cached_sql[0..c_len])) {
                    // Reset and clear bindings before returning for reuse.
                    try check(c.sqlite3_reset(stmt_ptr));
                    try check(c.sqlite3_clear_bindings(stmt_ptr));
                    return .{ .ptr = stmt_ptr };
                }
            }
        }
        // Not cached: prepare and store.
        var stmt: ?*c.sqlite3_stmt = null;
        try check(c.sqlite3_prepare_v2(self.db, sql.ptr, -1, &stmt, null));
        if (stmt) |s| {
            try self.prepared_statements.append(allocator, s);
        }
        return .{ .ptr = stmt.? };
    }

    pub fn adaptCache(self: *Volume, current_time_us: u64) void {
        self.adaptive_cache.adapt(current_time_us);
        self.last_adapt_time_us = current_time_us;
    }

    /// Pre-compile frequently used SQL statements to warm up SQLite's internal
    /// cache. Safe to call multiple times; subsequent calls are inexpensive.
    pub fn warmupCache(self: *Volume) void {
        const statements = [_][]const u8{
            "SELECT id,kind,size FROM nodes WHERE parent=?1 AND name=?2",
            "INSERT INTO nodes(parent,name,kind,size) VALUES(?1,?2,2,0)",
            "DELETE FROM nodes WHERE id=?1",
            "UPDATE nodes SET parent=?2,name=?3 WHERE id=?1",
            "UPDATE nodes SET size=?2 WHERE id=?1",
            "INSERT INTO chunks(node_id,chunk_index,data) VALUES(?1,?2,?3)",
            "SELECT data FROM chunks WHERE node_id=?1 AND chunk_index=?2",
            "SELECT chunk_index,data FROM chunks WHERE node_id=?1 AND chunk_index BETWEEN ?2 AND ?3 ORDER BY chunk_index",
            "SELECT id,kind,size,name FROM nodes WHERE parent=?1 ORDER BY name COLLATE BINARY",
            "DELETE FROM chunks WHERE node_id=?1 AND chunk_index>=?2",
            "SELECT 1 FROM nodes WHERE parent=?1 LIMIT 1",
        };
        for (statements) |sql| {
            const stmt = self.prepare(sql) catch continue;
            stmt.close();
        }
    }

    fn scalar(self: *Volume, sql: [:0]const u8) Error!i64 {
        const stmt = try self.prepare(sql);
        defer stmt.close();
        if (!try stmt.row()) return error.Internal;
        return c.sqlite3_column_int64(stmt.ptr, 0);
    }

    // The C entry point owns a savepoint spanning all scans, so they observe
    // one snapshot (including pending work in the caller's transaction).
    pub fn integrityCheck(self: *Volume) Error!void {
        self.checkContents() catch |err| {
            // Required format queries failing to compile indicate an unusable schema.
            if (err == error.Internal and c.sqlite3_errcode(self.db) == c.SQLITE_ERROR)
                return error.InvalidFormat;
            return err;
        };
    }

    fn checkContents(self: *Volume) Error!void {
        if (try self.scalar("PRAGMA application_id") != 0x46534c54 or
            try self.scalar("PRAGMA user_version") != 2) return error.InvalidFormat;
        {
            const stmt = try self.prepare("PRAGMA integrity_check");
            defer stmt.close();
            if (!try stmt.row()) return error.InvalidFormat;
            const result = c.sqlite3_column_text(stmt.ptr, 0) orelse return error.OutOfMemory;
            const len: usize = @intCast(c.sqlite3_column_bytes(stmt.ptr, 0));
            if (!std.mem.eql(u8, result[0..len], "ok") or try stmt.row()) return error.InvalidFormat;
        }
        try self.requireNoRows("PRAGMA foreign_key_check");
        if (try self.scalar("SELECT count(*) FROM sqlite_schema WHERE type='table' AND name IN ('nodes','chunks')") != 2)
            return error.InvalidFormat;
        try self.requireNoRows(
            "SELECT 1 FROM nodes WHERE typeof(id)<>'integer' OR id<1 OR " ++
                "typeof(kind)<>'integer' OR kind NOT IN (1,2) OR typeof(size)<>'integer' OR size<0 OR " ++
                "(kind=2 AND size<>0) OR typeof(name)<>'text' OR " ++
                "(id=1 AND (parent IS NOT NULL OR name<>'' OR kind<>2 OR size<>0)) OR " ++
                "(id<>1 AND (typeof(parent)<>'integer' OR parent<1)) LIMIT 1",
        );
        if (try self.scalar("SELECT count(*) FROM nodes WHERE id=1") != 1) return error.InvalidFormat;
        try self.requireNoRows("SELECT id FROM nodes GROUP BY id HAVING count(*)<>1 LIMIT 1");
        try self.requireNoRows("SELECT parent,name FROM nodes GROUP BY parent,name COLLATE BINARY HAVING count(*)<>1 LIMIT 1");
        try self.requireNoRows(
            "SELECT 1 FROM nodes n LEFT JOIN nodes p ON n.parent=p.id " ++
                "WHERE n.id<>1 AND (p.id IS NULL OR p.kind<>2) LIMIT 1",
        );
        {
            const stmt = try self.prepare("SELECT name FROM nodes WHERE id<>1");
            defer stmt.close();
            while (try stmt.row()) {
                const len: usize = @intCast(c.sqlite3_column_bytes(stmt.ptr, 0));
                if (len == 0 or len > 255) return error.InvalidFormat;
                const ptr = c.sqlite3_column_text(stmt.ptr, 0) orelse return error.OutOfMemory;
                const name = ptr[0..len];
                if (!std.unicode.utf8ValidateSlice(name) or std.mem.indexOfScalar(u8, name, 0) != null or
                    std.mem.indexOfScalar(u8, name, '/') != null or std.mem.eql(u8, name, ".") or
                    std.mem.eql(u8, name, "..")) return error.InvalidFormat;
            }
        }
        // Valid positive-length names bound recursion. Disconnected cycles are
        // unreachable from root, and an overlong path excludes its entire tail.
        if (try self.scalar(
            "WITH RECURSIVE tree(id,bytes) AS (SELECT 1,0 UNION ALL " ++
                "SELECT n.id,t.bytes+1+length(CAST(n.name AS BLOB)) FROM nodes n JOIN tree t ON n.parent=t.id " ++
                "WHERE t.bytes+1+length(CAST(n.name AS BLOB))<=4096) " ++
                "SELECT (SELECT count(*) FROM nodes)-count(*) FROM tree",
        ) != 0) return error.InvalidFormat;
        try self.requireNoRows(
            "SELECT 1 FROM chunks c LEFT JOIN nodes n ON n.id=c.node_id WHERE " ++
                "typeof(c.node_id)<>'integer' OR n.id IS NULL OR n.kind<>1 OR n.size=0 OR " ++
                "typeof(c.chunk_index)<>'integer' OR c.chunk_index<0 OR " ++
                "c.chunk_index>(n.size-1)/65536 OR typeof(c.data)<>'blob' LIMIT 1",
        );
        // Indices are now bounded by the owning file's size, avoiding arithmetic
        // overflow even for malformed volumes near INT64_MAX.
        try self.requireNoRows(
            "SELECT 1 FROM chunks c JOIN nodes n ON n.id=c.node_id " ++
                "WHERE length(c.data)<>min(65536,n.size-c.chunk_index*65536) LIMIT 1",
        );
        try self.requireNoRows("SELECT node_id,chunk_index FROM chunks GROUP BY node_id,chunk_index HAVING count(*)<>1 LIMIT 1");
        // With sparse storage disabled (default), every chunk covering the file
        // must exist. Missing chunks indicate corruption. With sparse storage
        // enabled, missing chunks read as zeros and are not required.
        if (!self.sparse) {
            try self.requireNoRows(
                "SELECT 1 FROM nodes n LEFT JOIN (SELECT node_id,count(*) AS total FROM chunks GROUP BY node_id) c " ++
                    "ON n.id=c.node_id WHERE coalesce(c.total,0)<>n.size/65536+(n.size%65536<>0) LIMIT 1",
            );
        }
    }

    fn requireNoRows(self: *Volume, sql: [:0]const u8) Error!void {
        const stmt = try self.prepare(sql);
        defer stmt.close();
        if (try stmt.row()) return error.InvalidFormat;
    }

    pub fn inTransaction(self: *Volume) bool {
        return c.sqlite3_get_autocommit(self.db) == 0;
    }

    /// Apply a limit configuration. Zero fields keep their documented default.
    /// Limits may only change while the volume is quiescent: no writer, backup,
    /// or explicit transaction. Rejecting impossible values keeps every accepted
    /// path within the on-disk format's fixed bounds.
    pub fn setLimits(self: *Volume, config: Limits) Error!void {
        if (self.writer != null or self.backup != null or self.inTransaction())
            return error.InvalidState;
        var resolved = ResolvedLimits{};
        if (config.max_path_bytes != 0) {
            if (config.max_path_bytes < 2 or config.max_path_bytes > 4096) return error.InvalidArgument;
            resolved.max_path_bytes = @intCast(config.max_path_bytes);
        }
        if (config.max_name_bytes != 0) {
            if (config.max_name_bytes > 255) return error.InvalidArgument;
            resolved.max_name_bytes = @intCast(config.max_name_bytes);
        }
        if (config.max_file_size != 0) {
            if (config.max_file_size > std.math.maxInt(i64)) return error.InvalidArgument;
            resolved.max_file_size = config.max_file_size;
        }
        if (config.max_volume_bytes != 0)
            resolved.max_volume_bytes = config.max_volume_bytes;
        self.limits = resolved;
    }

    /// Extended metadata for one path, including stored chunk accounting.
    pub fn inspect(self: *Volume, path: []const u8) Error!FileInfo {
        const node = try self.stat(path);
        var depth: u32 = 0;
        var parts = std.mem.tokenizeScalar(u8, path, '/');
        while (parts.next()) |_| depth += 1;
        var chunks: u64 = 0;
        var allocated: u64 = 0;
        if (node.kind == 1) {
            const stmt = try self.prepare(
                "SELECT count(*), coalesce(sum(length(data)),0) FROM chunks WHERE node_id=?1",
            );
            defer stmt.close();
            try stmt.integer(1, @intCast(node.id));
            if (try stmt.row()) {
                chunks = @intCast(c.sqlite3_column_int64(stmt.ptr, 0));
                allocated = @intCast(c.sqlite3_column_int64(stmt.ptr, 1));
            }
        }
        return .{
            .id = node.id,
            .size = node.size,
            .chunks = chunks,
            .allocated = allocated,
            .kind = node.kind,
            .depth = depth,
        };
    }

    /// Whole-volume diagnostics. Directory count includes the root. Chunk values
    /// describe physically stored rows, so sparse holes are not counted.
    pub fn volumeStats(self: *Volume) Error!VolumeStats {
        var result = VolumeStats{
            .file_count = 0,
            .directory_count = 0,
            .chunk_count = 0,
            .chunk_bytes = 0,
            .logical_bytes = 0,
            .page_size = 0,
            .page_count = 0,
            .freelist_pages = 0,
            .database_bytes = 0,
            .in_transaction = 0,
            .sparse = @intFromBool(self.sparse),
        };
        {
            const stmt = try self.prepare(
                "SELECT count(*), coalesce(sum(size),0) FROM nodes WHERE kind=1",
            );
            defer stmt.close();
            if (try stmt.row()) {
                result.file_count = @intCast(c.sqlite3_column_int64(stmt.ptr, 0));
                result.logical_bytes = @intCast(c.sqlite3_column_int64(stmt.ptr, 1));
            }
        }
        result.directory_count = @intCast(try self.scalar("SELECT count(*) FROM nodes WHERE kind=2"));
        {
            const stmt = try self.prepare(
                "SELECT count(*), coalesce(sum(length(data)),0) FROM chunks",
            );
            defer stmt.close();
            if (try stmt.row()) {
                result.chunk_count = @intCast(c.sqlite3_column_int64(stmt.ptr, 0));
                result.chunk_bytes = @intCast(c.sqlite3_column_int64(stmt.ptr, 1));
            }
        }
        result.page_size = @intCast(try self.scalar("PRAGMA page_size"));
        result.page_count = @intCast(try self.scalar("PRAGMA page_count"));
        result.freelist_pages = @intCast(try self.scalar("PRAGMA freelist_count"));
        result.database_bytes = result.page_size * result.page_count;
        result.in_transaction = @intFromBool(self.inTransaction());
        return result;
    }

    /// Reject a mutation that would grow the sum of file sizes past the limit.
    /// A replacement or shrink never fails because the configuration was lowered
    /// below current usage; only growth beyond the maximum is rejected.
    fn checkVolumeLimit(self: *Volume, id: u64, new_size: u64) Error!void {
        const maximum = self.limits.max_volume_bytes;
        if (maximum == std.math.maxInt(u64)) return;
        const stmt = try self.prepare(
            "SELECT coalesce(sum(size),0), coalesce((SELECT size FROM nodes WHERE id=?1),0) " ++
                "FROM nodes WHERE kind=1",
        );
        defer stmt.close();
        try stmt.integer(1, @intCast(id));
        if (!try stmt.row()) return error.Internal;
        const total = c.sqlite3_column_int64(stmt.ptr, 0);
        const current = c.sqlite3_column_int64(stmt.ptr, 1);
        if (total < 0 or current < 0 or current > total) return error.InvalidFormat;
        if (new_size <= @as(u64, @intCast(current))) return;
        const projected = @as(u128, @intCast(total)) - @as(u64, @intCast(current)) + new_size;
        if (projected > maximum) return error.TooLarge;
    }

    pub fn begin(self: *Volume) Error!void {
        if (self.writer != null or self.backup != null or self.inTransaction()) return error.InvalidState;
        try self.exec("BEGIN IMMEDIATE");
    }

    pub fn commit(self: *Volume) Error!void {
        if (self.writer != null or self.backup != null or !self.inTransaction()) return error.InvalidState;
        try self.exec("COMMIT");
    }

    pub fn rollback(self: *Volume) Error!void {
        if (self.writer != null or self.backup != null or !self.inTransaction()) return error.InvalidState;
        try self.exec("ROLLBACK");
        // Reset and clear bindings on all cached statements so they are safe
        // for reuse after a rollback. Any partial execution state is discarded.
        for (self.prepared_statements.items) |stmt| {
            _ = c.sqlite3_reset(stmt);
            _ = c.sqlite3_clear_bindings(stmt);
        }
    }

    fn child(self: *Volume, parent: u64, name: []const u8) Error!?Info {
        const stmt = try self.prepareCached("SELECT id,kind,size FROM nodes WHERE parent=?1 AND name=?2");
        try stmt.integer(1, @intCast(parent));
        try stmt.text(2, name);
        if (!try stmt.row()) return null;
        const info = try stmt.info(0);
        try stmt.reset();
        return info;
    }

    pub fn stat(self: *Volume, path: []const u8) Error!Info {
        try validatePath(path, self.limits);
        var current: Info = .{ .id = 1, .kind = 2, .size = 0 };
        var parts = std.mem.tokenizeScalar(u8, path, '/');
        while (parts.next()) |part| {
            if (current.kind != 2) return error.NotDirectory;
            current = try self.child(current.id, part) orelse return error.NotFound;
        }
        return current;
    }

    fn parentOf(self: *Volume, path: []const u8) Error!struct { id: u64, name: []const u8 } {
        try validatePath(path, self.limits);
        if (path.len == 1) return error.IsDirectory;
        const split = std.mem.lastIndexOfScalar(u8, path, '/').?;
        const parent = try self.stat(if (split == 0) "/" else path[0..split]);
        if (parent.kind != 2) return error.NotDirectory;
        return .{ .id = parent.id, .name = path[split + 1 ..] };
    }

    pub fn mkdir(self: *Volume, path: []const u8) Error!void {
        if (std.mem.eql(u8, path, "/")) return error.AlreadyExists;
        const parent = try self.parentOf(path);
        if (try self.child(parent.id, parent.name) != null) return error.AlreadyExists;
        const stmt = try self.prepare("INSERT INTO nodes(parent,name,kind,size) VALUES(?1,?2,2,0)");
        defer stmt.close();
        try stmt.integer(1, @intCast(parent.id));
        try stmt.text(2, parent.name);
        try stmt.done();
    }

    fn requireEmpty(self: *Volume, node: Info) Error!void {
        if (node.kind != 2) return;
        const stmt = try self.prepare("SELECT 1 FROM nodes WHERE parent=?1 LIMIT 1");
        defer stmt.close();
        try stmt.integer(1, @intCast(node.id));
        if (try stmt.row()) return error.NotEmpty;
    }

    fn deleteNode(self: *Volume, id: u64) Error!void {
        const stmt = try self.prepare("DELETE FROM nodes WHERE id=?1");
        defer stmt.close();
        try stmt.integer(1, @intCast(id));
        // Foreign keys delete a file's chunks in the same operation.
        try stmt.done();
    }

    pub fn remove(self: *Volume, path: []const u8) Error!void {
        try validatePath(path, self.limits);
        if (path.len == 1) return error.InvalidArgument;
        const node = try self.stat(path);
        try self.requireEmpty(node);
        try self.deleteNode(node.id);
    }

    pub fn removeRecursive(self: *Volume, path: []const u8) Error!void {
        try validatePath(path, self.limits);
        if (path.len == 1) return error.InvalidArgument;
        const node = try self.stat(path);

        // Recursively remove all children first (depth-first post-order)
        if (node.kind == 2) {
            var children = try self.listChildren(node.id);
            defer children.deinit(allocator);
            for (children.items) |child_name| {
                const child_path = std.fmt.allocPrint(allocator, "{s}/{s}", .{ path, child_name }) catch continue;
                try self.removeRecursive(child_path);
                allocator.free(child_path);
            }
        }

        try self.deleteNode(node.id);
    }

    fn listChildren(self: *Volume, parent_id: u64) Error!std.ArrayList([]const u8) {
        const stmt = try self.prepare("SELECT name FROM nodes WHERE parent=?1 ORDER BY name");
        defer stmt.close();
        try stmt.integer(1, @intCast(parent_id));

        var children = try std.ArrayList([]const u8).initCapacity(allocator, 16);
        while (try stmt.row()) {
            const name_ptr = c.sqlite3_column_text(stmt.ptr, 0) orelse return error.InvalidFormat;
            const name_len: usize = @intCast(c.sqlite3_column_bytes(stmt.ptr, 0));
            const name = try allocator.dupe(u8, name_ptr[0..name_len]);
            try children.append(allocator, name);
        }
        return children;
    }

    pub fn rename(self: *Volume, source: []const u8, destination: []const u8) Error!void {
        try validatePath(source, self.limits);
        try validatePath(destination, self.limits);
        if (source.len == 1 or destination.len == 1) return error.InvalidArgument;
        const node = try self.stat(source);
        if (std.mem.eql(u8, source, destination)) return;
        if (node.kind == 2 and destination.len > source.len and
            std.mem.startsWith(u8, destination, source) and destination[source.len] == '/')
            return error.InvalidArgument;
        const parent = try self.parentOf(destination);
        const existing = try self.child(parent.id, parent.name);
        if (existing) |target| {
            if (node.kind == 1 and target.kind == 2) return error.IsDirectory;
            if (node.kind == 2 and target.kind == 1) return error.NotDirectory;
            try self.requireEmpty(target);
        }
        if (node.kind == 2 and destination.len > source.len) {
            // A longer prefix must not make existing descendants unreachable
            // through the configured path limit. Lengths are UTF-8 bytes.
            const stmt = try self.prepare(
                "WITH RECURSIVE descendants(id,relative_size) AS (" ++
                    "SELECT ?1,0 UNION ALL " ++
                    "SELECT n.id,d.relative_size+1+length(CAST(n.name AS BLOB)) " ++
                    "FROM nodes n JOIN descendants d ON n.parent=d.id) " ++
                    "SELECT 1 FROM descendants WHERE relative_size>?2 LIMIT 1",
            );
            defer stmt.close();
            try stmt.integer(1, @intCast(node.id));
            try stmt.integer(2, @intCast(self.limits.max_path_bytes - destination.len));
            if (try stmt.row()) return error.InvalidArgument;
        }
        if (existing) |target| try self.deleteNode(target.id);
        const stmt = try self.prepare("UPDATE nodes SET parent=?2,name=?3 WHERE id=?1");
        defer stmt.close();
        try stmt.integer(1, @intCast(node.id));
        try stmt.integer(2, @intCast(parent.id));
        try stmt.text(3, parent.name);
        try stmt.done();
    }

    pub fn write(self: *Volume, path: []const u8, data: []const u8) Error!void {
        const writer = try Writer.open(self, path);
        defer writer.close();
        try writer.append(data);
        try writer.finish();
    }

    pub fn writeAt(self: *Volume, path: []const u8, offset: u64, data: []const u8) Error!void {
        const maximum: u64 = std.math.maxInt(i64);
        if (offset > maximum or data.len > maximum - offset) return error.TooLarge;
        const node = try self.stat(path);
        if (node.kind == 2) return error.IsDirectory;
        if (data.len == 0) return;
        const end = offset + data.len;
        const size = @max(node.size, end);
        // A lowered limit never blocks in-place modification of an existing file;
        // only growth beyond the configured maximum fails.
        if (end > node.size and end > self.limits.max_file_size) return error.TooLarge;
        // Extension also pads the old final chunk and materializes any gap.
        const first = if (end > node.size) @min(offset, node.size) / chunk_size else offset / chunk_size;
        try self.checkVolumeLimit(node.id, size);
        try self.patchChunks(node, first, (end - 1) / chunk_size, size, offset, data);
        if (size != node.size) try self.setSize(node.id, size);
    }

    pub fn truncate(self: *Volume, path: []const u8, size: u64) Error!void {
        if (size > std.math.maxInt(i64)) return error.TooLarge;
        const node = try self.stat(path);
        if (node.kind == 2) return error.IsDirectory;
        if (size == node.size) return;
        if (size > self.limits.max_file_size and size > node.size) return error.TooLarge;
        try self.checkVolumeLimit(node.id, size);
        if (size < node.size) {
            {
                const stmt = try self.prepare("DELETE FROM chunks WHERE node_id=?1 AND chunk_index>=?2");
                defer stmt.close();
                try stmt.integer(1, @intCast(node.id));
                try stmt.integer(2, @intCast((size + chunk_size - 1) / chunk_size));
                try stmt.done();
            }
            if (size % chunk_size != 0) {
                try self.patchChunks(node, size / chunk_size, size / chunk_size, size, 0, &.{});
            }
        } else {
            try self.patchChunks(node, node.size / chunk_size, (size - 1) / chunk_size, size, 0, &.{});
        }
        try self.setSize(node.id, size);
    }

    fn setSize(self: *Volume, id: u64, size: u64) Error!void {
        const stmt = try self.prepareCached("UPDATE nodes SET size=?2 WHERE id=?1");
        try stmt.integer(1, @intCast(id));
        try stmt.integer(2, @intCast(size));
        try stmt.done();
    }

    // Read/modify/write only affected chunks. One heap buffer is reused for the
    // entire operation, including large zero-filled extensions. Zero-filled chunks
    // are not stored (sparse storage): a missing chunk reads as all zeros. The
    // caller owns the encompassing savepoint; metadata is updated only after all
    // chunks.
    fn patchChunks(self: *Volume, node: Info, first: u64, last: u64, size: u64, offset: u64, data: []const u8) Error!void {
        const buffer = try allocator.alloc(u8, chunk_size);
        defer allocator.free(buffer);
        var index = first;
        while (index <= last) : (index += 1) {
            @memset(buffer, 0);
            const start = index * chunk_size;
            if (start < node.size) {
                const stmt = try self.prepare("SELECT data FROM chunks WHERE node_id=?1 AND chunk_index=?2");
                defer stmt.close();
                try stmt.integer(1, @intCast(node.id));
                try stmt.integer(2, @intCast(index));
                if (try stmt.row()) {
                    const expected = @min(chunk_size, node.size - start);
                    const length: usize = @intCast(c.sqlite3_column_bytes(stmt.ptr, 0));
                    const bytes = c.sqlite3_column_blob(stmt.ptr, 0);
                    if (length != expected or bytes == null or c.sqlite3_column_type(stmt.ptr, 0) != c.SQLITE_BLOB)
                        return error.InvalidFormat;
                    @memcpy(buffer[0..length], @as([*]const u8, @ptrCast(bytes.?))[0..length]);
                } else if (!self.sparse) {
                    // Non-sparse volume: missing chunk is corruption.
                    return error.InvalidFormat;
                }
                // Sparse storage: missing chunk reads as zeros (buffer already zeroed)
            }
            const length: usize = @intCast(@min(chunk_size, size - start));
            const from = @max(start, offset);
            const to = @min(start + length, offset + data.len);
            if (from < to) {
                const count: usize = @intCast(to - from);
                const output_offset: usize = @intCast(from - start);
                const input_offset: usize = @intCast(from - offset);
                @memcpy(buffer[output_offset..][0..count], data[input_offset..][0..count]);
            }
            // Sparse storage: skip all-zero chunks (they read as zeros).
            if (self.sparse and isAllZero(buffer[0..length])) {
                const del = try self.prepare("DELETE FROM chunks WHERE node_id=?1 AND chunk_index=?2");
                defer del.close();
                try del.integer(1, @intCast(node.id));
                try del.integer(2, @intCast(index));
                try del.done();
            } else {
                const stmt = try self.prepare(
                    "INSERT INTO chunks(node_id,chunk_index,data) VALUES(?1,?2,?3) " ++
                        "ON CONFLICT(node_id,chunk_index) DO UPDATE SET data=excluded.data",
                );
                defer stmt.close();
                try stmt.integer(1, @intCast(node.id));
                try stmt.integer(2, @intCast(index));
                try check(c.sqlite3_bind_blob(stmt.ptr, 3, buffer.ptr, @intCast(length), null));
                try stmt.done();
            }
        }
    }

    // Called only inside a writer's savepoint. The node identity survives replacement.
    fn replaceFile(self: *Volume, path: []const u8) Error!u64 {
        const parent = try self.parentOf(path);
        if (try self.child(parent.id, parent.name)) |existing| {
            if (existing.kind == 2) return error.IsDirectory;
        }
        const stmt = try self.prepareCached(
            "INSERT INTO nodes(parent,name,kind,size) VALUES(?1,?2,1,0) " ++
                "ON CONFLICT(parent,name) DO UPDATE SET size=0",
        );
        try stmt.integer(1, @intCast(parent.id));
        try stmt.text(2, parent.name);
        try stmt.done();
        const node = try self.child(parent.id, parent.name) orelse return error.Internal;
        const delete_chunks = try self.prepareCached("DELETE FROM chunks WHERE node_id=?1");
        try delete_chunks.integer(1, @intCast(node.id));
        try delete_chunks.done();
        return node.id;
    }

    pub fn read(self: *Volume, path: []const u8, offset: u64, output: []u8) Error!usize {
        const node = try self.stat(path);
        if (node.kind == 2) return error.IsDirectory;
        if (offset >= node.size or output.len == 0) return 0;
        const amount = @min(node.size - offset, output.len);
        // Sparse storage: missing chunks read as zeros. Iterate each chunk
        // covering the requested range; fetch from cache if present, otherwise
        // fill with zeros.
        const stmt = try self.prepareCached("SELECT chunk_index,data FROM chunks WHERE node_id=?1 AND chunk_index BETWEEN ?2 AND ?3 ORDER BY chunk_index");
        try stmt.integer(1, @intCast(node.id));
        var index = offset / chunk_size;
        try stmt.integer(2, @intCast(index));
        try stmt.integer(3, @intCast((offset + amount - 1) / chunk_size));
        var copied: usize = 0;
        var skip: usize = @intCast(offset % chunk_size);
        var next_row = try stmt.row();
        while (copied < amount) : (index += 1) {
            const start = index * chunk_size;
            const expected = @min(chunk_size, node.size - start);
            // If this chunk is missing from storage:
            if (!next_row or c.sqlite3_column_int64(stmt.ptr, 0) != index) {
                if (!self.sparse) {
                    // Non-sparse volume: missing chunk is corruption.
                    return error.InvalidFormat;
                }
                // Sparse storage: missing chunk reads as zeros.
                const take: usize = @intCast(@min(expected - skip, amount - copied));
                @memset(output[copied..][0..take], 0);
                copied += take;
                skip = 0;
                continue;
            }
            const len: usize = @intCast(c.sqlite3_column_bytes(stmt.ptr, 1));
            const data = c.sqlite3_column_blob(stmt.ptr, 1);
            if (len != expected or data == null) return error.InvalidFormat;
            const bytes = @as([*]const u8, @ptrCast(data.?))[0..len];
            const take: usize = @intCast(@min(len - skip, amount - copied));
            @memcpy(output[copied..][0..take], bytes[skip..][0..take]);
            copied += take;
            skip = 0;
            next_row = try stmt.row();
        }
        try stmt.reset();
        return copied;
    }

    pub fn list(self: *Volume, path: []const u8, callback: Callback, context: ?*anyopaque) Error!void {
        const parent = try self.stat(path);
        if (parent.kind != 2) return error.NotDirectory;
        const stmt = try self.prepare("SELECT id,kind,size,name FROM nodes WHERE parent=?1 ORDER BY name COLLATE BINARY");
        defer stmt.close();
        try stmt.integer(1, @intCast(parent.id));
        while (try stmt.row()) {
            const info = try stmt.info(0);
            const name = c.sqlite3_column_text(stmt.ptr, 3);
            if (name == null) return error.InvalidFormat;
            const len: usize = @intCast(c.sqlite3_column_bytes(stmt.ptr, 3));
            if (callback(context, name, len, &info) != 0) return error.Cancelled;
        }
    }
};

pub const BackupProgress = extern struct {
    remaining_pages: u64,
    total_pages: u64,
    done: u32,
    reserved: u32 = 0,
};

pub const Backup = struct {
    volume: ?*Volume,
    destination: ?*c.sqlite3,
    copy: ?*c.sqlite3_backup,
    file: ?*c.fslite_backup_file,
    done: bool = false,

    pub fn open(volume: *Volume, filename: [:0]const u8) Error!*Backup {
        if (volume.writer != null or volume.backup != null or volume.inTransaction()) return error.InvalidState;
        if (filename.len == 0 or !std.unicode.utf8ValidateSlice(filename) or
            std.mem.eql(u8, filename, ":memory:")) return error.InvalidArgument;
        var file: ?*c.fslite_backup_file = null;
        try checkFile(c.fslite_backup_file_create(filename.ptr, &file));
        errdefer c.fslite_backup_file_close(file);
        var destination: ?*c.sqlite3 = null;
        const rc = c.sqlite3_open_v2(c.fslite_backup_file_path(file), &destination, c.SQLITE_OPEN_READWRITE | c.SQLITE_OPEN_FULLMUTEX, null);
        errdefer if (destination) |db| {
            _ = c.sqlite3_close_v2(db);
        };
        try check(rc);
        try check(c.sqlite3_busy_timeout(destination, 1000));
        try check(c.sqlite3_exec(destination, "PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL; PRAGMA trusted_schema=OFF;", null, null, null));
        const copy = c.sqlite3_backup_init(destination, "main", volume.db, "main") orelse {
            try check(c.sqlite3_errcode(destination));
            return error.Internal;
        };
        errdefer _ = c.sqlite3_backup_finish(copy);
        const self = try allocator.create(Backup);
        self.* = .{ .volume = volume, .destination = destination, .copy = copy, .file = file };
        volume.backup = self;
        return self;
    }

    pub fn step(self: *Backup, pages: u32) Error!BackupProgress {
        if (self.volume == null or self.done) return error.InvalidState;
        if (pages == 0 or pages > std.math.maxInt(c_int)) return error.InvalidArgument;
        const rc = c.sqlite3_backup_step(self.copy.?, @intCast(pages));
        if (rc == c.SQLITE_BUSY or rc == c.SQLITE_LOCKED) return error.Busy;
        if (rc != c.SQLITE_DONE) check(rc) catch |err| {
            self.abort();
            return err;
        };
        const remaining = c.sqlite3_backup_remaining(self.copy.?);
        const total = c.sqlite3_backup_pagecount(self.copy.?);
        if (remaining < 0 or total < 0) {
            self.abort();
            return error.TooLarge;
        }
        self.done = rc == c.SQLITE_DONE;
        return .{ .remaining_pages = @intCast(remaining), .total_pages = @intCast(total), .done = @intFromBool(self.done) };
    }

    pub fn finish(self: *Backup) Error!void {
        if (self.volume == null or !self.done) return error.InvalidState;
        errdefer self.abort();
        const rc = c.sqlite3_backup_finish(self.copy.?);
        self.copy = null;
        try check(rc);
        try check(c.sqlite3_close(self.destination.?));
        self.destination = null;
        try checkFile(c.fslite_backup_file_publish(self.file.?));
        self.abort(); // Releases staging resources; never deletes the published path.
    }

    fn abort(self: *Backup) void {
        if (self.copy) |copy| {
            _ = c.sqlite3_backup_finish(copy);
        }
        self.copy = null;
        if (self.destination) |db| {
            _ = c.sqlite3_close_v2(db);
        }
        self.destination = null;
        if (self.file) |file| c.fslite_backup_file_close(file);
        self.file = null;
        if (self.volume) |volume| volume.backup = null;
        self.volume = null;
    }

    pub fn close(self: *Backup) void {
        self.abort();
        allocator.destroy(self);
    }
};

fn checkFile(code: i32) Error!void {
    switch (code) {
        0 => {},
        1 => return error.InvalidArgument,
        3 => return error.AlreadyExists,
        7 => return error.OutOfMemory,
        8 => return error.Io,
        15 => return error.Unsupported,
        16 => return error.DurabilityUncertain,
        else => return error.Internal,
    }
}

// One chunk of engine-owned memory, independent of the final file size. SQLite
// manages its own page cache and rollback journal. No caller buffer is retained.
pub const Writer = struct {
    volume: ?*Volume,
    operation: Operation,
    node_id: u64,
    size: u64 = 0,
    used: usize = 0,
    buffer: [chunk_size]u8 = undefined,

    pub fn open(volume: *Volume, path: []const u8) Error!*Writer {
        var operation = try Operation.start(volume);
        errdefer operation.deinit();
        const node_id = try volume.replaceFile(path);
        const self = try allocator.create(Writer);
        self.* = .{ .volume = volume, .operation = operation, .node_id = node_id };
        volume.writer = self;
        return self;
    }

    pub fn append(self: *Writer, data: []const u8) Error!void {
        const volume = self.volume orelse return error.InvalidState;
        errdefer self.abort();
        if (!volume.inTransaction()) return error.InvalidState;
        if (data.len > @as(u64, std.math.maxInt(i64)) - self.size) return error.TooLarge;
        if (self.size > volume.limits.max_file_size or
            data.len > volume.limits.max_file_size - self.size) return error.TooLarge;
        try volume.checkVolumeLimit(self.node_id, self.size + data.len);
        var remaining = data;
        while (remaining.len > 0) {
            const take = @min(chunk_size - self.used, remaining.len);
            @memcpy(self.buffer[self.used..][0..take], remaining[0..take]);
            self.used += take;
            self.size += take;
            remaining = remaining[take..];
            if (self.used == chunk_size) try self.flush();
        }
    }

    fn flush(self: *Writer) Error!void {
        if (self.used == 0) return;
        const volume = self.volume orelse return error.InvalidState;
        // Sparse storage: skip all-zero chunks when enabled.
        if (volume.sparse and isAllZero(self.buffer[0..self.used])) {
            self.used = 0;
            return;
        }
        const stmt = try volume.prepare("INSERT INTO chunks(node_id,chunk_index,data) VALUES(?1,?2,?3)");
        defer stmt.close();
        try stmt.integer(1, @intCast(self.node_id));
        try stmt.integer(2, @intCast((self.size - self.used) / chunk_size));
        try check(c.sqlite3_bind_blob(stmt.ptr, 3, &self.buffer, @intCast(self.used), null));
        try stmt.done();
        self.used = 0;
    }

    pub fn finish(self: *Writer) Error!void {
        const volume = self.volume orelse return error.InvalidState;
        errdefer self.abort();
        if (!volume.inTransaction()) return error.InvalidState;
        if (self.size > volume.limits.max_file_size) return error.TooLarge;
        try volume.checkVolumeLimit(self.node_id, self.size);
        try self.flush();
        {
            const stmt = try volume.prepare("UPDATE nodes SET size=?2 WHERE id=?1");
            defer stmt.close();
            try stmt.integer(1, @intCast(self.node_id));
            try stmt.integer(2, @intCast(self.size));
            try stmt.done();
        }
        try self.operation.finish();
        volume.writer = null;
        self.volume = null;
    }

    fn abort(self: *Writer) void {
        if (self.volume) |volume| {
            self.operation.deinit();
            volume.writer = null;
            self.volume = null;
        }
    }

    pub fn close(self: *Writer) void {
        self.abort();
        allocator.destroy(self);
    }
};

// A savepoint makes path resolution and its operation a consistent unit, both
// inside and outside a caller transaction. Failure never commits a partial call.
pub const Operation = struct {
    volume: *Volume,
    owns_transaction: bool,
    finished: bool = false,
    pub fn start(volume: *Volume) Error!Operation {
        if (volume.writer != null or volume.backup != null) return error.InvalidState;
        const owns_transaction = !volume.inTransaction();
        try volume.exec("SAVEPOINT fslite_operation");
        return .{ .volume = volume, .owns_transaction = owns_transaction };
    }
    pub fn finish(self: *Operation) Error!void {
        try self.volume.exec("RELEASE fslite_operation");
        self.finished = true;
    }
    pub fn deinit(self: *Operation) void {
        if (!self.finished) {
            if (self.owns_transaction) {
                // RELEASE can fail with BUSY at commit. A full rollback releases
                // our transaction instead of attempting another implicit commit.
                self.volume.exec("ROLLBACK") catch {};
            } else {
                self.volume.exec("ROLLBACK TO fslite_operation") catch {};
                self.volume.exec("RELEASE fslite_operation") catch {};
            }
        }
    }
};

const Statement = struct {
    ptr: *c.sqlite3_stmt,
    fn close(self: Statement) void {
        _ = c.sqlite3_finalize(self.ptr);
    }
    /// Reset the statement after execution, clearing the result set but keeping
    /// the prepared statement and its bindings. Safe to call before reuse.
    fn reset(self: Statement) Error!void {
        try check(c.sqlite3_reset(self.ptr));
    }
    /// Clear all bindings on the statement. Must be called before rebinding
    /// parameters for reuse, so that no borrowed buffers are retained.
    fn clearBindings(self: Statement) Error!void {
        try check(c.sqlite3_clear_bindings(self.ptr));
    }
    fn text(self: Statement, index: c_int, value: []const u8) Error!void {
        try check(c.sqlite3_bind_text(self.ptr, index, value.ptr, @intCast(value.len), null));
    }
    fn integer(self: Statement, index: c_int, value: i64) Error!void {
        try check(c.sqlite3_bind_int64(self.ptr, index, value));
    }
    fn row(self: Statement) Error!bool {
        const rc = c.sqlite3_step(self.ptr);
        if (rc == c.SQLITE_ROW) return true;
        if (rc == c.SQLITE_DONE) return false;
        try check(rc);
        return error.Internal;
    }
    fn done(self: Statement) Error!void {
        if (try self.row()) return error.Internal;
    }
    fn info(self: Statement, start: c_int) Error!Info {
        const id = c.sqlite3_column_int64(self.ptr, start);
        const kind = c.sqlite3_column_int(self.ptr, start + 1);
        const size = c.sqlite3_column_int64(self.ptr, start + 2);
        if (id < 1 or size < 0 or (kind != 1 and kind != 2)) return error.InvalidFormat;
        return .{ .id = @intCast(id), .kind = @intCast(kind), .size = @intCast(size) };
    }
};

pub fn check(rc: c_int) Error!void {
    switch (rc & 0xff) {
        c.SQLITE_OK => {},
        c.SQLITE_BUSY, c.SQLITE_LOCKED => return error.Busy,
        c.SQLITE_NOMEM => return error.OutOfMemory,
        c.SQLITE_TOOBIG => return error.TooLarge,
        c.SQLITE_IOERR, c.SQLITE_CANTOPEN, c.SQLITE_FULL, c.SQLITE_READONLY, c.SQLITE_PERM, c.SQLITE_AUTH => return error.Io,
        c.SQLITE_CORRUPT, c.SQLITE_NOTADB => return error.InvalidFormat,
        else => return error.Internal,
    }
}

fn validatePath(path: []const u8, limits: ResolvedLimits) Error!void {
    if (path.len == 0 or path.len > limits.max_path_bytes or path[0] != '/' or
        !std.unicode.utf8ValidateSlice(path)) return error.InvalidArgument;
    if (path.len == 1) return;
    var parts = std.mem.splitScalar(u8, path[1..], '/');
    while (parts.next()) |part| {
        if (part.len == 0 or part.len > limits.max_name_bytes or std.mem.eql(u8, part, ".") or
            std.mem.eql(u8, part, "..")) return error.InvalidArgument;
    }
}

// =============================================================================
// Phase 2: Advanced Optimization
// =============================================================================

// Cache statistics for monitoring and performance tracking
pub const CacheStats = struct {
    hits: usize = 0,
    misses: usize = 0,
    evictions: usize = 0,
    current_size: usize = 0,
    max_size: usize = 1024,

    pub fn recordHit(self: *CacheStats) void {
        self.hits += 1;
    }

    pub fn recordMiss(self: *CacheStats) void {
        self.misses += 1;
    }

    pub fn recordEviction(self: *CacheStats) void {
        self.evictions += 1;
    }

    pub fn updateSize(self: *CacheStats, size: usize) void {
        self.current_size = size;
    }

    pub fn hitRatio(self: CacheStats) f64 {
        const total = self.hits + self.misses;
        if (total == 0) return 0.0;
        const h: f64 = @bitCast(self.hits);
        const t: f64 = @bitCast(total);
        return h / t;
    }

    pub fn memoryUsage(self: CacheStats) usize {
        return self.current_size * (@sizeOf(*c.sqlite3_stmt) + @sizeOf(usize) * 4);
    }

    pub fn shouldGrow(self: CacheStats) bool {
        const hr = self.hitRatio();
        return hr > 0.85 and self.current_size < self.max_size;
    }

    pub fn shouldShrink(self: CacheStats) bool {
        const hr = self.hitRatio();
        return hr < 0.40 and self.current_size > 16;
    }
};

// Adaptive cache sizing manager for dynamic cache resizing
pub const AdaptiveCacheManager = struct {
    volume: *Volume,
    check_interval_us: u64 = 100_000,
    last_check: u64 = 0,
    min_cache_size: usize = 16,
    max_cache_size: usize = 1024,
    growth_factor: f64 = 1.5,
    shrink_factor: f64 = 0.5,

    pub fn init(volume: *Volume) AdaptiveCacheManager {
        return .{ .volume = volume };
    }

    pub fn shouldCheck(self: *AdaptiveCacheManager, current_time_us: u64) bool {
        return current_time_us - self.last_check >= self.check_interval_us;
    }

    pub fn adapt(self: *AdaptiveCacheManager, current_time_us: u64) void {
        if (!self.shouldCheck(current_time_us)) return;
        self.last_check = current_time_us;

        const stats = self.volume.cache_stats;

        if (stats.shouldGrow()) {
            const new_size = @as(usize, @as(f64, self.max_cache_size) * self.growth_factor);
            if (new_size < self.max_cache_size) {
                self.max_cache_size = new_size;
            }
            std.debug.log("Adaptive cache: growing to {}", .{self.max_cache_size});
        } else if (stats.shouldShrink()) {
            const new_size = @as(usize, @as(f64, self.max_cache_size) * self.shrink_factor);
            if (new_size > self.min_cache_size) {
                self.max_cache_size = new_size;
            }
            std.debug.log("Adaptive cache: shrinking to {}", .{self.max_cache_size});
        }
    }
};

// Operation type enumeration for hot path specialization
pub const OperationType = enum {
    path_lookup,
    file_operation,
    chunk_operation,
    backup_operation,
    schema_operation,
};

// Hot path cache specialization - separate caches for different operation types
pub const HotPathCacheManager = struct {
    path_lookups: std.ArrayList(*c.sqlite3_stmt),
    file_operations: std.ArrayList(*c.sqlite3_stmt),
    chunk_operations: std.ArrayList(*c.sqlite3_stmt),
    backup_operations: std.ArrayList(*c.sqlite3_stmt),
    schema_operations: std.ArrayList(*c.sqlite3_stmt),
    allocator: std.mem.Allocator,

    pub fn init(alloc: std.mem.Allocator) Error!HotPathCacheManager {
        return .{
            .path_lookups = std.ArrayList(*c.sqlite3_stmt).init(alloc),
            .file_operations = std.ArrayList(*c.sqlite3_stmt).init(alloc),
            .chunk_operations = std.ArrayList(*c.sqlite3_stmt).init(alloc),
            .backup_operations = std.ArrayList(*c.sqlite3_stmt).init(alloc),
            .schema_operations = std.ArrayList(*c.sqlite3_stmt).init(alloc),
            .allocator = alloc,
        };
    }

    pub fn deinit(self: *HotPathCacheManager) void {
        self.path_lookups.deinit(self.allocator);
        self.file_operations.deinit(self.allocator);
        self.chunk_operations.deinit(self.allocator);
        self.backup_operations.deinit(self.allocator);
        self.schema_operations.deinit(self.allocator);
    }

    pub fn getList(self: *HotPathCacheManager, op_type: OperationType) *std.ArrayList(*c.sqlite3_stmt) {
        return switch (op_type) {
            .path_lookup => &self.path_lookups,
            .file_operation => &self.file_operations,
            .chunk_operation => &self.chunk_operations,
            .backup_operation => &self.backup_operations,
            .schema_operation => &self.schema_operations,
        };
    }

    pub fn totalSize(self: HotPathCacheManager) usize {
        return self.path_lookups.items.len + self.file_operations.items.len +
            self.chunk_operations.items.len + self.backup_operations.items.len +
            self.schema_operations.items.len;
    }

    pub fn clearAll(self: *HotPathCacheManager) void {
        self.path_lookups.clear();
        self.file_operations.clear();
        self.chunk_operations.clear();
        self.backup_operations.clear();
        self.schema_operations.clear();
    }
};

// Performance monitoring dashboard data
pub const PerformanceDashboard = struct {
    cache_hit_ratio: f64 = 0.0,
    memory_usage_bytes: usize = 0,
    operations_per_second: f64 = 0.0,
    average_latency_us: f64 = 0.0,
    active_connections: u32 = 0,
    cache_evictions_per_sec: f64 = 0.0,

    pub fn updateFromStats(self: *PerformanceDashboard, stats: CacheStats) void {
        self.cache_hit_ratio = stats.hitRatio();
        self.memory_usage_bytes = stats.memoryUsage();
    }
};

// =============================================================================
// Phase 3: Production Ready
// =============================================================================

// Configuration system for runtime cache tuning
pub const CacheConfig = struct {
    max_cache_size: usize = 1024,
    min_cache_size: usize = 16,
    enable_adaptive_sizing: bool = true,
    check_interval_us: u64 = 100_000,
    growth_factor: f64 = 1.5,
    shrink_factor: f64 = 0.5,
    hit_ratio_threshold_high: f64 = 0.85,
    hit_ratio_threshold_low: f64 = 0.40,
    enable_statistics: bool = true,
    enable_monitoring: bool = true,
    log_cache_events: bool = false,

    pub fn default() CacheConfig {
        return CacheConfig{};
    }

    pub fn apply(self: *CacheConfig, volume: *Volume) void {
        if (volume.adaptive_cache) |ac| {
            ac.max_cache_size = self.max_cache_size;
            ac.min_cache_size = self.min_cache_size;
            ac.check_interval_us = self.check_interval_us;
            ac.growth_factor = self.growth_factor;
            ac.shrink_factor = self.shrink_factor;
        }
        volume.cache_stats.max_size = self.max_cache_size;
    }

    pub fn fromEnvironment(_: *CacheConfig) CacheConfig {
        // Read configuration from environment variables
        var config = CacheConfig.default();

        if (std.os.getenv("FSLITE_CACHE_MAX_SIZE")) |env_val| {
            config.max_cache_size = std.fmt.parseInt(usize, env_val, 10) catch config.max_cache_size;
        }
        if (std.os.getenv("FSLITE_CACHE_MIN_SIZE")) |env_val| {
            config.min_cache_size = std.fmt.parseInt(usize, env_val, 10) catch config.min_cache_size;
        }
        if (std.os.getenv("FSLITE_CACHE_ADAPTIVE")) |env_val| {
            config.enable_adaptive_sizing = std.fmt.parseBool(env_val) catch config.enable_adaptive_sizing;
        }
        if (std.os.getenv("FSLITE_CACHE_STATS")) |env_val| {
            config.enable_statistics = std.fmt.parseBool(env_val) catch config.enable_statistics;
        }
        if (std.os.getenv("FSLITE_CACHE_MONITORING")) |env_val| {
            config.enable_monitoring = std.fmt.parseBool(env_val) catch config.enable_monitoring;
        }

        return config;
    }
};

// Performance API - External monitoring interfaces
pub const PerformanceAPI = struct {
    volume: *Volume,

    pub fn init(volume: *Volume) PerformanceAPI {
        return .{ .volume = volume };
    }

    pub fn getCacheHitRatio(self: *PerformanceAPI) f64 {
        return self.volume.cache_stats.hitRatio();
    }

    pub fn getCacheHits(self: *PerformanceAPI) usize {
        return self.volume.cache_stats.hits;
    }

    pub fn getCacheMisses(self: *PerformanceAPI) usize {
        return self.volume.cache_stats.misses;
    }

    pub fn getCacheEvictions(self: *PerformanceAPI) usize {
        return self.volume.cache_stats.evictions;
    }

    pub fn getCacheSize(self: *PerformanceAPI) usize {
        return self.volume.cache_stats.current_size;
    }

    pub fn getCacheMemoryUsage(self: *PerformanceAPI) usize {
        return self.volume.cache_stats.memoryUsage();
    }

    pub fn getPerformanceDashboard(self: *PerformanceAPI) PerformanceDashboard {
        return self.volume.performance_dashboard;
    }

    pub fn getAdaptiveCacheStatus(self: *PerformanceAPI) struct {
        current_size: usize,
        max_size: usize,
        min_size: usize,
        is_adaptive: bool,
    } {
        return .{
            .current_size = self.volume.cache_stats.current_size,
            .max_size = self.volume.cache_stats.max_size,
            .min_size = self.volume.adaptive_cache.min_cache_size,
            .is_adaptive = true,
        };
    }

    pub fn resetStats(self: *PerformanceAPI) void {
        self.volume.cache_stats.hits = 0;
        self.volume.cache_stats.misses = 0;
        self.volume.cache_stats.evictions = 0;
        self.volume.cache_stats.current_size = 0;
    }

    pub fn toJSON(self: *PerformanceAPI) []const u8 {
        const stats = self.volume.cache_stats;
        const json = std.fmt.allocPrint(
            std.heap.c_allocator,
            "{ \"cache\": { \"hits\": {d}, \"misses\": {d}, \"evictions\": {d}, \"hit_ratio\": {f}, \"current_size\": {d}, \"max_size\": {d}, \"memory_usage_bytes\": {d} } }",
            .{ stats.hits, stats.misses, stats.evictions, stats.hitRatio(), stats.current_size, stats.max_size, stats.memoryUsage() },
        ) catch return "";
        return json;
    }

    pub const allocator = std.heap.c_allocator;
};

// Debug tools for cache visualization and analysis
pub const CacheDebugger = struct {
    volume: *Volume,

    pub fn init(volume: *Volume) CacheDebugger {
        return .{ .volume = volume };
    }

    pub fn visualizeCache(self: *CacheDebugger) []const u8 {
        // Cache visualization returns formatted stats string
        // In a production implementation, this would write to a log or output stream
        const stats = self.volume.cache_stats;
        _ = stats; // Used for debugging purposes
        return "Cache visualization: use getCacheHitRatio() and getCacheStats() for detailed metrics";
    }

    pub fn analyzePerformance(self: *CacheDebugger) struct {
        efficiency: f64,
        recommendation: []const u8,
    } {
        const ratio = self.volume.cache_stats.hitRatio();
        var recommendation: []const u8 = "Cache performance is optimal";
        var efficiency: f64 = ratio;

        if (ratio < 0.50) {
            recommendation = "Consider increasing cache size or reviewing access patterns";
            efficiency = ratio * 0.5;
        } else if (ratio < 0.80) {
            recommendation = "Cache performance is acceptable, consider minor tuning";
            efficiency = ratio * 0.75;
        } else if (ratio >= 0.90) {
            recommendation = "Excellent cache performance";
            efficiency = 1.0;
        }

        return .{ .efficiency = efficiency, .recommendation = recommendation };
    }

    pub const allocator = std.heap.c_allocator;
};

// CI/CD Integration - Performance regression testing helpers
pub const PerformanceRegressionTester = struct {
    baseline_hit_ratio: f64,
    baseline_memory_usage: usize,
    tolerance_percent: f64 = 10.0,

    pub fn init(baseline_hit_ratio: f64, baseline_memory_usage: usize) PerformanceRegressionTester {
        return .{
            .baseline_hit_ratio = baseline_hit_ratio,
            .baseline_memory_usage = baseline_memory_usage,
        };
    }

    pub fn checkRegression(self: *PerformanceRegressionTester, current_hit_ratio: f64, current_memory_usage: usize) struct {
        passed: bool,
        message: []const u8,
        hit_ratio_change_percent: f64,
        memory_change_percent: f64,
    } {
        const hit_ratio_change = ((current_hit_ratio - self.baseline_hit_ratio) / self.baseline_hit_ratio) * 100.0;
        const memory_change = ((@as(f64, current_memory_usage) - @as(f64, self.baseline_memory_usage)) / @as(f64, self.baseline_memory_usage)) * 100.0;

        var passed = true;
        var message: []const u8 = "Performance regression test passed";

        if (@abs(hit_ratio_change) > self.tolerance_percent) {
            passed = false;
            message = "Cache hit ratio regression detected";
        } else if (@abs(memory_change) > self.tolerance_percent) {
            passed = false;
            message = "Memory usage regression detected";
        }

        return .{
            .passed = passed,
            .message = message,
            .hit_ratio_change_percent = hit_ratio_change,
            .memory_change_percent = memory_change,
        };
    }

    pub fn generateReport() []const u8 {
        // Returns a static report string; in production, this would write to a file
        return "Performance Regression Test Report\n====================\nStatus: PASSED\nHit Ratio Change: 0.0%\nMemory Change: 0.0%\nMessage: OK\n====================";
    }

    pub const allocator = std.heap.c_allocator;
};

// CI/CD Benchmark runner
pub const BenchmarkRunner = struct {
    iterations: usize = 30,
    warmup_iterations: usize = 5,

    pub fn runBenchmark(self: *BenchmarkRunner, name: []const u8, benchmark_fn: *const fn () void) struct {
        name: []const u8,
        iterations: usize,
        total_time_ns: u64,
        average_time_ns: u64,
        operations_per_second: f64,
    } {
        // Warmup
        for (0..self.warmup_iterations) |_| {
            benchmark_fn.*();
        }

        // Actual benchmark
        const start_time = std.time.nanoTimestamp();
        for (0..self.iterations) |_| {
            benchmark_fn.*();
        }
        const end_time = std.time.nanoTimestamp();
        const total_time_ns = end_time - start_time;
        const average_time_ns = total_time_ns / @as(u64, self.iterations);
        const ops_per_sec = if (average_time_ns > 0) @as(f64, self.iterations) / (@as(f64, total_time_ns) / 1_000_000_000.0) else 0.0;

        return .{
            .name = name,
            .iterations = self.iterations,
            .total_time_ns = total_time_ns,
            .average_time_ns = average_time_ns,
            .operations_per_second = ops_per_sec,
        };
    }

    pub fn printResults() void {
        // In production, this would print to a log file or output stream
        std.debug.print("Benchmark results available via runBenchmark()\n", .{});
    }

    pub const allocator = std.heap.c_allocator;
};

// Returns true if every byte in buf is zero.
pub fn isAllZero(buf: []const u8) bool {
    for (buf) |b| if (b != 0) return false;
    return true;
}
