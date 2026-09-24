const std = @import("std");
const rfb_client = @import("../rfb/client.zig");
const keysym = @import("../rfb/keysym.zig");
const registry_mod = @import("../registry.zig");
const image = @import("../image.zig");
const helper = @import("../helper.zig");
const cal = @import("../calibration.zig");

const log = std.log.scoped(.tools);
const JsonValue = std.json.Value;

/// Connection pool — maps endpoint ID to active RFB client
var connections: ?*ConnectionPool = null;
pub var helper_connections: ?*HelperPool = null;
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
            entry.value_ptr.deinit();
        }
        self.entries.deinit();
    }

    pub fn getOrCreate(self: *HelperPool, ep: *const registry_mod.Endpoint, password: ?[]const u8) !*helper.HelperConnection {
        if (self.entries.getPtr(ep.id)) |conn| return conn;

        const conn = try helper.HelperConnection.init(self.allocator, ep.host, ep.helper_port, password);
        try self.entries.put(ep.id, conn);
        return self.entries.getPtr(ep.id).?;
    }

    /// Shutdown the read side of all active helper sockets.
    /// This unblocks any worker thread stuck in recv(), causing it to
    /// see EOF and exit cleanly. Used by the R6 timeout handler.
    pub fn shutdownAll(self: *HelperPool) void {
        var it = self.entries.iterator();
        while (it.next()) |entry| {
            entry.value_ptr.shutdown();
        }
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

/// MCP resource reader — returns base64-encoded content for a given URI
pub const ResourceContent = struct {
    blob: []const u8, // base64-encoded
    mime_type: []const u8,
    meta_text: []const u8, // resolution metadata
};

pub fn readResource(allocator: std.mem.Allocator, uri: []const u8) !ResourceContent {
    // vnc://screenshot or vnc://screenshot/{endpoint}
    if (std.mem.startsWith(u8, uri, "vnc://screenshot")) {
        // Prefer agent DXGI capture; fall back to RFB framebuffer (agent
        // absent/error or black-frame guard).
        if (tryAgentScreenshotResource(allocator)) |agent_res| {
            return agent_res;
        }
        const client = try getClient(null); // default endpoint
        const fb = try client.screenshot();
        const jpeg = try image.encodeJpeg(allocator, fb, 90);
        defer allocator.free(jpeg);

        const base64_encoder = std.base64.standard;
        const encoded_len = base64_encoder.Encoder.calcSize(jpeg.len);
        const encoded = try allocator.alloc(u8, encoded_len);
        _ = base64_encoder.Encoder.encode(encoded, jpeg);

        const meta = try std.fmt.allocPrint(allocator, "Resolution: {d}x{d} pixels", .{ fb.width, fb.height });

        return ResourceContent{
            .blob = encoded,
            .mime_type = "image/jpeg",
            .meta_text = meta,
        };
    }

    return error.ResourceNotFound;
}

fn tryAgentScreenshotResource(allocator: std.mem.Allocator) ?ResourceContent {
    const params = std.fmt.allocPrint(allocator, "\"quality\":90", .{}) catch return null;
    defer allocator.free(params);

    const response = callHelper(allocator, null, "screenshot", params) catch return null;

    const parsed = std.json.parseFromSlice(std.json.Value, allocator, response, .{
        .ignore_unknown_fields = true,
    }) catch return null;
    defer parsed.deinit();

    const root = if (parsed.value == .object) parsed.value.object else return null;
    const status = if (root.get("status")) |s| (if (s == .string) s.string else null) else null;
    if (status == null or !std.mem.eql(u8, status.?, "ok")) return null;

    const data = if (root.get("data")) |d| (if (d == .object) d.object else null) else null;
    if (data == null) return null;

    const content_b64_ref = if (data.?.get("content")) |c| (if (c == .string) c.string else null) else null;
    if (content_b64_ref == null) return null;
    if (isSuspiciouslyBlack(content_b64_ref.?)) return null;

    // Copy base64 content out of the parse arena (freed by deferred parsed.deinit)
    const content_b64 = allocator.dupe(u8, content_b64_ref.?) catch return null;

    var res_w: i64 = if (data.?.get("width")) |w| (if (w == .integer) w.integer else 0) else 0;
    var res_h: i64 = if (data.?.get("height")) |h| (if (h == .integer) h.integer else 0) else 0;

    if (res_w <= 0 or res_h <= 0) {
        getScreenDims(allocator, null, &res_w, &res_h);
    }

    const meta = std.fmt.allocPrint(allocator, "Resolution: {d}x{d} pixels (WinMCP agent DXGI capture)", .{ res_w, res_h }) catch return null;

    return ResourceContent{
        .blob = content_b64,
        .mime_type = "image/jpeg",
        .meta_text = meta,
    };
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
    } else if (std.mem.eql(u8, name, "vnc_capture_burst")) {
        return toolCaptureBurst(allocator, arguments);
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
    } else if (std.mem.eql(u8, name, "vnc_scroll")) {
        return toolScroll(allocator, arguments);
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
    } else if (std.mem.eql(u8, name, "vnc_shell")) {
        return toolShell(allocator, arguments);
    } else if (std.mem.eql(u8, name, "vnc_browser_eval")) {
        return toolBrowserEval(allocator, arguments);
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
    } else if (std.mem.eql(u8, name, "vnc_registry_read")) {
        return toolRegistryRead(allocator, arguments);
    } else if (std.mem.eql(u8, name, "vnc_registry_write")) {
        return toolRegistryWrite(allocator, arguments);
    } else if (std.mem.eql(u8, name, "vnc_registry_list")) {
        return toolRegistryList(allocator, arguments);
    } else if (std.mem.eql(u8, name, "vnc_list_processes")) {
        return toolListProcesses(allocator, arguments);
    } else if (std.mem.eql(u8, name, "vnc_kill_process")) {
        return toolKillProcess(allocator, arguments);
    } else if (std.mem.eql(u8, name, "vnc_list_services")) {
        return toolListServices(allocator, arguments);
    } else if (std.mem.eql(u8, name, "vnc_service_control")) {
        return toolServiceControl(allocator, arguments);
    } else if (std.mem.eql(u8, name, "vnc_calibrate")) {
        return toolCalibrate(allocator, arguments);
    } else {
        return textContent(allocator, "Unknown tool");
    }
}

fn toolScreenshot(allocator: std.mem.Allocator, arguments: ?JsonValue) !JsonValue {
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

    // Prefer the WinMCP agent's DXGI capture; fall back to RFB framebuffer
    // when the agent is absent, errors, or returns a black frame (DXGI
    // session-state pathology guard).
    if (tryAgentScreenshot(allocator, arguments, quality)) |agent_result| {
        return agent_result;
    }

    // VNC framebuffer capture (fallback and non-agent endpoints)
    const client = try getClient(arguments);
    const fb = try client.screenshot();
    const jpeg = try image.encodeJpeg(allocator, fb, quality);
    defer allocator.free(jpeg);

    // Include resolution metadata so AI agents can compute coordinates,
    // plus this client's calibration state for this endpoint/resolution.
    const ep = try getEndpoint(arguments);
    const status = cal.statusLine(allocator, ep.id, fb.width, fb.height);
    defer allocator.free(status);
    const meta = try std.fmt.allocPrint(allocator, "Resolution: {d}x{d} pixels\n{s}", .{ fb.width, fb.height, status });
    return imageContentWithMeta(allocator, jpeg, meta);
}

/// vnc_capture_burst (#21) — rapid frame sequence for transient UI states
/// (toasts, hover highlights, animations) whose lifetime is shorter than
/// one screenshot round trip. One non-incremental baseline frame, then
/// ticks on a fixed interval; between ticks incremental server updates are
/// pumped via kqueue waits, so mostly-static screens cost almost nothing
/// per frame. Frames are snapshotted (region crop + scale) at tick time
/// and JPEG-encoded after capture, with quality degraded if the payload
/// cap is exceeded.
fn toolCaptureBurst(allocator: std.mem.Allocator, arguments: ?JsonValue) !JsonValue {
    var count: u32 = 15;
    var interval_ms: u32 = 333;
    var quality: u8 = 60;
    var scale: f32 = 0.5;
    var region: ?[4]u16 = null; // x, y, w, h in framebuffer pixels
    if (arguments) |args| {
        if (args == .object) {
            if (getInt(args.object, "count")) |v| count = @intCast(@max(1, @min(v, 30)));
            if (getInt(args.object, "interval_ms")) |v| interval_ms = @intCast(@max(50, @min(v, 5000)));
            if (getInt(args.object, "quality")) |v| quality = @intCast(@max(1, @min(v, 100)));
            if (getFloat(args.object, "scale")) |v| scale = @floatCast(std.math.clamp(v, 0.05, 4.0));
            const rx_v = getInt(args.object, "region_x");
            const ry_v = getInt(args.object, "region_y");
            const rw_v = getInt(args.object, "region_w");
            const rh_v = getInt(args.object, "region_h");
            if (rx_v != null and ry_v != null and rw_v != null and rh_v != null and
                rw_v.? > 0 and rh_v.? > 0)
            {
                region = .{
                    @intCast(@max(0, @min(rx_v.?, 65535))),
                    @intCast(@max(0, @min(ry_v.?, 65535))),
                    @intCast(@min(rw_v.?, 65535)),
                    @intCast(@min(rh_v.?, 65535)),
                };
            }
        }
    }

    const client = try getClient(arguments);

    // Baseline: full non-incremental frame so every pixel is defined
    try client.requestUpdate(false);
    try client.receiveUpdate();
    if (client.framebuffer == null) return error.FramebufferNotReady;
    const fb_w = client.framebuffer.?.width;
    const fb_h = client.framebuffer.?.height;
    const rect: [4]u16 = region orelse .{ 0, 0, fb_w, fb_h };

    var timer = try std.time.Timer.start();

    var snaps = std.ArrayList([]u8){};
    defer for (snaps.items) |s| allocator.free(s);
    var snap_w: u16 = 0;
    var snap_h: u16 = 0;
    var stamps = std.ArrayList(i64){};
    defer stamps.deinit(allocator);

    var i: u32 = 0;
    while (i < count) : (i += 1) {
        if (i > 0) {
            // Request deltas up front so they stream in DURING the wait,
            // then pump updates as they arrive until the tick.
            try client.requestUpdate(true);
            const target: i64 = @as(i64, i) * interval_ms;
            while (true) {
                const now: i64 = @intCast(timer.read() / std.time.ns_per_ms);
                const remain = target - now;
                if (remain <= 0) break;
                if (client.waitForData(@intCast(@min(remain, 50)))) {
                    client.receiveUpdate() catch break;
                }
            }
        }
        const fbp = &(client.framebuffer orelse return error.FramebufferNotReady);
        try snaps.append(allocator, try image.snapshotRegionRgb(allocator, fbp, rect[0], rect[1], rect[2], rect[3], scale, &snap_w, &snap_h));
        try stamps.append(allocator, @intCast(timer.read() / std.time.ns_per_ms));
    }

    // Encode; degrade quality if the payload cap (6MB raw, ~8MB base64) trips
    const payload_cap: usize = 6_000_000;
    var degrade_note: []const u8 = "";
    var q = quality;
    var jpegs = std.ArrayList([]u8){};
    defer for (jpegs.items) |j| allocator.free(j);
    while (true) {
        for (snaps.items) |s| {
            try jpegs.append(allocator, try image.encodeJpegRgb(allocator, s, snap_w, snap_h, q));
        }
        var total: usize = 0;
        for (jpegs.items) |j| total += j.len;
        if (total <= payload_cap) break;
        if (q <= 25) {
            degrade_note = try std.fmt.allocPrint(allocator, "\nNOTE: payload cap hit — kept quality {d}; response may be large.", .{q});
            break;
        }
        for (jpegs.items) |j| allocator.free(j);
        jpegs.clearRetainingCapacity();
        q = @max(25, q - 15);
        degrade_note = try std.fmt.allocPrint(allocator, "\nNOTE: quality reduced to {d} (payload cap).", .{q});
    }

    var meta_buf = std.ArrayList(u8){};
    defer meta_buf.deinit(allocator);
    const header = try std.fmt.allocPrint(allocator, "Burst: {d} frames, interval {d} ms, region {d}x{d}+{d}+{d} @ {d:.2}x scale, Resolution: {d}x{d} framebuffer\nFrames (ms from start):", .{ snaps.items.len, interval_ms, rect[2], rect[3], rect[0], rect[1], scale, fb_w, fb_h });
    defer allocator.free(header);
    try meta_buf.appendSlice(allocator, header);
    for (stamps.items, 0..) |t, n| {
        const s = try std.fmt.allocPrint(allocator, " {d}:{d}", .{ n + 1, t });
        defer allocator.free(s);
        try meta_buf.appendSlice(allocator, s);
    }
    try meta_buf.appendSlice(allocator, degrade_note);
    try meta_buf.append(allocator, '\n');
    const ep = try getEndpoint(arguments);
    const status = cal.statusLine(allocator, ep.id, fb_w, fb_h);
    defer allocator.free(status);
    try meta_buf.appendSlice(allocator, status);

    const base64_encoder = std.base64.standard;
    var content_arr = std.json.Array.init(allocator);
    var text_item = std.json.ObjectMap.init(allocator);
    try text_item.put("type", JsonValue{ .string = "text" });
    try text_item.put("text", JsonValue{ .string = try allocator.dupe(u8, meta_buf.items) });
    try content_arr.append(JsonValue{ .object = text_item });
    for (jpegs.items) |j| {
        const encoded_len = base64_encoder.Encoder.calcSize(j.len);
        const encoded = try allocator.alloc(u8, encoded_len);
        _ = base64_encoder.Encoder.encode(encoded, j);
        var img_item = std.json.ObjectMap.init(allocator);
        try img_item.put("type", JsonValue{ .string = "image" });
        try img_item.put("data", JsonValue{ .string = encoded });
        try img_item.put("mimeType", JsonValue{ .string = "image/jpeg" });
        try content_arr.append(JsonValue{ .object = img_item });
    }
    var result = std.json.ObjectMap.init(allocator);
    try result.put("content", JsonValue{ .array = content_arr });
    return JsonValue{ .object = result };
}

/// Try to capture a screenshot via the WinMCP agent's native DXGI backend.
/// Returns null if the agent is unavailable, has no DLL, or the response
/// can't be parsed — the caller should fall back to VNC framebuffer.
/// Heuristic guard for the DXGI session-state pathology: an all-black
/// 1918×968 JPEG at q≤90 is a few KB, a real desktop never under 32KB.
/// False positives (legit dark screens) merely fall back to RFB, which
/// returns the same pixels — never a wrong image.
fn isSuspiciouslyBlack(content_b64: []const u8) bool {
    const decoded_len = content_b64.len * 3 / 4;
    return decoded_len < 32768;
}

fn tryAgentScreenshot(allocator: std.mem.Allocator, arguments: ?JsonValue, quality: u8) ?JsonValue {
    const params = std.fmt.allocPrint(allocator, "\"quality\":{d}", .{quality}) catch return null;
    defer allocator.free(params);

    const response = callHelper(allocator, arguments, "screenshot", params) catch return null;
    // response is allocator-owned, not freed here (same pattern as other tools)

    const parsed = std.json.parseFromSlice(std.json.Value, allocator, response, .{
        .ignore_unknown_fields = true,
    }) catch return null;
    defer parsed.deinit();

    const root = if (parsed.value == .object) parsed.value.object else return null;
    const status = if (root.get("status")) |s| (if (s == .string) s.string else null) else null;
    if (status == null or !std.mem.eql(u8, status.?, "ok")) return null;

    const data = if (root.get("data")) |d| (if (d == .object) d.object else null) else null;
    if (data == null) return null;

    const content_b64_ref = if (data.?.get("content")) |c| (if (c == .string) c.string else null) else null;
    if (content_b64_ref == null) return null;
    if (isSuspiciouslyBlack(content_b64_ref.?)) return null;

    // Copy base64 content out of the parse arena (freed by deferred parsed.deinit)
    const content_b64 = allocator.dupe(u8, content_b64_ref.?) catch return null;

    // Get resolution from agent response (w/h may be 0 for full-screen)
    var res_w: i64 = if (data.?.get("width")) |w| (if (w == .integer) w.integer else 0) else 0;
    var res_h: i64 = if (data.?.get("height")) |h| (if (h == .integer) h.integer else 0) else 0;

    // If agent reported 0×0 (full-screen capture), get actual dims from screen_info
    if (res_w <= 0 or res_h <= 0) {
        getScreenDims(allocator, arguments, &res_w, &res_h);
    }

    // Build MCP image content with pre-encoded base64 JPEG
    const ep = getEndpoint(arguments) catch return null;
    const cal_status = cal.statusLine(allocator, ep.id, @intCast(@max(0, @min(res_w, 65535))), @intCast(@max(0, @min(res_h, 65535))));
    defer allocator.free(cal_status);
    const meta = std.fmt.allocPrint(allocator, "Resolution: {d}x{d} pixels (WinMCP agent DXGI capture)\n{s}", .{ res_w, res_h, cal_status }) catch return null;

    var content_arr = std.json.Array.init(allocator);

    var text_item = std.json.ObjectMap.init(allocator);
    text_item.put("type", JsonValue{ .string = "text" }) catch return null;
    text_item.put("text", JsonValue{ .string = meta }) catch return null;
    content_arr.append(JsonValue{ .object = text_item }) catch return null;

    var img_item = std.json.ObjectMap.init(allocator);
    img_item.put("type", JsonValue{ .string = "image" }) catch return null;
    img_item.put("data", JsonValue{ .string = content_b64 }) catch return null;
    img_item.put("mimeType", JsonValue{ .string = "image/jpeg" }) catch return null;
    content_arr.append(JsonValue{ .object = img_item }) catch return null;

    var result = std.json.ObjectMap.init(allocator);
    result.put("content", JsonValue{ .array = content_arr }) catch return null;
    return JsonValue{ .object = result };
}

/// Extract primary monitor dimensions from screen_info agent response.
/// Response format: {"status":"ok","data":{"monitors":[{"x":0,"y":0,"w":1918,"h":968,"primary":true}],"dpi":96}}
fn getScreenDims(allocator: std.mem.Allocator, arguments: ?JsonValue, w: *i64, h: *i64) void {
    const si_resp = callHelper(allocator, arguments, "screen_info", null) catch return;
    const si_parsed = std.json.parseFromSlice(std.json.Value, allocator, si_resp, .{
        .ignore_unknown_fields = true,
    }) catch return;
    defer si_parsed.deinit();

    const root = if (si_parsed.value == .object) si_parsed.value.object else return;
    const data = if (root.get("data")) |d| (if (d == .object) d.object else null) else null;
    if (data == null) return;
    const monitors = if (data.?.get("monitors")) |m| (if (m == .array) m.array else null) else null;
    if (monitors == null or monitors.?.items.len == 0) return;

    // Use primary monitor (first entry)
    const mon = if (monitors.?.items[0] == .object) monitors.?.items[0].object else return;
    if (mon.get("w")) |mw| if (mw == .integer) {
        w.* = mw.integer;
    };
    if (mon.get("h")) |mh| if (mh == .integer) {
        h.* = mh.integer;
    };
}

fn toolProbe(allocator: std.mem.Allocator, arguments: ?JsonValue) !JsonValue {
    const args = if (arguments) |a| (if (a == .object) a.object else return error.InvalidArgument) else return error.InvalidArgument;

    const x_in: i64 = getInt(args, "x") orelse return error.InvalidArgument;
    const y_in: i64 = getInt(args, "y") orelse return error.InvalidArgument;

    // Cognitive forcing parameters — parsed but not acted on.
    // Their presence in the schema encourages the agent to validate
    // that coordinates were derived from a known-resolution image.
    _ = getBool(args, "used_full_resolution");
    _ = getString(args, "coordinate_source_resolution");
    _ = getBool(args, "used_grid");

    const resolved = try resolveCoords(allocator, args, arguments, x_in, y_in);
    if (std.mem.startsWith(u8, resolved.note, "ERROR")) return textContent(allocator, resolved.note);
    const x = resolved.x;
    const y = resolved.y;

    const client = try getClient(arguments);
    const fb = try client.screenshot();

    const jpeg = try image.encodeJpegWithProbe(allocator, fb, 75, x, y);
    defer allocator.free(jpeg);

    const ep = try getEndpoint(arguments);
    const status = cal.statusLine(allocator, ep.id, fb.width, fb.height);
    defer allocator.free(status);
    const meta = try std.fmt.allocPrint(allocator, "Center of probe marker at ({d}, {d}) \u{2014} Resolution: {d}x{d} pixels{s}{s}\n{s}", .{
        x,                                                                         y,             fb.width, fb.height,
        if (resolved.note.len > 0) @as([]const u8, "\n") else @as([]const u8, ""), resolved.note, status,
    });
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

    const ep = try getEndpoint(arguments);
    const status = cal.statusLine(allocator, ep.id, fb.width, fb.height);
    defer allocator.free(status);
    try meta_buf.appendSlice(allocator, status);

    const meta = try allocator.dupe(u8, meta_buf.items);
    return imageContentWithMeta(allocator, jpeg, meta);
}

fn toolClick(allocator: std.mem.Allocator, arguments: ?JsonValue) !JsonValue {
    const args = if (arguments) |a| (if (a == .object) a.object else return error.InvalidArgument) else return error.InvalidArgument;

    const x_in: i64 = getInt(args, "x") orelse return error.InvalidArgument;
    const y_in: i64 = getInt(args, "y") orelse return error.InvalidArgument;

    const resolved = try resolveCoords(allocator, args, arguments, x_in, y_in);
    if (std.mem.startsWith(u8, resolved.note, "ERROR")) return textContent(allocator, resolved.note);
    const x: u16 = resolved.x;
    const y: u16 = resolved.y;

    const button_str = getString(args, "button") orelse "left";
    const double_click = getBool(args, "double");

    // Provenance parameters — parsed but not acted on.
    // Their presence in the schema encourages the agent to validate
    // coordinates via probe before committing to a click.
    _ = getBool(args, "probe_validated");
    _ = getBool(args, "coordinates_confirmed");

    // Try agent-based click (SendInput) first — prefer native input over VNC
    const agent_click = blk: {
        const click_params = std.fmt.allocPrint(allocator, "\"x\":{d},\"y\":{d},\"button\":\"{s}\",\"double\":{s}", .{ x, y, button_str, if (double_click) @as([]const u8, "1") else @as([]const u8, "0") }) catch break :blk false;
        defer allocator.free(click_params);
        _ = callHelper(allocator, arguments, "mouse_click", click_params) catch break :blk false;
        break :blk true;
    };

    if (!agent_click) {
        // Fall back to VNC RFB pointer events
        const button_mask: u8 = if (std.mem.eql(u8, button_str, "right"))
            4
        else if (std.mem.eql(u8, button_str, "middle"))
            2
        else
            1;

        const client = try getClient(arguments);

        try client.sendPointerEvent(x, y, button_mask);
        std.Thread.sleep(50 * std.time.ns_per_ms);
        try client.sendPointerEvent(x, y, 0);

        if (double_click) {
            std.Thread.sleep(50 * std.time.ns_per_ms);
            try client.sendPointerEvent(x, y, button_mask);
            std.Thread.sleep(50 * std.time.ns_per_ms);
            try client.sendPointerEvent(x, y, 0);
        }
    }

    // Visual confirmation: draw marker at click point + capture screenshot
    // Best-effort — if helper is unavailable, still return the click result
    const marker_params = try std.fmt.allocPrint(allocator, "\"x\":{d},\"y\":{d}", .{ x, y });
    defer allocator.free(marker_params);
    _ = callHelper(allocator, arguments, "click_marker", marker_params) catch {};

    // Wait for marker to render + screen to settle after click
    std.Thread.sleep(300 * std.time.ns_per_ms);

    // Capture confirmation screenshot
    const fb = (getClient(arguments) catch null);
    const screenshot_data = if (fb) |c| (c.screenshot() catch null) else null;
    const jpeg = if (screenshot_data) |sd| (image.encodeJpeg(allocator, sd, 65) catch null) else null;

    if (jpeg) |j| {
        defer allocator.free(j);

        const ep = try getEndpoint(arguments);
        const click_msg = if (fb) |c| blk: {
            const status = cal.statusLine(allocator, ep.id, c.width, c.height);
            defer allocator.free(status);
            break :blk try std.fmt.allocPrint(allocator, "Clicked at ({d}, {d}) \u{2014} Center of yellow marker shows click location{s}{s}\n{s}", .{
                x,                                                                         y,
                if (resolved.note.len > 0) @as([]const u8, "\n") else @as([]const u8, ""), resolved.note,
                status,
            });
        } else try std.fmt.allocPrint(allocator, "Clicked at ({d}, {d}) \u{2014} Center of yellow marker shows click location{s}{s}", .{
            x,                                                                         y,
            if (resolved.note.len > 0) @as([]const u8, "\n") else @as([]const u8, ""), resolved.note,
        });

        const base64_encoder = std.base64.standard;
        const encoded_len = base64_encoder.Encoder.calcSize(j.len);
        const encoded = try allocator.alloc(u8, encoded_len);
        _ = base64_encoder.Encoder.encode(encoded, j);

        var content_arr = std.json.Array.init(allocator);

        var text_item = std.json.ObjectMap.init(allocator);
        try text_item.put("type", JsonValue{ .string = "text" });
        try text_item.put("text", JsonValue{ .string = click_msg });
        try content_arr.append(JsonValue{ .object = text_item });

        var img_item = std.json.ObjectMap.init(allocator);
        try img_item.put("type", JsonValue{ .string = "image" });
        try img_item.put("data", JsonValue{ .string = encoded });
        try img_item.put("mimeType", JsonValue{ .string = "image/jpeg" });
        try content_arr.append(JsonValue{ .object = img_item });

        var result = std.json.ObjectMap.init(allocator);
        try result.put("content", JsonValue{ .array = content_arr });
        return JsonValue{ .object = result };
    }

    const msg = try std.fmt.allocPrint(allocator, "Clicked at ({d}, {d}){s}{s}", .{
        x,                                                                         y,
        if (resolved.note.len > 0) @as([]const u8, "\n") else @as([]const u8, ""), resolved.note,
    });
    return textContent(allocator, msg);
}

fn toolTypeText(allocator: std.mem.Allocator, arguments: ?JsonValue) !JsonValue {
    const args = if (arguments) |a| (if (a == .object) a.object else return error.InvalidArgument) else return error.InvalidArgument;
    const text = getString(args, "text") orelse return error.InvalidArgument;

    // Try agent-based typing (SendInput KEYEVENTF_UNICODE) first — full Unicode support
    const agent_typed = blk: {
        const escaped = helper.jsonEscape(allocator, text) catch break :blk false;
        defer allocator.free(escaped);
        const params = std.fmt.allocPrint(allocator, "\"text\":\"{s}\"", .{escaped}) catch break :blk false;
        defer allocator.free(params);
        _ = callHelper(allocator, arguments, "type_text", params) catch break :blk false;
        break :blk true;
    };

    if (!agent_typed) {
        // Fall back to VNC keysym events
        const client = try getClient(arguments);

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
    }

    return textContent(allocator, "Text typed");
}

fn toolKeyPress(allocator: std.mem.Allocator, arguments: ?JsonValue) !JsonValue {
    const args = if (arguments) |a| (if (a == .object) a.object else return error.InvalidArgument) else return error.InvalidArgument;
    const keys_str = getString(args, "keys") orelse return error.InvalidArgument;

    // Try agent-based key press (SendInput) first
    const agent_pressed = blk: {
        const escaped = helper.jsonEscape(allocator, keys_str) catch break :blk false;
        defer allocator.free(escaped);
        const params = std.fmt.allocPrint(allocator, "\"keys\":\"{s}\"", .{escaped}) catch break :blk false;
        defer allocator.free(params);
        _ = callHelper(allocator, arguments, "key_press", params) catch break :blk false;
        break :blk true;
    };

    if (!agent_pressed) {
        // Fall back to VNC keysym events
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
            main_key = keysym.modifierKeysym(main_part);
        }

        const mk = main_key orelse return textContent(allocator, "Unknown key");

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
    }

    return textContent(allocator, "Key press sent");
}

fn toolMoveMouse(allocator: std.mem.Allocator, arguments: ?JsonValue) !JsonValue {
    const args = if (arguments) |a| (if (a == .object) a.object else return error.InvalidArgument) else return error.InvalidArgument;

    const x_in: i64 = getInt(args, "x") orelse return error.InvalidArgument;
    const y_in: i64 = getInt(args, "y") orelse return error.InvalidArgument;

    const resolved = try resolveCoords(allocator, args, arguments, x_in, y_in);
    if (std.mem.startsWith(u8, resolved.note, "ERROR")) return textContent(allocator, resolved.note);
    const x: u16 = resolved.x;
    const y: u16 = resolved.y;

    // Try agent-based mouse move (SetCursorPos) first
    const agent_moved = blk: {
        const params = std.fmt.allocPrint(allocator, "\"x\":{d},\"y\":{d}", .{ x, y }) catch break :blk false;
        defer allocator.free(params);
        _ = callHelper(allocator, arguments, "mouse_move", params) catch break :blk false;
        break :blk true;
    };

    if (!agent_moved) {
        // Fall back to VNC pointer event
        const client = try getClient(arguments);
        try client.sendPointerEvent(x, y, 0);
    }

    const msg = try std.fmt.allocPrint(allocator, "Mouse moved to ({d},{d}){s}{s}", .{
        x,                                                                         y,
        if (resolved.note.len > 0) @as([]const u8, "\n") else @as([]const u8, ""), resolved.note,
    });
    return textContent(allocator, msg);
}

fn toolDrag(allocator: std.mem.Allocator, arguments: ?JsonValue) !JsonValue {
    const args = if (arguments) |a| (if (a == .object) a.object else return error.InvalidArgument) else return error.InvalidArgument;

    const x1_in: i64 = getInt(args, "x1") orelse return error.InvalidArgument;
    const y1_in: i64 = getInt(args, "y1") orelse return error.InvalidArgument;
    const x2_in: i64 = getInt(args, "x2") orelse return error.InvalidArgument;
    const y2_in: i64 = getInt(args, "y2") orelse return error.InvalidArgument;

    const from = try resolveCoords(allocator, args, arguments, x1_in, y1_in);
    if (std.mem.startsWith(u8, from.note, "ERROR")) return textContent(allocator, from.note);
    const to = try resolveCoords(allocator, args, arguments, x2_in, y2_in);
    const x1: u16 = from.x;
    const y1: u16 = from.y;
    const x2: u16 = to.x;
    const y2: u16 = to.y;

    // Try agent-based drag (SendInput) first
    const agent_dragged = blk: {
        const params = std.fmt.allocPrint(allocator, "\"x1\":{d},\"y1\":{d},\"x2\":{d},\"y2\":{d}", .{ x1, y1, x2, y2 }) catch break :blk false;
        defer allocator.free(params);
        _ = callHelper(allocator, arguments, "mouse_drag", params) catch break :blk false;
        break :blk true;
    };

    if (!agent_dragged) {
        // Fall back to VNC pointer events
        const client = try getClient(arguments);

        try client.sendPointerEvent(x1, y1, 0);
        std.Thread.sleep(50 * std.time.ns_per_ms);
        try client.sendPointerEvent(x1, y1, 1);
        std.Thread.sleep(50 * std.time.ns_per_ms);

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
        try client.sendPointerEvent(x2, y2, 0);
    }

    const drag_note: []const u8 = if (from.note.len > 0 or to.note.len > 0)
        try std.fmt.allocPrint(allocator, "\n{s} -> {s}", .{ from.note, to.note })
    else
        "";
    const msg = try std.fmt.allocPrint(allocator, "Drag completed ({d},{d}) -> ({d},{d}){s}", .{ x1, y1, x2, y2, drag_note });
    return textContent(allocator, msg);
}

fn toolScroll(allocator: std.mem.Allocator, arguments: ?JsonValue) !JsonValue {
    const args = if (arguments) |a| (if (a == .object) a.object else return error.InvalidArgument) else return error.InvalidArgument;

    const x_in: i64 = getInt(args, "x") orelse return error.InvalidArgument;
    const y_in: i64 = getInt(args, "y") orelse return error.InvalidArgument;
    const amount = getInt(args, "amount") orelse return error.InvalidArgument;

    const resolved = try resolveCoords(allocator, args, arguments, x_in, y_in);
    if (std.mem.startsWith(u8, resolved.note, "ERROR")) return textContent(allocator, resolved.note);
    const x: u16 = resolved.x;
    const y: u16 = resolved.y;

    if (amount == 0) return textContent(allocator, "No scroll (amount=0)");

    const client = try getClient(arguments);

    // RFB button mask: bit 3 (value 8) = wheel up, bit 4 (value 16) = wheel down
    const button_mask: u8 = if (amount > 0) 8 else 16;
    const notches: usize = @intCast(if (amount > 0) amount else -amount);

    // Send one press+release per notch, with small delays between
    for (0..notches) |_| {
        try client.sendPointerEvent(x, y, button_mask);
        std.Thread.sleep(20 * std.time.ns_per_ms);
        try client.sendPointerEvent(x, y, 0);
        std.Thread.sleep(30 * std.time.ns_per_ms);
    }

    const msg = try std.fmt.allocPrint(allocator, "Scrolled {s} {d} notch(es) at ({d},{d}){s}{s}", .{
        if (amount > 0) @as([]const u8, "up") else @as([]const u8, "down"),
        notches,
        x,
        y,
        if (resolved.note.len > 0) @as([]const u8, "\n") else @as([]const u8, ""),
        resolved.note,
    });
    return textContent(allocator, msg);
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

/// Helper tool: call the WinMCP agent on the endpoint (persistent connection).
/// timeout_secs: override SO_RCVTIMEO for this call (0 = use default 30s).
fn callHelper(allocator: std.mem.Allocator, arguments: ?JsonValue, command: []const u8, extra_params: ?[]const u8) ![]u8 {
    return callHelperWithTimeout(allocator, arguments, command, extra_params, 0);
}

fn callHelperWithTimeout(allocator: std.mem.Allocator, arguments: ?JsonValue, command: []const u8, extra_params: ?[]const u8, timeout_secs: u32) ![]u8 {
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

    return conn.callWithTimeout(request, timeout_secs);
}

fn helperNotConfigured(allocator: std.mem.Allocator) !JsonValue {
    return textContent(allocator, "Helper agent not configured for this endpoint. Set helper_port in endpoints.json and run winmcp.exe on the target machine.");
}

fn helperNotAvailable(allocator: std.mem.Allocator) !JsonValue {
    return textContent(allocator, "Helper agent not available. Ensure winmcp.exe is running on the target machine.");
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
    return textContent(allocator, injectScreenDims(allocator, arguments, response));
}

fn toolActiveWindow(allocator: std.mem.Allocator, arguments: ?JsonValue) !JsonValue {
    const response = callHelper(allocator, arguments, "active_window", null) catch |err| {
        if (err == error.FramebufferNotReady) return helperNotConfigured(allocator);
        return helperNotAvailable(allocator);
    };
    return textContent(allocator, injectScreenDims(allocator, arguments, response));
}

/// Inject screen dimensions into a helper JSON response's "data" object.
/// Transforms {"status":"ok","data":{...}} → {"status":"ok","data":{"screen":{"w":W,"h":H},...}}
fn injectScreenDims(allocator: std.mem.Allocator, arguments: ?JsonValue, response: []const u8) []const u8 {
    var sw: i64 = 0;
    var sh: i64 = 0;
    getScreenDims(allocator, arguments, &sw, &sh);
    if (sw <= 0 or sh <= 0) return response;

    // Find "data":{ and inject screen field after the opening brace
    const needle = "\"data\":{";
    const pos = std.mem.indexOf(u8, response, needle) orelse return response;
    const insert_at = pos + needle.len;

    const injection = std.fmt.allocPrint(allocator, "\"screen\":{{\"w\":{d},\"h\":{d}}},", .{ sw, sh }) catch return response;
    const result = std.fmt.allocPrint(allocator, "{s}{s}{s}", .{ response[0..insert_at], injection, response[insert_at..] }) catch return response;
    return result;
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
        if (err == error.ReadTimeout) return textContent(allocator, "No window found for the given criteria (helper timed out). The process may not have a visible top-level window.");
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

    // Extract timeout parameter (milliseconds). Default 30000ms.
    const timeout_ms: u32 = if (getInt(args, "timeout")) |t|
        @intCast(@max(1000, @min(t, 300000)))
    else
        30000;

    // Convert to seconds for SO_RCVTIMEO, add 5s margin for helper overhead
    const socket_timeout_secs: u32 = (timeout_ms / 1000) + 5;

    // Build extra params
    var extra: []u8 = undefined;
    if (getInt(args, "timeout")) |t| {
        extra = try std.fmt.allocPrint(allocator, "\"cmd\":\"{s}\",\"timeout\":{d}", .{ escaped_cmd, t });
    } else {
        extra = try std.fmt.allocPrint(allocator, "\"cmd\":\"{s}\"", .{escaped_cmd});
    }
    defer allocator.free(extra);

    const response = callHelperWithTimeout(allocator, arguments, "run_command", extra, socket_timeout_secs) catch |err| {
        if (err == error.FramebufferNotReady) return helperNotConfigured(allocator);
        if (err == error.ReadTimeout) {
            const msg = std.fmt.allocPrint(allocator, "Command timed out after {d}ms. The command may still be running on the remote machine.", .{timeout_ms}) catch
                return helperNotAvailable(allocator);
            return textContent(allocator, msg);
        }
        return helperNotAvailable(allocator);
    };
    return textContent(allocator, response);
}

/// vnc_shell — script execution on the target. shell="powershell" (default)
/// runs in the agent's persistent PowerShell session: state ($variables,
/// Set-Location, imported modules) survives across calls, PowerShell quoting
/// is never mangled by cmd.exe, and repeat calls skip the 1-2s pwsh startup.
/// shell="cmd" routes to the stateless run_command path instead.
fn toolShell(allocator: std.mem.Allocator, arguments: ?JsonValue) !JsonValue {
    const args = if (arguments) |a| (if (a == .object) a.object else return error.InvalidArgument) else return error.InvalidArgument;
    const script = getString(args, "script") orelse return error.InvalidArgument;

    const shell = getString(args, "shell") orelse "powershell";
    const is_powershell = std.mem.eql(u8, shell, "powershell");
    if (!is_powershell and !std.mem.eql(u8, shell, "cmd")) {
        return textContent(allocator, "Invalid shell — use \"powershell\" (persistent session) or \"cmd\" (stateless run_command)");
    }

    const timeout_ms: u32 = if (getInt(args, "timeout_ms")) |t|
        @intCast(@max(1000, @min(t, 600000)))
    else
        60000;

    // Agent-side work is bounded by timeout_ms; add radial margin for the
    // round trip. Long pwsh timeouts need a wide socket window.
    const socket_timeout_secs: u32 = (timeout_ms / 1000) + 15;

    const escaped = try helper.jsonEscape(allocator, script);
    defer allocator.free(escaped);

    const extra = if (is_powershell)
        try std.fmt.allocPrint(allocator, "\"script\":\"{s}\",\"timeout_ms\":{d}", .{ escaped, timeout_ms })
    else
        try std.fmt.allocPrint(allocator, "\"cmd\":\"{s}\",\"timeout\":{d}", .{ escaped, timeout_ms });
    defer allocator.free(extra);

    const response = callHelperWithTimeout(allocator, arguments, if (is_powershell) "powershell_exec" else "run_command", extra, socket_timeout_secs) catch |err| {
        if (err == error.FramebufferNotReady) return helperNotConfigured(allocator);
        if (err == error.ReadTimeout) {
            const msg = std.fmt.allocPrint(allocator, "Shell call timed out after {d}ms at the transport level. The agent may be stuck on the script.", .{timeout_ms}) catch
                return helperNotAvailable(allocator);
            return textContent(allocator, msg);
        }
        return helperNotAvailable(allocator);
    };
    return textContent(allocator, response);
}

/// vnc_browser_eval — evaluate JavaScript in the target's browser via the
/// agent's Marionette client (Phase 8 path; see doc/browser-control-decision.md).
/// context="content" (default) targets the current tab, context="chrome"
/// gives browser-privileged JS (Services, ChromeUtils). Requires the browser
/// running with its remote agent (Firefox/Bloom -marionette, port 2828).
fn toolBrowserEval(allocator: std.mem.Allocator, arguments: ?JsonValue) !JsonValue {
    const args = if (arguments) |a| (if (a == .object) a.object else return error.InvalidArgument) else return error.InvalidArgument;
    const script = getString(args, "script") orelse return error.InvalidArgument;

    const context = getString(args, "context") orelse "content";
    if (!std.mem.eql(u8, context, "content") and !std.mem.eql(u8, context, "chrome")) {
        return textContent(allocator, "Invalid context — use \"content\" (current tab) or \"chrome\" (browser-privileged)");
    }

    const timeout_ms: u32 = if (getInt(args, "timeout_ms")) |t|
        @intCast(@max(1000, @min(t, 120000)))
    else
        30000;
    const socket_timeout_secs: u32 = (timeout_ms / 1000) + 15;

    const escaped = try helper.jsonEscape(allocator, script);
    defer allocator.free(escaped);

    const extra = try std.fmt.allocPrint(allocator, "\"script\":\"{s}\",\"context\":\"{s}\",\"timeout_ms\":{d}", .{ escaped, context, timeout_ms });
    defer allocator.free(extra);

    const response = callHelperWithTimeout(allocator, arguments, "browser_eval", extra, socket_timeout_secs) catch |err| {
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

    // Don't trust "status":"ok" blindly — historically the agent silently
    // truncated large uploads and reported success (vnc-mcp-server#22).
    // Compare the agent's reported byte count against the actual file size.
    if (std.json.parseFromSlice(std.json.Value, allocator, response, .{ .ignore_unknown_fields = true })) |*parsed| {
        defer parsed.deinit();
        if (parsed.value == .object) {
            const root = parsed.value.object;
            const ok = if (root.get("status")) |s| (s == .string and std.mem.eql(u8, s.string, "ok")) else false;
            if (ok) {
                if (root.get("data")) |d| {
                    if (d == .object) {
                        if (d.object.get("bytes")) |b| {
                            const reported: i64 = switch (b) {
                                .integer => b.integer,
                                .float => @intFromFloat(b.float),
                                else => -1,
                            };
                            if (reported >= 0 and reported != @as(i64, @intCast(file_data.len))) {
                                const msg = try std.fmt.allocPrint(allocator, "ERROR: upload verification FAILED — agent wrote {d} bytes but the local file is {d} bytes. The remote file is corrupt. Response: {s}", .{ reported, file_data.len, response });
                                return textContent(allocator, msg);
                            }
                        }
                    }
                }
            }
        }
    } else |_| {
        // Unparseable response from the agent — surface it, don't claim success.
        const msg = try std.fmt.allocPrint(allocator, "ERROR: agent returned an unparseable upload response (possible truncation). Response: {s}", .{response});
        return textContent(allocator, msg);
    }
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

fn buildUiaExtra(allocator: std.mem.Allocator, args: std.json.ObjectMap) !?[]u8 {
    var parts = std.ArrayList(u8){};
    defer parts.deinit(allocator);

    if (args.get("name")) |v| {
        if (v == .string) {
            const escaped = try helper.jsonEscape(allocator, v.string);
            defer allocator.free(escaped);
            const chunk = try std.fmt.allocPrint(allocator, "\"name\":\"{s}\"", .{escaped});
            defer allocator.free(chunk);
            try parts.appendSlice(allocator, chunk);
        }
    }
    if (args.get("automation_id")) |v| {
        if (v == .string) {
            if (parts.items.len > 0) try parts.append(allocator, ',');
            const escaped = try helper.jsonEscape(allocator, v.string);
            defer allocator.free(escaped);
            const chunk = try std.fmt.allocPrint(allocator, "\"automation_id\":\"{s}\"", .{escaped});
            defer allocator.free(chunk);
            try parts.appendSlice(allocator, chunk);
        }
    }
    if (args.get("control_type")) |v| {
        if (v == .string) {
            if (parts.items.len > 0) try parts.append(allocator, ',');
            const escaped = try helper.jsonEscape(allocator, v.string);
            defer allocator.free(escaped);
            const chunk = try std.fmt.allocPrint(allocator, "\"control_type\":\"{s}\"", .{escaped});
            defer allocator.free(chunk);
            try parts.appendSlice(allocator, chunk);
        }
    }
    if (args.get("match")) |v| {
        if (v == .string) {
            if (parts.items.len > 0) try parts.append(allocator, ',');
            const escaped = try helper.jsonEscape(allocator, v.string);
            defer allocator.free(escaped);
            const chunk = try std.fmt.allocPrint(allocator, "\"match\":\"{s}\"", .{escaped});
            defer allocator.free(chunk);
            try parts.appendSlice(allocator, chunk);
        }
    }
    if (args.get("index")) |v| {
        if (v == .integer) {
            if (parts.items.len > 0) try parts.append(allocator, ',');
            const chunk = try std.fmt.allocPrint(allocator, "\"index\":{d}", .{v.integer});
            defer allocator.free(chunk);
            try parts.appendSlice(allocator, chunk);
        }
    }

    if (parts.items.len == 0) return null;
    return try parts.toOwnedSlice(allocator);
}

fn toolUiElementText(allocator: std.mem.Allocator, arguments: ?JsonValue) !JsonValue {
    const args = if (arguments) |a| (if (a == .object) a.object else return error.InvalidArgument) else return error.InvalidArgument;

    const name = getString(args, "name");
    const automation_id = getString(args, "automation_id");

    if (name == null and automation_id == null) return error.InvalidArgument;

    const extra = try buildUiaExtra(allocator, args);
    defer if (extra) |e| allocator.free(e);

    const response = callHelper(allocator, arguments, "ui_element_text", extra orelse "") catch |err| {
        if (err == error.FramebufferNotReady) return helperNotConfigured(allocator);
        return helperNotAvailable(allocator);
    };
    return textContent(allocator, response);
}

fn toolUiClickElement(allocator: std.mem.Allocator, arguments: ?JsonValue) !JsonValue {
    const args = if (arguments) |a| (if (a == .object) a.object else return error.InvalidArgument) else return error.InvalidArgument;

    const name = getString(args, "name");
    const automation_id = getString(args, "automation_id");

    if (name == null and automation_id == null) return error.InvalidArgument;

    const extra = try buildUiaExtra(allocator, args);
    defer if (extra) |e| allocator.free(e);

    const response = callHelper(allocator, arguments, "ui_click_element", extra orelse "") catch |err| {
        if (err == error.FramebufferNotReady) return helperNotConfigured(allocator);
        return helperNotAvailable(allocator);
    };
    return textContent(allocator, response);
}

fn toolRegistryRead(allocator: std.mem.Allocator, arguments: ?JsonValue) !JsonValue {
    const args = if (arguments) |a| (if (a == .object) a.object else return error.InvalidArgument) else return error.InvalidArgument;
    const key = getString(args, "key") orelse return error.InvalidArgument;

    const escaped_key = try helper.jsonEscape(allocator, key);
    defer allocator.free(escaped_key);

    var extra: []u8 = undefined;
    if (getString(args, "value")) |value_name| {
        const escaped_val = try helper.jsonEscape(allocator, value_name);
        defer allocator.free(escaped_val);
        extra = try std.fmt.allocPrint(allocator, "\"key\":\"{s}\",\"value\":\"{s}\"", .{ escaped_key, escaped_val });
    } else {
        extra = try std.fmt.allocPrint(allocator, "\"key\":\"{s}\"", .{escaped_key});
    }
    defer allocator.free(extra);

    const response = callHelper(allocator, arguments, "registry_read", extra) catch |err| {
        if (err == error.FramebufferNotReady) return helperNotConfigured(allocator);
        return helperNotAvailable(allocator);
    };
    return textContent(allocator, response);
}

fn toolRegistryWrite(allocator: std.mem.Allocator, arguments: ?JsonValue) !JsonValue {
    const args = if (arguments) |a| (if (a == .object) a.object else return error.InvalidArgument) else return error.InvalidArgument;
    const key = getString(args, "key") orelse return error.InvalidArgument;

    const escaped_key = try helper.jsonEscape(allocator, key);
    defer allocator.free(escaped_key);

    var parts = std.ArrayList(u8){};
    defer parts.deinit(allocator);

    {
        const chunk = try std.fmt.allocPrint(allocator, "\"key\":\"{s}\"", .{escaped_key});
        defer allocator.free(chunk);
        try parts.appendSlice(allocator, chunk);
    }

    if (getString(args, "value")) |value_name| {
        const escaped = try helper.jsonEscape(allocator, value_name);
        defer allocator.free(escaped);
        const chunk = try std.fmt.allocPrint(allocator, ",\"value\":\"{s}\"", .{escaped});
        defer allocator.free(chunk);
        try parts.appendSlice(allocator, chunk);
    }
    if (getString(args, "type")) |reg_type| {
        const escaped = try helper.jsonEscape(allocator, reg_type);
        defer allocator.free(escaped);
        const chunk = try std.fmt.allocPrint(allocator, ",\"type\":\"{s}\"", .{escaped});
        defer allocator.free(chunk);
        try parts.appendSlice(allocator, chunk);
    }
    if (getString(args, "data")) |data| {
        const escaped = try helper.jsonEscape(allocator, data);
        defer allocator.free(escaped);
        const chunk = try std.fmt.allocPrint(allocator, ",\"data\":\"{s}\"", .{escaped});
        defer allocator.free(chunk);
        try parts.appendSlice(allocator, chunk);
    } else if (getInt(args, "data")) |data_int| {
        const chunk = try std.fmt.allocPrint(allocator, ",\"data\":{d}", .{data_int});
        defer allocator.free(chunk);
        try parts.appendSlice(allocator, chunk);
    }

    const extra = try allocator.dupe(u8, parts.items);
    defer allocator.free(extra);

    const response = callHelper(allocator, arguments, "registry_write", extra) catch |err| {
        if (err == error.FramebufferNotReady) return helperNotConfigured(allocator);
        return helperNotAvailable(allocator);
    };
    return textContent(allocator, response);
}

fn toolRegistryList(allocator: std.mem.Allocator, arguments: ?JsonValue) !JsonValue {
    const args = if (arguments) |a| (if (a == .object) a.object else return error.InvalidArgument) else return error.InvalidArgument;
    const key = getString(args, "key") orelse return error.InvalidArgument;

    const escaped_key = try helper.jsonEscape(allocator, key);
    defer allocator.free(escaped_key);

    const extra = try std.fmt.allocPrint(allocator, "\"key\":\"{s}\"", .{escaped_key});
    defer allocator.free(extra);

    const response = callHelper(allocator, arguments, "registry_list", extra) catch |err| {
        if (err == error.FramebufferNotReady) return helperNotConfigured(allocator);
        return helperNotAvailable(allocator);
    };
    return textContent(allocator, response);
}

fn toolListProcesses(allocator: std.mem.Allocator, arguments: ?JsonValue) !JsonValue {
    const response = callHelper(allocator, arguments, "process_list", null) catch |err| {
        if (err == error.FramebufferNotReady) return helperNotConfigured(allocator);
        return helperNotAvailable(allocator);
    };
    return textContent(allocator, response);
}

fn toolKillProcess(allocator: std.mem.Allocator, arguments: ?JsonValue) !JsonValue {
    const args = if (arguments) |a| (if (a == .object) a.object else return error.InvalidArgument) else return error.InvalidArgument;

    var parts = std.ArrayList(u8){};
    defer parts.deinit(allocator);

    if (getInt(args, "pid")) |pid| {
        const chunk = try std.fmt.allocPrint(allocator, "\"pid\":{d}", .{pid});
        defer allocator.free(chunk);
        try parts.appendSlice(allocator, chunk);
    }
    if (getString(args, "name")) |n| {
        if (parts.items.len > 0) try parts.append(allocator, ',');
        const escaped = try helper.jsonEscape(allocator, n);
        defer allocator.free(escaped);
        const chunk = try std.fmt.allocPrint(allocator, "\"name\":\"{s}\"", .{escaped});
        defer allocator.free(chunk);
        try parts.appendSlice(allocator, chunk);
    }

    if (parts.items.len == 0) return textContent(allocator, "Provide 'pid' or 'name' to kill");

    const extra = try allocator.dupe(u8, parts.items);
    defer allocator.free(extra);

    const response = callHelper(allocator, arguments, "process_kill", extra) catch |err| {
        if (err == error.FramebufferNotReady) return helperNotConfigured(allocator);
        return helperNotAvailable(allocator);
    };
    return textContent(allocator, response);
}

fn toolListServices(allocator: std.mem.Allocator, arguments: ?JsonValue) !JsonValue {
    const response = callHelper(allocator, arguments, "service_list", null) catch |err| {
        if (err == error.FramebufferNotReady) return helperNotConfigured(allocator);
        return helperNotAvailable(allocator);
    };
    return textContent(allocator, response);
}

fn toolServiceControl(allocator: std.mem.Allocator, arguments: ?JsonValue) !JsonValue {
    const args = if (arguments) |a| (if (a == .object) a.object else return error.InvalidArgument) else return error.InvalidArgument;
    const svc_name = getString(args, "name") orelse return error.InvalidArgument;
    const action = getString(args, "action") orelse return error.InvalidArgument;

    const escaped_name = try helper.jsonEscape(allocator, svc_name);
    defer allocator.free(escaped_name);
    const escaped_action = try helper.jsonEscape(allocator, action);
    defer allocator.free(escaped_action);

    const extra = try std.fmt.allocPrint(allocator, "\"name\":\"{s}\",\"action\":\"{s}\"", .{ escaped_name, escaped_action });
    defer allocator.free(extra);

    const response = callHelper(allocator, arguments, "service_control", extra) catch |err| {
        if (err == error.FramebufferNotReady) return helperNotConfigured(allocator);
        return helperNotAvailable(allocator);
    };
    return textContent(allocator, response);
}

// ===================================================================
// Coordinate calibration
// ===================================================================

fn getFloat(obj: std.json.ObjectMap, key: []const u8) ?f64 {
    if (obj.get(key)) |val| {
        return switch (val) {
            .integer => @floatFromInt(val.integer),
            .float => val.float,
            else => null,
        };
    }
    return null;
}

/// Current framebuffer dimensions for the endpoint in these arguments.
/// Handshake dimensions are available right after connect — no screenshot needed.
fn currentDims(arguments: ?JsonValue) !struct { w: u16, h: u16 } {
    const ep = try getEndpoint(arguments);
    const pool = connections orelse return error.ConnectionFailed;
    const client = try pool.getOrConnect(ep);
    return .{ .w = client.width, .h = client.height };
}

const ResolvedCoords = struct { x: u16, y: u16, note: []const u8 };

/// Map tool-call coordinates into framebuffer pixels.
/// coordinate_space="framebuffer" (default): pass through, unchanged behavior.
/// coordinate_space="calibrated": apply the stored transform for
/// (client_id, endpoint, current resolution). Missing/generic-identity or a
/// resolution-mismatched record is a hard error text — never a wrong click.
fn resolveCoords(allocator: std.mem.Allocator, args: std.json.ObjectMap, arguments: ?JsonValue, x_in: i64, y_in: i64) !ResolvedCoords {
    const space = getString(args, "coordinate_space") orelse "framebuffer";
    if (std.mem.eql(u8, space, "framebuffer")) {
        return .{
            .x = @intCast(std.math.clamp(x_in, 0, 65535)),
            .y = @intCast(std.math.clamp(y_in, 0, 65535)),
            .note = "",
        };
    }
    if (!std.mem.eql(u8, space, "calibrated")) {
        return .{ .x = 0, .y = 0, .note = try allocator.dupe(u8, "ERROR: unknown coordinate_space (use \"framebuffer\" or \"calibrated\") — NO ACTION WAS TAKEN.") };
    }

    const cid_opt = try cal.clientId(allocator);
    const cid = cid_opt orelse {
        return .{ .x = 0, .y = 0, .note = try allocator.dupe(u8, "ERROR: coordinate_space=calibrated but your MCP client did not identify itself (no clientInfo.name in initialize). Calibration is per-agent identity — NO ACTION WAS TAKEN.") };
    };
    defer allocator.free(cid);
    if (!cal.isUsableClient(cid)) {
        const note = try std.fmt.allocPrint(allocator, "ERROR: coordinate_space=calibrated but client identity '{s}' is generic — calibrations must be keyed to a real agent identity. NO ACTION WAS TAKEN.", .{cid});
        return .{ .x = 0, .y = 0, .note = note };
    }

    const ep = try getEndpoint(arguments);
    const dims = try currentDims(arguments);

    var store = try cal.load(allocator);
    defer store.deinit();

    const rec = store.find(cid, ep.id, dims.w, dims.h) orelse {
        if (store.findAnyForEndpoint(cid, ep.id)) |stale| {
            const note = try std.fmt.allocPrint(allocator, "ERROR: calibration for endpoint {s} is STALE — saved at {d}x{d}, current framebuffer is {d}x{d}. Run vnc_calibrate again. NO ACTION WAS TAKEN.", .{ ep.id, stale.width, stale.height, dims.w, dims.h });
            return .{ .x = 0, .y = 0, .note = note };
        }
        const note = try std.fmt.allocPrint(allocator, "ERROR: no calibration for ({s}, {s}, {d}x{d}). Clicks are inaccurate until calibrated — run vnc_calibrate (action=start) first. NO ACTION WAS TAKEN.", .{ cid, ep.id, dims.w, dims.h });
        return .{ .x = 0, .y = 0, .note = note };
    };

    const record = cal.Record{
        .client_id = rec.client_id,
        .client_name = rec.client_name,
        .client_version = rec.client_version,
        .endpoint_id = rec.endpoint_id,
        .width = rec.width,
        .height = rec.height,
        .x_a = rec.x_a,
        .x_b = rec.x_b,
        .y_a = rec.y_a,
        .y_b = rec.y_b,
        .rmse = rec.rmse,
        .rounds = rec.rounds,
        .created_at = rec.created_at,
        .updated_at = rec.updated_at,
    };
    const fb = record.toFb(@floatFromInt(x_in), @floatFromInt(y_in));
    const note = try std.fmt.allocPrint(allocator, "(input ({d},{d}) mapped to framebuffer ({d},{d}); calibration rmse {d:.1}px)", .{ x_in, y_in, fb.x, fb.y, rec.rmse });
    return .{ .x = fb.x, .y = fb.y, .note = note };
}

/// Combine a tool note (from resolveCoords or "") with the calibration status
/// line for the endpoint/resolution that applies to this response.
fn withCalibrationStatus(allocator: std.mem.Allocator, arguments: ?JsonValue, base_text: []const u8, width: u16, height: u16) []const u8 {
    const ep = getEndpoint(arguments) catch return base_text;
    const status = cal.statusLine(allocator, ep.id, width, height);
    defer if (!std.mem.eql(u8, status.ptr, base_text.ptr) or true) allocator.free(status);
    const merged = std.fmt.allocPrint(allocator, "{s}\n{s}", .{ base_text, status }) catch return base_text;
    return merged;
}

/// Per-client calibration clause appended to spatial tool descriptions.
/// Returns "" when no tailoring applies.
fn calibrationClause(allocator: std.mem.Allocator) ![]u8 {
    const cid_opt = try cal.clientId(allocator);
    const cid = cid_opt orelse
        return allocator.dupe(u8, "CALIBRATION: this client sent no identity (clientInfo.name) — calibrated coordinate mapping is unavailable; coordinates must be framebuffer pixels computed from the Resolution metadata.");
    defer allocator.free(cid);

    if (!cal.isUsableClient(cid)) {
        return std.fmt.allocPrint(allocator, "CALIBRATION: disabled — client identity '{s}' is too generic to key a saved calibration.", .{cid});
    }

    var store = cal.load(allocator) catch
        return allocator.dupe(u8, "CALIBRATION: state unknown (calibration.json unreadable); treat coordinates as framebuffer pixels.");
    defer store.deinit();

    if (store.findAnyForClient(cid)) |_| {
        return allocator.dupe(u8, "CALIBRATION: you have saved calibration record(s). For coordinate targeting pass coordinate_space=\"calibrated\" and read positions directly from returned images — no manual scaling. Records are per (client, endpoint, resolution); every spatial tool response carries a 'Calibration:' status line — honor it.");
    }
    return allocator.dupe(u8, "CALIBRATION: NOT CALIBRATED. Image-space coordinate estimates ARE INACCURATE — either run vnc_calibrate (one-time per client/endpoint/resolution) or compute framebuffer coordinates from the Resolution metadata.");
}

/// Append the calibration state clause to spatial tool descriptions.
/// Called by the server on every tools/list; mutates the parsed schema value.
pub fn tailorDescriptions(allocator: std.mem.Allocator, root: *std.json.Value) !void {
    const clause = calibrationClause(allocator) catch return;
    defer allocator.free(clause);
    if (clause.len == 0) {
        allocator.free(clause);
        return;
    }

    if (root.* != .array) return;
    for (root.array.items) |*tv| {
        if (tv.* != .object) continue;
        const name_v = tv.object.get("name") orelse continue;
        if (name_v != .string) continue;
        if (!std.mem.startsWith(u8, name_v.string, "vnc_screenshot") and
            !std.mem.eql(u8, name_v.string, "vnc_probe") and
            !std.mem.eql(u8, name_v.string, "vnc_grid") and
            !std.mem.eql(u8, name_v.string, "vnc_click") and
            !std.mem.eql(u8, name_v.string, "vnc_drag") and
            !std.mem.eql(u8, name_v.string, "vnc_move_mouse") and
            !std.mem.eql(u8, name_v.string, "vnc_scroll")) continue;

        const desc_v = tv.object.get("description") orelse continue;
        if (desc_v != .string) continue;
        const merged = std.fmt.allocPrint(allocator, "{s}\n\n{s}", .{ desc_v.string, clause }) catch continue;
        try tv.object.put("description", .{ .string = merged });
    }
}

// ---- vnc_calibrate tool ----

const rmse_accept_px: f64 = 4.0;
const max_rounds: u32 = 5;

const CalMarker = struct {
    id: []u8,
    fb_x: u16,
    fb_y: u16,
    obs_x: ?f64 = null,
    obs_y: ?f64 = null,
};

const PendingCal = struct {
    endpoint_id: []u8,
    width: u16,
    height: u16,
    rounds: u32,
    markers: []CalMarker,
    solution: ?cal.Solution = null,
};

var pending_cal: ?PendingCal = null;

fn calResetPendingP() void {
    if (pending_cal) |*p| {
        global_allocator.free(p.endpoint_id);
        for (p.markers) |m| global_allocator.free(m.id);
        global_allocator.free(p.markers);
        if (p.solution) |*s| global_allocator.free(s.residuals);
        pending_cal = null;
    }
}

fn toolCalibrate(allocator: std.mem.Allocator, arguments: ?JsonValue) !JsonValue {
    const args = if (arguments) |a| (if (a == .object) a.object else return error.InvalidArgument) else return error.InvalidArgument;
    const action = getString(args, "action") orelse
        return textContent(allocator, "ERROR: 'action' is required: start | submit | commit | status | clear");

    if (std.mem.eql(u8, action, "start")) return calActionStart(allocator, arguments, args);
    if (std.mem.eql(u8, action, "submit")) return calActionSubmit(allocator, arguments, args);
    if (std.mem.eql(u8, action, "commit")) return calActionCommit(allocator, arguments);
    if (std.mem.eql(u8, action, "status")) return calActionStatus(allocator, arguments);
    if (std.mem.eql(u8, action, "clear")) return calActionClear(allocator, arguments);
    return textContent(allocator, "ERROR: unknown action (use start | submit | commit | status | clear)");
}

fn calActionStart(allocator: std.mem.Allocator, arguments: ?JsonValue, args: std.json.ObjectMap) !JsonValue {
    _ = args;
    const ep = try getEndpoint(arguments);
    const client = try getClient(arguments);

    calResetPendingP();

    var markers = try global_allocator.alloc(CalMarker, 9);
    var labeled = try allocator.alloc(image.LabeledMarker, 9);
    defer allocator.free(labeled);
    var meta_lines = std.ArrayList(u8){};
    defer meta_lines.deinit(allocator);

    const fracs = [_]f64{ 0.125, 0.5, 0.875 };
    var n: usize = 0;
    for (fracs) |fy| {
        for (fracs) |fx| {
            const mx: u16 = @intFromFloat(@round(fx * @as(f64, @floatFromInt(client.width))));
            const my: u16 = @intFromFloat(@round(fy * @as(f64, @floatFromInt(client.height))));
            const id = try std.fmt.allocPrint(global_allocator, "M{d}", .{n + 1});
            markers[n] = .{ .id = id, .fb_x = mx, .fb_y = my };
            labeled[n] = .{ .label = id, .x = mx, .y = my };
            const line = try std.fmt.allocPrint(allocator, "{s}=({d},{d}) ", .{ id, mx, my });
            defer allocator.free(line);
            try meta_lines.appendSlice(allocator, line);
            n += 1;
        }
    }

    pending_cal = .{
        .endpoint_id = try global_allocator.dupe(u8, ep.id),
        .width = client.width,
        .height = client.height,
        .rounds = 1,
        .markers = markers,
    };

    const fb = try client.screenshot();
    const jpeg = try image.encodeJpegWithLabeledMarkers(allocator, fb, 75, labeled);
    defer allocator.free(jpeg);

    const meta = try std.fmt.allocPrint(allocator, "Calibration round 1 — 9 numbered markers placed on endpoint {s} ({d}x{d} framebuffer).\nMarker framebuffer positions: {s}\nResolution: {d}x{d} pixels\n\nFor EACH numbered marker, report the position of its MAGENTA CENTER DOT as it appears to you in this displayed image (the image may be scaled/cropped on your side — use the coordinate system of this image exactly as you see it, fractional pixels are fine).\nThen call vnc_calibrate action=\"submit\" with: {{\"samples\":[{{\"id\":\"M1\",\"x\":12.5,\"y\":34}}, ...]}}.", .{ ep.id, client.width, client.height, meta_lines.items, client.width, client.height });
    return imageContentWithMeta(allocator, jpeg, meta);
}

fn calActionSubmit(allocator: std.mem.Allocator, arguments: ?JsonValue, args: std.json.ObjectMap) !JsonValue {
    if (pending_cal == null) {
        return textContent(allocator, "ERROR: no calibration round in progress — call action=\"start\" first.");
    }
    const p = &pending_cal.?;

    const ep = try getEndpoint(arguments);

    // Parse and merge samples (same id replaces; new ids must match round markers)
    const samples_v = args.get("samples");
    if (samples_v == null or samples_v.? != .array or samples_v.?.array.items.len == 0) {
        return textContent(allocator, "ERROR: 'samples' array required: [{\"id\":\"M1\",\"x\":12.5,\"y\":34}, ...]");
    }
    var referenced: usize = 0;
    for (samples_v.?.array.items) |sv| {
        if (sv != .object) continue;
        const id_s = getString(sv.object, "id") orelse continue;
        const ox = getFloat(sv.object, "x") orelse continue;
        const oy = getFloat(sv.object, "y") orelse continue;
        for (p.markers) |*m| {
            if (std.mem.eql(u8, m.id, id_s)) {
                m.obs_x = ox;
                m.obs_y = oy;
                referenced += 1;
                break;
            }
        }
    }
    if (referenced == 0) {
        return textContent(allocator, "ERROR: none of the sample ids match pending markers — check ids from the last start/refine image.");
    }

    // Collect observed markers
    var sample_list = std.ArrayList(cal.Sample){};
    defer sample_list.deinit(allocator);
    for (p.markers) |m| {
        if (m.obs_x) |ox| {
            if (m.obs_y) |oy| {
                try sample_list.append(allocator, .{ .id = m.id, .fb_x = @floatFromInt(m.fb_x), .fb_y = @floatFromInt(m.fb_y), .obs_x = ox, .obs_y = oy });
            }
        }
    }
    if (sample_list.items.len < 2) {
        const msg = try std.fmt.allocPrint(allocator, "ERROR: only {d} observed sample(s) — at least 2 needed. Measure more markers and submit again.", .{sample_list.items.len});
        return textContent(allocator, msg);
    }

    const sol = (try cal.solve(allocator, sample_list.items)) orelse {
        return textContent(allocator, "ERROR: observations are degenerate (identical positions) — cannot solve a transform. Re-check the marker positions you reported.");
    };

    if (sol.rmse > rmse_accept_px and p.rounds < max_rounds) {
        allocator.free(sol.residuals); // refine round re-solves next submit

        // Place additional markers near the worst residual points
        const worst = worstResidualMarkers(p.markers, sample_list.items, sol, allocator) catch
            return textContent(allocator, "ERROR: internal error building refine round");
        defer allocator.free(worst);

        var new_markers = std.ArrayList(CalMarker){};
        var id_num: usize = p.markers.len;
        for (worst) |wm| {
            const offsets = [3][2]i32{ .{ 64, 48 }, .{ -64, 48 }, .{ 0, -80 } };
            for (offsets) |off| {
                const nx_i = std.math.clamp(@as(i32, wm.fb_x) + off[0], 8, @as(i32, p.width) - 8);
                const ny_i = std.math.clamp(@as(i32, wm.fb_y) + off[1], 8, @as(i32, p.height) - 8);
                // Skip if too close to an existing marker
                var dupe = false;
                for (p.markers) |m| {
                    const dx = @as(i32, m.fb_x) - nx_i;
                    const dy = @as(i32, m.fb_y) - ny_i;
                    if (dx * dx + dy * dy < 40 * 40) {
                        dupe = true;
                        break;
                    }
                }
                for (new_markers.items) |m| {
                    const dx = @as(i32, m.fb_x) - nx_i;
                    const dy = @as(i32, m.fb_y) - ny_i;
                    if (dx * dx + dy * dy < 40 * 40) {
                        dupe = true;
                        break;
                    }
                }
                if (dupe) continue;
                id_num += 1;
                const id = try std.fmt.allocPrint(global_allocator, "M{d}", .{id_num});
                try new_markers.append(global_allocator, .{ .id = id, .fb_x = @intCast(nx_i), .fb_y = @intCast(ny_i) });
            }
        }

        if (new_markers.items.len == 0) {
            // No space for refine markers — accept what we have but warn
            p.solution = sol;
            const msg = try std.fmt.allocPrint(allocator, "Fit rmse {d:.1}px exceeds the {d:.1}px target, but no room remains to place refine markers. Call action=\"commit\" to accept, or action=\"start\" to retry with fresh screenshots.", .{ sol.rmse, rmse_accept_px });
            return textContent(allocator, msg);
        }

        // Merge markers into pending
        const merged_markers = try global_allocator.alloc(CalMarker, p.markers.len + new_markers.items.len);
        @memcpy(merged_markers[0..p.markers.len], p.markers);
        @memcpy(merged_markers[p.markers.len..], new_markers.items);
        global_allocator.free(p.markers);
        global_allocator.free(new_markers.items);
        p.markers = merged_markers;
        p.rounds += 1;

        var lbl = std.ArrayList(image.LabeledMarker){};
        defer lbl.deinit(allocator);
        var id_list = std.ArrayList(u8){};
        defer id_list.deinit(allocator);
        for (p.markers) |m| {
            if (m.obs_x == null) {
                try lbl.append(allocator, .{ .label = m.id, .x = m.fb_x, .y = m.fb_y });
                const seg = try std.fmt.allocPrint(allocator, "{s}=({d},{d}) ", .{ m.id, m.fb_x, m.fb_y });
                defer allocator.free(seg);
                try id_list.appendSlice(allocator, seg);
            }
        }

        const client = try getClient(arguments);
        const fb = try client.screenshot();
        const jpeg = try image.encodeJpegWithLabeledMarkers(allocator, fb, 75, lbl.items);
        defer allocator.free(jpeg);

        const meta = try std.fmt.allocPrint(allocator, "Fit so far: rmse {d:.1}px over {d} samples — above the {d:.1}px target; refine round {d} placed {d} additional markers near the worst-fitting points.\nNew markers (framebuffer positions): {s}\nResolution: {d}x{d} pixels\n\nMeasure the NEW markers (ids listed) the same way — position of each magenta dot in this image as displayed to you — then call vnc_calibrate action=\"submit\" with those samples. Previously submitted samples are kept and remain part of the fit.", .{ sol.rmse, sample_list.items.len, rmse_accept_px, p.rounds, lbl.items.len, id_list.items, p.width, p.height });
        return imageContentWithMeta(allocator, jpeg, meta);
    }

    if (sol.rmse > rmse_accept_px) {
        // Max rounds exhausted — offer commit-or-restart
        p.solution = sol;
        const msg = try std.fmt.allocPrint(allocator, "Fit rmse {d:.1}px still above {d:.1}px after {d} rounds. The display scaling may be non-uniform (letterboxing or per-region scaling). Call action=\"commit\" to accept this fit, or action=\"start\" to retry. Consider capturing a vnc_grid image and verifying the IDE's rendering scale manually.", .{ sol.rmse, rmse_accept_px, p.rounds });
        return textContent(allocator, msg);
    }

    p.solution = sol;

    // Summarize fit + residuals
    var worst_i: usize = 0;
    for (sol.residuals, 0..) |r, i| {
        if (r > sol.residuals[worst_i]) worst_i = i;
    }
    const summary = try std.fmt.allocPrint(allocator, "Calibration fit OK: rmse {d:.1}px over {d} samples across {d} round(s).\nTransform: fb_x = {d:.4}·obs_x + {d:.1}; fb_y = {d:.4}·obs_y + {d:.1}\nWorst residual: {d:.1}px at marker {s}\n\nCall vnc_calibrate action=\"commit\" to save. After that, coordinate tools accept coordinate_space=\"calibrated\" with positions read directly from returned images.", .{ sol.rmse, sample_list.items.len, p.rounds, sol.x_a, sol.x_b, sol.y_a, sol.y_b, sol.residuals[worst_i], sample_list.items[worst_i].id });
    const msg = try std.fmt.allocPrint(allocator, "{s}\n{s}", .{ summary, cal.statusLine(allocator, ep.id, p.width, p.height) });
    allocator.free(summary);
    return textContent(allocator, msg);
}

fn worstResidualMarkers(markers: []CalMarker, samples: []cal.Sample, sol: cal.Solution, allocator: std.mem.Allocator) ![]CalMarker {
    _ = samples;
    // Index residuals parallel to samples order — residuals align with the
    // observed-subset order, so recompute the observed list to stay paired.
    var obs = try allocator.alloc(CalMarker, markers.len);
    var n: usize = 0;
    for (markers) |m| {
        if (m.obs_x != null) {
            obs[n] = m;
            n += 1;
        }
    }
    // Take up to 2 worst by residual (residuals[i] pairs with obs[i])
    var idx = try allocator.alloc(usize, n);
    defer allocator.free(idx);
    for (0..n) |i| idx[i] = i;
    std.mem.sort(usize, idx, sol.residuals, struct {
        fn lessThan(res: []f64, a: usize, b: usize) bool {
            return res[a] > res[b];
        }
    }.lessThan);

    var out = try allocator.alloc(CalMarker, @min(n, 2));
    const take = @min(n, 2);
    for (0..take) |i| {
        out[i] = obs[idx[i]];
    }
    return out;
}

fn calActionCommit(allocator: std.mem.Allocator, arguments: ?JsonValue) !JsonValue {
    _ = arguments;
    const p = &(pending_cal orelse
        return textContent(allocator, "ERROR: nothing to commit — call action=\"start\", then submit samples."));
    _ = p;
    var pc = pending_cal.?;
    const sol = pc.solution orelse
        return textContent(allocator, "ERROR: no solved transform yet — submit samples first (action=\"submit\").");

    const cid_opt = try cal.clientId(allocator);
    const cid = cid_opt orelse
        return textContent(allocator, "ERROR: this client did not identify itself (no clientInfo.name) — calibrations are keyed per agent identity; cannot save.");
    defer allocator.free(cid);
    if (!cal.isUsableClient(cid)) {
        const msg = try std.fmt.allocPrint(allocator, "ERROR: client identity '{s}' is generic — refusing to save an unkeyed calibration that could be picked up by unrelated agents.", .{cid});
        return textContent(allocator, msg);
    }

    const id = try cal.calibrationId(allocator, cid, pc.endpoint_id, pc.width, pc.height);
    defer allocator.free(id);

    const record = cal.Record{
        .client_id = cid,
        .client_name = cal.clientName(),
        .client_version = cal.clientVersion(),
        .endpoint_id = pc.endpoint_id,
        .width = pc.width,
        .height = pc.height,
        .x_a = sol.x_a,
        .x_b = sol.x_b,
        .y_a = sol.y_a,
        .y_b = sol.y_b,
        .rmse = sol.rmse,
        .rounds = pc.rounds,
        .created_at = 0,
        .updated_at = 0,
    };
    try cal.upsert(allocator, &record, id);

    const path = try cal.filePath(allocator);
    defer allocator.free(path);
    const msg = try std.fmt.allocPrint(allocator, "Calibration saved. id={s} endpoint={s} resolution={d}x{d} rmse={d:.1}px rounds={d}\nStored in {s}.\nUsage: pass coordinate_space=\"calibrated\" and coordinates taken directly from returned images. Valid until the target resolution changes or you re-run vnc_calibrate.", .{ id, pc.endpoint_id, pc.width, pc.height, sol.rmse, pc.rounds, path });
    allocator.free(sol.residuals);
    pc.solution = null;
    calResetPendingP();
    return textContent(allocator, msg);
}

fn calActionStatus(allocator: std.mem.Allocator, arguments: ?JsonValue) !JsonValue {
    var out = std.ArrayList(u8){};
    errdefer out.deinit(allocator);

    const cid_opt = try cal.clientId(allocator);
    defer if (cid_opt) |c| allocator.free(c);

    if (cid_opt) |cid| {
        const usable = cal.isUsableClient(cid);
        const line = try std.fmt.allocPrint(allocator, "client_id: {s} (from clientInfo name '{s}' v{s}){s}\n", .{ cid, cal.clientName() orelse "?", cal.clientVersion() orelse "?", if (usable) "" else " — GENERIC, cannot save calibrations" });
        defer allocator.free(line);
        try out.appendSlice(allocator, line);
    } else {
        try out.appendSlice(allocator, "client_id: none (client did not send clientInfo.name)\n");
    }

    const ep = getEndpoint(arguments) catch {
        try out.appendSlice(allocator, "endpoint: (unresolvable)\n");
        return textContent(allocator, try out.toOwnedSlice(allocator));
    };
    const dims = currentDims(arguments) catch {
        try out.appendSlice(allocator, "resolution: (endpoint unreachable)\n");
        return textContent(allocator, try out.toOwnedSlice(allocator));
    };
    const dl = try std.fmt.allocPrint(allocator, "endpoint: {s}  current resolution: {d}x{d}\n", .{ ep.id, dims.w, dims.h });
    defer allocator.free(dl);
    try out.appendSlice(allocator, dl);

    const cid = cid_opt orelse {
        try out.appendSlice(allocator, cal.statusLine(allocator, ep.id, dims.w, dims.h));
        return textContent(allocator, try out.toOwnedSlice(allocator));
    };

    var store = try cal.load(allocator);
    defer store.deinit();

    if (store.find(cid, ep.id, dims.w, dims.h)) |rec| {
        const id = try cal.calibrationId(allocator, cid, ep.id, dims.w, dims.h);
        defer allocator.free(id);
        const line = try std.fmt.allocPrint(allocator, "record: id={s} rmse={d:.2}px rounds={d}\n  transform: fb_x={d:.4}·obs_x+{d:.2}  fb_y={d:.4}·obs_y+{d:.2}\n  created={d} updated={d}\n", .{ id, rec.rmse, rec.rounds, rec.x_a, rec.x_b, rec.y_a, rec.y_b, rec.created_at, rec.updated_at });
        defer allocator.free(line);
        try out.appendSlice(allocator, line);
    } else if (store.findAnyForEndpoint(cid, ep.id)) |rec| {
        const line = try std.fmt.allocPrint(allocator, "record: STALE — saved for {d}x{d}, current is {d}x{d}. Re-run vnc_calibrate action=\"start\".\n", .{ rec.width, rec.height, dims.w, dims.h });
        defer allocator.free(line);
        try out.appendSlice(allocator, line);
    } else {
        try out.appendSlice(allocator, "record: none for this endpoint/resolution — run vnc_calibrate action=\"start\".\n");
    }

    try out.appendSlice(allocator, cal.statusLine(allocator, ep.id, dims.w, dims.h));
    return textContent(allocator, try out.toOwnedSlice(allocator));
}

fn calActionClear(allocator: std.mem.Allocator, arguments: ?JsonValue) !JsonValue {
    const cid_opt = try cal.clientId(allocator);
    const cid = cid_opt orelse
        return textContent(allocator, "ERROR: client did not identify itself — nothing can be attributed to you, nothing to clear.");
    defer allocator.free(cid);

    const ep = try getEndpoint(arguments);
    const dims = try currentDims(arguments);
    const removed = try cal.remove(allocator, cid, ep.id, dims.w, dims.h);
    const msg = if (removed)
        try std.fmt.allocPrint(allocator, "Calibration cleared for ({s}, {s}, {d}x{d}).", .{ cid, ep.id, dims.w, dims.h })
    else
        try std.fmt.allocPrint(allocator, "No calibration record existed for ({s}, {s}, {d}x{d}).", .{ cid, ep.id, dims.w, dims.h });
    calResetPendingP();
    return textContent(allocator, msg);
}
