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

/// Draw a yellow circle marker with crosshair on RGB888 pixel data.
/// Pure pixel math — no external library, no Windows interaction.
pub fn drawMarker(rgb: []u8, width: u16, height: u16, cx: u16, cy: u16, radius: u16, stroke: u16) void {
    const r = @as(i32, radius);
    const s = @as(i32, stroke);
    const w = @as(i32, width);
    const h = @as(i32, height);
    const mcx = @as(i32, cx);
    const mcy = @as(i32, cy);

    // Bounding box
    const x0 = @max(mcx - r - s, 0);
    const y0 = @max(mcy - r - s, 0);
    const x1 = @min(mcx + r + s, w - 1);
    const y1 = @min(mcy + r + s, h - 1);

    var py = y0;
    while (py <= y1) : (py += 1) {
        var px = x0;
        while (px <= x1) : (px += 1) {
            const dx = px - mcx;
            const dy = py - mcy;
            const dist_sq = dx * dx + dy * dy;
            const inner = (r - s) * (r - s);
            const outer = (r + s) * (r + s);

            // Ring: between inner and outer radius
            const on_ring = dist_sq >= inner and dist_sq <= outer;
            // Crosshair: thin lines through center (2px wide)
            const on_cross = (@abs(dx) <= 1 and @abs(dy) <= r) or
                (@abs(dy) <= 1 and @abs(dx) <= r);

            if (on_ring or on_cross) {
                const idx: usize = (@as(usize, @intCast(py)) * @as(usize, @intCast(w)) + @as(usize, @intCast(px))) * 3;
                if (idx + 2 < rgb.len) {
                    rgb[idx] = 255; // R
                    rgb[idx + 1] = 255; // G
                    rgb[idx + 2] = 0; // B — yellow
                }
            }
        }
    }
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

/// Encode a framebuffer as JPEG with a yellow marker drawn at (cx, cy)
pub fn encodeJpegWithMarker(allocator: std.mem.Allocator, fb: *const rfb_client.Framebuffer, quality: u8, cx: u16, cy: u16) ![]u8 {
    const rgb = try fb.toRgb888(allocator);
    defer allocator.free(rgb);

    drawMarker(rgb, fb.width, fb.height, cx, cy, 20, 2);

    var ctx = WriteContext{ .allocator = allocator };
    errdefer ctx.deinit();

    const result = c.stbi_write_jpg_to_func(
        stbWriteCallback,
        &ctx,
        @intCast(fb.width),
        @intCast(fb.height),
        3,
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
