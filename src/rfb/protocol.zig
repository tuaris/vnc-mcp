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
// Uses OpenSSL libcrypto for the actual DES encryption
pub const VncAuth = struct {
    const c = @cImport({
        @cInclude("openssl/des.h");
    });

    pub fn encrypt(challenge: *const [16]u8, password: []const u8) [16]u8 {
        var key: [8]u8 = [_]u8{0} ** 8;
        const len = @min(password.len, 8);
        @memcpy(key[0..len], password[0..len]);

        // VNC reverses bits in each key byte
        for (&key) |*b| {
            b.* = reverseBits(b.*);
        }

        // Set up DES key schedule
        var ks: c.DES_key_schedule = undefined;
        c.DES_set_key_unchecked(@ptrCast(&key), &ks);

        // Encrypt both 8-byte halves
        var result: [16]u8 = undefined;
        c.DES_ecb_encrypt(@constCast(@ptrCast(challenge[0..8])), @ptrCast(result[0..8]), &ks, c.DES_ENCRYPT);
        c.DES_ecb_encrypt(@constCast(@ptrCast(challenge[8..16])), @ptrCast(result[8..16]), &ks, c.DES_ENCRYPT);
        return result;
    }

    fn reverseBits(b: u8) u8 {
        var x = b;
        x = ((x & 0x55) << 1) | ((x & 0xAA) >> 1);
        x = ((x & 0x33) << 2) | ((x & 0xCC) >> 2);
        x = ((x & 0x0F) << 4) | ((x & 0xF0) >> 4);
        return x;
    }
};
