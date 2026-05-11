const std = @import("std");
const rfb_client = @import("../rfb/client.zig");
const keysym = @import("../rfb/keysym.zig");
const registry_mod = @import("../registry.zig");
const image = @import("../image.zig");

const log = std.log.scoped(.tools);
const JsonValue = std.json.Value;

/// Connection pool — maps endpoint ID to active RFB client
var connections: ?*ConnectionPool = null;
var global_registry: ?*registry_mod.Registry = null;
var global_allocator: std.mem.Allocator = undefined;

pub const ConnectionPool = struct {
    entries: std.StringHashMap(rfb_client.Client),
    allocator: std.mem.Allocator,

    pub fn init(allocator: std.mem.Allocator) ConnectionPool {
        return ConnectionPool{
            .entries = std.StringHashMap(rfb_client.Client).init(allocator),
            .allocator = allocator,
        };
    }

    pub fn deinit(self: *ConnectionPool) void {
        var it = self.entries.iterator();
        while (it.next()) |entry| {
            entry.value_ptr.disconnect();
        }
        self.entries.deinit();
    }

    pub fn getOrConnect(self: *ConnectionPool, ep: *const registry_mod.Endpoint) !*rfb_client.Client {
        if (self.entries.getPtr(ep.id)) |client| {
            if (client.connected) return client;
            // Reconnect
            client.disconnect();
            _ = self.entries.remove(ep.id);
        }

        // Read password if configured
        var password: ?[]u8 = null;
        defer if (password) |pw| self.allocator.free(pw);

        if (ep.password_file.len > 0) {
            password = registry_mod.Registry.readPassword(self.allocator, ep.password_file) catch |err| {
                log.err("failed to read password for {s}: {}", .{ ep.id, err });
                return error.AuthFailed;
            };
        }

        const pw_slice: ?[]const u8 = if (password) |pw| pw else null;

        log.info("connecting to {s} ({s}:{d})", .{ ep.id, ep.host, ep.port });
        var client = rfb_client.Client.connect(self.allocator, ep.host, ep.port, pw_slice) catch |err| {
            log.err("connection failed for {s}: {}", .{ ep.id, err });
            return err;
        };
        _ = &client;

        try self.entries.put(ep.id, client);
        return self.entries.getPtr(ep.id).?;
    }
};

pub fn setup(allocator: std.mem.Allocator, reg: *registry_mod.Registry, pool: *ConnectionPool) void {
    global_allocator = allocator;
    global_registry = reg;
    connections = pool;
}

fn getEndpoint(arguments: ?JsonValue) !*const registry_mod.Endpoint {
    const reg = global_registry orelse return error.FramebufferNotReady;

    if (arguments) |args| {
        if (args == .object) {
            if (args.object.get("endpoint")) |ep_val| {
                if (ep_val == .string) {
                    if (reg.getById(ep_val.string)) |ep| return ep;
                    return error.ConnectionFailed;
                }
            }
        }
    }

    return reg.getDefault() orelse return error.ConnectionFailed;
}

fn getClient(arguments: ?JsonValue) !*rfb_client.Client {
    const pool = connections orelse return error.ConnectionFailed;
    const ep = try getEndpoint(arguments);
    return pool.getOrConnect(ep);
}

fn getInt(obj: std.json.ObjectMap, key: []const u8) ?i64 {
    if (obj.get(key)) |val| {
        return switch (val) {
            .integer => val.integer,
            .float => @intFromFloat(val.float),
            else => null,
        };
    }
    return null;
}

fn getString(obj: std.json.ObjectMap, key: []const u8) ?[]const u8 {
    if (obj.get(key)) |val| {
        if (val == .string) return val.string;
    }
    return null;
}

fn getBool(obj: std.json.ObjectMap, key: []const u8) bool {
    if (obj.get(key)) |val| {
        if (val == .bool) return val.bool;
    }
    return false;
}

/// MCP tool content response helpers
fn textContent(allocator: std.mem.Allocator, text: []const u8) !JsonValue {
    var content_arr = std.json.Array.init(allocator);
    var item = std.json.ObjectMap.init(allocator);
    try item.put("type", JsonValue{ .string = "text" });
    try item.put("text", JsonValue{ .string = text });
    try content_arr.append(JsonValue{ .object = item });

    var result = std.json.ObjectMap.init(allocator);
    try result.put("content", JsonValue{ .array = content_arr });
    return JsonValue{ .object = result };
}

fn imageContent(allocator: std.mem.Allocator, jpeg_data: []const u8) !JsonValue {
    const base64_encoder = std.base64.standard;
    const encoded_len = base64_encoder.Encoder.calcSize(jpeg_data.len);
    const encoded = try allocator.alloc(u8, encoded_len);
    _ = base64_encoder.Encoder.encode(encoded, jpeg_data);

    var content_arr = std.json.Array.init(allocator);
    var item = std.json.ObjectMap.init(allocator);
    try item.put("type", JsonValue{ .string = "image" });
    try item.put("data", JsonValue{ .string = encoded });
    try item.put("mimeType", JsonValue{ .string = "image/jpeg" });
    try content_arr.append(JsonValue{ .object = item });

    var result = std.json.ObjectMap.init(allocator);
    try result.put("content", JsonValue{ .array = content_arr });
    return JsonValue{ .object = result };
}

/// Main tool dispatch — called by MCP server
pub fn handleTool(allocator: std.mem.Allocator, name: []const u8, arguments: ?JsonValue) anyerror!JsonValue {
    if (std.mem.eql(u8, name, "vnc_screenshot")) {
        return toolScreenshot(allocator, arguments);
    } else if (std.mem.eql(u8, name, "vnc_click")) {
        return toolClick(allocator, arguments);
    } else if (std.mem.eql(u8, name, "vnc_type_text")) {
        return toolTypeText(allocator, arguments);
    } else if (std.mem.eql(u8, name, "vnc_key_press")) {
        return toolKeyPress(allocator, arguments);
    } else if (std.mem.eql(u8, name, "vnc_move_mouse")) {
        return toolMoveMouse(allocator, arguments);
    } else if (std.mem.eql(u8, name, "vnc_drag")) {
        return toolDrag(allocator, arguments);
    } else if (std.mem.eql(u8, name, "vnc_clipboard_set")) {
        return toolClipboardSet(allocator, arguments);
    } else if (std.mem.eql(u8, name, "vnc_paste_text")) {
        return toolPasteText(allocator, arguments);
    } else if (std.mem.eql(u8, name, "vnc_list_endpoints")) {
        return toolListEndpoints(allocator);
    } else {
        return textContent(allocator, "Unknown tool");
    }
}

fn toolScreenshot(allocator: std.mem.Allocator, arguments: ?JsonValue) !JsonValue {
    const client = try getClient(arguments);

    var quality: u8 = 75;
    if (arguments) |args| {
        if (args == .object) {
            if (getInt(args.object, "quality")) |q| {
                quality = @intCast(std.math.clamp(q, 1, 100));
            }
        }
    }

    const fb = try client.screenshot();
    const jpeg = try image.encodeJpeg(allocator, fb, quality);
    defer allocator.free(jpeg);

    return imageContent(allocator, jpeg);
}

fn toolClick(allocator: std.mem.Allocator, arguments: ?JsonValue) !JsonValue {
    const args = if (arguments) |a| (if (a == .object) a.object else return error.InvalidArgument) else return error.InvalidArgument;

    const x: u16 = @intCast(getInt(args, "x") orelse return error.InvalidArgument);
    const y: u16 = @intCast(getInt(args, "y") orelse return error.InvalidArgument);

    const button_str = getString(args, "button") orelse "left";
    const button_mask: u8 = if (std.mem.eql(u8, button_str, "right"))
        4
    else if (std.mem.eql(u8, button_str, "middle"))
        2
    else
        1; // left

    const double_click = getBool(args, "double");

    const client = try getClient(arguments);

    // Move to position, press, release
    try client.sendPointerEvent(x, y, button_mask);
    std.Thread.sleep(50 * std.time.ns_per_ms);
    try client.sendPointerEvent(x, y, 0);

    if (double_click) {
        std.Thread.sleep(50 * std.time.ns_per_ms);
        try client.sendPointerEvent(x, y, button_mask);
        std.Thread.sleep(50 * std.time.ns_per_ms);
        try client.sendPointerEvent(x, y, 0);
    }

    return textContent(allocator, "Click sent");
}

fn toolTypeText(allocator: std.mem.Allocator, arguments: ?JsonValue) !JsonValue {
    const args = if (arguments) |a| (if (a == .object) a.object else return error.InvalidArgument) else return error.InvalidArgument;
    const text = getString(args, "text") orelse return error.InvalidArgument;

    const client = try getClient(arguments);

    // Decode UTF-8 and send each codepoint as a keysym
    var i: usize = 0;
    while (i < text.len) {
        const seq_len = std.unicode.utf8ByteSequenceLength(text[i]) catch {
            i += 1;
            continue;
        };
        if (i + seq_len > text.len) break;

        const codepoint = std.unicode.utf8Decode(text[i..][0..seq_len]) catch {
            i += seq_len;
            continue;
        };

        const ks = keysym.unicodeToKeysym(codepoint);
        try client.sendKeyEvent(ks, true);
        try client.sendKeyEvent(ks, false);
        std.Thread.sleep(10 * std.time.ns_per_ms);

        i += seq_len;
    }

    return textContent(allocator, "Text typed");
}

fn toolKeyPress(allocator: std.mem.Allocator, arguments: ?JsonValue) !JsonValue {
    const args = if (arguments) |a| (if (a == .object) a.object else return error.InvalidArgument) else return error.InvalidArgument;
    const keys_str = getString(args, "keys") orelse return error.InvalidArgument;

    const client = try getClient(arguments);

    // Parse combo like "ctrl+c", "alt+F4", "shift+a"
    var modifiers: [4]u32 = undefined;
    var mod_count: usize = 0;
    var main_key: ?u32 = null;

    var it = std.mem.splitScalar(u8, keys_str, '+');
    var parts: [8][]const u8 = undefined;
    var part_count: usize = 0;

    while (it.next()) |part| {
        if (part_count < parts.len) {
            parts[part_count] = part;
            part_count += 1;
        }
    }

    if (part_count == 0) return textContent(allocator, "No key specified");

    // Last part is the main key, preceding parts are modifiers
    for (parts[0 .. part_count - 1]) |part| {
        if (keysym.modifierKeysym(part)) |ks| {
            if (mod_count < modifiers.len) {
                modifiers[mod_count] = ks;
                mod_count += 1;
            }
        }
    }

    const main_part = parts[part_count - 1];
    main_key = keysym.namedKeysym(main_part);

    if (main_key == null) {
        // Try as modifier-only (e.g., just "ctrl")
        main_key = keysym.modifierKeysym(main_part);
    }

    const mk = main_key orelse return textContent(allocator, "Unknown key");

    // Press modifiers, press key, release key, release modifiers
    for (modifiers[0..mod_count]) |mod| {
        try client.sendKeyEvent(mod, true);
    }
    try client.sendKeyEvent(mk, true);
    try client.sendKeyEvent(mk, false);

    var ri: usize = mod_count;
    while (ri > 0) {
        ri -= 1;
        try client.sendKeyEvent(modifiers[ri], false);
    }

    return textContent(allocator, "Key press sent");
}

fn toolMoveMouse(allocator: std.mem.Allocator, arguments: ?JsonValue) !JsonValue {
    const args = if (arguments) |a| (if (a == .object) a.object else return error.InvalidArgument) else return error.InvalidArgument;

    const x: u16 = @intCast(getInt(args, "x") orelse return error.InvalidArgument);
    const y: u16 = @intCast(getInt(args, "y") orelse return error.InvalidArgument);

    const client = try getClient(arguments);
    try client.sendPointerEvent(x, y, 0);

    return textContent(allocator, "Mouse moved");
}

fn toolDrag(allocator: std.mem.Allocator, arguments: ?JsonValue) !JsonValue {
    const args = if (arguments) |a| (if (a == .object) a.object else return error.InvalidArgument) else return error.InvalidArgument;

    const x1: u16 = @intCast(getInt(args, "x1") orelse return error.InvalidArgument);
    const y1: u16 = @intCast(getInt(args, "y1") orelse return error.InvalidArgument);
    const x2: u16 = @intCast(getInt(args, "x2") orelse return error.InvalidArgument);
    const y2: u16 = @intCast(getInt(args, "y2") orelse return error.InvalidArgument);

    const client = try getClient(arguments);

    // Move to start, press, move to end, release
    try client.sendPointerEvent(x1, y1, 0);
    std.Thread.sleep(50 * std.time.ns_per_ms);
    try client.sendPointerEvent(x1, y1, 1); // Left button down
    std.Thread.sleep(50 * std.time.ns_per_ms);

    // Interpolate a few points for smoother drag
    const steps: usize = 10;
    for (1..steps) |step| {
        const t: f32 = @as(f32, @floatFromInt(step)) / @as(f32, @floatFromInt(steps));
        const ix: u16 = @intFromFloat(@as(f32, @floatFromInt(x1)) + (@as(f32, @floatFromInt(x2)) - @as(f32, @floatFromInt(x1))) * t);
        const iy: u16 = @intFromFloat(@as(f32, @floatFromInt(y1)) + (@as(f32, @floatFromInt(y2)) - @as(f32, @floatFromInt(y1))) * t);
        try client.sendPointerEvent(ix, iy, 1);
        std.Thread.sleep(20 * std.time.ns_per_ms);
    }

    try client.sendPointerEvent(x2, y2, 1);
    std.Thread.sleep(50 * std.time.ns_per_ms);
    try client.sendPointerEvent(x2, y2, 0); // Release

    return textContent(allocator, "Drag completed");
}

fn toolClipboardSet(allocator: std.mem.Allocator, arguments: ?JsonValue) !JsonValue {
    const args = if (arguments) |a| (if (a == .object) a.object else return error.InvalidArgument) else return error.InvalidArgument;
    const text = getString(args, "text") orelse return error.InvalidArgument;

    const client = try getClient(arguments);
    try client.sendClipboard(text);

    return textContent(allocator, "Clipboard set");
}

fn toolPasteText(allocator: std.mem.Allocator, arguments: ?JsonValue) !JsonValue {
    const args = if (arguments) |a| (if (a == .object) a.object else return error.InvalidArgument) else return error.InvalidArgument;
    const text = getString(args, "text") orelse return error.InvalidArgument;

    const client = try getClient(arguments);

    // Set clipboard
    try client.sendClipboard(text);
    std.Thread.sleep(100 * std.time.ns_per_ms);

    // Send Ctrl+V
    try client.sendKeyEvent(0xFFE3, true); // Control_L down
    try client.sendKeyEvent(0x0076, true); // 'v' down
    try client.sendKeyEvent(0x0076, false); // 'v' up
    try client.sendKeyEvent(0xFFE3, false); // Control_L up

    return textContent(allocator, "Text pasted");
}

fn toolListEndpoints(allocator: std.mem.Allocator) !JsonValue {
    const reg = global_registry orelse return textContent(allocator, "No registry configured");
    const pool = connections orelse return textContent(allocator, "No connection pool");

    // Build result string using allocPrint
    var text: []u8 = try allocator.dupe(u8, "Endpoints:\n");

    for (reg.endpoints) |ep| {
        const connected = if (pool.entries.get(ep.id)) |c| c.connected else false;
        const status = if (connected) "connected" else "disconnected";
        const def = if (ep.default) " [default]" else "";

        const line = try std.fmt.allocPrint(allocator, "  {s}: {s}:{d} ({s}){s}\n", .{ ep.id, ep.host, ep.port, status, def });
        const new_text = try std.fmt.allocPrint(allocator, "{s}{s}", .{ text, line });
        allocator.free(text);
        allocator.free(line);
        text = new_text;

        if (ep.description.len > 0) {
            const desc = try std.fmt.allocPrint(allocator, "    {s}\n", .{ep.description});
            const new_text2 = try std.fmt.allocPrint(allocator, "{s}{s}", .{ text, desc });
            allocator.free(text);
            allocator.free(desc);
            text = new_text2;
        }
    }

    return textContent(allocator, text);
}
