//! Per-(client, endpoint, resolution) coordinate calibration.
//!
//! Maps the coordinate space the calling agent sees in its (downscaled) IDE
//! image to real framebuffer pixels. Records live in
//! ~/.config/vnc-mcp/calibration.json and are valid until re-calibrated.
//! Identity: calibration_id = sha256(client_id | endpoint_id | WxH)[:16].
//! clientInfo.version is stored for diagnostics but never hashed.

const std = @import("std");
const log = std.log.scoped(.calibration);

pub const generic_names = [_][]const u8{ "", "test", "mcp", "client", "unknown" };

// ---- Client identity (set once from MCP initialize params.clientInfo) ----

var identity_taken = false;
var name_buf: [128]u8 = undefined;
var name_len: usize = 0;
var version_buf: [64]u8 = undefined;
var version_len: usize = 0;

pub fn setClientIdentity(name: ?[]const u8, version: ?[]const u8) void {
    if (identity_taken) return; // first initialize wins, per MCP lifecycle
    identity_taken = true;
    if (name) |n| {
        name_len = @min(n.len, name_buf.len);
        @memcpy(name_buf[0..name_len], n[0..name_len]);
    }
    if (version) |v| {
        version_len = @min(v.len, version_buf.len);
        @memcpy(version_buf[0..version_len], v[0..version_len]);
    }
}

pub fn clientName() ?[]const u8 {
    return if (name_len > 0) name_buf[0..name_len] else null;
}

pub fn clientVersion() ?[]const u8 {
    return if (version_len > 0) version_buf[0..version_len] else null;
}

/// Normalized, hash-stable client identity: lowercase, runs of
/// space/underscore/dot collapsed to '-', ends trimmed.
/// Caller owns the returned slice. Returns null when identity is unknown.
pub fn clientId(allocator: std.mem.Allocator) !?[]u8 {
    const raw = clientName() orelse return null;
    var out = try std.ascii.allocLowerString(allocator, raw);
    errdefer allocator.free(out);
    var w: usize = 0;
    var pending_sep = false;
    for (out) |ch| {
        if (ch == ' ' or ch == '_' or ch == '.') {
            pending_sep = w > 0;
            continue;
        }
        if (pending_sep) {
            out[w] = '-';
            w += 1;
            pending_sep = false;
        }
        out[w] = ch;
        w += 1;
    }
    return try allocator.realloc(out, w);
}

pub fn isUsableClient(id: []const u8) bool {
    for (generic_names) |g| {
        if (std.mem.eql(u8, id, g)) return false;
    }
    return true;
}

pub fn calibrationId(allocator: std.mem.Allocator, client_id: []const u8, endpoint_id: []const u8, width: u16, height: u16) ![]u8 {
    const input = try std.fmt.allocPrint(allocator, "{s}|{s}|{d}x{d}", .{ client_id, endpoint_id, width, height });
    defer allocator.free(input);

    var digest: [32]u8 = undefined;
    std.crypto.hash.sha2.Sha256.hash(input, &digest, .{});
    const hex = try allocator.alloc(u8, 16);
    _ = std.fmt.bufPrint(hex, "{s}", .{std.fmt.bytesToHex(digest[0..8], .lower)}) catch unreachable;
    return hex;
}

// ---- Paths ----

pub fn configDir(allocator: std.mem.Allocator) ![]u8 {
    if (std.posix.getenv("XDG_CONFIG_HOME")) |xdg| {
        if (xdg.len > 0) return std.fmt.allocPrint(allocator, "{s}/vnc-mcp", .{xdg});
    }
    const home = std.posix.getenv("HOME") orelse return error.NoHome;
    return std.fmt.allocPrint(allocator, "{s}/.config/vnc-mcp", .{home});
}

pub fn filePath(allocator: std.mem.Allocator) ![]u8 {
    const dir = try configDir(allocator);
    defer allocator.free(dir);
    return std.fmt.allocPrint(allocator, "{s}/calibration.json", .{dir});
}

// ---- Record / document model ----

pub const Record = struct {
    client_id: []const u8,
    client_name: ?[]const u8 = null,
    client_version: ?[]const u8 = null,
    endpoint_id: []const u8,
    width: u16,
    height: u16,
    x_a: f64,
    x_b: f64,
    y_a: f64,
    y_b: f64,
    rmse: f64,
    rounds: u32,
    created_at: i64,
    updated_at: i64,

    pub fn toFb(self: *const Record, ax: f64, ay: f64) struct { x: u16, y: u16 } {
        const fx = @round(self.x_a * ax + self.x_b);
        const fy = @round(self.y_a * ay + self.y_b);
        const max_x: f64 = @floatFromInt(self.width - 1);
        const max_y: f64 = @floatFromInt(self.height - 1);
        return .{
            .x = @intFromFloat(std.math.clamp(fx, @as(f64, 0), max_x)),
            .y = @intFromFloat(std.math.clamp(fy, @as(f64, 0), max_y)),
        };
    }
};

const JsonRecord = struct {
    client_id: []const u8,
    client_name: ?[]const u8 = null,
    client_version: ?[]const u8 = null,
    endpoint_id: []const u8,
    width: u16,
    height: u16,
    x_a: f64,
    x_b: f64,
    y_a: f64,
    y_b: f64,
    rmse: f64,
    rounds: u32 = 1,
    created_at: i64,
    updated_at: i64,
};

const JsonDoc = struct {
    version: u32 = 1,
    calibrations: std.json.ArrayHashMap(JsonRecord),
};

pub const Store = struct {
    parsed: std.json.Parsed(JsonDoc),

    pub fn deinit(self: *Store) void {
        self.parsed.deinit();
    }

    /// Find a record by exact (client, endpoint, resolution) key.
    pub fn find(self: *const Store, client_id: []const u8, endpoint_id: []const u8, width: u16, height: u16) ?*const JsonRecord {
        var it = self.parsed.value.calibrations.map.iterator();
        while (it.next()) |entry| {
            const r = entry.value_ptr;
            if (std.mem.eql(u8, r.client_id, client_id) and
                std.mem.eql(u8, r.endpoint_id, endpoint_id) and
                r.width == width and r.height == height)
                return r;
        }
        return null;
    }

    pub fn findAnyForClient(self: *const Store, client_id: []const u8) ?*const JsonRecord {
        var it = self.parsed.value.calibrations.map.iterator();
        while (it.next()) |entry| {
            if (std.mem.eql(u8, entry.value_ptr.client_id, client_id)) return entry.value_ptr;
        }
        return null;
    }

    /// Latest calibration of this client for the endpoint, any resolution.
    pub fn findAnyForEndpoint(self: *const Store, client_id: []const u8, endpoint_id: []const u8) ?*const JsonRecord {
        var best: ?*const JsonRecord = null;
        var it = self.parsed.value.calibrations.map.iterator();
        while (it.next()) |entry| {
            const r = entry.value_ptr;
            if (std.mem.eql(u8, r.client_id, client_id) and std.mem.eql(u8, r.endpoint_id, endpoint_id)) {
                if (best == null or r.updated_at > best.?.updated_at) best = r;
            }
        }
        return best;
    }
};

/// Load the calibration store. An absent file is an empty store, not an error.
pub fn load(allocator: std.mem.Allocator) !Store {
    const path = try filePath(allocator);
    defer allocator.free(path);

    const file = std.fs.openFileAbsolute(path, .{}) catch |err| switch (err) {
        error.FileNotFound => return Store{
            .parsed = try std.json.parseFromSlice(JsonDoc, allocator, "{\"version\":1,\"calibrations\":{}}", .{}),
        },
        else => return err,
    };
    defer file.close();

    const data = try file.readToEndAlloc(allocator, 4 * 1024 * 1024);
    defer allocator.free(data);

    return Store{
        .parsed = try std.json.parseFromSlice(JsonDoc, allocator, data, .{
            .ignore_unknown_fields = true,
            // String fields must be copied — `data` is freed above.
            .allocate = .alloc_always,
        }),
    };
}

/// Merge-or-insert a record and rewrite the store atomically (tmp + rename).
/// Preserves `created_at` when replacing an existing record with the same key.
pub fn upsert(allocator: std.mem.Allocator, record: *const Record, id: []const u8) !void {
    var store = try load(allocator);
    defer store.deinit();

    const now = std.time.timestamp();

    // Build fresh document as a JSON value tree, copying prior records.
    // NOTE: cals must be fully populated BEFORE being moved into root —
    // ObjectMap copies are shallow and a put-induced rehash invalidates
    // the copy already stored in root.
    var root = std.json.ObjectMap.init(allocator);
    defer root.deinit();
    try root.put("version", .{ .integer = 1 });
    var cals = std.json.ObjectMap.init(allocator);

    var preserved_created: ?i64 = null;
    var it = store.parsed.value.calibrations.map.iterator();
    while (it.next()) |entry| {
        const r = entry.value_ptr;
        const is_same = std.mem.eql(u8, r.client_id, record.client_id) and
            std.mem.eql(u8, r.endpoint_id, record.endpoint_id) and
            r.width == record.width and r.height == record.height;
        if (is_same) {
            preserved_created = r.created_at;
            continue; // replaced by the new record below
        }
        try cals.put(entry.key_ptr.*, try recordToJson(allocator, r, r.created_at, r.updated_at));
    }

    try cals.put(try allocator.dupe(u8, id), try recordToJson(allocator, &.{
        .client_id = record.client_id,
        .client_name = record.client_name,
        .client_version = record.client_version,
        .endpoint_id = record.endpoint_id,
        .width = record.width,
        .height = record.height,
        .x_a = record.x_a,
        .x_b = record.x_b,
        .y_a = record.y_a,
        .y_b = record.y_b,
        .rmse = record.rmse,
        .rounds = record.rounds,
        .created_at = if (record.created_at > 0) record.created_at else (preserved_created orelse now),
        .updated_at = now,
    }, if (record.created_at > 0) record.created_at else (preserved_created orelse now), now));

    try root.put("calibrations", .{ .object = cals });

    const out = try std.fmt.allocPrint(allocator, "{f}", .{std.json.fmt(std.json.Value{ .object = root }, .{ .whitespace = .indent_2 })});
    defer allocator.free(out);

    const dir = try configDir(allocator);
    defer allocator.free(dir);
    std.fs.makeDirAbsolute(dir) catch |err| switch (err) {
        error.PathAlreadyExists => {},
        else => return err,
    };

    const path = try filePath(allocator);
    defer allocator.free(path);
    const tmp_path = try std.fmt.allocPrint(allocator, "{s}.tmp", .{path});
    defer allocator.free(tmp_path);

    const f = try std.fs.createFileAbsolute(tmp_path, .{ .truncate = true });
    try f.writeAll(out);
    f.close();
    try std.fs.renameAbsolute(tmp_path, path);
}

fn recordToJson(allocator: std.mem.Allocator, r: *const JsonRecord, created: i64, updated: i64) !std.json.Value {
    var obj = std.json.ObjectMap.init(allocator);
    try obj.put("client_id", .{ .string = r.client_id });
    if (r.client_name) |v| try obj.put("client_name", .{ .string = v });
    if (r.client_version) |v| try obj.put("client_version", .{ .string = v });
    try obj.put("endpoint_id", .{ .string = r.endpoint_id });
    try obj.put("width", .{ .integer = r.width });
    try obj.put("height", .{ .integer = r.height });
    try obj.put("x_a", .{ .float = r.x_a });
    try obj.put("x_b", .{ .float = r.x_b });
    try obj.put("y_a", .{ .float = r.y_a });
    try obj.put("y_b", .{ .float = r.y_b });
    try obj.put("rmse", .{ .float = r.rmse });
    try obj.put("rounds", .{ .integer = r.rounds });
    try obj.put("created_at", .{ .integer = created });
    try obj.put("updated_at", .{ .integer = updated });
    return std.json.Value{ .object = obj };
}

/// Remove a record. Returns true when something was deleted.
pub fn remove(allocator: std.mem.Allocator, client_id: []const u8, endpoint_id: []const u8, width: u16, height: u16) !bool {
    var store = try load(allocator);
    defer store.deinit();

    var root = std.json.ObjectMap.init(allocator);
    defer root.deinit();
    try root.put("version", .{ .integer = 1 });
    var cals = std.json.ObjectMap.init(allocator);

    var removed = false;
    var it = store.parsed.value.calibrations.map.iterator();
    while (it.next()) |entry| {
        const r = entry.value_ptr;
        const is_match = std.mem.eql(u8, r.client_id, client_id) and
            std.mem.eql(u8, r.endpoint_id, endpoint_id) and
            r.width == width and r.height == height;
        if (is_match) {
            removed = true;
            continue;
        }
        try cals.put(entry.key_ptr.*, try recordToJson(allocator, r, r.created_at, r.updated_at));
    }
    if (!removed) return false;
    try root.put("calibrations", .{ .object = cals });

    const out = try std.fmt.allocPrint(allocator, "{f}", .{std.json.fmt(std.json.Value{ .object = root }, .{ .whitespace = .indent_2 })});
    defer allocator.free(out);

    const path = try filePath(allocator);
    defer allocator.free(path);
    const tmp_path = try std.fmt.allocPrint(allocator, "{s}.tmp", .{path});
    defer allocator.free(tmp_path);
    const f = try std.fs.createFileAbsolute(tmp_path, .{ .truncate = true });
    try f.writeAll(out);
    f.close();
    try std.fs.renameAbsolute(tmp_path, path);
    return true;
}

// ---- Least-squares solve ----

pub const Sample = struct {
    id: []const u8,
    fb_x: f64,
    fb_y: f64,
    obs_x: f64,
    obs_y: f64,
};

pub const Solution = struct {
    x_a: f64,
    x_b: f64,
    y_a: f64,
    y_b: f64,
    rmse: f64, // framebuffer px, combined x/y residual RMS
    residuals: []f64, // per-sample |error| in fb px (caller-owned)
};

/// Solve fb = a*obs + b independently per axis. Needs >= 2 non-degenerate
/// samples. Returns null when samples can't determine the transform.
pub fn solve(allocator: std.mem.Allocator, samples: []const Sample) !?Solution {
    if (samples.len < 2) return null;

    const xs = try allocator.alloc(f64, samples.len);
    defer allocator.free(xs);
    const ys = try allocator.alloc(f64, samples.len);
    defer allocator.free(ys);
    const obs_x = try allocator.alloc(f64, samples.len);
    defer allocator.free(obs_x);
    const obs_y = try allocator.alloc(f64, samples.len);
    defer allocator.free(obs_y);

    for (samples, 0..) |s, i| {
        xs[i] = s.fb_x;
        ys[i] = s.fb_y;
        obs_x[i] = s.obs_x;
        obs_y[i] = s.obs_y;
    }

    const x_fit = fitLinear(xs, obs_x) orelse return null;
    const y_fit = fitLinear(ys, obs_y) orelse return null;

    const residuals = try allocator.alloc(f64, samples.len);
    errdefer allocator.free(residuals);
    var sum_sq: f64 = 0;
    for (samples, 0..) |_, i| {
        const ex = (x_fit.a * obs_x[i] + x_fit.b) - xs[i];
        const ey = (y_fit.a * obs_y[i] + y_fit.b) - ys[i];
        residuals[i] = @sqrt(ex * ex + ey * ey);
        sum_sq += ex * ex + ey * ey;
    }

    return Solution{
        .x_a = x_fit.a,
        .x_b = x_fit.b,
        .y_a = y_fit.a,
        .y_b = y_fit.b,
        .rmse = @sqrt(sum_sq / @as(f64, @floatFromInt(samples.len * 2))),
        .residuals = residuals,
    };
}

const Fit = struct { a: f64, b: f64 };

fn fitLinear(target: []const f64, obs: []const f64) ?Fit {
    const n: f64 = @floatFromInt(target.len);
    var sum_o: f64 = 0;
    var sum_t: f64 = 0;
    var sum_oo: f64 = 0;
    var sum_ot: f64 = 0;
    for (target, obs) |t, o| {
        sum_o += o;
        sum_t += t;
        sum_oo += o * o;
        sum_ot += o * t;
    }
    const denom = n * sum_oo - sum_o * sum_o;
    if (@abs(denom) < 1e-9) return null; // degenerate: all observations identical
    const a = (n * sum_ot - sum_o * sum_t) / denom;
    const b = (sum_t - a * sum_o) / n;
    return .{ .a = a, .b = b };
}

// ---- Status line (appended to spatial tool responses) ----

/// Human/agent-facing one-liner about calibration state for this endpoint at
/// this resolution. Always includes endpoint + resolution so the model sees
/// exactly what the note applies to.
pub fn statusLine(allocator: std.mem.Allocator, endpoint_id: []const u8, width: u16, height: u16) []u8 {
    const cid = clientId(allocator) catch {
        return allocDupe(allocator, "Calibration: UNAVAILABLE (internal error)");
    };
    if (cid) |c| {
        defer allocator.free(c);
        if (!isUsableClient(c)) {
            return std.fmt.allocPrint(allocator, "Calibration: DISABLED (client identity '{s}' is generic) — pass framebuffer coordinates computed from the Resolution metadata.", .{c}) catch allocDupe(allocator, "Calibration: DISABLED");
        }
        var store = load(allocator) catch {
            return std.fmt.allocPrint(allocator, "Calibration: UNKNOWN (error reading calibration.json) — treat coordinates as framebuffer pixels.", .{}) catch allocDupe(allocator, "Calibration: UNKNOWN");
        };
        defer store.deinit();
        if (store.find(c, endpoint_id, width, height)) |rec| {
            const id = calibrationId(allocator, c, endpoint_id, width, height) catch allocDupe(allocator, "?");
            defer allocator.free(id);
            return std.fmt.allocPrint(allocator, "Calibration: ACTIVE (id {s}, rmse {d:.1}px, endpoint {s} {d}x{d}) — image-space coordinates are mapped automatically when coordinate_space=\"calibrated\"; you may read targets directly from returned images.", .{ id, rec.rmse, endpoint_id, width, height }) catch allocDupe(allocator, "Calibration: ACTIVE");
        }
        if (store.findAnyForEndpoint(c, endpoint_id)) |rec| {
            return std.fmt.allocPrint(allocator, "Calibration: STALE (saved for {s} at {d}x{d}, current is {d}x{d}) — re-run vnc_calibrate; do NOT trust image-space coordinates until recalibrated.", .{ endpoint_id, rec.width, rec.height, width, height }) catch allocDupe(allocator, "Calibration: STALE");
        }
        return std.fmt.allocPrint(allocator, "Calibration: ABSENT (endpoint {s} at {d}x{d}) — clicks are INACCURATE until calibration is done; run vnc_calibrate, or compute coordinates from the Resolution metadata.", .{ endpoint_id, width, height }) catch allocDupe(allocator, "Calibration: ABSENT");
    }
    return allocDupe(allocator, "Calibration: UNKNOWN (client did not identify itself) — compute coordinates from the Resolution metadata.");
}

fn allocDupe(allocator: std.mem.Allocator, s: []const u8) []u8 {
    return allocator.dupe(u8, s) catch @constCast("out of memory");
}

// ---- Discovery log ----

/// Append one JSON line per initialize to client-log.jsonl — the empirical
/// record of which clientInfo strings each agent sends across sessions.
pub fn logInitialize(allocator: std.mem.Allocator, protocol_version: ?[]const u8) void {
    const dir = configDir(allocator) catch return;
    defer allocator.free(dir);
    std.fs.makeDirAbsolute(dir) catch {};
    const path = std.fmt.allocPrint(allocator, "{s}/client-log.jsonl", .{dir}) catch return;
    defer allocator.free(path);

    const esc_name = jsonEscapeAlloc(allocator, clientName() orelse "") catch return;
    defer allocator.free(esc_name);
    const esc_ver = jsonEscapeAlloc(allocator, clientVersion() orelse "") catch return;
    defer allocator.free(esc_ver);
    const esc_pv = jsonEscapeAlloc(allocator, protocol_version orelse "") catch return;
    defer allocator.free(esc_pv);

    const line = std.fmt.allocPrint(allocator, "{{\"ts\":{d},\"protocolVersion\":\"{s}\",\"clientInfo\":{{\"name\":\"{s}\",\"version\":\"{s}\"}}}}\n", .{ std.time.timestamp(), esc_pv, esc_name, esc_ver }) catch return;
    defer allocator.free(line);

    const f = std.fs.createFileAbsolute(path, .{ .truncate = false }) catch return;
    defer f.close();
    f.seekFromEnd(0) catch return;
    f.writeAll(line) catch {};
}

fn jsonEscapeAlloc(allocator: std.mem.Allocator, input: []const u8) ![]u8 {
    var extra: usize = 0;
    for (input) |ch| {
        if (ch == '"' or ch == '\\') extra += 1;
    }
    const out = try allocator.alloc(u8, input.len + extra);
    var i: usize = 0;
    for (input) |ch| {
        if (ch == '"' or ch == '\\') {
            out[i] = '\\';
            i += 1;
        }
        out[i] = ch;
        i += 1;
    }
    return out;
}
