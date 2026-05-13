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

/// Test if pixel (px, py) is on the marker ring or crosshair
fn isMarkerPixel(dx: i32, dy: i32, r: i32, s: i32) bool {
    const dist_sq = dx * dx + dy * dy;
    const inner = (r - s) * (r - s);
    const outer = (r + s) * (r + s);
    const on_ring = dist_sq >= inner and dist_sq <= outer;
    const on_cross = (@abs(dx) <= 1 and @abs(dy) <= r) or
        (@abs(dy) <= 1 and @abs(dx) <= r);
    return on_ring or on_cross;
}

/// Set a pixel in RGB888 buffer
fn setPixel(rgb: []u8, w: i32, h: i32, px: i32, py: i32, r: u8, g: u8, b: u8) void {
    if (px < 0 or py < 0 or px >= w or py >= h) return;
    const idx: usize = (@as(usize, @intCast(py)) * @as(usize, @intCast(w)) + @as(usize, @intCast(px))) * 3;
    if (idx + 2 < rgb.len) {
        rgb[idx] = r;
        rgb[idx + 1] = g;
        rgb[idx + 2] = b;
    }
}

/// Draw a yellow circle marker with crosshair and black outline on RGB888 pixel data.
/// Two-pass rendering: black outline (1px expansion) then yellow fill for contrast.
pub fn drawMarker(rgb: []u8, width: u16, height: u16, cx: u16, cy: u16, radius: u16, stroke: u16) void {
    const r = @as(i32, radius);
    const s = @as(i32, stroke);
    const w = @as(i32, width);
    const h = @as(i32, height);
    const mcx = @as(i32, cx);
    const mcy = @as(i32, cy);

    const outline: i32 = 2; // outline expansion in pixels
    const x0 = @max(mcx - r - s - outline, 0);
    const y0 = @max(mcy - r - s - outline, 0);
    const x1 = @min(mcx + r + s + outline, w - 1);
    const y1 = @min(mcy + r + s + outline, h - 1);

    // Pass 1: black outline — draw black where any neighbor is a marker pixel
    var py = y0;
    while (py <= y1) : (py += 1) {
        var px = x0;
        while (px <= x1) : (px += 1) {
            const dx = px - mcx;
            const dy = py - mcy;
            if (isMarkerPixel(dx, dy, r, s)) continue; // will be yellow in pass 2

            // Check if any neighbor within outline distance is a marker pixel
            var is_outline = false;
            var oy: i32 = -outline;
            while (oy <= outline and !is_outline) : (oy += 1) {
                var ox: i32 = -outline;
                while (ox <= outline and !is_outline) : (ox += 1) {
                    if (isMarkerPixel(dx + ox, dy + oy, r, s)) {
                        is_outline = true;
                    }
                }
            }
            if (is_outline) {
                setPixel(rgb, w, h, px, py, 0, 0, 0);
            }
        }
    }

    // Pass 2: yellow marker fill
    py = @max(mcx - r - s, 0);
    py = @max(mcy - r - s, 0);
    while (py <= @min(mcy + r + s, h - 1)) : (py += 1) {
        var px = @max(mcx - r - s, 0);
        while (px <= @min(mcx + r + s, w - 1)) : (px += 1) {
            if (isMarkerPixel(px - mcx, py - mcy, r, s)) {
                setPixel(rgb, w, h, px, py, 255, 255, 0);
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

/// Minimal 5x7 bitmap font for grid labels (digits 0-9, uppercase A-Z)
/// Each glyph is 7 rows of 5 bits, stored MSB-first in the upper 5 bits of each byte.
const font_5x7 = [36][7]u8{
    // '0'
    .{ 0x70, 0x88, 0x98, 0xA8, 0xC8, 0x88, 0x70 },
    // '1'
    .{ 0x20, 0x60, 0x20, 0x20, 0x20, 0x20, 0x70 },
    // '2'
    .{ 0x70, 0x88, 0x08, 0x10, 0x20, 0x40, 0xF8 },
    // '3'
    .{ 0xF8, 0x10, 0x20, 0x10, 0x08, 0x88, 0x70 },
    // '4'
    .{ 0x10, 0x30, 0x50, 0x90, 0xF8, 0x10, 0x10 },
    // '5'
    .{ 0xF8, 0x80, 0xF0, 0x08, 0x08, 0x88, 0x70 },
    // '6'
    .{ 0x30, 0x40, 0x80, 0xF0, 0x88, 0x88, 0x70 },
    // '7'
    .{ 0xF8, 0x08, 0x10, 0x20, 0x40, 0x40, 0x40 },
    // '8'
    .{ 0x70, 0x88, 0x88, 0x70, 0x88, 0x88, 0x70 },
    // '9'
    .{ 0x70, 0x88, 0x88, 0x78, 0x08, 0x10, 0x60 },
    // 'A'
    .{ 0x70, 0x88, 0x88, 0xF8, 0x88, 0x88, 0x88 },
    // 'B'
    .{ 0xF0, 0x88, 0x88, 0xF0, 0x88, 0x88, 0xF0 },
    // 'C'
    .{ 0x70, 0x88, 0x80, 0x80, 0x80, 0x88, 0x70 },
    // 'D'
    .{ 0xE0, 0x90, 0x88, 0x88, 0x88, 0x90, 0xE0 },
    // 'E'
    .{ 0xF8, 0x80, 0x80, 0xF0, 0x80, 0x80, 0xF8 },
    // 'F'
    .{ 0xF8, 0x80, 0x80, 0xF0, 0x80, 0x80, 0x80 },
    // 'G'
    .{ 0x70, 0x88, 0x80, 0xB8, 0x88, 0x88, 0x70 },
    // 'H'
    .{ 0x88, 0x88, 0x88, 0xF8, 0x88, 0x88, 0x88 },
    // 'I'
    .{ 0x70, 0x20, 0x20, 0x20, 0x20, 0x20, 0x70 },
    // 'J'
    .{ 0x38, 0x10, 0x10, 0x10, 0x10, 0x90, 0x60 },
    // 'K'
    .{ 0x88, 0x90, 0xA0, 0xC0, 0xA0, 0x90, 0x88 },
    // 'L'
    .{ 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0xF8 },
    // 'M'
    .{ 0x88, 0xD8, 0xA8, 0xA8, 0x88, 0x88, 0x88 },
    // 'N'
    .{ 0x88, 0xC8, 0xA8, 0x98, 0x88, 0x88, 0x88 },
    // 'O'
    .{ 0x70, 0x88, 0x88, 0x88, 0x88, 0x88, 0x70 },
    // 'P'
    .{ 0xF0, 0x88, 0x88, 0xF0, 0x80, 0x80, 0x80 },
    // 'Q' (unused but kept for completeness)
    .{ 0x70, 0x88, 0x88, 0x88, 0xA8, 0x90, 0x68 },
    // 'R' (unused)
    .{ 0xF0, 0x88, 0x88, 0xF0, 0xA0, 0x90, 0x88 },
    // 'S' (unused)
    .{ 0x70, 0x88, 0x80, 0x70, 0x08, 0x88, 0x70 },
    // 'T' (unused)
    .{ 0xF8, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20 },
    // 'U' (unused)
    .{ 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x70 },
    // 'V' (unused)
    .{ 0x88, 0x88, 0x88, 0x88, 0x88, 0x50, 0x20 },
    // 'W' (unused)
    .{ 0x88, 0x88, 0x88, 0xA8, 0xA8, 0xD8, 0x88 },
    // 'X' (unused)
    .{ 0x88, 0x88, 0x50, 0x20, 0x50, 0x88, 0x88 },
    // 'Y' (unused)
    .{ 0x88, 0x88, 0x50, 0x20, 0x20, 0x20, 0x20 },
    // 'Z' (unused)
    .{ 0xF8, 0x08, 0x10, 0x20, 0x40, 0x80, 0xF8 },
};

/// Get glyph index for a character (0-9 → 0-9, A-Z → 10-35), null if unsupported
fn glyphIndex(ch: u8) ?usize {
    if (ch >= '0' and ch <= '9') return ch - '0';
    if (ch >= 'A' and ch <= 'Z') return (ch - 'A') + 10;
    if (ch >= 'a' and ch <= 'z') return (ch - 'a') + 10;
    return null;
}

/// Draw a single character at (px, py) with given scale and color on RGB888 buffer
fn drawChar(rgb: []u8, width: u16, height: u16, ch: u8, px: i32, py: i32, scale: u8, r: u8, g: u8, b: u8) void {
    const idx = glyphIndex(ch) orelse return;
    const glyph = font_5x7[idx];
    const w = @as(i32, width);
    const h = @as(i32, height);
    const sc = @as(i32, scale);

    for (0..7) |row| {
        const row_data = glyph[row];
        for (0..5) |col| {
            // Check if pixel is set (MSB-first, bits 7..3 represent columns 0..4)
            const bit: u3 = @intCast(7 - col);
            if ((row_data >> bit) & 1 == 0) continue;

            // Draw scaled pixel block
            var sy: i32 = 0;
            while (sy < sc) : (sy += 1) {
                var sx: i32 = 0;
                while (sx < sc) : (sx += 1) {
                    const fx = px + @as(i32, @intCast(col)) * sc + sx;
                    const fy = py + @as(i32, @intCast(row)) * sc + sy;
                    if (fx < 0 or fy < 0 or fx >= w or fy >= h) continue;
                    const offset: usize = (@as(usize, @intCast(fy)) * @as(usize, @intCast(w)) + @as(usize, @intCast(fx))) * 3;
                    if (offset + 2 < rgb.len) {
                        rgb[offset] = r;
                        rgb[offset + 1] = g;
                        rgb[offset + 2] = b;
                    }
                }
            }
        }
    }
}

/// Draw a string at (px, py) with given scale and color on RGB888 buffer
fn drawString(rgb: []u8, width: u16, height: u16, text: []const u8, px: i32, py: i32, scale: u8, r: u8, g: u8, b: u8) void {
    const char_width = @as(i32, scale) * 6; // 5px glyph + 1px spacing
    var x = px;
    for (text) |ch| {
        drawChar(rgb, width, height, ch, x, py, scale, r, g, b);
        x += char_width;
    }
}

/// Draw a labeled grid overlay on RGB888 pixel data.
/// Columns labeled A-P, rows labeled 1-12. Each cell center coordinate returned via metadata.
pub fn drawGrid(rgb: []u8, width: u16, height: u16, cols: u8, rows: u8) void {
    const w = @as(i32, width);
    const h = @as(i32, height);
    const col_width = @divFloor(w, @as(i32, cols));
    const row_height = @divFloor(h, @as(i32, rows));

    // Draw vertical gridlines (cyan, 1px)
    for (1..@as(usize, cols)) |ci| {
        const x = @as(i32, @intCast(ci)) * col_width;
        if (x >= w) continue;
        var y: i32 = 0;
        while (y < h) : (y += 1) {
            const offset: usize = (@as(usize, @intCast(y)) * @as(usize, @intCast(w)) + @as(usize, @intCast(x))) * 3;
            if (offset + 2 < rgb.len) {
                // Cyan with some transparency effect (blend toward cyan)
                rgb[offset] = rgb[offset] / 2;
                rgb[offset + 1] = @as(u8, @intCast((@as(u16, rgb[offset + 1]) + 255) / 2));
                rgb[offset + 2] = @as(u8, @intCast((@as(u16, rgb[offset + 2]) + 255) / 2));
            }
        }
    }

    // Draw horizontal gridlines (cyan, 1px)
    for (1..@as(usize, rows)) |r_idx| {
        const y = @as(i32, @intCast(r_idx)) * row_height;
        if (y >= h) continue;
        var x: i32 = 0;
        while (x < w) : (x += 1) {
            const offset: usize = (@as(usize, @intCast(y)) * @as(usize, @intCast(w)) + @as(usize, @intCast(x))) * 3;
            if (offset + 2 < rgb.len) {
                rgb[offset] = rgb[offset] / 2;
                rgb[offset + 1] = @as(u8, @intCast((@as(u16, rgb[offset + 1]) + 255) / 2));
                rgb[offset + 2] = @as(u8, @intCast((@as(u16, rgb[offset + 2]) + 255) / 2));
            }
        }
    }

    // Draw cell labels at center of each cell (white text with black outline for contrast)
    const scale: u8 = 2; // 10x14 pixel characters
    for (0..@as(usize, rows)) |r_idx| {
        for (0..@as(usize, cols)) |c_idx| {
            const cx = @as(i32, @intCast(c_idx)) * col_width + @divFloor(col_width, 2);
            const cy = @as(i32, @intCast(r_idx)) * row_height + @divFloor(row_height, 2);

            // Build label: column letter + row number (e.g., "A1", "B12")
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

            // Center the label
            const text_width = @as(i32, @intCast(label_len)) * @as(i32, scale) * 6;
            const text_height = @as(i32, scale) * 7;
            const lx = cx - @divFloor(text_width, 2);
            const ly = cy - @divFloor(text_height, 2);

            // Draw black outline (offset by 1 in each direction)
            const offsets = [_][2]i32{ .{ -1, -1 }, .{ 0, -1 }, .{ 1, -1 }, .{ -1, 0 }, .{ 1, 0 }, .{ -1, 1 }, .{ 0, 1 }, .{ 1, 1 } };
            for (offsets) |off| {
                drawString(rgb, width, height, label[0..label_len], lx + off[0], ly + off[1], scale, 0, 0, 0);
            }
            // Draw white text
            drawString(rgb, width, height, label[0..label_len], lx, ly, scale, 255, 255, 255);
        }
    }
}

/// Encode a framebuffer as JPEG with a grid overlay
pub fn encodeJpegWithGrid(allocator: std.mem.Allocator, fb: *const rfb_client.Framebuffer, quality: u8, cols: u8, rows: u8) ![]u8 {
    const rgb = try fb.toRgb888(allocator);
    defer allocator.free(rgb);

    drawGrid(rgb, fb.width, fb.height, cols, rows);

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
