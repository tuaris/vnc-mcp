const std = @import("std");

// RFB Protocol constants and types per RFC 6143

pub const Version = struct {
    major: u8,
    minor: u8,

    pub const v3_3 = Version{ .major = 3, .minor = 3 };
    pub const v3_7 = Version{ .major = 3, .minor = 7 };
    pub const v3_8 = Version{ .major = 3, .minor = 8 };

    pub fn parse(buf: *const [12]u8) ?Version {
        if (!std.mem.startsWith(u8, buf, "RFB ")) return null;
        if (buf[7] != '.') return null;
        if (buf[11] != '\n') return null;
        const major = std.fmt.parseInt(u8, buf[4..7], 10) catch return null;
        const minor = std.fmt.parseInt(u8, buf[8..11], 10) catch return null;
        return Version{ .major = major, .minor = minor };
    }

    pub fn format(self: Version) [12]u8 {
        var buf: [12]u8 = undefined;
        _ = std.fmt.bufPrint(&buf, "RFB {d:0>3}.{d:0>3}\n", .{ self.major, self.minor }) catch unreachable;
        return buf;
    }
};

// Security types
pub const SecurityType = enum(u8) {
    invalid = 0,
    none = 1,
    vnc_authentication = 2,
    _,
};

// Security result
pub const SecurityResult = enum(u32) {
    ok = 0,
    failed = 1,
    _,
};

// Pixel format (16 bytes on wire)
pub const PixelFormat = struct {
    bits_per_pixel: u8,
    depth: u8,
    big_endian: bool,
    true_colour: bool,
    red_max: u16,
    green_max: u16,
    blue_max: u16,
    red_shift: u8,
    green_shift: u8,
    blue_shift: u8,

    pub const default_rgb888 = PixelFormat{
        .bits_per_pixel = 32,
        .depth = 24,
        .big_endian = false,
        .true_colour = true,
        .red_max = 255,
        .green_max = 255,
        .blue_max = 255,
        .red_shift = 16,
        .green_shift = 8,
        .blue_shift = 0,
    };

    pub fn decode(buf: *const [16]u8) PixelFormat {
        return PixelFormat{
            .bits_per_pixel = buf[0],
            .depth = buf[1],
            .big_endian = buf[2] != 0,
            .true_colour = buf[3] != 0,
            .red_max = std.mem.readInt(u16, buf[4..6], .big),
            .green_max = std.mem.readInt(u16, buf[6..8], .big),
            .blue_max = std.mem.readInt(u16, buf[8..10], .big),
            .red_shift = buf[10],
            .green_shift = buf[11],
            .blue_shift = buf[12],
        };
    }

    pub fn encode(self: PixelFormat) [16]u8 {
        var buf = [_]u8{0} ** 16;
        buf[0] = self.bits_per_pixel;
        buf[1] = self.depth;
        buf[2] = if (self.big_endian) 1 else 0;
        buf[3] = if (self.true_colour) 1 else 0;
        std.mem.writeInt(u16, buf[4..6], self.red_max, .big);
        std.mem.writeInt(u16, buf[6..8], self.green_max, .big);
        std.mem.writeInt(u16, buf[8..10], self.blue_max, .big);
        buf[10] = self.red_shift;
        buf[11] = self.green_shift;
        buf[12] = self.blue_shift;
        return buf;
    }

    pub fn bytesPerPixel(self: PixelFormat) u8 {
        return self.bits_per_pixel / 8;
    }
};

// Server init message
pub const ServerInit = struct {
    width: u16,
    height: u16,
    pixel_format: PixelFormat,
    name: []const u8,
};

// Client-to-server message types
pub const ClientMessageType = enum(u8) {
    set_pixel_format = 0,
    set_encodings = 2,
    framebuffer_update_request = 3,
    key_event = 4,
    pointer_event = 5,
    client_cut_text = 6,
    _,
};

// Server-to-client message types
pub const ServerMessageType = enum(u8) {
    framebuffer_update = 0,
    set_colour_map_entries = 1,
    bell = 2,
    server_cut_text = 3,
    _,
};

// Encoding types
pub const EncodingType = enum(i32) {
    raw = 0,
    copy_rect = 1,
    rre = 2,
    hextile = 5,
    zlib = 6,
    tight = 7,
    zrle = 16,
    cursor = -239,
    desktop_size = -223,
    _,
};

// Framebuffer update rectangle header
pub const RectHeader = struct {
    x: u16,
    y: u16,
    width: u16,
    height: u16,
    encoding: EncodingType,

    pub fn decode(buf: *const [12]u8) RectHeader {
        return RectHeader{
            .x = std.mem.readInt(u16, buf[0..2], .big),
            .y = std.mem.readInt(u16, buf[2..4], .big),
            .width = std.mem.readInt(u16, buf[4..6], .big),
            .height = std.mem.readInt(u16, buf[6..8], .big),
            .encoding = @enumFromInt(std.mem.readInt(i32, buf[8..12], .big)),
        };
    }
};

// VNC DES authentication — RFC 6143 Section 7.2.2
// VNC uses a modified DES where each byte of the key has its bits reversed
pub const VncAuth = struct {
    // Standard DES S-boxes, permutation tables, etc.
    // VNC auth: encrypt 16-byte challenge with DES using password as key

    pub fn encrypt(challenge: *const [16]u8, password: []const u8) [16]u8 {
        var key: [8]u8 = [_]u8{0} ** 8;
        const len = @min(password.len, 8);
        @memcpy(key[0..len], password[0..len]);

        // VNC reverses bits in each key byte
        for (&key) |*b| {
            b.* = reverseBits(b.*);
        }

        var result: [16]u8 = undefined;
        desEncryptBlock(challenge[0..8], &key, result[0..8]);
        desEncryptBlock(challenge[8..16], &key, result[8..16]);
        return result;
    }

    fn reverseBits(b: u8) u8 {
        var x = b;
        x = ((x & 0x55) << 1) | ((x & 0xAA) >> 1);
        x = ((x & 0x33) << 2) | ((x & 0xCC) >> 2);
        x = ((x & 0x0F) << 4) | ((x & 0xF0) >> 4);
        return x;
    }

    // DES implementation — minimal single-block ECB for VNC auth
    const IP: [64]u8 = .{
        58, 50, 42, 34, 26, 18, 10, 2, 60, 52, 44, 36, 28, 20, 12, 4,
        62, 54, 46, 38, 30, 22, 14, 6, 64, 56, 48, 40, 32, 24, 16, 8,
        57, 49, 41, 33, 25, 17, 9,  1, 59, 51, 43, 35, 27, 19, 11, 3,
        61, 53, 45, 37, 29, 21, 13, 5, 63, 55, 47, 39, 31, 23, 15, 7,
    };

    const FP: [64]u8 = .{
        40, 8, 48, 16, 56, 24, 64, 32, 39, 7, 47, 15, 55, 23, 63, 31,
        38, 6, 46, 14, 54, 22, 62, 30, 37, 5, 45, 13, 53, 21, 61, 29,
        36, 4, 44, 12, 52, 20, 60, 28, 35, 3, 43, 11, 51, 19, 59, 27,
        34, 2, 42, 10, 50, 18, 58, 26, 33, 1, 41, 9,  49, 17, 57, 25,
    };

    const E: [48]u8 = .{
        32, 1,  2,  3,  4,  5,  4,  5,  6,  7,  8,  9,
        8,  9,  10, 11, 12, 13, 12, 13, 14, 15, 16, 17,
        16, 17, 18, 19, 20, 21, 20, 21, 22, 23, 24, 25,
        24, 25, 26, 27, 28, 29, 28, 29, 30, 31, 32, 1,
    };

    const P: [32]u8 = .{
        16, 7, 20, 21, 29, 12, 28, 17, 1,  15, 23, 26, 5,  18, 31, 10,
        2,  8, 24, 14, 32, 27, 3,  9,  19, 13, 30, 6,  22, 11, 4,  25,
    };

    const S: [8][64]u8 = .{
        .{ 14, 4, 13, 1, 2, 15, 11, 8, 3, 10, 6, 12, 5, 9, 0, 7, 0, 15, 7, 4, 14, 2, 13, 1, 10, 6, 12, 11, 9, 5, 3, 8, 4, 1, 14, 8, 13, 6, 2, 11, 15, 12, 9, 7, 3, 10, 5, 0, 15, 12, 8, 2, 4, 9, 1, 7, 5, 11, 3, 14, 10, 0, 6, 13 },
        .{ 15, 1, 8, 14, 6, 11, 3, 4, 9, 7, 2, 13, 12, 0, 5, 10, 3, 13, 4, 7, 15, 2, 8, 14, 12, 0, 1, 10, 6, 9, 11, 5, 0, 14, 7, 11, 10, 4, 13, 1, 5, 8, 12, 6, 9, 3, 2, 15, 13, 8, 10, 1, 3, 15, 4, 2, 11, 6, 7, 12, 0, 5, 14, 9 },
        .{ 10, 0, 9, 14, 6, 3, 15, 5, 1, 13, 12, 7, 11, 4, 2, 8, 13, 7, 0, 9, 3, 4, 6, 10, 2, 8, 5, 14, 12, 11, 15, 1, 13, 6, 4, 9, 8, 15, 3, 0, 11, 1, 2, 12, 5, 10, 14, 7, 1, 10, 13, 0, 6, 9, 8, 7, 4, 15, 14, 3, 11, 5, 2, 12 },
        .{ 7, 13, 14, 3, 0, 6, 9, 10, 1, 2, 8, 5, 11, 12, 4, 15, 13, 8, 11, 5, 6, 15, 0, 3, 4, 7, 2, 12, 1, 10, 14, 9, 10, 6, 9, 0, 12, 11, 7, 13, 15, 1, 3, 14, 5, 2, 8, 4, 3, 15, 0, 6, 10, 1, 13, 8, 9, 4, 5, 11, 12, 7, 2, 14 },
        .{ 2, 12, 4, 1, 7, 10, 11, 6, 8, 5, 3, 15, 13, 0, 14, 9, 14, 11, 2, 12, 4, 7, 13, 1, 5, 0, 15, 10, 3, 9, 8, 6, 4, 2, 1, 11, 10, 13, 7, 8, 15, 9, 12, 5, 6, 3, 0, 14, 11, 8, 12, 7, 1, 14, 2, 13, 6, 15, 0, 9, 10, 4, 5, 3 },
        .{ 12, 1, 10, 15, 9, 2, 6, 8, 0, 13, 3, 4, 14, 7, 5, 11, 10, 15, 4, 2, 7, 12, 9, 5, 6, 1, 13, 14, 0, 11, 3, 8, 9, 14, 15, 5, 2, 8, 12, 3, 7, 0, 4, 10, 1, 13, 11, 6, 4, 3, 2, 12, 9, 5, 15, 10, 11, 14, 1, 7, 6, 0, 8, 13 },
        .{ 4, 11, 2, 14, 15, 0, 8, 13, 3, 12, 9, 7, 5, 10, 6, 1, 13, 0, 11, 7, 4, 9, 1, 10, 14, 3, 5, 12, 2, 15, 8, 6, 1, 4, 11, 13, 12, 3, 7, 14, 10, 15, 6, 8, 0, 5, 9, 2, 6, 11, 13, 8, 1, 4, 10, 7, 9, 5, 0, 15, 14, 2, 3, 12 },
        .{ 13, 2, 8, 4, 6, 15, 11, 1, 10, 9, 3, 14, 5, 0, 12, 7, 1, 15, 13, 8, 10, 3, 7, 4, 12, 5, 6, 2, 0, 14, 9, 11, 7, 0, 1, 13, 11, 6, 2, 8, 5, 14, 12, 3, 9, 10, 15, 4, 2, 1, 14, 7, 4, 10, 8, 13, 15, 12, 9, 0, 3, 5, 6, 11 },
    };

    const PC1: [56]u8 = .{
        57, 49, 41, 33, 25, 17, 9,  1, 58, 50, 42, 34, 26, 18,
        10, 2,  59, 51, 43, 35, 27, 19, 11, 3, 60, 52, 44, 36,
        63, 55, 47, 39, 31, 23, 15, 7, 62, 54, 46, 38, 30, 22,
        14, 6,  61, 53, 45, 37, 29, 21, 13, 5, 28, 20, 12, 4,
    };

    const PC2: [48]u8 = .{
        14, 17, 11, 24, 1,  5,  3,  28, 15, 6,  21, 10,
        23, 19, 12, 4,  26, 8,  16, 7,  27, 20, 13, 2,
        41, 52, 31, 37, 47, 55, 30, 40, 51, 45, 33, 48,
        44, 49, 39, 56, 34, 53, 46, 42, 50, 36, 29, 32,
    };

    const SHIFTS: [16]u8 = .{ 1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1 };

    fn getBit(data: []const u8, pos: u8) u1 {
        const byte_idx = @as(usize, pos - 1) / 8;
        const bit_idx: u3 = @intCast(7 - (@as(usize, pos - 1) % 8));
        return @truncate((data[byte_idx] >> bit_idx) & 1);
    }

    fn setBit(data: []u8, pos: u8, val: u1) void {
        const byte_idx = @as(usize, pos - 1) / 8;
        const bit_idx: u3 = @intCast(7 - (@as(usize, pos - 1) % 8));
        if (val == 1) {
            data[byte_idx] |= @as(u8, 1) << bit_idx;
        } else {
            data[byte_idx] &= ~(@as(u8, 1) << bit_idx);
        }
    }

    fn permute(input: []const u8, table: []const u8, output: []u8) void {
        @memset(output, 0);
        for (table, 0..) |pos, i| {
            const bit = getBit(input, pos);
            setBit(output, @intCast(i + 1), bit);
        }
    }

    fn leftRotate28(val: u32, count: u8) u32 {
        const c: u5 = @intCast(count);
        return ((val << c) | (val >> @intCast(28 - @as(u5, c)))) & 0x0FFFFFFF;
    }

    fn desEncryptBlock(input: *const [8]u8, key: *const [8]u8, output: *[8]u8) void {
        // Key schedule
        var cd: [7]u8 = undefined;
        permute(key, &PC1, &cd);

        var c: u32 = (@as(u32, cd[0]) << 20) | (@as(u32, cd[1]) << 12) | (@as(u32, cd[2]) << 4) | (@as(u32, cd[3]) >> 4);
        var d: u32 = (@as(u32, cd[3] & 0x0F) << 24) | (@as(u32, cd[4]) << 16) | (@as(u32, cd[5]) << 8) | @as(u32, cd[6]);

        var subkeys: [16][6]u8 = undefined;
        for (0..16) |i| {
            c = leftRotate28(c, SHIFTS[i]);
            d = leftRotate28(d, SHIFTS[i]);

            var cd56: [7]u8 = undefined;
            cd56[0] = @truncate(c >> 20);
            cd56[1] = @truncate(c >> 12);
            cd56[2] = @truncate(c >> 4);
            cd56[3] = @truncate((c << 4) | (d >> 24));
            cd56[4] = @truncate(d >> 16);
            cd56[5] = @truncate(d >> 8);
            cd56[6] = @truncate(d);

            permute(&cd56, &PC2, &subkeys[i]);
        }

        // Initial permutation
        var ip_out: [8]u8 = undefined;
        permute(input, &IP, &ip_out);

        var left: u32 = (@as(u32, ip_out[0]) << 24) | (@as(u32, ip_out[1]) << 16) | (@as(u32, ip_out[2]) << 8) | @as(u32, ip_out[3]);
        var right: u32 = (@as(u32, ip_out[4]) << 24) | (@as(u32, ip_out[5]) << 16) | (@as(u32, ip_out[6]) << 8) | @as(u32, ip_out[7]);

        // 16 Feistel rounds
        for (0..16) |i| {
            const f_result = feistel(right, &subkeys[i]);
            const new_right = left ^ f_result;
            left = right;
            right = new_right;
        }

        // Pre-output (swap halves)
        var pre: [8]u8 = undefined;
        pre[0] = @truncate(right >> 24);
        pre[1] = @truncate(right >> 16);
        pre[2] = @truncate(right >> 8);
        pre[3] = @truncate(right);
        pre[4] = @truncate(left >> 24);
        pre[5] = @truncate(left >> 16);
        pre[6] = @truncate(left >> 8);
        pre[7] = @truncate(left);

        // Final permutation
        permute(&pre, &FP, output);
    }

    fn feistel(r: u32, subkey: *const [6]u8) u32 {
        // Expand R from 32 to 48 bits
        var r_bytes: [4]u8 = undefined;
        r_bytes[0] = @truncate(r >> 24);
        r_bytes[1] = @truncate(r >> 16);
        r_bytes[2] = @truncate(r >> 8);
        r_bytes[3] = @truncate(r);

        var expanded: [6]u8 = undefined;
        permute(&r_bytes, &E, &expanded);

        // XOR with subkey
        for (0..6) |i| {
            expanded[i] ^= subkey[i];
        }

        // S-box substitution
        var sbox_out: u32 = 0;
        for (0..8) |si| {
            const bit_offset: u6 = @intCast(si * 6);
            // Extract 6 bits for this S-box
            const all48: u48 = (@as(u48, expanded[0]) << 40) | (@as(u48, expanded[1]) << 32) | (@as(u48, expanded[2]) << 24) | (@as(u48, expanded[3]) << 16) | (@as(u48, expanded[4]) << 8) | @as(u48, expanded[5]);

            const shift_amount: u6 = 42 - bit_offset;
            const six_bits: u6 = @truncate(all48 >> shift_amount);

            const row: u6 = ((six_bits & 0x20) >> 4) | (six_bits & 0x01);
            const col: u6 = (six_bits >> 1) & 0x0F;
            const sval: u32 = S[si][@as(usize, row) * 16 + @as(usize, col)];
            sbox_out |= sval << @intCast(28 - (si * 4));
        }

        // P permutation
        var sbox_bytes: [4]u8 = undefined;
        sbox_bytes[0] = @truncate(sbox_out >> 24);
        sbox_bytes[1] = @truncate(sbox_out >> 16);
        sbox_bytes[2] = @truncate(sbox_out >> 8);
        sbox_bytes[3] = @truncate(sbox_out);

        var p_out: [4]u8 = undefined;
        permute(&sbox_bytes, &P, &p_out);

        return (@as(u32, p_out[0]) << 24) | (@as(u32, p_out[1]) << 16) | (@as(u32, p_out[2]) << 8) | @as(u32, p_out[3]);
    }
};
