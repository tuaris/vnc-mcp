const std = @import("std");
const rfb_client = @import("../rfb/client.zig");
const keysym = @import("../rfb/keysym.zig");
const registry_mod = @import("../registry.zig");
const image = @import("../image.zig");
const helper = @import("../helper.zig");

const log = std.log.scoped(.tools);
const JsonValue = std.json.Value;

/// Connection pool — maps endpoint ID to active RFB client
var connections: ?*ConnectionPool = null;
var helper_connections: ?*HelperPool = null;
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

pub const HelperPool = struct {
    entries: std.StringHashMap(helper.HelperConnection),
    allocator: std.mem.Allocator,

    pub fn init(allocator: std.mem.Allocator) HelperPool {
        return HelperPool{
            .entries = std.StringHashMap(helper.HelperConnection).init(allocator),
            .allocator = allocator,
        };
    }

    pub fn deinit(self: *HelperPool) void {
        var it = self.entries.iterator();
        while (it.next()) |entry| {
            entry.value_ptr.disconnect();
        }
        self.entries.deinit();
    }

    pub fn getOrCreate(self: *HelperPool, ep: *const registry_mod.Endpoint, password: ?[]const u8) !*helper.HelperConnection {
        if (self.entries.getPtr(ep.id)) |conn| return conn;

        const conn = helper.HelperConnection.init(self.allocator, ep.host, ep.helper_port, password);
        try self.entries.put(ep.id, conn);
        return self.entries.getPtr(ep.id).?;
    }
};

pub fn setup(allocator: std.mem.Allocator, reg: *registry_mod.Registry, pool: *ConnectionPool, h_pool: *HelperPool) void {
    global_allocator = allocator;
    global_registry = reg;
    connections = pool;
    helper_connections = h_pool;
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

fn imageContentWithMeta(allocator: std.mem.Allocator, jpeg_data: []const u8, meta_text: []const u8) !JsonValue {
    const base64_encoder = std.base64.standard;
    const encoded_len = base64_encoder.Encoder.calcSize(jpeg_data.len);
    const encoded = try allocator.alloc(u8, encoded_len);
    _ = base64_encoder.Encoder.encode(encoded, jpeg_data);

    var content_arr = std.json.Array.init(allocator);

    // Text metadata (resolution, etc.)
    var text_item = std.json.ObjectMap.init(allocator);
    try text_item.put("type", JsonValue{ .string = "text" });
    try text_item.put("text", JsonValue{ .string = meta_text });
    try content_arr.append(JsonValue{ .object = text_item });

    // Image data
    var img_item = std.json.ObjectMap.init(allocator);
    try img_item.put("type", JsonValue{ .string = "image" });
    try img_item.put("data", JsonValue{ .string = encoded });
    try img_item.put("mimeType", JsonValue{ .string = "image/jpeg" });
    try content_arr.append(JsonValue{ .object = img_item });

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
    } else if (std.mem.eql(u8, name, "vnc_probe")) {
        return toolProbe(allocator, arguments);
    } else if (std.mem.eql(u8, name, "vnc_grid")) {
        return toolGrid(allocator, arguments);
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
    } else if (std.mem.eql(u8, name, "vnc_cursor_position")) {
        return toolCursorPosition(allocator, arguments);
    } else if (std.mem.eql(u8, name, "vnc_window_list")) {
        return toolWindowList(allocator, arguments);
    } else if (std.mem.eql(u8, name, "vnc_active_window")) {
        return toolActiveWindow(allocator, arguments);
    } else if (std.mem.eql(u8, name, "vnc_set_active_window")) {
        return toolSetActiveWindow(allocator, arguments);
    } else if (std.mem.eql(u8, name, "vnc_manage_window")) {
        return toolManageWindow(allocator, arguments);
    } else if (std.mem.eql(u8, name, "vnc_helper_clipboard_get")) {
        return toolHelperClipboardGet(allocator, arguments);
    } else if (std.mem.eql(u8, name, "vnc_helper_clipboard_set")) {
        return toolHelperClipboardSet(allocator, arguments);
    } else if (std.mem.eql(u8, name, "vnc_run_command")) {
        return toolRunCommand(allocator, arguments);
    } else if (std.mem.eql(u8, name, "vnc_screen_info")) {
        return toolScreenInfo(allocator, arguments);
    } else if (std.mem.eql(u8, name, "vnc_upload_file")) {
        return toolUploadFile(allocator, arguments);
    } else if (std.mem.eql(u8, name, "vnc_download_file")) {
        return toolDownloadFile(allocator, arguments);
    } else if (std.mem.eql(u8, name, "vnc_ocr_region")) {
        return toolOcrRegion(allocator, arguments);
    } else if (std.mem.eql(u8, name, "vnc_clipboard_get")) {
        return toolClipboardGet(allocator, arguments);
    } else if (std.mem.eql(u8, name, "vnc_ui_tree")) {
        return toolUiTree(allocator, arguments);
    } else if (std.mem.eql(u8, name, "vnc_ui_element_text")) {
        return toolUiElementText(allocator, arguments);
    } else if (std.mem.eql(u8, name, "vnc_ui_click_element")) {
        return toolUiClickElement(allocator, arguments);
    } else {
        return textContent(allocator, "Unknown tool");
    }
}

fn toolScreenshot(allocator: std.mem.Allocator, arguments: ?JsonValue) !JsonValue {
    const client = try getClient(arguments);

    var quality: u8 = 75;
    var delay_ms: u64 = 0;
    if (arguments) |args| {
        if (args == .object) {
            if (getInt(args.object, "quality")) |q| {
                quality = @intCast(std.math.clamp(q, 1, 100));
            }
            if (getInt(args.object, "delay")) |d| {
                delay_ms = @intCast(std.math.clamp(d, 0, 10000));
            }
        }
    }

    // Wait for screen to settle after prior actions (click, type, paste)
    if (delay_ms > 0) {
        std.Thread.sleep(delay_ms * std.time.ns_per_ms);
    }

    const fb = try client.screenshot();
    const jpeg = try image.encodeJpeg(allocator, fb, quality);
    defer allocator.free(jpeg);

    // Include resolution metadata so AI agents can compute coordinates
    const meta = try std.fmt.allocPrint(allocator, "Resolution: {d}x{d} pixels", .{ fb.width, fb.height });
    return imageContentWithMeta(allocator, jpeg, meta);
}

fn toolProbe(allocator: std.mem.Allocator, arguments: ?JsonValue) !JsonValue {
    const args = if (arguments) |a| (if (a == .object) a.object else return error.InvalidArgument) else return error.InvalidArgument;

    const x: u16 = @intCast(getInt(args, "x") orelse return error.InvalidArgument);
    const y: u16 = @intCast(getInt(args, "y") orelse return error.InvalidArgument);

    const client = try getClient(arguments);
    const fb = try client.screenshot();

    const jpeg = try image.encodeJpegWithMarker(allocator, fb, 75, x, y);
    defer allocator.free(jpeg);

    const meta = try std.fmt.allocPrint(allocator, "Probe marker at ({d}, {d}) — Resolution: {d}x{d} pixels", .{ x, y, fb.width, fb.height });
    return imageContentWithMeta(allocator, jpeg, meta);
}

fn toolGrid(allocator: std.mem.Allocator, arguments: ?JsonValue) !JsonValue {
    var cols: u8 = 8;
    var rows: u8 = 6;

    if (arguments) |args| {
        if (args == .object) {
            if (getInt(args.object, "columns")) |c| {
                cols = @intCast(std.math.clamp(c, 2, 16));
            }
            if (getInt(args.object, "rows")) |r| {
                rows = @intCast(std.math.clamp(r, 2, 12));
            }
        }
    }

    const client = try getClient(arguments);
    const fb = try client.screenshot();

    const jpeg = try image.encodeJpegWithGrid(allocator, fb, 75, cols, rows);
    defer allocator.free(jpeg);

    // Build cell coordinate map as text metadata
    const col_width = @as(u32, fb.width) / @as(u32, cols);
    const row_height = @as(u32, fb.height) / @as(u32, rows);

    // Format: "Grid 8x6 on 1918x968. Cell size: 239x161. A1=(120,80) A2=(359,80) ..."
    var meta_buf = std.ArrayList(u8){};
    defer meta_buf.deinit(allocator);

    const header = try std.fmt.allocPrint(allocator, "Grid {d}x{d} on {d}x{d}px. Cell size: {d}x{d}px.\n", .{ cols, rows, fb.width, fb.height, col_width, row_height });
    defer allocator.free(header);
    try meta_buf.appendSlice(allocator, header);

    for (0..@as(usize, rows)) |r_idx| {
        for (0..@as(usize, cols)) |c_idx| {
            const cx = @as(u32, @intCast(c_idx)) * col_width + col_width / 2;
            const cy = @as(u32, @intCast(r_idx)) * row_height + row_height / 2;

            var label: [4]u8 = undefined;
            var label_len: usize = 0;
            label[0] = 'A' + @as(u8, @intCast(c_idx));
            label_len = 1;
            const row_num = r_idx + 1;
            if (row_num >= 10) {
                label[1] = '0' + @as(u8, @intCast(row_num / 10));
                label[2] = '0' + @as(u8, @intCast(row_num % 10));
                label_len = 3;
            } else {
                label[1] = '0' + @as(u8, @intCast(row_num));
                label_len = 2;
            }

            const entry = try std.fmt.allocPrint(allocator, "{s}=({d},{d}) ", .{ label[0..label_len], cx, cy });
            defer allocator.free(entry);
            try meta_buf.appendSlice(allocator, entry);
        }
        try meta_buf.append(allocator, '\n');
    }

    const meta = try allocator.dupe(u8, meta_buf.items);
    return imageContentWithMeta(allocator, jpeg, meta);
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

    // Visual confirmation: draw marker at click point + capture screenshot
    // Best-effort — if helper is unavailable, still return the click result
    const marker_params = try std.fmt.allocPrint(allocator, "\"x\":{d},\"y\":{d}", .{ x, y });
    defer allocator.free(marker_params);
    _ = callHelper(allocator, arguments, "click_marker", marker_params) catch {};

    // Wait for marker to render + screen to settle after click
    std.Thread.sleep(300 * std.time.ns_per_ms);

    // Capture confirmation screenshot
    const fb = client.screenshot() catch {
        // If screenshot fails, just return text
        const msg = try std.fmt.allocPrint(allocator, "Clicked at ({d}, {d})", .{ x, y });
        return textContent(allocator, msg);
    };
    const jpeg = image.encodeJpeg(allocator, fb, 65) catch {
        const msg = try std.fmt.allocPrint(allocator, "Clicked at ({d}, {d})", .{ x, y });
        return textContent(allocator, msg);
    };
    defer allocator.free(jpeg);

    // Return multi-content: text description + visual confirmation image
    const click_msg = try std.fmt.allocPrint(allocator, "Clicked at ({d}, {d}) — yellow marker shows click location", .{ x, y });

    const base64_encoder = std.base64.standard;
    const encoded_len = base64_encoder.Encoder.calcSize(jpeg.len);
    const encoded = try allocator.alloc(u8, encoded_len);
    _ = base64_encoder.Encoder.encode(encoded, jpeg);

    var content_arr = std.json.Array.init(allocator);

    // Text item
    var text_item = std.json.ObjectMap.init(allocator);
    try text_item.put("type", JsonValue{ .string = "text" });
    try text_item.put("text", JsonValue{ .string = click_msg });
    try content_arr.append(JsonValue{ .object = text_item });

    // Image item
    var img_item = std.json.ObjectMap.init(allocator);
    try img_item.put("type", JsonValue{ .string = "image" });
    try img_item.put("data", JsonValue{ .string = encoded });
    try img_item.put("mimeType", JsonValue{ .string = "image/jpeg" });
    try content_arr.append(JsonValue{ .object = img_item });

    var result = std.json.ObjectMap.init(allocator);
    try result.put("content", JsonValue{ .array = content_arr });
    return JsonValue{ .object = result };
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

    // Set clipboard via ClientCutText — TightVNC sets Windows clipboard
    // asynchronously through its message loop, needs time to propagate
    try client.sendClipboard(text);
    std.Thread.sleep(300 * std.time.ns_per_ms);

    // Send Ctrl+V with inter-key delays for reliability
    try client.sendKeyEvent(0xFFE3, true); // Control_L down
    std.Thread.sleep(20 * std.time.ns_per_ms);
    try client.sendKeyEvent(0x0076, true); // 'v' down
    std.Thread.sleep(20 * std.time.ns_per_ms);
    try client.sendKeyEvent(0x0076, false); // 'v' up
    std.Thread.sleep(20 * std.time.ns_per_ms);
    try client.sendKeyEvent(0xFFE3, false); // Control_L up
    std.Thread.sleep(50 * std.time.ns_per_ms);

    return textContent(allocator, "Text pasted");
}

/// Helper tool: call the vnc-helper agent on the endpoint (persistent connection)
fn callHelper(allocator: std.mem.Allocator, arguments: ?JsonValue, command: []const u8, extra_params: ?[]const u8) ![]u8 {
    const ep = try getEndpoint(arguments);
    if (ep.helper_port == 0) {
        return error.FramebufferNotReady; // will be caught and shown as error
    }

    const h_pool = helper_connections orelse return error.ConnectionFailed;

    // Read VNC password for helper auth (same password_file as VNC connection)
    var password: ?[]u8 = null;
    defer if (password) |pw| allocator.free(pw);

    if (ep.password_file.len > 0) {
        password = registry_mod.Registry.readPassword(allocator, ep.password_file) catch null;
    }

    const pw_slice: ?[]const u8 = if (password) |pw| pw else null;

    const conn = try h_pool.getOrCreate(ep, pw_slice);

    var request: []u8 = undefined;
    if (extra_params) |params| {
        request = try std.fmt.allocPrint(allocator, "{{\"command\":\"{s}\",{s}}}", .{ command, params });
    } else {
        request = try std.fmt.allocPrint(allocator, "{{\"command\":\"{s}\"}}", .{command});
    }
    defer allocator.free(request);

    return conn.call(request);
}

fn helperNotConfigured(allocator: std.mem.Allocator) !JsonValue {
    return textContent(allocator, "Helper agent not configured for this endpoint. Set helper_port in endpoints.json and run vnc-helper.exe on the target machine.");
}

fn helperNotAvailable(allocator: std.mem.Allocator) !JsonValue {
    return textContent(allocator, "Helper agent not available. Ensure vnc-helper.exe is running on the target machine.");
}

fn toolCursorPosition(allocator: std.mem.Allocator, arguments: ?JsonValue) !JsonValue {
    const response = callHelper(allocator, arguments, "cursor_position", null) catch |err| {
        if (err == error.FramebufferNotReady) return helperNotConfigured(allocator);
        return helperNotAvailable(allocator);
    };
    return textContent(allocator, response);
}

fn toolWindowList(allocator: std.mem.Allocator, arguments: ?JsonValue) !JsonValue {
    const response = callHelper(allocator, arguments, "window_list", null) catch |err| {
        if (err == error.FramebufferNotReady) return helperNotConfigured(allocator);
        return helperNotAvailable(allocator);
    };
    return textContent(allocator, response);
}

fn toolActiveWindow(allocator: std.mem.Allocator, arguments: ?JsonValue) !JsonValue {
    const response = callHelper(allocator, arguments, "active_window", null) catch |err| {
        if (err == error.FramebufferNotReady) return helperNotConfigured(allocator);
        return helperNotAvailable(allocator);
    };
    return textContent(allocator, response);
}

fn toolSetActiveWindow(allocator: std.mem.Allocator, arguments: ?JsonValue) !JsonValue {
    const args = if (arguments) |a| (if (a == .object) a.object else return error.InvalidArgument) else return error.InvalidArgument;

    var parts: [3][]const u8 = undefined;
    var part_count: usize = 0;

    if (getString(args, "title")) |t| {
        const escaped = try helper.jsonEscape(allocator, t);
        defer allocator.free(escaped);
        const part = try std.fmt.allocPrint(allocator, "\"title\":\"{s}\"", .{escaped});
        parts[part_count] = part;
        part_count += 1;
    }
    if (getString(args, "class")) |c| {
        const escaped = try helper.jsonEscape(allocator, c);
        defer allocator.free(escaped);
        const part = try std.fmt.allocPrint(allocator, "\"class\":\"{s}\"", .{escaped});
        parts[part_count] = part;
        part_count += 1;
    }
    if (getInt(args, "pid")) |p| {
        const part = try std.fmt.allocPrint(allocator, "\"pid\":{d}", .{p});
        parts[part_count] = part;
        part_count += 1;
    }

    if (part_count == 0) return textContent(allocator, "Provide at least one of: title, class, pid");

    // Join parts with commas
    var extra = try allocator.alloc(u8, 0);
    for (0..part_count) |i| {
        const old = extra;
        if (i == 0) {
            extra = try allocator.dupe(u8, parts[i]);
        } else {
            extra = try std.fmt.allocPrint(allocator, "{s},{s}", .{ old, parts[i] });
            allocator.free(old);
        }
        allocator.free(parts[i]);
    }
    defer allocator.free(extra);

    const response = callHelper(allocator, arguments, "set_active_window", extra) catch |err| {
        if (err == error.FramebufferNotReady) return helperNotConfigured(allocator);
        return helperNotAvailable(allocator);
    };
    return textContent(allocator, response);
}

fn toolManageWindow(allocator: std.mem.Allocator, arguments: ?JsonValue) !JsonValue {
    const args = if (arguments) |a| (if (a == .object) a.object else return error.InvalidArgument) else return error.InvalidArgument;

    const action = getString(args, "action") orelse return error.InvalidArgument;

    var parts = std.ArrayList(u8){};
    defer parts.deinit(allocator);

    {
        const escaped = try helper.jsonEscape(allocator, action);
        defer allocator.free(escaped);
        const chunk = try std.fmt.allocPrint(allocator, "\"action\":\"{s}\"", .{escaped});
        defer allocator.free(chunk);
        try parts.appendSlice(allocator, chunk);
    }

    if (getString(args, "title")) |t| {
        try parts.append(allocator, ',');
        const escaped = try helper.jsonEscape(allocator, t);
        defer allocator.free(escaped);
        const chunk = try std.fmt.allocPrint(allocator, "\"title\":\"{s}\"", .{escaped});
        defer allocator.free(chunk);
        try parts.appendSlice(allocator, chunk);
    }
    if (getString(args, "class")) |c| {
        try parts.append(allocator, ',');
        const escaped = try helper.jsonEscape(allocator, c);
        defer allocator.free(escaped);
        const chunk = try std.fmt.allocPrint(allocator, "\"class\":\"{s}\"", .{escaped});
        defer allocator.free(chunk);
        try parts.appendSlice(allocator, chunk);
    }
    if (getInt(args, "pid")) |p| {
        const chunk = try std.fmt.allocPrint(allocator, ",\"pid\":{d}", .{p});
        defer allocator.free(chunk);
        try parts.appendSlice(allocator, chunk);
    }

    const extra = try allocator.dupe(u8, parts.items);
    defer allocator.free(extra);

    const response = callHelper(allocator, arguments, "manage_window", extra) catch |err| {
        if (err == error.FramebufferNotReady) return helperNotConfigured(allocator);
        return helperNotAvailable(allocator);
    };
    return textContent(allocator, response);
}

fn toolHelperClipboardGet(allocator: std.mem.Allocator, arguments: ?JsonValue) !JsonValue {
    const response = callHelper(allocator, arguments, "clipboard_get", null) catch |err| {
        if (err == error.FramebufferNotReady) return helperNotConfigured(allocator);
        return helperNotAvailable(allocator);
    };
    return textContent(allocator, response);
}

fn toolHelperClipboardSet(allocator: std.mem.Allocator, arguments: ?JsonValue) !JsonValue {
    const args = if (arguments) |a| (if (a == .object) a.object else return error.InvalidArgument) else return error.InvalidArgument;
    const text = getString(args, "text") orelse return error.InvalidArgument;

    const escaped = try helper.jsonEscape(allocator, text);
    defer allocator.free(escaped);

    const extra = try std.fmt.allocPrint(allocator, "\"text\":\"{s}\"", .{escaped});
    defer allocator.free(extra);

    const response = callHelper(allocator, arguments, "clipboard_set", extra) catch |err| {
        if (err == error.FramebufferNotReady) return helperNotConfigured(allocator);
        return helperNotAvailable(allocator);
    };
    return textContent(allocator, response);
}

fn toolRunCommand(allocator: std.mem.Allocator, arguments: ?JsonValue) !JsonValue {
    const args = if (arguments) |a| (if (a == .object) a.object else return error.InvalidArgument) else return error.InvalidArgument;
    const cmd_str = getString(args, "cmd") orelse return error.InvalidArgument;

    // JSON-escape the command string
    const escaped_cmd = try helper.jsonEscape(allocator, cmd_str);
    defer allocator.free(escaped_cmd);

    // Build extra params
    var extra: []u8 = undefined;
    if (getInt(args, "timeout")) |t| {
        extra = try std.fmt.allocPrint(allocator, "\"cmd\":\"{s}\",\"timeout\":{d}", .{ escaped_cmd, t });
    } else {
        extra = try std.fmt.allocPrint(allocator, "\"cmd\":\"{s}\"", .{escaped_cmd});
    }
    defer allocator.free(extra);

    const response = callHelper(allocator, arguments, "run_command", extra) catch |err| {
        if (err == error.FramebufferNotReady) return helperNotConfigured(allocator);
        return helperNotAvailable(allocator);
    };
    return textContent(allocator, response);
}

fn toolScreenInfo(allocator: std.mem.Allocator, arguments: ?JsonValue) !JsonValue {
    const response = callHelper(allocator, arguments, "screen_info", null) catch |err| {
        if (err == error.FramebufferNotReady) return helperNotConfigured(allocator);
        return helperNotAvailable(allocator);
    };
    return textContent(allocator, response);
}

fn toolUploadFile(allocator: std.mem.Allocator, arguments: ?JsonValue) !JsonValue {
    const args = if (arguments) |a| (if (a == .object) a.object else return error.InvalidArgument) else return error.InvalidArgument;
    const local_path = getString(args, "local_path") orelse return error.InvalidArgument;
    const remote_path = getString(args, "remote_path") orelse return error.InvalidArgument;

    // Read local file
    const file = std.fs.openFileAbsolute(local_path, .{}) catch |err| {
        const msg = try std.fmt.allocPrint(allocator, "Failed to open local file: {s}: {}", .{ local_path, err });
        return textContent(allocator, msg);
    };
    defer file.close();

    const file_data = file.readToEndAlloc(allocator, 10 * 1024 * 1024) catch |err| {
        const msg = try std.fmt.allocPrint(allocator, "Failed to read local file: {}", .{err});
        return textContent(allocator, msg);
    };
    defer allocator.free(file_data);

    // Base64 encode
    const b64_encoder = std.base64.standard;
    const b64_len = b64_encoder.Encoder.calcSize(file_data.len);
    const b64_data = try allocator.alloc(u8, b64_len);
    defer allocator.free(b64_data);
    _ = b64_encoder.Encoder.encode(b64_data, file_data);

    // Escape remote_path for JSON
    const escaped_path = try helper.jsonEscape(allocator, remote_path);
    defer allocator.free(escaped_path);

    // Build extra params: "path":"<remote>","content":"<b64>"
    const extra = try std.fmt.allocPrint(allocator, "\"path\":\"{s}\",\"content\":\"{s}\"", .{ escaped_path, b64_data });
    defer allocator.free(extra);

    const response = callHelper(allocator, arguments, "file_upload", extra) catch |err| {
        if (err == error.FramebufferNotReady) return helperNotConfigured(allocator);
        return helperNotAvailable(allocator);
    };
    return textContent(allocator, response);
}

fn toolDownloadFile(allocator: std.mem.Allocator, arguments: ?JsonValue) !JsonValue {
    const args = if (arguments) |a| (if (a == .object) a.object else return error.InvalidArgument) else return error.InvalidArgument;
    const remote_path = getString(args, "remote_path") orelse return error.InvalidArgument;
    const local_path = getString(args, "local_path") orelse return error.InvalidArgument;

    // Escape remote_path for JSON
    const escaped_path = try helper.jsonEscape(allocator, remote_path);
    defer allocator.free(escaped_path);

    const extra = try std.fmt.allocPrint(allocator, "\"path\":\"{s}\"", .{escaped_path});
    defer allocator.free(extra);

    const response = callHelper(allocator, arguments, "file_download", extra) catch |err| {
        if (err == error.FramebufferNotReady) return helperNotConfigured(allocator);
        return helperNotAvailable(allocator);
    };

    // Parse response to extract base64 content and save to local file
    const parsed = std.json.parseFromSlice(std.json.Value, allocator, response, .{
        .ignore_unknown_fields = true,
    }) catch {
        return textContent(allocator, response);
    };
    defer parsed.deinit();

    const root = if (parsed.value == .object) parsed.value.object else return textContent(allocator, response);
    const status = if (root.get("status")) |s| (if (s == .string) s.string else null) else null;
    if (status == null or !std.mem.eql(u8, status.?, "ok")) {
        return textContent(allocator, response);
    }

    const data = if (root.get("data")) |d| (if (d == .object) d.object else null) else null;
    if (data == null) return textContent(allocator, response);

    const content_b64 = if (data.?.get("content")) |c| (if (c == .string) c.string else null) else null;
    if (content_b64 == null) return textContent(allocator, response);

    // Decode base64
    const b64_decoder = std.base64.standard;
    const b64 = content_b64.?;
    const decoded_upper = b64_decoder.Decoder.calcSizeUpperBound(b64.len) catch {
        return textContent(allocator, "Invalid base64 content from helper");
    };
    const decoded_buf = try allocator.alloc(u8, decoded_upper);
    defer allocator.free(decoded_buf);

    b64_decoder.Decoder.decode(decoded_buf, b64) catch {
        return textContent(allocator, "Failed to decode base64 content from helper");
    };

    // Calculate exact decoded size from base64 padding (upper bound may be 1-2 bytes too large)
    var exact_len = (b64.len / 4) * 3;
    if (b64.len > 0 and b64[b64.len - 1] == '=') exact_len -= 1;
    if (b64.len > 1 and b64[b64.len - 2] == '=') exact_len -= 1;

    // Write to local file
    const out_file = std.fs.createFileAbsolute(local_path, .{}) catch |err| {
        const msg = try std.fmt.allocPrint(allocator, "Failed to create local file {s}: {}", .{ local_path, err });
        return textContent(allocator, msg);
    };
    defer out_file.close();
    out_file.writeAll(decoded_buf[0..exact_len]) catch |err| {
        const msg = try std.fmt.allocPrint(allocator, "Failed to write local file: {}", .{err});
        return textContent(allocator, msg);
    };

    const msg = try std.fmt.allocPrint(allocator, "Downloaded {d} bytes from {s} to {s}", .{ exact_len, remote_path, local_path });
    return textContent(allocator, msg);
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

fn toolClipboardGet(allocator: std.mem.Allocator, arguments: ?JsonValue) !JsonValue {
    const client = try getClient(arguments);

    // Flush pending server messages with multiple update cycles.
    // ServerCutText may arrive after a FramebufferUpdate response,
    // so we do two cycles with a delay to catch late clipboard messages.
    try client.requestUpdate(true);
    try client.receiveUpdate();
    std.Thread.sleep(300 * std.time.ns_per_ms);
    try client.requestUpdate(true);
    try client.receiveUpdate();

    if (client.getClipboard()) |text| {
        return textContent(allocator, text);
    }

    return textContent(allocator, "(clipboard empty — no ServerCutText received yet)");
}

fn toolOcrRegion(allocator: std.mem.Allocator, arguments: ?JsonValue) !JsonValue {
    const args = if (arguments) |a| (if (a == .object) a.object else return error.InvalidArgument) else return error.InvalidArgument;

    const x = getInt(args, "x") orelse return error.InvalidArgument;
    const y = getInt(args, "y") orelse return error.InvalidArgument;
    const w = getInt(args, "w") orelse return error.InvalidArgument;
    const h = getInt(args, "h") orelse return error.InvalidArgument;

    // Build extra params JSON
    var extra: []u8 = undefined;
    if (getString(args, "lang")) |lang| {
        const escaped_lang = try helper.jsonEscape(allocator, lang);
        defer allocator.free(escaped_lang);
        extra = try std.fmt.allocPrint(allocator, "\"x\":{d},\"y\":{d},\"w\":{d},\"h\":{d},\"lang\":\"{s}\"", .{ x, y, w, h, escaped_lang });
    } else {
        extra = try std.fmt.allocPrint(allocator, "\"x\":{d},\"y\":{d},\"w\":{d},\"h\":{d}", .{ x, y, w, h });
    }
    defer allocator.free(extra);

    const response = callHelper(allocator, arguments, "ocr_region", extra) catch |err| {
        if (err == error.FramebufferNotReady) return helperNotConfigured(allocator);
        return helperNotAvailable(allocator);
    };
    return textContent(allocator, response);
}

fn toolUiTree(allocator: std.mem.Allocator, arguments: ?JsonValue) !JsonValue {
    var extra: []u8 = undefined;

    if (arguments) |args| {
        if (args == .object) {
            const depth = getInt(args.object, "depth") orelse 3;
            if (getInt(args.object, "pid")) |pid| {
                extra = try std.fmt.allocPrint(allocator, "\"depth\":{d},\"pid\":{d}", .{ depth, pid });
            } else {
                extra = try std.fmt.allocPrint(allocator, "\"depth\":{d}", .{depth});
            }
        } else {
            extra = try std.fmt.allocPrint(allocator, "\"depth\":3", .{});
        }
    } else {
        extra = try std.fmt.allocPrint(allocator, "\"depth\":3", .{});
    }
    defer allocator.free(extra);

    const response = callHelper(allocator, arguments, "ui_tree", extra) catch |err| {
        if (err == error.FramebufferNotReady) return helperNotConfigured(allocator);
        return helperNotAvailable(allocator);
    };
    return textContent(allocator, response);
}

fn toolUiElementText(allocator: std.mem.Allocator, arguments: ?JsonValue) !JsonValue {
    const args = if (arguments) |a| (if (a == .object) a.object else return error.InvalidArgument) else return error.InvalidArgument;

    const name = getString(args, "name");
    const automation_id = getString(args, "automation_id");
    const control_type = getString(args, "control_type");

    if (name == null and automation_id == null) return error.InvalidArgument;

    // Build extra params
    var parts = std.ArrayList(u8){};
    defer parts.deinit(allocator);

    if (name) |n| {
        const escaped = try helper.jsonEscape(allocator, n);
        defer allocator.free(escaped);
        const chunk = try std.fmt.allocPrint(allocator, "\"name\":\"{s}\"", .{escaped});
        defer allocator.free(chunk);
        try parts.appendSlice(allocator, chunk);
    }
    if (automation_id) |aid| {
        if (parts.items.len > 0) try parts.append(allocator, ',');
        const escaped = try helper.jsonEscape(allocator, aid);
        defer allocator.free(escaped);
        const chunk = try std.fmt.allocPrint(allocator, "\"automation_id\":\"{s}\"", .{escaped});
        defer allocator.free(chunk);
        try parts.appendSlice(allocator, chunk);
    }
    if (control_type) |ct| {
        if (parts.items.len > 0) try parts.append(allocator, ',');
        const escaped = try helper.jsonEscape(allocator, ct);
        defer allocator.free(escaped);
        const chunk = try std.fmt.allocPrint(allocator, "\"control_type\":\"{s}\"", .{escaped});
        defer allocator.free(chunk);
        try parts.appendSlice(allocator, chunk);
    }

    const extra = try allocator.dupe(u8, parts.items);
    defer allocator.free(extra);

    const response = callHelper(allocator, arguments, "ui_element_text", extra) catch |err| {
        if (err == error.FramebufferNotReady) return helperNotConfigured(allocator);
        return helperNotAvailable(allocator);
    };
    return textContent(allocator, response);
}

fn toolUiClickElement(allocator: std.mem.Allocator, arguments: ?JsonValue) !JsonValue {
    const args = if (arguments) |a| (if (a == .object) a.object else return error.InvalidArgument) else return error.InvalidArgument;

    const name = getString(args, "name");
    const automation_id = getString(args, "automation_id");
    const control_type = getString(args, "control_type");

    if (name == null and automation_id == null) return error.InvalidArgument;

    // Build extra params
    var parts = std.ArrayList(u8){};
    defer parts.deinit(allocator);

    if (name) |n| {
        const escaped = try helper.jsonEscape(allocator, n);
        defer allocator.free(escaped);
        const chunk = try std.fmt.allocPrint(allocator, "\"name\":\"{s}\"", .{escaped});
        defer allocator.free(chunk);
        try parts.appendSlice(allocator, chunk);
    }
    if (automation_id) |aid| {
        if (parts.items.len > 0) try parts.append(allocator, ',');
        const escaped = try helper.jsonEscape(allocator, aid);
        defer allocator.free(escaped);
        const chunk = try std.fmt.allocPrint(allocator, "\"automation_id\":\"{s}\"", .{escaped});
        defer allocator.free(chunk);
        try parts.appendSlice(allocator, chunk);
    }
    if (control_type) |ct| {
        if (parts.items.len > 0) try parts.append(allocator, ',');
        const escaped = try helper.jsonEscape(allocator, ct);
        defer allocator.free(escaped);
        const chunk = try std.fmt.allocPrint(allocator, "\"control_type\":\"{s}\"", .{escaped});
        defer allocator.free(chunk);
        try parts.appendSlice(allocator, chunk);
    }

    const extra = try allocator.dupe(u8, parts.items);
    defer allocator.free(extra);

    const response = callHelper(allocator, arguments, "ui_click_element", extra) catch |err| {
        if (err == error.FramebufferNotReady) return helperNotConfigured(allocator);
        return helperNotAvailable(allocator);
    };
    return textContent(allocator, response);
}
