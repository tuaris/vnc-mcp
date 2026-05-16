const std = @import("std");
const tools_mod = @import("tools.zig");

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
            } else if (std.mem.eql(u8, method, "resources/list")) {
                try self.handleResourcesList(id);
            } else if (std.mem.eql(u8, method, "resources/read")) {
                try self.handleResourcesRead(id, params);
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
        const id_str = try self.formatId(id);
        defer self.allocator.free(id_str);

        const instructions =
            "VNC remote desktop control server. Use vnc_list_endpoints to see available machines. " ++
            "All tools accept an optional 'endpoint' parameter to target a specific machine.\n\n" ++
            "CRITICAL — Screenshot Coordinates:\n" ++
            "Screenshots include 'Resolution: WxH pixels' metadata (e.g., 1918x968). Your IDE displays them SCALED DOWN (~500px wide). " ++
            "You MUST compute coordinates using the ACTUAL resolution, NOT the scaled visual. " ++
            "If resolution is 1918x968 and a target appears at visual center, its real coordinate is (959, 484), NOT (250, 125).\n\n" ++
            "IMPORTANT — Interaction Strategy:\n" ++
            "1. Prefer keyboard navigation (Alt+Tab, Win+R, Tab, Enter, F6, Alt+D, Escape) over clicks — keys are always reliable.\n" ++
            "2. Before clicking, verify which window has focus via vnc_active_window.\n" ++
            "3. NEVER estimate pixel coordinates from the scaled screenshot image. Use these methods instead:\n" ++
            "   (a) vnc_grid — overlays a labeled coordinate grid. Best for toolbars, ribbons, and dense UI. Use columns=12, rows=8 for fine targets.\n" ++
            "   (b) vnc_active_window/vnc_window_list — get window position, add known UI offsets (title bar ~32px, menu ~22px).\n" ++
            "   (c) vnc_probe — place a marker to verify coordinates visually before committing to a click.\n" ++
            "4. Use vnc_ocr_region to verify text content at coordinates before acting on assumptions.\n" ++
            "5. For window switching, use Alt+Tab or vnc_set_active_window — do NOT click taskbar buttons by guessing.\n" ++
            "6. When adjusting after a miss, change ONLY ONE axis at a time.\n" ++
            "7. Use vnc_probe BEFORE clicking uncertain targets. If the marker is NOT on the intended target, adjust and re-probe. Never ignore probe results.\n" ++
            "8. Use vnc_grid for toolbars and ribbons — these dense areas have many small targets that are impossible to hit by coordinate estimation. " ++
            "One grid call replaces multiple probe attempts.\n" ++
            "9. vnc_ui_click_element is UNRELIABLE for disambiguation — partial name matching may activate the WRONG element. " ++
            "It returns empty data on success with no confirmation of what was clicked. Always verify with a screenshot after using it. " ++
            "Prefer keyboard shortcuts or grid-based clicking for critical interactions.\n" ++
            "10. Helper tools (vnc_run_command, vnc_window_list, vnc_active_window, vnc_screen_info, vnc_ocr_region, vnc_list_processes, vnc_list_services, vnc_registry_read) provide authoritative system state — prefer them over visual guessing.";

        const escaped_instructions = try jsonEscape(self.allocator, instructions);
        defer self.allocator.free(escaped_instructions);

        const response = try std.fmt.allocPrint(self.allocator, "{{\"jsonrpc\":\"2.0\",\"id\":{s},\"result\":{{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{{\"tools\":{{}}," ++
            "\"resources\":{{}}}},\"serverInfo\":{{\"name\":\"vnc-mcp-server\",\"version\":\"0.5.0\"}},\"instructions\":\"{s}\"}}}}", .{ id_str, escaped_instructions });
        defer self.allocator.free(response);

        try self.writeLine(response);
    }

    fn handleToolsList(self: *McpServer, id: ?JsonValue) !void {
        const tools_json = @embedFile("tools_schema.json");

        const id_str = try self.formatId(id);
        defer self.allocator.free(id_str);

        const response = try std.fmt.allocPrint(self.allocator, "{{\"jsonrpc\":\"2.0\",\"id\":{s},\"result\":{{\"tools\":{s}}}}}", .{ id_str, tools_json });
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

        // Execute tool with timeout to prevent indefinite IDE hangs (R6).
        // Spawn tool on a worker thread; main thread polls completion with
        // a 45-second deadline. If the worker hasn't finished, shutdown the
        // helper socket to unblock any pending recv(), then join the thread.
        const timeout_ns: u64 = 45 * std.time.ns_per_s;
        const ToolCtx = struct {
            value: ?JsonValue = null,
            err_msg: ?[]const u8 = null,
            done: std.atomic.Value(bool) = std.atomic.Value(bool).init(false),
        };

        const handler = self.tool_handler;
        const alloc = self.allocator;
        var tool_ctx = ToolCtx{};

        const thread = std.Thread.spawn(.{}, struct {
            fn run(ctx: *ToolCtx, h: @TypeOf(handler), a: std.mem.Allocator, n: []const u8, ar: ?JsonValue) void {
                ctx.value = h(a, n, ar) catch |err| {
                    ctx.err_msg = std.fmt.allocPrint(a, "Tool error: {}", .{err}) catch "Tool error (unknown)";
                    ctx.done.store(true, .release);
                    return;
                };
                ctx.done.store(true, .release);
            }
        }.run, .{ &tool_ctx, handler, alloc, name, arguments }) catch {
            try self.sendToolError(id, "Failed to spawn tool thread");
            return;
        };

        // Poll for completion with 100ms intervals
        const deadline = @as(u64, @intCast(std.time.nanoTimestamp())) + timeout_ns;
        var timed_out = false;
        while (!tool_ctx.done.load(.acquire)) {
            if (@as(u64, @intCast(std.time.nanoTimestamp())) >= deadline) {
                timed_out = true;
                log.err("tool call '{s}' timed out after 45s — interrupting", .{name});

                // Shutdown the helper socket to unblock read() in the worker.
                // This makes stream.read() return 0 (EOF), which triggers
                // disconnect() and the worker exits cleanly.
                if (tools_mod.helper_connections) |h_pool| {
                    h_pool.shutdownAll();
                }
                break;
            }
            std.Thread.sleep(100 * std.time.ns_per_ms);
        }

        // Always join — after shutdown, worker will unblock and exit promptly.
        // Give it a brief grace period, then join unconditionally.
        if (timed_out) {
            // Wait up to 5s for worker to notice the shutdown and exit
            const grace_deadline = @as(u64, @intCast(std.time.nanoTimestamp())) + 5 * std.time.ns_per_s;
            while (!tool_ctx.done.load(.acquire)) {
                if (@as(u64, @intCast(std.time.nanoTimestamp())) >= grace_deadline) break;
                std.Thread.sleep(50 * std.time.ns_per_ms);
            }
            thread.detach(); // Last resort if worker is truly stuck
            try self.sendToolError(id, "Tool call timed out after 45 seconds");
            return;
        }
        thread.join();

        if (tool_ctx.err_msg) |err_msg| {
            try self.sendToolError(id, err_msg);
            return;
        }

        if (tool_ctx.value) |result| {
            try self.sendToolResult(id, result);
        } else {
            try self.sendToolError(id, "Tool returned no result");
        }
    }

    fn handleResourcesList(self: *McpServer, id: ?JsonValue) !void {
        const id_str = try self.formatId(id);
        defer self.allocator.free(id_str);

        const response = try std.fmt.allocPrint(self.allocator,
            "{{\"jsonrpc\":\"2.0\",\"id\":{s},\"result\":{{\"resources\":[" ++
            "{{\"uri\":\"vnc://screenshot\",\"name\":\"Desktop Screenshot\",\"description\":\"Full-resolution screenshot of the remote desktop. Use this to view the current screen state.\",\"mimeType\":\"image/jpeg\"}}" ++
            "]}}}}", .{id_str});
        defer self.allocator.free(response);

        try self.writeLine(response);
    }

    fn handleResourcesRead(self: *McpServer, id: ?JsonValue, params: ?JsonValue) !void {
        const p = params orelse {
            try self.sendError(id, -32602, "Missing params");
            return;
        };
        const obj = if (p == .object) p.object else {
            try self.sendError(id, -32602, "Params must be an object");
            return;
        };
        const uri = if (obj.get("uri")) |u| (if (u == .string) u.string else {
            try self.sendError(id, -32602, "URI must be a string");
            return;
        }) else {
            try self.sendError(id, -32602, "Missing URI");
            return;
        };

        const resource = tools_mod.readResource(self.allocator, uri) catch {
            try self.sendError(id, -32602, "Resource not found");
            return;
        };

        const id_str = try self.formatId(id);
        defer self.allocator.free(id_str);

        const escaped_uri = try jsonEscape(self.allocator, uri);
        defer self.allocator.free(escaped_uri);

        // Build response: contents array with one blob entry + text metadata
        const response = try std.fmt.allocPrint(self.allocator,
            "{{\"jsonrpc\":\"2.0\",\"id\":{s},\"result\":{{\"contents\":[" ++
            "{{\"uri\":\"{s}\",\"mimeType\":\"{s}\",\"text\":\"{s}\"}}," ++
            "{{\"uri\":\"{s}\",\"mimeType\":\"{s}\",\"blob\":\"{s}\"}}" ++
            "]}}}}", .{ id_str, escaped_uri, "text/plain", resource.meta_text, escaped_uri, resource.mime_type, resource.blob });
        defer self.allocator.free(response);

        try self.writeLine(response);
    }

    fn sendToolResult(self: *McpServer, id: ?JsonValue, content: JsonValue) !void {
        const id_str = try self.formatId(id);
        defer self.allocator.free(id_str);

        // Serialize the content value to JSON
        const content_json = try std.fmt.allocPrint(self.allocator, "{f}", .{std.json.fmt(content, .{})});
        defer self.allocator.free(content_json);

        const response = try std.fmt.allocPrint(self.allocator, "{{\"jsonrpc\":\"2.0\",\"id\":{s},\"result\":{s}}}", .{ id_str, content_json });
        defer self.allocator.free(response);

        try self.writeLine(response);
    }

    fn sendToolError(self: *McpServer, id: ?JsonValue, message: []const u8) !void {
        const id_str = try self.formatId(id);
        defer self.allocator.free(id_str);

        // Escape message for JSON string
        const escaped = try jsonEscape(self.allocator, message);
        defer self.allocator.free(escaped);

        const response = try std.fmt.allocPrint(self.allocator, "{{\"jsonrpc\":\"2.0\",\"id\":{s},\"result\":{{\"content\":[{{\"type\":\"text\",\"text\":\"{s}\"}}],\"isError\":true}}}}", .{ id_str, escaped });
        defer self.allocator.free(response);

        try self.writeLine(response);
    }

    fn sendError(self: *McpServer, id: ?JsonValue, code: i32, message: []const u8) !void {
        const id_str = try self.formatId(id);
        defer self.allocator.free(id_str);

        const response = try std.fmt.allocPrint(self.allocator, "{{\"jsonrpc\":\"2.0\",\"id\":{s},\"error\":{{\"code\":{d},\"message\":\"{s}\"}}}}", .{ id_str, code, message });
        defer self.allocator.free(response);

        try self.writeLine(response);
    }

    pub fn sendResult(self: *McpServer, id: ?JsonValue, result: JsonValue) !void {
        const id_str = try self.formatId(id);
        defer self.allocator.free(id_str);

        const result_json = try std.fmt.allocPrint(self.allocator, "{f}", .{std.json.fmt(result, .{})});
        defer self.allocator.free(result_json);

        const response = try std.fmt.allocPrint(self.allocator, "{{\"jsonrpc\":\"2.0\",\"id\":{s},\"result\":{s}}}", .{ id_str, result_json });
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
