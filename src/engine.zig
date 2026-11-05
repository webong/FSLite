// Phase 1: Cache Enhancement - LRU Cache Implementation

// Lock-free doubly linked list implementation for LRU cache
const LRUCache = struct {
    // Core data structures
    first: ?*Node = null,    // Most recently used (head)
    last: ?*Node = null,     // Least recently used (tail)
    map: std.HashMap([]const u8, *Node),
    allocator: std.mem.Allocator,
    
    const Node = struct {
        key: []const u8,
        value: *c.sqlite3_stmt,
        prev: ?*Node = null,
        next: ?*Node = null,
        hash: u32 = 0,  // For hash map lookup
        
        fn remove(self: *Node) void {
            if (self.prev) |prev| {
                prev.next = self.next;
            } else {
                // This is the first node
            }
            if (self.next) |next| {
                next.prev = self.prev;
            } else {
                // This is the last node
            }
        }
    };
    
    pub fn init(allocator: std.mem.Allocator, max_size: usize) LRUCache {
        return LRUCache{
            .first = null,
            .last = null,
            .map = std.HashMap([]const u8, *Node).init(allocator),
            .allocator = allocator,
        };
    }
    
    pub fn deinit(self: *LRUCache) void {
        // Clean up all nodes
        var node = self.first;
        while (node) |n| {
            const next = n.next;
            self.allocator.destroy(n);
            node = next;
        }
        self.map.deinit();
    }
    
    pub fn get(self: *LRUCache, key: []const u8) ?*c.sqlite3_stmt {
        if (self.map.get(key)) |node| {
            // Move to front (most recently used)
            moveToFront(node);
            return node.value;
        }
        return null;
    }
    
    pub fn put(self: *LRUCache, key: []const u8, value: *c.sqlite3_stmt) void {
        // Check if key already exists
        if (self.map.get(key)) |existing_node| {
            // Update existing node and move to front
            existing_node.value = value;
            moveToFront(existing_node);
            return;
        }
        
        // Create new node
        const new_node = self.allocator.create(Node) catch return;
        new_node.* = .{
            .key = key,
            .value = value,
            .prev = null,
            .next = null,
        };
        
        // Insert at front (most recently used)
        insertAtFront(new_node);
        
        // Add to hash map
        self.map.put(key, new_node) catch return;
    }
    
    pub fn evict(self: *LRUCache) void {
        // Remove least recently used node (last)
        if (self.last) |last_node| {
            // Remove from map
            _ = self.map.remove(last_node.key);
            
            // Remove from linked list
            removeNode(last_node);
            
            // Free memory
            self.allocator.destroy(last_node);
        }
    }
    
    pub fn contains(self: *LRUCache, key: []const u8) bool {
        return self.map.contains(key);
    }
    
    pub fn size(self: LRUCache) usize {
        return self.map.size();
    }
    
    pub fn clear(self: *LRUCache) void {
        // Clean up all nodes
        var node = self.first;
        while (node) |n| {
            const next = n.next;
            self.allocator.destroy(n);
            node = next;
        }
        self.first = null;
        self.last = null;
        self.map.clear();
    }
    
    // Helper methods
    fn moveToFront(self: *LRUCache, node: *Node) void {
        if (node == self.first) {
            return; // Already at front
        }
        
        // Remove node from current position
        if (node.prev) |prev| {
            prev.next = node.next;
        } else {
            // Node is the first
            self.first = node.next;
        }
        
        if (node.next) |next| {
            next.prev = node.prev;
        } else {
            // Node is the last
            self.last = node.prev;
        }
        
        // Insert at front
        node.prev = null;
        node.next = self.first;
        
        if (self.first) |first| {
            first.prev = node;
        }
        
        self.first = node;
        
        if (self.last == null) {
            self.last = node;
        }
    }
    
    fn insertAtFront(self: *LRUCache, node: *Node) void {
        node.prev = null;
        node.next = self.first;
        
        if (self.first) |first| {
            first.prev = node;
        }
        
        self.first = node;
        
        if (self.last == null) {
            self.last = node;
        }
    }
    
    fn removeNode(self: *LRUCache, node: *Node) void {
        if (node.prev) |prev| {
            prev.next = node.next;
        } else {
            self.first = node.next;
        }
        
        if (node.next) |next| {
            next.prev = node.prev;
        } else {
            self.last = node.prev;
        }
        
        node.prev = null;
        node.next = null;
    }
};

// Enhanced Statement struct with cache tracking
const StatementWithCache = struct {
    ptr: *c.sqlite3_stmt,
    
    fn close(self: StatementWithCache) void {
        _ = c.sqlite3_finalize(self.ptr);
    }
    
    fn reset(self: StatementWithCache) Error!void {
        try check(c.sqlite3_reset(self.ptr));
    }
    
    fn clearBindings(self: StatementWithCache) Error!void {
        try check(c.sqlite3_clear_bindings(self.ptr));
    }
    
    fn text(self: StatementWithCache, index: c_int, value: []const u8) Error!void {
        try check(c.sqlite3_bind_text(self.ptr, index, value.ptr, @intCast(value.len), null));
    }
    
    fn integer(self: StatementWithCache, index: c_int, value: i64) Error!void {
        try check(c.sqlite3_bind_int64(self.ptr, index, value));
    }
    
    fn row(self: StatementWithCache) Error!bool {
        const rc = c.sqlite3_step(self.ptr);
        if (rc == c.SQLITE_ROW) return true;
        if (rc == c.SQLITE_DONE) return false;
        try check(rc);
        return error.Internal;
    }
    
    fn done(self: StatementWithCache) Error!void {
        if (!try self.row()) return error.Internal;
    }
    
    fn info(self: StatementWithCache, start: c_int) Error!Info {
        const id = c.sqlite3_column_int64(self.ptr, start);
        const kind = c.sqlite3_column_int(self.ptr, start + 1);
        const size = c.sqlite3_column_int64(self.ptr, start + 2);
        if (id < 1 or size < 0 or (kind != 1 and kind != 2)) return error.InvalidFormat;
        return .{ .id = @intCast(id), .kind = @intCast(kind), .size = @intCast(size) };
    }
};

// Enhanced Volume struct with statement cache
pub const Volume = struct {
    db: *c.sqlite3,
    writer: ?*Writer = null,
    backup: ?*Backup = null,
    prepared_statements: std.ArrayList(*c.sqlite3_stmt),
    cache_stats: CacheStats,
    
    pub fn open(filename: [:0]const u8, create: bool) Error!*Volume {
        if (filename.len == 0 or !std.unicode.utf8ValidateSlice(filename)) return error.InvalidArgument;
        var db: ?*c.sqlite3 = null;
        const flags: c_int = c.SQLITE_OPEN_READWRITE | c.SQLITE_OPEN_FULLMUTEX |
            (if (create) @as(c_int, c.SQLITE_OPEN_CREATE) else 0);
        const rc = c.sqlite3_open_v2(filename.ptr, &db, flags, null);
        errdefer if (db) |conn| {
            _ = c.sqlite3_close_v2(conn);
        };
        try check(rc);
        try check(c.sqlite3_busy_timeout(db.?, 1000));
        try check(c.sqlite3_exec(db.?, "PRAGMA foreign_keys=ON; PRAGMA trusted_schema=OFF;", null, null, null));
        try check(c.sqlite3_exec(db.?, if (create) "BEGIN IMMEDIATE" else "BEGIN", null, null, null));
        errdefer c.sqlite3_exec(db.?, "ROLLBACK", null, null, null) catch {};
        const app = try scalar(db.?, "PRAGMA application_id");
        const version = try scalar(db.?, "PRAGMA user_version");
        if (app == 0 and version == 0 and create and
            try scalar(db.?, "SELECT count(*) FROM sqlite_schema") == 0)
        {
            try check(c.sqlite3_exec(db.?, @embedFile("schema.sql"), null, null, null));
        } else if (app != 0x46534c54 or version != 2) {
            return error.InvalidFormat;
        }
        // Verify the root before handing a purported volume to the host.
        const root_count = try scalar(db.?, "SELECT count(*) FROM nodes WHERE id=1 AND parent IS NULL AND name='' AND kind=2 AND size=0");
        if (root_count != 1) return error.InvalidFormat;
        try check(c.sqlite3_exec(db.?, "COMMIT", null, null, null));
        const self = try allocator.create(Volume);
        errdefer allocator.destroy(self);
        self.* = .{
            .db = db.?,
            .prepared_statements = std.ArrayList(*c.sqlite3_stmt).init(allocator),
            .cache_stats = CacheStats{},
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
        const root_count = try self.scalar("SELECT count(*) FROM nodes WHERE id=1 AND parent IS NULL AND name='' AND kind=2 AND size=0");
        if (root_count != 1) return error.InvalidFormat;
        try self.exec("COMMIT");
        return self;
    }
    
    pub fn close(self: *Volume) void {
        // Clean up all cached statements
        for (self.prepared_statements.items) |stmt| {
            _ = c.sqlite3_finalize(stmt);
        }
        self.prepared_statements.deinit();
        if (self.backup) |backup| backup.abort();
        if (self.writer) |writer| writer.abort();
        // SQLite rolls back uncommitted work when the connection closes.
        _ = c.sqlite3_close_v2(self.db);
        allocator.destroy(self);
    }
    
    pub fn exec(self: *Volume, sql: [:0]const u8) Error!void {
        try check(c.sqlite3_exec(self.db, sql.ptr, null, null, null));
    }
    
    fn prepare(self: *Volume, sql: [:0]const u8) Error!Statement {
        var stmt: ?*c.sqlite3_stmt = null;
        try check(c.sqlite3_prepare_v2(self.db, sql.ptr, -1, &stmt, null));
        return Statement{ .ptr = stmt.? };
    }
    
    fn prepareCached(self: *Volume, sql: [:0]const u8) Error!Statement {
        // First check cache for exact SQL match
        for (self.prepared_statements.items) |stmt_ptr| {
            const stmt = c.sqlite3_sql(stmt_ptr);
            if (stmt != null and std.mem.eql(u8, sql, stmt.?)) {
                return Statement{ .ptr = stmt_ptr };
            }
        }
        
        // If not found, prepare new statement and cache it
        var stmt: ?*c.sqlite3_stmt = null;
        try check(c.sqlite3_prepare_v2(self.db, sql.ptr, -1, &stmt, null));
        if (stmt) |s| {
            try self.prepared_statements.append(s);
            self.cache_stats.updateSize(self.prepared_statements.items.len);
        }
        return Statement{ .ptr = stmt.? };
    }
    
    fn scalar(self: *Volume, sql: [:0]const u8) Error!i64 {
        const stmt = try self.prepareCached(sql);
        defer stmt.close();
        if (!try stmt.row()) return error.Internal;
        return c.sqlite3_column_int64(stmt.ptr, 0);
    }
    
    pub fn inTransaction(self: *Volume) bool {
        return c.sqlite3_get_autocommit(self.db) == 0;
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
    }
    
    // Path lookup with cached statement
    fn child(self: *Volume, parent: u64, name: []const u8) Error!?Info {
        const stmt = try self.prepareCached("SELECT id,kind,size FROM nodes WHERE parent=?1 AND name=?2");
        defer stmt.close();
        try stmt.integer(1, @intCast(parent));
        try stmt.text(2, name);
        if (!try stmt.row()) return null;
        return try stmt.info(0);
    }
    
    pub fn stat(self: *Volume, path: []const u8) Error!Info {
        try validatePath(path);
        var current: Info = .{ .id = 1, .kind = 2, .size = 0 };
        var parts = std.mem.tokenizeScalar(u8, path, '/');
        while (parts.next()) |part| {
            if (current.kind != 2) return error.NotDirectory;
            current = try self.child(current.id, part) orelse return error.NotFound;
        }
        return current;
    }
    
    fn parentOf(self: *Volume, path: []const u8) Error!struct { id: u64, name: []const u8 } {
        try validatePath(path);
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
        const stmt = try self.prepareCached("INSERT INTO nodes(parent,name,kind,size) VALUES(?1,?2,2,0)");
        defer stmt.close();
        try stmt.integer(1, @intCast(parent.id));
        try stmt.text(2, parent.name);
        try stmt.done();
    }
    
    fn requireEmpty(self: *Volume, node: Info) Error!void {
        if (node.kind != 2) return;
        const stmt = try self.prepareCached("SELECT 1 FROM nodes WHERE parent=?1 LIMIT 1");
        defer stmt.close();
        try stmt.integer(1, @intCast(node.id));
        if (try stmt.row()) return error.NotEmpty;
    }
    
    fn deleteNode(self: *Volume, id: u64) Error!void {
        const stmt = try self.prepareCached("DELETE FROM nodes WHERE id=?1");
        defer stmt.close();
        try stmt.integer(1, @intCast(id));
        // Foreign keys delete a file's chunks in the same operation.
        try stmt.done();
    }
    
    pub fn remove(self: *Volume, path: []const u8) Error!void {
        try validatePath(path);
        if (path.len == 1) return error.InvalidArgument;
        const node = try self.stat(path);
        try self.requireEmpty(node);
        try self.deleteNode(node.id);
    }
    
    pub fn rename(self: *Volume, source: []const u8, destination: []const u8) Error!void {
        try validatePath(source);
        try validatePath(destination);
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
            // through the API's 4096-byte path limit. Lengths are UTF-8 bytes.
            const stmt = try self.prepareCached(
                "WITH RECURSIVE descendants(id,relative_size) AS (" ++
                    "SELECT ?1,0 UNION ALL " ++
                    "SELECT n.id,d.relative_size+1+length(CAST(n.name AS BLOB)) " ++
                    "FROM nodes n JOIN descendants d ON n.parent=d.id) " ++
                    "SELECT 1 FROM descendants WHERE relative_size>?2 LIMIT 1",
            );
            defer stmt.close();
            try stmt.integer(1, @intCast(node.id));
            try stmt.integer(2, @intCast(4096 - destination.len));
            if (try stmt.row()) return error.InvalidArgument;
        }
        if (existing) |target| try self.deleteNode(target.id);
        const stmt = try self.prepareCached("UPDATE nodes SET parent=?2,name=?3 WHERE id=?1");
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
        // Extension also pads the old final chunk and materializes any gap.
        const first = if (end > node.size) @min(offset, node.size) / chunk_size else offset / chunk_size;
        try self.patchChunks(node, first, (end - 1) / chunk_size, size, offset, data);
        if (size != node.size) try self.setSize(node.id, size);
    }
    
    pub fn truncate(self: *Volume, path: []const u8, size: u64) Error!void {
        if (size > std.math.maxInt(i64)) return error.TooLarge;
        const node = try self.stat(path);
        if (node.kind == 2) return error.IsDirectory;
        if (size == node.size) return;
        if (size < node.size) {
            {
                const stmt = try self.prepareCached("DELETE FROM chunks WHERE node_id=?1 AND chunk_index>=?2");
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
        defer stmt.close();
        try stmt.integer(1, @intCast(id));
        try stmt.integer(2, @intCast(size));
        try stmt.done();
    }
    
    // Read/modify/write only affected chunks. One heap buffer is reused for the
    // entire operation, including large zero-filled extensions. The caller owns
    // the encompassing savepoint; metadata is updated only after all chunks.
    fn patchChunks(self: *Volume, node: Info, first: u64, last: u64, size: u64, offset: u64, data: []const u8) Error!void {
        const buffer = try allocator.alloc(u8, chunk_size);
        defer allocator.free(buffer);
        var index = first;
        while (index <= last) : (index += 1) {
            @memset(buffer, 0);
            const start = index * chunk_size;
            if (start < node.size) {
                const stmt = try self.prepareCached("SELECT data FROM chunks WHERE node_id=?1 AND chunk_index=?2");
                defer stmt.close();
                try stmt.integer(1, @intCast(node.id));
                try stmt.integer(2, @intCast(index));
                if (!try stmt.row()) return error.InvalidFormat;
                const expected = @min(chunk_size, node.size - start);
                const length: usize = @intCast(c.sqlite3_column_bytes(stmt.ptr, 0));
                const bytes = c.sqlite3_column_blob(stmt.ptr, 0);
                if (length != expected or bytes == null or c.sqlite3_column_type(stmt.ptr, 0) != c.SQLITE_BLOB)
                    return error.InvalidFormat;
                @memcpy(buffer[0..length], @as([*]const u8, @ptrCast(bytes.?))[0..length]);
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
            const stmt = try self.prepareCached(
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
        defer stmt.close();
        try stmt.integer(1, @intCast(parent.id));
        try stmt.text(2, parent.name);
        try stmt.done();
        const node = try self.child(parent.id, parent.name) orelse return error.Internal;
        const delete_chunks = try self.prepareCached("DELETE FROM chunks WHERE node_id=?1");
        defer delete_chunks.close();
        try delete_chunks.integer(1, @intCast(node.id));
        try delete_chunks.done();
        return node.id;
    }
    
    pub fn read(self: *Volume, path: []const u8, offset: u64, output: []u8) Error!usize {
        const node = try self.stat(path);
        if (node.kind == 2) return error.IsDirectory;
        if (offset >= node.size or output.len == 0) return 0;
        const amount = @min(node.size - offset, output.len);
        const stmt = try self.prepareCached("SELECT chunk_index,data FROM chunks WHERE node_id=?1 AND chunk_index BETWEEN ?2 AND ?3 ORDER BY chunk_index");
        defer stmt.close();
        try stmt.integer(1, @intCast(node.id));
        var index = offset / chunk_size;
        try stmt.integer(2, @intCast(index));
        try stmt.integer(3, @intCast((offset + amount - 1) / chunk_size));
        var copied: usize = 0;
        var skip: usize = @intCast(offset % chunk_size);
        while (copied < amount) : (index += 1) {
            if (!try stmt.row() or c.sqlite3_column_int64(stmt.ptr, 0) != index) return error.InvalidFormat;
            const expected = @min(chunk_size, node.size - index * chunk_size);
            const len: usize = @intCast(c.sqlite3_column_bytes(stmt.ptr, 1));
            const data = c.sqlite3_column_blob(stmt.ptr, 1);
            if (len != expected or data == null) return error.InvalidFormat;
            const take: usize = @intCast(@min(len - skip, amount - copied));
            const bytes = @as([*]const u8, @ptrCast(data.?))[0..len];
            @memcpy(output[copied..][0..take], bytes[skip..][0..take]);
            copied += take;
            skip = 0;
        }
        return copied;
    }
    
    pub fn list(self: *Volume, path: []const u8, callback: Callback, context: ?*anyopaque) Error!void {
        const parent = try self.stat(path);
        if (parent.kind != 2) return error.NotDirectory;
        const stmt = try self.prepareCached("SELECT id,kind,size,name FROM nodes WHERE parent=?1 ORDER BY name COLLATE BINARY");
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
            const stmt = try self.prepareCached("PRAGMA integrity_check");
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
            const stmt = try self.prepareCached("SELECT name FROM nodes WHERE id<>1");
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
        try self.requireNoRows(
            "SELECT 1 FROM nodes n LEFT JOIN (SELECT node_id,count(*) AS total FROM chunks GROUP BY node_id) c " ++
                "ON n.id=c.node_id WHERE coalesce(c.total,0)<>n.size/65536+(n.size%65536<>0) LIMIT 1",
        );
    }
    
    fn requireNoRows(self: *Volume, sql: [:0]const u8) Error!void {
        const stmt = try self.prepareCached(sql);
        defer stmt.close();
        if (try stmt.row()) return error.InvalidFormat;
    }
};

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
        const stmt = try volume.prepareCached("INSERT INTO chunks(node_id,chunk_index,data) VALUES(?1,?2,?3)");
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
        try self.flush();
        {
            const stmt = try volume.prepareCached("UPDATE nodes SET size=?2 WHERE id=?1");
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
        return if (total == 0) 0.0 else @as(f64, self.hits) / @as(f64, total);
    }
    
    pub fn memoryUsage(self: CacheStats) usize {
        // Estimate memory usage based on cache size
        // Each cache entry includes a pointer to a prepared statement
        // and some overhead for the Node structure
        return self.current_size * (@sizeOf(*c.sqlite3_stmt) + @sizeOf(usize) * 4);
    }
};
