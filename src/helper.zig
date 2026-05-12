const std = @import("std");
const VncAuth = @import("rfb/protocol.zig").VncAuth;

const log = std.log.scoped(.helper);

/// Escape a string for embedding in a JSON string value.
pub fn jsonEscape(allocator: std.mem.Allocator, input: []const u8) ![]u8 {
    var result = std.ArrayList(u8){};
    errdefer result.deinit(allocator);

    for (input) |ch| {
        switch (ch) {
            '"' => try result.appendSlice(allocator, "\\\""),
            '\\' => try result.appendSlice(allocator, "\\\\"),
            '\n' => try result.appendSlice(allocator, "\\n"),
            '\r' => try result.appendSlice(allocator, "\\r"),
            '\t' => try result.appendSlice(allocator, "\\t"),
            else => {
                if (ch < 0x20) {
                    // Control character — encode as \u00XX
                    const hex = try std.fmt.allocPrint(allocator, "\\u{x:0>4}", .{ch});
                    defer allocator.free(hex);
                    try result.appendSlice(allocator, hex);
                } else {
                    try result.append(allocator, ch);
                }
            },
        }
    }

    const owned = try allocator.dupe(u8, result.items);
    result.deinit(allocator);
    return owned;
}

/// Perform VNC DES challenge-response authentication with the helper.
/// The helper sends a 16-byte challenge, we encrypt it with the VNC password
/// and send back the 16-byte response, then read a 4-byte result.
fn authenticate(stream: std.net.Stream, password: ?[]const u8) !void {
    // Read 16-byte challenge
    var challenge: [16]u8 = undefined;
    var total: usize = 0;
    while (total < 16) {
        const n = stream.read(challenge[total..]) catch return error.ConnectionFailed;
        if (n == 0) return error.ConnectionFailed;
        total += n;
    }

    // Encrypt with VNC DES
    const pw = password orelse "";
    const response = VncAuth.encrypt(&challenge, pw);
    stream.writeAll(&response) catch return error.ConnectionFailed;

    // Read 4-byte security result (big-endian u32: 0=OK, 1=failed)
    var result_buf: [4]u8 = undefined;
    total = 0;
    while (total < 4) {
        const n = stream.read(result_buf[total..]) catch return error.ConnectionFailed;
        if (n == 0) return error.ConnectionFailed;
        total += n;
    }

    const result = std.mem.readInt(u32, &result_buf, .big);
    if (result != 0) {
        log.warn("helper auth failed (wrong password)", .{});
        return error.AuthFailed;
    }
}

/// Persistent helper connection with auto-reconnect.
/// Thread-safe: uses a mutex to serialize requests on the shared TCP stream.
pub const HelperConnection = struct {
    allocator: std.mem.Allocator,
    host: []const u8,
    port: u16,
    password: ?[]const u8,
    stream: ?std.net.Stream = null,
    mutex: std.Thread.Mutex = .{},

    pub fn init(allocator: std.mem.Allocator, host: []const u8, port: u16, password: ?[]const u8) HelperConnection {
        return .{
            .allocator = allocator,
            .host = host,
            .port = port,
            .password = password,
        };
    }

    /// Ensure we have a live TCP connection. Connect + auth if needed.
    fn ensureConnected(self: *HelperConnection) !std.net.Stream {
        if (self.stream) |s| return s;

        const stream = std.net.tcpConnectToHost(self.allocator, self.host, self.port) catch |err| {
            log.warn("helper connection to {s}:{d} failed: {}", .{ self.host, self.port, err });
            return error.ConnectionFailed;
        };

        if (self.password != null and self.password.?.len > 0) {
            authenticate(stream, self.password) catch |err| {
                stream.close();
                log.warn("helper auth handshake failed: {}", .{err});
                return error.AuthFailed;
            };
        }

        log.info("helper connected to {s}:{d}", .{ self.host, self.port });
        self.stream = stream;
        return stream;
    }

    pub fn disconnect(self: *HelperConnection) void {
        if (self.stream) |s| {
            s.close();
            self.stream = null;
        }
    }

    /// Read one newline-delimited response from the stream.
    fn readResponse(self: *HelperConnection, stream: std.net.Stream) ![]u8 {
        var response = std.ArrayList(u8){};
        errdefer response.deinit(self.allocator);

        var buf: [8192]u8 = undefined;
        while (true) {
            const n = stream.read(&buf) catch {
                self.disconnect();
                response.deinit(self.allocator);
                return error.ConnectionFailed;
            };
            if (n == 0) {
                self.disconnect();
                if (response.items.len > 0) break;
                response.deinit(self.allocator);
                return error.ConnectionFailed;
            }

            // Check for newline delimiter
            for (buf[0..n], 0..) |ch, i| {
                if (ch == '\n') {
                    try response.appendSlice(self.allocator, buf[0..i]);
                    const owned = try self.allocator.dupe(u8, response.items);
                    response.deinit(self.allocator);
                    return owned;
                }
            }
            try response.appendSlice(self.allocator, buf[0..n]);

            // Safety limit: 4MB
            if (response.items.len > 4 * 1024 * 1024) break;
        }

        if (response.items.len > 0) {
            const owned = try self.allocator.dupe(u8, response.items);
            response.deinit(self.allocator);
            return owned;
        }

        response.deinit(self.allocator);
        return error.ConnectionFailed;
    }

    /// Send a JSON request and return the response. Thread-safe.
    /// On connection failure, retries once with a fresh connection.
    pub fn call(self: *HelperConnection, request_json: []const u8) ![]u8 {
        self.mutex.lock();
        defer self.mutex.unlock();

        // First attempt: use existing or new connection
        if (self.sendAndReceive(request_json)) |resp| return resp else |_| {}

        // Retry once with fresh connection
        self.disconnect();
        return self.sendAndReceive(request_json);
    }

    fn sendAndReceive(self: *HelperConnection, request_json: []const u8) ![]u8 {
        const stream = try self.ensureConnected();

        stream.writeAll(request_json) catch {
            self.disconnect();
            return error.ConnectionFailed;
        };
        stream.writeAll("\n") catch {
            self.disconnect();
            return error.ConnectionFailed;
        };

        return self.readResponse(stream);
    }
};

/// Legacy connect-per-request call (convenience wrapper).
/// Creates a temporary connection, sends one request, returns the response.
pub fn call(allocator: std.mem.Allocator, host: []const u8, port: u16, password: ?[]const u8, request_json: []const u8) ![]u8 {
    var conn = HelperConnection.init(allocator, host, port, password);
    defer conn.disconnect();
    return conn.call(request_json);
}
