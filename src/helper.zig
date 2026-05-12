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

/// Send a JSON command to the helper agent and return the raw response.
/// Performs VNC DES challenge-response authentication if password is provided.
/// Returns the response as an owned string slice, or error on failure.
pub fn call(allocator: std.mem.Allocator, host: []const u8, port: u16, password: ?[]const u8, request_json: []const u8) ![]u8 {
    const stream = std.net.tcpConnectToHost(allocator, host, port) catch |err| {
        log.warn("helper connection to {s}:{d} failed: {}", .{ host, port, err });
        return error.ConnectionFailed;
    };
    defer stream.close();

    // Authenticate if the helper sends a challenge (16 bytes before JSON)
    // We attempt to read — if the helper has auth enabled, it sends a challenge.
    // If auth is disabled, the helper goes straight to waiting for our JSON request.
    //
    // Protocol: if password is available, always attempt auth handshake.
    // The helper will either send a challenge (auth enabled) or wait for JSON
    // (auth disabled). Since we always try auth when we have a password,
    // both sides must agree: either both have auth or neither does.
    if (password != null and password.?.len > 0) {
        authenticate(stream, password) catch |err| {
            log.warn("helper auth handshake failed: {}", .{err});
            return error.AuthFailed;
        };
    }

    // Send request + newline
    stream.writeAll(request_json) catch return error.ConnectionFailed;
    stream.writeAll("\n") catch return error.ConnectionFailed;

    // Read response up to newline or EOF
    var response = std.ArrayList(u8){};
    errdefer response.deinit(allocator);

    var buf: [8192]u8 = undefined;
    while (true) {
        const n = stream.read(&buf) catch break;
        if (n == 0) break;

        // Check for newline delimiter
        for (buf[0..n], 0..) |ch, i| {
            if (ch == '\n') {
                try response.appendSlice(allocator, buf[0..i]);
                const owned = try allocator.dupe(u8, response.items);
                response.deinit(allocator);
                return owned;
            }
        }
        try response.appendSlice(allocator, buf[0..n]);

        // Safety limit: 4MB
        if (response.items.len > 4 * 1024 * 1024) break;
    }

    if (response.items.len > 0) {
        const owned = try allocator.dupe(u8, response.items);
        response.deinit(allocator);
        return owned;
    }

    response.deinit(allocator);
    return error.ConnectionFailed;
}
