const std = @import("std");
const mcp_server = @import("mcp/server.zig");
const tools = @import("mcp/tools.zig");
const registry_mod = @import("registry.zig");

// Re-export modules so tests can access them
pub const rfb = struct {
    pub const protocol = @import("rfb/protocol.zig");
    pub const client = @import("rfb/client.zig");
    pub const encodings = @import("rfb/encodings.zig");
    pub const keysym = @import("rfb/keysym.zig");
};
pub const image = @import("image.zig");
pub const registry = registry_mod;
pub const mcp = struct {
    pub const server = mcp_server;
};

const log = std.log.scoped(.main);

pub const std_options: std.Options = .{
    .log_level = .info,
    .logFn = stderrLog,
};

fn stderrLog(
    comptime level: std.log.Level,
    comptime scope: @TypeOf(.enum_literal),
    comptime format: []const u8,
    args: anytype,
) void {
    var buf: [4096]u8 = undefined;
    const prefix = "[" ++ @tagName(scope) ++ "] " ++ @tagName(level) ++ ": ";
    const msg = std.fmt.bufPrint(&buf, prefix ++ format ++ "\n", args) catch return;
    std.fs.File.stderr().writeAll(msg) catch {};
}

fn getConfigPath(allocator: std.mem.Allocator) ![]u8 {
    // Check VNC_MCP_CONFIG env var first
    if (std.process.getEnvVarOwned(allocator, "VNC_MCP_CONFIG")) |path| {
        return path;
    } else |_| {}

    // Check command line args
    var args = try std.process.argsWithAllocator(allocator);
    defer args.deinit();

    _ = args.next(); // skip program name
    while (args.next()) |arg| {
        if (std.mem.startsWith(u8, arg, "--config=")) {
            return try allocator.dupe(u8, arg["--config=".len..]);
        }
    }

    // Default: ~/.config/vnc-mcp/endpoints.json
    if (std.process.getEnvVarOwned(allocator, "HOME")) |home| {
        defer allocator.free(home);
        return try std.fmt.allocPrint(allocator, "{s}/.config/vnc-mcp/endpoints.json", .{home});
    } else |_| {}

    return error.FileNotFound;
}

pub fn main() !void {
    var gpa = std.heap.GeneralPurposeAllocator(.{}){};
    defer _ = gpa.deinit();
    const allocator = gpa.allocator();

    // Load config
    const config_path = getConfigPath(allocator) catch |err| {
        log.err("failed to determine config path: {}", .{err});
        std.process.exit(1);
    };
    defer allocator.free(config_path);

    var reg = registry_mod.Registry.load(allocator, config_path) catch |err| {
        log.err("failed to load config from {s}: {}", .{ config_path, err });
        std.process.exit(1);
    };
    defer reg.deinit();

    // Connection pool
    var pool = tools.ConnectionPool.init(allocator);
    defer pool.deinit();

    // Wire up tools
    tools.setup(allocator, &reg, &pool);

    // MCP server on stdio
    const stdin_file = std.fs.File.stdin();
    const stdout_file = std.fs.File.stdout();

    var server = mcp_server.McpServer.init(allocator, stdin_file, stdout_file, tools.handleTool);

    log.info("vnc-mcp-server starting (config: {s})", .{config_path});

    server.run() catch |err| {
        log.err("server error: {}", .{err});
        std.process.exit(1);
    };

    log.info("vnc-mcp-server shutting down", .{});
}
