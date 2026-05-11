const std = @import("std");

const log = std.log.scoped(.mcp);

pub const JsonValue = std.json.Value;

pub const Request = struct {
    jsonrpc: []const u8,
    id: ?JsonValue = null,
    method: []const u8,
    params: ?JsonValue = null,
};

fn jsonEscape(allocator: std.mem.Allocator, input: []const u8) ![]u8 {
    // Count needed size
    var size: usize = 0;
    for (input) |ch| {
        size += switch (ch) {
            '"', '\\' => 2,
            '\n', '\r', '\t' => 2,
            else => 1,
        };
    }
    const result = try allocator.alloc(u8, size);
    var i: usize = 0;
    for (input) |ch| {
        switch (ch) {
            '"' => {
                result[i] = '\\';
                result[i + 1] = '"';
                i += 2;
            },
            '\\' => {
                result[i] = '\\';
                result[i + 1] = '\\';
                i += 2;
            },
            '\n' => {
                result[i] = '\\';
                result[i + 1] = 'n';
                i += 2;
            },
            '\r' => {
                result[i] = '\\';
                result[i + 1] = 'r';
                i += 2;
            },
            '\t' => {
                result[i] = '\\';
                result[i + 1] = 't';
                i += 2;
            },
            else => {
                result[i] = ch;
                i += 1;
            },
        }
    }
    return result;
}

pub const McpServer = struct {
    allocator: std.mem.Allocator,
    stdin: std.fs.File,
    stdout: std.fs.File,
    initialized: bool = false,
    tool_handler: *const fn (std.mem.Allocator, []const u8, ?JsonValue) anyerror!JsonValue,

    pub fn init(
        allocator: std.mem.Allocator,
        stdin: std.fs.File,
        stdout: std.fs.File,
        tool_handler: *const fn (std.mem.Allocator, []const u8, ?JsonValue) anyerror!JsonValue,
    ) McpServer {
        return McpServer{
            .allocator = allocator,
            .stdin = stdin,
            .stdout = stdout,
            .tool_handler = tool_handler,
        };
    }

    pub fn run(self: *McpServer) !void {
        var line_buf = std.ArrayList(u8){};
        defer line_buf.deinit(self.allocator);

        // Leftover data from previous reads that came after a newline
        var leftover = std.ArrayList(u8){};
        defer leftover.deinit(self.allocator);

        var read_buf: [8192]u8 = undefined;

        while (true) {
            line_buf.clearRetainingCapacity();

            // First, try to extract a complete line from leftover
            var got_line = false;
            if (leftover.items.len > 0) {
                for (leftover.items, 0..) |ch, i| {
                    if (ch == '\n') {
                        line_buf.appendSlice(self.allocator, leftover.items[0..i]) catch return;
                        // Shift remaining leftover
                        const rest_start = i + 1;
                        const rest_len = leftover.items.len - rest_start;
                        if (rest_len > 0) {
                            std.mem.copyForwards(u8, leftover.items[0..rest_len], leftover.items[rest_start..]);
                        }
                        leftover.items = leftover.items[0..rest_len];
                        got_line = true;
                        break;
                    }
                }
                if (!got_line) {
                    // No newline in leftover — move it all to line_buf as partial
                    line_buf.appendSlice(self.allocator, leftover.items) catch return;
                    leftover.items = leftover.items[0..0];
                }
            }

            // If we didn't get a complete line from leftover, read from stdin
            if (!got_line) {
                while (true) {
                    const n = self.stdin.read(&read_buf) catch return;
                    if (n == 0) return; // EOF

                    var found_newline = false;
                    for (read_buf[0..n], 0..) |ch, i| {
                        if (ch == '\n') {
                            line_buf.appendSlice(self.allocator, read_buf[0..i]) catch return;
                            // Save remaining data as leftover
                            if (i + 1 < n) {
                                leftover.appendSlice(self.allocator, read_buf[i + 1 .. n]) catch return;
                            }
                            found_newline = true;
                            break;
                        }
                    }
                    if (found_newline) break;
                    line_buf.appendSlice(self.allocator, read_buf[0..n]) catch return;
                }
            }

            const line = line_buf.items;
            if (line.len == 0) continue;

            // Parse JSON-RPC request
            const parsed = std.json.parseFromSlice(std.json.Value, self.allocator, line, .{
                .ignore_unknown_fields = true,
            }) catch |err| {
                log.err("JSON parse error: {}", .{err});
                try self.sendError(null, -32700, "Parse error");
                continue;
            };
            defer parsed.deinit();

            const obj = parsed.value.object;

            const method = if (obj.get("method")) |m| m.string else {
                try self.sendError(null, -32600, "Missing method");
                continue;
            };

            const id = obj.get("id");
            const params = obj.get("params");

            // Route method
            if (std.mem.eql(u8, method, "initialize")) {
                try self.handleInitialize(id);
            } else if (std.mem.eql(u8, method, "initialized")) {
                self.initialized = true;
                // Notification — no response needed
            } else if (std.mem.eql(u8, method, "ping")) {
                try self.sendResult(id, .{ .object = blk: {
                    const m = std.json.ObjectMap.init(self.allocator);
                    break :blk m;
                } });
            } else if (std.mem.eql(u8, method, "tools/list")) {
                try self.handleToolsList(id);
            } else if (std.mem.eql(u8, method, "tools/call")) {
                try self.handleToolsCall(id, params);
            } else if (std.mem.eql(u8, method, "notifications/cancelled")) {
                // Ignore cancellation notifications
            } else {
                // Unknown method — check if it's a notification (no id)
                if (id != null) {
                    try self.sendError(id, -32601, "Method not found");
                }
            }
        }
    }

    fn handleInitialize(self: *McpServer, id: ?JsonValue) !void {
        const response = try std.fmt.allocPrint(self.allocator,
            \\{{"jsonrpc":"2.0","id":{s},"result":{{"protocolVersion":"2024-11-05","capabilities":{{"tools":{{}}}},"serverInfo":{{"name":"vnc-mcp-server","version":"0.1.0"}},"instructions":"VNC remote desktop control server. Use vnc_list_endpoints to see available machines. Use vnc_screenshot to capture the screen. All tools accept an optional 'endpoint' parameter to target a specific machine."}}}}
        , .{try self.formatId(id)});
        defer self.allocator.free(response);

        try self.writeLine(response);
    }

    fn handleToolsList(self: *McpServer, id: ?JsonValue) !void {
        const tools_json = @embedFile("tools_schema.json");

        const id_str = try self.formatId(id);
        defer self.allocator.free(id_str);

        const response = try std.fmt.allocPrint(self.allocator,
            \\{{"jsonrpc":"2.0","id":{s},"result":{{"tools":{s}}}}}
        , .{ id_str, tools_json });
        defer self.allocator.free(response);

        try self.writeLine(response);
    }

    fn handleToolsCall(self: *McpServer, id: ?JsonValue, params: ?JsonValue) !void {
        const p = params orelse {
            try self.sendError(id, -32602, "Missing params");
            return;
        };

        const obj = if (p == .object) p.object else {
            try self.sendError(id, -32602, "Params must be an object");
            return;
        };

        const name = if (obj.get("name")) |n| (if (n == .string) n.string else {
            try self.sendError(id, -32602, "Tool name must be a string");
            return;
        }) else {
            try self.sendError(id, -32602, "Missing tool name");
            return;
        };

        const arguments = obj.get("arguments");

        const result = self.tool_handler(self.allocator, name, arguments) catch |err| {
            const err_msg = try std.fmt.allocPrint(self.allocator, "Tool error: {}", .{err});
            defer self.allocator.free(err_msg);

            try self.sendToolError(id, err_msg);
            return;
        };

        try self.sendToolResult(id, result);
    }

    fn sendToolResult(self: *McpServer, id: ?JsonValue, content: JsonValue) !void {
        const id_str = try self.formatId(id);
        defer self.allocator.free(id_str);

        // Serialize the content value to JSON
        const content_json = try std.fmt.allocPrint(self.allocator, "{f}", .{std.json.fmt(content, .{})});
        defer self.allocator.free(content_json);

        const response = try std.fmt.allocPrint(self.allocator,
            \\{{"jsonrpc":"2.0","id":{s},"result":{s}}}
        , .{ id_str, content_json });
        defer self.allocator.free(response);

        try self.writeLine(response);
    }

    fn sendToolError(self: *McpServer, id: ?JsonValue, message: []const u8) !void {
        const id_str = try self.formatId(id);
        defer self.allocator.free(id_str);

        // Escape message for JSON string
        const escaped = try jsonEscape(self.allocator, message);
        defer self.allocator.free(escaped);

        const response = try std.fmt.allocPrint(self.allocator,
            \\{{"jsonrpc":"2.0","id":{s},"result":{{"content":[{{"type":"text","text":"{s}"}}],"isError":true}}}}
        , .{ id_str, escaped });
        defer self.allocator.free(response);

        try self.writeLine(response);
    }

    fn sendError(self: *McpServer, id: ?JsonValue, code: i32, message: []const u8) !void {
        const id_str = try self.formatId(id);
        defer self.allocator.free(id_str);

        const response = try std.fmt.allocPrint(self.allocator,
            \\{{"jsonrpc":"2.0","id":{s},"error":{{"code":{d},"message":"{s}"}}}}
        , .{ id_str, code, message });
        defer self.allocator.free(response);

        try self.writeLine(response);
    }

    pub fn sendResult(self: *McpServer, id: ?JsonValue, result: JsonValue) !void {
        const id_str = try self.formatId(id);
        defer self.allocator.free(id_str);

        const result_json = try std.fmt.allocPrint(self.allocator, "{f}", .{std.json.fmt(result, .{})});
        defer self.allocator.free(result_json);

        const response = try std.fmt.allocPrint(self.allocator,
            \\{{"jsonrpc":"2.0","id":{s},"result":{s}}}
        , .{ id_str, result_json });
        defer self.allocator.free(response);

        try self.writeLine(response);
    }

    fn formatId(self: *McpServer, id: ?JsonValue) ![]u8 {
        if (id) |val| {
            switch (val) {
                .integer => |i| return try std.fmt.allocPrint(self.allocator, "{d}", .{i}),
                .string => |s| return try std.fmt.allocPrint(self.allocator, "\"{s}\"", .{s}),
                .null => return try self.allocator.dupe(u8, "null"),
                else => return try self.allocator.dupe(u8, "null"),
            }
        }
        return try self.allocator.dupe(u8, "null");
    }

    fn writeLine(self: *McpServer, line: []const u8) !void {
        self.stdout.writeAll(line) catch return error.BrokenPipe;
        self.stdout.writeAll("\n") catch return error.BrokenPipe;
    }
};
