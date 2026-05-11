const std = @import("std");
const rfb_client = @import("rfb/client.zig");

const c = @cImport({
    @cInclude("stb_image_write.h");
});

/// Context for stb write-to-memory callback
const WriteContext = struct {
    items: []u8 = &.{},
    len: usize = 0,
    allocator: std.mem.Allocator,

    fn appendData(self: *WriteContext, data: []const u8) void {
        if (self.len + data.len > self.items.len) {
            const new_cap = @max(self.items.len * 2, self.len + data.len);
            const new_buf = self.allocator.alloc(u8, new_cap) catch return;
            if (self.len > 0) @memcpy(new_buf[0..self.len], self.items[0..self.len]);
            if (self.items.len > 0) self.allocator.free(self.items);
            self.items = new_buf;
        }
        @memcpy(self.items[self.len .. self.len + data.len], data);
        self.len += data.len;
    }

    fn deinit(self: *WriteContext) void {
        if (self.items.len > 0) self.allocator.free(self.items);
    }

    fn toOwnedSlice(self: *WriteContext) ![]u8 {
        const result = try self.allocator.alloc(u8, self.len);
        @memcpy(result, self.items[0..self.len]);
        self.deinit();
        return result;
    }
};

fn stbWriteCallback(context: ?*anyopaque, data: ?*anyopaque, size: c_int) callconv(.c) void {
    const ctx: *WriteContext = @ptrCast(@alignCast(context orelse return));
    const ptr: [*]const u8 = @ptrCast(data orelse return);
    const len: usize = @intCast(size);
    ctx.appendData(ptr[0..len]);
}

/// Encode a framebuffer as JPEG and return the bytes
pub fn encodeJpeg(allocator: std.mem.Allocator, fb: *const rfb_client.Framebuffer, quality: u8) ![]u8 {
    // Convert framebuffer to packed RGB888
    const rgb = try fb.toRgb888(allocator);
    defer allocator.free(rgb);

    var ctx = WriteContext{ .allocator = allocator };
    errdefer ctx.deinit();

    const result = c.stbi_write_jpg_to_func(
        stbWriteCallback,
        &ctx,
        @intCast(fb.width),
        @intCast(fb.height),
        3, // RGB components
        rgb.ptr,
        @intCast(@min(quality, 100)),
    );

    if (result == 0) {
        ctx.deinit();
        return error.EncodingFailed;
    }

    return ctx.toOwnedSlice();
}

/// Encode a framebuffer as PNG and return the bytes
pub fn encodePng(allocator: std.mem.Allocator, fb: *const rfb_client.Framebuffer) ![]u8 {
    const rgb = try fb.toRgb888(allocator);
    defer allocator.free(rgb);

    var ctx = WriteContext{ .allocator = allocator };
    errdefer ctx.deinit();

    const result = c.stbi_write_png_to_func(
        stbWriteCallback,
        &ctx,
        @intCast(fb.width),
        @intCast(fb.height),
        3,
        rgb.ptr,
        @intCast(@as(usize, fb.width) * 3), // stride
    );

    if (result == 0) {
        ctx.deinit();
        return error.EncodingFailed;
    }

    return ctx.toOwnedSlice();
}
