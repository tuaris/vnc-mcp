const std = @import("std");
const protocol = @import("protocol.zig");
const client_mod = @import("client.zig");

const Client = client_mod.Client;
const Framebuffer = client_mod.Framebuffer;
const ClientError = client_mod.ClientError;

/// Decode a Raw encoding rectangle into the framebuffer
pub fn decodeRaw(client: *Client, fb: *Framebuffer, rect: protocol.RectHeader) ClientError!void {
    const bpp: usize = fb.pixel_format.bytesPerPixel();
    const row_bytes = @as(usize, rect.width) * bpp;

    // Read row by row directly into framebuffer
    for (0..rect.height) |row| {
        const y = @as(usize, rect.y) + row;
        if (y >= fb.height) break;

        const fb_offset = (y * @as(usize, fb.width) + @as(usize, rect.x)) * bpp;
        const copy_width = @min(@as(usize, rect.width), @as(usize, fb.width) - @as(usize, rect.x));
        const copy_bytes = copy_width * bpp;

        // Read the full row from wire
        if (copy_bytes == row_bytes) {
            // Common case: rect fits entirely in framebuffer
            try client.readExact(fb.data[fb_offset .. fb_offset + copy_bytes]);
        } else {
            // Rect extends beyond framebuffer edge — read full row, copy what fits
            var row_buf: [16384]u8 = undefined;
            if (row_bytes <= row_buf.len) {
                try client.readExact(row_buf[0..row_bytes]);
                @memcpy(fb.data[fb_offset .. fb_offset + copy_bytes], row_buf[0..copy_bytes]);
            } else {
                // Very wide row — read in chunks
                var remaining = row_bytes;
                var written: usize = 0;
                while (remaining > 0) {
                    const chunk = @min(remaining, row_buf.len);
                    try client.readExact(row_buf[0..chunk]);
                    if (written < copy_bytes) {
                        const to_copy = @min(chunk, copy_bytes - written);
                        @memcpy(fb.data[fb_offset + written .. fb_offset + written + to_copy], row_buf[0..to_copy]);
                        written += to_copy;
                    }
                    remaining -= chunk;
                }
            }
        }
    }
}
