const std = @import("std");

const log = std.log.scoped(.registry);

pub const Endpoint = struct {
    id: []const u8,
    description: []const u8 = "",
    host: []const u8,
    port: u16 = 5900,
    password_file: []const u8 = "",
    helper_port: u16 = 0,
    default: bool = false,
};

pub const Registry = struct {
    endpoints: []Endpoint,
    allocator: std.mem.Allocator,
    json_str: []const u8,
    parsed: std.json.Parsed(Config),

    const Config = struct {
        endpoints: []const JsonEndpoint,
    };

    const JsonEndpoint = struct {
        id: []const u8,
        description: []const u8 = "",
        host: []const u8,
        port: u16 = 5900,
        password_file: []const u8 = "",
        helper_port: u16 = 0,
        default: bool = false,
    };

    pub fn load(allocator: std.mem.Allocator, path: []const u8) !Registry {
        const file = std.fs.openFileAbsolute(path, .{}) catch |err| {
            log.err("failed to open config: {s}: {}", .{ path, err });
            return err;
        };
        defer file.close();

        const json_str = file.readToEndAlloc(allocator, 1024 * 1024) catch |err| {
            log.err("failed to read config: {}", .{err});
            return err;
        };

        const parsed = std.json.parseFromSlice(Config, allocator, json_str, .{
            .ignore_unknown_fields = true,
        }) catch |err| {
            log.err("failed to parse config: {}", .{err});
            allocator.free(json_str);
            return err;
        };

        const json_endpoints = parsed.value.endpoints;
        const endpoints = try allocator.alloc(Endpoint, json_endpoints.len);

        for (json_endpoints, 0..) |je, i| {
            endpoints[i] = Endpoint{
                .id = je.id,
                .description = je.description,
                .host = je.host,
                .port = je.port,
                .password_file = je.password_file,
                .helper_port = je.helper_port,
                .default = je.default,
            };
        }

        log.info("loaded {d} endpoints from {s}", .{ endpoints.len, path });

        return Registry{
            .endpoints = endpoints,
            .allocator = allocator,
            .json_str = json_str,
            .parsed = parsed,
        };
    }

    pub fn deinit(self: *Registry) void {
        self.allocator.free(self.endpoints);
        self.parsed.deinit();
        self.allocator.free(self.json_str);
    }

    pub fn getDefault(self: *const Registry) ?*const Endpoint {
        for (self.endpoints) |*ep| {
            if (ep.default) return ep;
        }
        // If no default set, return first
        if (self.endpoints.len > 0) return &self.endpoints[0];
        return null;
    }

    pub fn getById(self: *const Registry, id: []const u8) ?*const Endpoint {
        for (self.endpoints) |*ep| {
            if (std.mem.eql(u8, ep.id, id)) return ep;
        }
        return null;
    }

    /// Read password from a password file (first line, trimmed)
    pub fn readPassword(allocator: std.mem.Allocator, path: []const u8) ![]u8 {
        if (path.len == 0) return error.FileNotFound;

        const file = try std.fs.openFileAbsolute(path, .{});
        defer file.close();

        const content = try file.readToEndAlloc(allocator, 4096);
        defer allocator.free(content);

        // Return first line, trimmed
        var end: usize = content.len;
        for (content, 0..) |ch, i| {
            if (ch == '\n' or ch == '\r') {
                end = i;
                break;
            }
        }

        const pw = try allocator.alloc(u8, end);
        @memcpy(pw, content[0..end]);
        return pw;
    }
};
