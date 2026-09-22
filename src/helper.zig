const std = @import("std");
const VncAuth = @import("rfb/protocol.zig").VncAuth;

const log = std.log.scoped(.helper);

// EV_EOF is defined in FreeBSD sys/event.h but missing from Zig's std.c.EV
const EV_EOF: u16 = 0x8000;

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

    /// The password is duplicated — the connection owns its copy so that
    /// re-authentication after an idle drop never touches caller memory
    /// (callers free their copy when the tool call returns).
    pub fn init(allocator: std.mem.Allocator, host: []const u8, port: u16, password: ?[]const u8) !HelperConnection {
        return .{
            .allocator = allocator,
            .host = host,
            .port = port,
            .password = if (password) |pw| try allocator.dupe(u8, pw) else null,
        };
    }

    /// Close the stream and free the owned password copy.
    pub fn deinit(self: *HelperConnection) void {
        self.disconnect();
        if (self.password) |pw| {
            self.allocator.free(pw);
            self.password = null;
        }
    }

    /// Check if a socket is still alive using kqueue (event-driven, instant).
    /// Returns false if the remote end has closed (EV_EOF) or an error occurred.
    fn isSocketAlive(fd: std.posix.fd_t) bool {
        const kq = std.posix.kqueue() catch return false;
        defer std.posix.close(kq);

        var changelist = [_]std.posix.Kevent{.{
            .ident = @intCast(fd),
            .filter = std.c.EVFILT.READ,
            .flags = std.c.EV.ADD | std.c.EV.ONESHOT,
            .fflags = 0,
            .data = 0,
            .udata = 0,
        }};
        var eventlist: [1]std.posix.Kevent = undefined;

        const timeout = std.posix.timespec{ .sec = 0, .nsec = 0 };
        const n = std.posix.kevent(kq, &changelist, &eventlist, &timeout) catch return false;

        if (n == 0) return true; // No events — socket is idle and alive

        if (eventlist[0].flags & EV_EOF != 0) return false;
        if (eventlist[0].flags & std.c.EV.ERROR != 0) return false;
        // Unexpected data on an idle helper socket means remote closed or protocol desync
        if (eventlist[0].data > 0) return false;

        return true;
    }

    /// TCP connect with a bounded timeout (non-blocking + kqueue).
    /// A blocked helper host (agent down, firewall drop) otherwise hangs
    /// connect() for the kernel's ~75s SYN retransmit stack — wedging the
    /// MCP server and the IDE's session with it.
    fn connectWithTimeout(allocator: std.mem.Allocator, host: []const u8, port: u16, timeout_ms: u64) !std.net.Stream {
        const list = try std.net.getAddressList(allocator, host, port);
        defer list.deinit();
        if (list.addrs.len == 0) return error.UnknownHostName;
        const addr = list.addrs[0];

        const sock = try std.posix.socket(addr.any.family, std.posix.SOCK.STREAM | std.posix.SOCK.NONBLOCK, std.posix.IPPROTO.TCP);
        errdefer std.posix.close(sock);

        std.posix.connect(sock, &addr.any, addr.getOsSockLen()) catch |err| switch (err) {
            error.WouldBlock => {}, // in progress — wait below
            else => return error.ConnectionFailed,
        };

        const kq = std.posix.kqueue() catch return error.ConnectionFailed;
        defer std.posix.close(kq);

        var changelist = [_]std.posix.Kevent{.{
            .ident = @intCast(sock),
            .filter = std.c.EVFILT.WRITE,
            .flags = std.c.EV.ADD | std.c.EV.ONESHOT,
            .fflags = 0,
            .data = 0,
            .udata = 0,
        }};
        var eventlist: [1]std.posix.Kevent = undefined;
        const ts = std.posix.timespec{
            .sec = @intCast(timeout_ms / 1000),
            .nsec = @intCast((timeout_ms % 1000) * 1_000_000),
        };
        const n = std.posix.kevent(kq, &changelist, &eventlist, &ts) catch return error.ConnectionFailed;
        if (n == 0) return error.ConnectTimeout;

        std.posix.getsockoptError(sock) catch |err| {
            return switch (err) {
                error.ConnectionRefused => error.ConnectionRefused,
                else => error.ConnectionFailed,
            };
        };

        // Connected — restore blocking mode for the read/write loop.
        const flags = try std.posix.fcntl(sock, std.posix.F.GETFL, 0);
        const nonblock: u32 = @bitCast(std.posix.O{ .NONBLOCK = true });
        _ = try std.posix.fcntl(sock, std.posix.F.SETFL, flags & ~@as(@TypeOf(flags), nonblock));

        return std.net.Stream{ .handle = sock };
    }

    /// Ensure we have a live TCP connection. Connect + auth if needed.
    fn ensureConnected(self: *HelperConnection) !std.net.Stream {
        if (self.stream) |s| {
            if (isSocketAlive(s.handle)) return s;
            log.info("stale helper socket detected, reconnecting", .{});
            self.disconnect();
        }

        const stream = connectWithTimeout(self.allocator, self.host, self.port, 5000) catch |err| {
            log.warn("helper connection to {s}:{d} failed: {}", .{ self.host, self.port, err });
            return error.ConnectionFailed;
        };

        // Set a default read timeout — callers can override via setReadTimeout()
        const timeout = std.posix.timeval{ .sec = 30, .usec = 0 };
        std.posix.setsockopt(stream.handle, std.posix.SOL.SOCKET, std.posix.SO.RCVTIMEO, std.mem.asBytes(&timeout)) catch {};

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

    /// Adjust SO_RCVTIMEO on the live socket. Call after ensureConnected().
    fn setReadTimeout(self: *HelperConnection, seconds: u32) void {
        if (self.stream) |s| {
            const tv = std.posix.timeval{ .sec = @intCast(seconds), .usec = 0 };
            std.posix.setsockopt(s.handle, std.posix.SOL.SOCKET, std.posix.SO.RCVTIMEO, std.mem.asBytes(&tv)) catch {};
        }
    }

    pub fn disconnect(self: *HelperConnection) void {
        if (self.stream) |s| {
            s.close();
            self.stream = null;
        }
    }

    /// Shutdown the read side of the socket without closing it.
    /// This unblocks any thread blocked in recv() — it will see EOF (read returns 0).
    /// The socket is NOT closed here; the worker thread's error path will call disconnect().
    pub fn shutdown(self: *HelperConnection) void {
        if (self.stream) |s| {
            std.posix.shutdown(s.handle, .recv) catch {};
        }
    }

    /// Read one newline-delimited response from the stream.
    /// Returns error.ReadTimeout if SO_RCVTIMEO fires (EAGAIN/WouldBlock).
    /// Returns error.ConnectionFailed for genuine connection loss.
    fn readResponse(self: *HelperConnection, stream: std.net.Stream) ![]u8 {
        var response = std.ArrayList(u8){};
        errdefer response.deinit(self.allocator);

        var buf: [8192]u8 = undefined;
        while (true) {
            const n = stream.read(&buf) catch |err| {
                self.disconnect();
                response.deinit(self.allocator);
                // Distinguish SO_RCVTIMEO timeout from real connection loss
                if (err == error.WouldBlock) return error.ReadTimeout;
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
    /// On read timeout (SO_RCVTIMEO), does NOT retry — the helper is busy.
    pub fn call(self: *HelperConnection, request_json: []const u8) ![]u8 {
        return self.callWithTimeout(request_json, 0);
    }

    /// Like call(), but temporarily sets SO_RCVTIMEO to `timeout_secs` before reading.
    /// Pass 0 to use the default (30s set at connection time).
    pub fn callWithTimeout(self: *HelperConnection, request_json: []const u8, timeout_secs: u32) ![]u8 {
        self.mutex.lock();
        defer self.mutex.unlock();

        // First attempt: use existing or new connection
        const first_result = self.sendAndReceive(request_json, timeout_secs);
        if (first_result) |resp| return resp else |err| {
            // Never retry on read timeout — the helper is busy processing
            // the command we just sent. Reconnecting would be counterproductive.
            if (err == error.ReadTimeout) {
                log.warn("helper read timeout ({}s) — not retrying", .{if (timeout_secs > 0) timeout_secs else @as(u32, 30)});
                return error.ReadTimeout;
            }
        }

        // Retry once with fresh connection (genuine connection loss only)
        self.disconnect();
        return self.sendAndReceive(request_json, timeout_secs);
    }

    fn sendAndReceive(self: *HelperConnection, request_json: []const u8, timeout_secs: u32) ![]u8 {
        const stream = try self.ensureConnected();

        // Override SO_RCVTIMEO if caller specified a custom timeout
        if (timeout_secs > 0) {
            self.setReadTimeout(timeout_secs);
        }

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
    var conn = try HelperConnection.init(allocator, host, port, password);
    defer conn.deinit();
    return conn.call(request_json);
}

/// Legacy connect-per-request call with custom timeout.
pub fn callWithTimeout(allocator: std.mem.Allocator, host: []const u8, port: u16, password: ?[]const u8, request_json: []const u8, timeout_secs: u32) ![]u8 {
    var conn = try HelperConnection.init(allocator, host, port, password);
    defer conn.deinit();
    return conn.callWithTimeout(request_json, timeout_secs);
}
