const std = @import("std");
const protocol = @import("protocol.zig");
const encodings = @import("encodings.zig");

const log = std.log.scoped(.rfb);

pub const ClientError = error{
    ConnectionFailed,
    VersionMismatch,
    AuthFailed,
    AuthRequired,
    NoSupportedSecurity,
    ProtocolError,
    Timeout,
    Disconnected,
    UnsupportedEncoding,
    FramebufferNotReady,
    OutOfMemory,
    Unexpected,
    ConnectionResetByPeer,
    EndOfStream,
    BrokenPipe,
    InputOutput,
    NotOpenForReading,
    OperationAborted,
    SocketNotConnected,
    WouldBlock,
    ConnectionRefused,
    NetworkUnreachable,
    AddressNotAvailable,
    AddressInUse,
    FileDescriptorInvalid,
    AccessDenied,
    HostUnreachable,
    SystemResources,
    InvalidArgument,
    NameTooLong,
    TemporaryNameError,
    AddressFamilyNotSupported,
    SymLinkLoop,
    FileNotFound,
    IsDir,
    PermissionDenied,
    NotDir,
    TimedOut,
    NetworkSubsystemFailed,
    UnexpectedReadFailure,
    ProcessFdInterrupted,
};

pub const Framebuffer = struct {
    width: u16,
    height: u16,
    pixel_format: protocol.PixelFormat,
    data: []u8,
    allocator: std.mem.Allocator,

    pub fn init(allocator: std.mem.Allocator, width: u16, height: u16, pf: protocol.PixelFormat) !Framebuffer {
        const bpp: usize = pf.bytesPerPixel();
        const size = @as(usize, width) * @as(usize, height) * bpp;
        const data = try allocator.alloc(u8, size);
        @memset(data, 0);
        return Framebuffer{
            .width = width,
            .height = height,
            .pixel_format = pf,
            .data = data,
            .allocator = allocator,
        };
    }

    pub fn deinit(self: *Framebuffer) void {
        self.allocator.free(self.data);
    }

    /// Get RGB pixel values at (x, y) — always returns (R, G, B) regardless of pixel format
    pub fn getPixelRgb(self: *const Framebuffer, x: u16, y: u16) [3]u8 {
        const bpp: usize = self.pixel_format.bytesPerPixel();
        const offset = (@as(usize, y) * @as(usize, self.width) + @as(usize, x)) * bpp;
        const pf = self.pixel_format;

        // Read raw pixel value
        var raw: u32 = 0;
        if (bpp == 4) {
            if (pf.big_endian) {
                raw = std.mem.readInt(u32, self.data[offset..][0..4], .big);
            } else {
                raw = std.mem.readInt(u32, self.data[offset..][0..4], .little);
            }
        } else if (bpp == 2) {
            if (pf.big_endian) {
                raw = std.mem.readInt(u16, self.data[offset..][0..2], .big);
            } else {
                raw = std.mem.readInt(u16, self.data[offset..][0..2], .little);
            }
        } else if (bpp == 1) {
            raw = self.data[offset];
        }

        const r: u8 = if (pf.red_max > 0) @truncate(((raw >> @intCast(pf.red_shift)) & pf.red_max) * 255 / pf.red_max) else 0;
        const g: u8 = if (pf.green_max > 0) @truncate(((raw >> @intCast(pf.green_shift)) & pf.green_max) * 255 / pf.green_max) else 0;
        const b: u8 = if (pf.blue_max > 0) @truncate(((raw >> @intCast(pf.blue_shift)) & pf.blue_max) * 255 / pf.blue_max) else 0;

        return .{ r, g, b };
    }

    /// Convert framebuffer to packed RGB888 for JPEG encoding
    pub fn toRgb888(self: *const Framebuffer, allocator: std.mem.Allocator) ![]u8 {
        const pixel_count = @as(usize, self.width) * @as(usize, self.height);
        const rgb = try allocator.alloc(u8, pixel_count * 3);

        for (0..@as(usize, self.height)) |y| {
            for (0..@as(usize, self.width)) |x| {
                const px = self.getPixelRgb(@intCast(x), @intCast(y));
                const idx = (y * @as(usize, self.width) + x) * 3;
                rgb[idx] = px[0];
                rgb[idx + 1] = px[1];
                rgb[idx + 2] = px[2];
            }
        }

        return rgb;
    }
};

pub const Client = struct {
    stream: std.net.Stream,
    server_name: [256]u8 = undefined,
    server_name_len: usize = 0,
    framebuffer: ?Framebuffer = null,
    pixel_format: protocol.PixelFormat = protocol.PixelFormat.default_rgb888,
    width: u16 = 0,
    height: u16 = 0,
    allocator: std.mem.Allocator,
    connected: bool = false,
    last_clipboard: ?[]u8 = null,
    /// FIFO of outstanding FramebufferUpdateRequests (value = incremental
    /// flag). RFB has no request/response correlation, but servers reply in
    /// request order over the ordered TCP stream, so each received
    /// FramebufferUpdate is mapped to the oldest unsatisfied request. This
    /// lets a caller tell a full (non-incremental) response — which by
    /// itself completely defines the frame — apart from a straggler delta
    /// belonging to an earlier call, which must not be mistaken for a
    /// fresh baseline.
    pending_updates: std.ArrayList(bool) = .{},

    pub fn connect(allocator: std.mem.Allocator, host: []const u8, port: u16, password: ?[]const u8) ClientError!Client {
        const stream = std.net.tcpConnectToHost(allocator, host, port) catch return error.ConnectionFailed;

        // Set socket read timeout to prevent indefinite hangs.
        // Without this, any tool call that reads from the VNC server
        // (screenshot, clipboard_get, etc.) can block the MCP server
        // process forever if the VNC server is unresponsive.
        const timeout = std.posix.timeval{ .sec = 15, .usec = 0 };
        std.posix.setsockopt(stream.handle, std.posix.SOL.SOCKET, std.posix.SO.RCVTIMEO, std.mem.asBytes(&timeout)) catch |err| {
            log.warn("failed to set SO_RCVTIMEO: {}", .{err});
        };

        var self = Client{
            .stream = stream,
            .allocator = allocator,
        };
        errdefer self.disconnect();

        try self.handshake(password);
        self.connected = true;
        return self;
    }

    pub fn disconnect(self: *Client) void {
        if (self.framebuffer) |*fb| {
            fb.deinit();
            self.framebuffer = null;
        }
        if (self.last_clipboard) |cb| {
            self.allocator.free(cb);
            self.last_clipboard = null;
        }
        self.pending_updates.deinit(self.allocator);
        self.stream.close();
        self.connected = false;
    }

    pub fn readExact(self: *Client, buf: []u8) ClientError!void {
        var total: usize = 0;
        while (total < buf.len) {
            const n = self.stream.read(buf[total..]) catch |err| {
                log.err("read error: {}", .{err});
                self.connected = false;
                return error.Disconnected;
            };
            if (n == 0) {
                self.connected = false;
                return error.Disconnected;
            }
            total += n;
        }
    }

    fn writeAll(self: *Client, data: []const u8) ClientError!void {
        self.stream.writeAll(data) catch {
            self.connected = false;
            return error.Disconnected;
        };
    }

    fn handshake(self: *Client, password: ?[]const u8) ClientError!void {
        // Read server version
        var ver_buf: [12]u8 = undefined;
        try self.readExact(&ver_buf);

        const server_ver = protocol.Version.parse(&ver_buf) orelse return error.VersionMismatch;
        log.info("server version: {d}.{d}", .{ server_ver.major, server_ver.minor });

        // Send our version (match server, capped at 3.8)
        const our_ver = if (server_ver.minor >= 8) protocol.Version.v3_8 else if (server_ver.minor >= 7) protocol.Version.v3_7 else protocol.Version.v3_3;

        const ver_str = our_ver.format();
        try self.writeAll(&ver_str);

        // Security negotiation
        if (our_ver.minor >= 7) {
            // 3.7+: server sends list of security types
            var num_types_buf: [1]u8 = undefined;
            try self.readExact(&num_types_buf);
            const num_types = num_types_buf[0];

            if (num_types == 0) {
                // Read error reason
                try self.readErrorMessage();
                return error.AuthFailed;
            }

            var types_buf: [256]u8 = undefined;
            try self.readExact(types_buf[0..num_types]);

            // Prefer VNC auth if password provided, otherwise None
            var chosen: ?protocol.SecurityType = null;
            for (types_buf[0..num_types]) |t| {
                const st: protocol.SecurityType = @enumFromInt(t);
                if (password != null and st == .vnc_authentication) {
                    chosen = st;
                    break;
                }
                if (st == .none) {
                    chosen = st;
                }
            }

            if (chosen == null) return error.NoSupportedSecurity;

            // Send chosen type
            const chosen_byte = [_]u8{@intFromEnum(chosen.?)};
            try self.writeAll(&chosen_byte);

            switch (chosen.?) {
                .vnc_authentication => {
                    if (password) |pw| {
                        try self.doVncAuth(pw);
                    } else {
                        return error.AuthRequired;
                    }
                },
                .none => {},
                else => return error.NoSupportedSecurity,
            }

            // Read security result (3.8 always sends it)
            if (our_ver.minor >= 8) {
                var result_buf: [4]u8 = undefined;
                try self.readExact(&result_buf);
                const result: protocol.SecurityResult = @enumFromInt(std.mem.readInt(u32, &result_buf, .big));
                if (result != .ok) {
                    try self.readErrorMessage();
                    return error.AuthFailed;
                }
            }
        } else {
            // 3.3: server chooses security type
            var type_buf: [4]u8 = undefined;
            try self.readExact(&type_buf);
            const sec_type: protocol.SecurityType = @enumFromInt(std.mem.readInt(u32, &type_buf, .big));

            switch (sec_type) {
                .vnc_authentication => {
                    if (password) |pw| {
                        try self.doVncAuth(pw);
                    } else {
                        return error.AuthRequired;
                    }
                },
                .none => {},
                else => return error.NoSupportedSecurity,
            }
        }

        // ClientInit — shared flag = 1 (don't disconnect other clients)
        try self.writeAll(&[_]u8{1});

        // ServerInit
        var init_buf: [24]u8 = undefined;
        try self.readExact(&init_buf);

        self.width = std.mem.readInt(u16, init_buf[0..2], .big);
        self.height = std.mem.readInt(u16, init_buf[2..4], .big);
        const server_pf = protocol.PixelFormat.decode(init_buf[4..20]);
        log.info("server pixel format: {d}bpp depth={d} be={} tc={} rmax={d} gmax={d} bmax={d} rs={d} gs={d} bs={d}", .{
            server_pf.bits_per_pixel, server_pf.depth,      server_pf.big_endian, server_pf.true_colour,
            server_pf.red_max,        server_pf.green_max,  server_pf.blue_max,   server_pf.red_shift,
            server_pf.green_shift,    server_pf.blue_shift,
        });

        const name_len = std.mem.readInt(u32, init_buf[20..24], .big);
        if (name_len > 0) {
            const read_len = @min(name_len, self.server_name.len);
            try self.readExact(self.server_name[0..read_len]);
            self.server_name_len = read_len;
            // Skip remainder if name is longer than buffer
            if (name_len > self.server_name.len) {
                var skip_buf: [256]u8 = undefined;
                var remaining = name_len - self.server_name.len;
                while (remaining > 0) {
                    const chunk = @min(remaining, skip_buf.len);
                    try self.readExact(skip_buf[0..chunk]);
                    remaining -= chunk;
                }
            }
        }

        log.info("connected: {d}x{d} \"{s}\"", .{ self.width, self.height, self.server_name[0..self.server_name_len] });

        // Explicitly set pixel format — use server's native format but send
        // SetPixelFormat to confirm (TightVNC requires explicit confirmation)
        try self.setPixelFormat(server_pf);

        // Set encodings (Raw only for now)
        try self.setEncodings(&[_]protocol.EncodingType{.raw});

        // Allocate framebuffer
        self.framebuffer = try Framebuffer.init(self.allocator, self.width, self.height, self.pixel_format);
    }

    fn doVncAuth(self: *Client, password: []const u8) ClientError!void {
        var challenge: [16]u8 = undefined;
        try self.readExact(&challenge);

        const response = protocol.VncAuth.encrypt(&challenge, password);
        try self.writeAll(&response);
    }

    fn readErrorMessage(self: *Client) ClientError!void {
        var len_buf: [4]u8 = undefined;
        try self.readExact(&len_buf);
        const len = std.mem.readInt(u32, &len_buf, .big);
        if (len > 0) {
            var msg_buf: [1024]u8 = undefined;
            const read_len = @min(len, msg_buf.len);
            try self.readExact(msg_buf[0..read_len]);
            log.err("server error: {s}", .{msg_buf[0..read_len]});
        }
    }

    fn setPixelFormat(self: *Client, pf: protocol.PixelFormat) ClientError!void {
        var msg: [20]u8 = [_]u8{0} ** 20;
        msg[0] = @intFromEnum(protocol.ClientMessageType.set_pixel_format);
        // 3 bytes padding
        const encoded = pf.encode();
        @memcpy(msg[4..20], &encoded);
        try self.writeAll(&msg);
        self.pixel_format = pf;
    }

    fn setEncodings(self: *Client, enc_types: []const protocol.EncodingType) ClientError!void {
        var msg: [4]u8 = undefined;
        msg[0] = @intFromEnum(protocol.ClientMessageType.set_encodings);
        msg[1] = 0; // padding
        std.mem.writeInt(u16, msg[2..4], @intCast(enc_types.len), .big);
        try self.writeAll(&msg);

        for (enc_types) |et| {
            var enc_buf: [4]u8 = undefined;
            std.mem.writeInt(i32, &enc_buf, @intFromEnum(et), .big);
            try self.writeAll(&enc_buf);
        }
    }

    /// Request a framebuffer update. The request is queued in
    /// pending_updates so a later FramebufferUpdate response can be
    /// classified as delta (incremental) or complete (non-incremental).
    pub fn requestUpdate(self: *Client, incremental: bool) ClientError!void {
        var msg: [10]u8 = undefined;
        msg[0] = @intFromEnum(protocol.ClientMessageType.framebuffer_update_request);
        msg[1] = if (incremental) 1 else 0;
        std.mem.writeInt(u16, msg[2..4], 0, .big);
        std.mem.writeInt(u16, msg[4..6], 0, .big);
        std.mem.writeInt(u16, msg[6..8], self.width, .big);
        std.mem.writeInt(u16, msg[8..10], self.height, .big);
        try self.writeAll(&msg);
        // Bound the queue; if it overflows the mapping has already desynced
        // (server ignoring requests), so restart it — full-coverage detection
        // in receiveUpdate keeps syncFullFrame correct regardless.
        if (self.pending_updates.items.len >= 64) {
            log.warn("pending update queue desynced, resetting", .{});
            self.pending_updates.clearRetainingCapacity();
        }
        self.pending_updates.append(self.allocator, incremental) catch {};
    }

    /// Read and process server messages until a framebuffer update has been
    /// applied. Returns true when the local framebuffer is afterwards a
    /// complete frame — i.e. the update answered a non-incremental request
    /// (per the pending_updates FIFO), or the received rects rewrote every
    /// pixel, in which case prior state no longer matters. False means only
    /// a delta was applied and older regions may predate this call.
    pub fn receiveUpdate(self: *Client) ClientError!bool {
        while (true) {
            var msg_type_buf: [1]u8 = undefined;
            try self.readExact(&msg_type_buf);

            const msg_type: protocol.ServerMessageType = @enumFromInt(msg_type_buf[0]);

            switch (msg_type) {
                .framebuffer_update => {
                    const full_coverage = try self.handleFramebufferUpdate();
                    const mapped_full = if (self.pending_updates.items.len > 0)
                        !self.pending_updates.orderedRemove(0)
                    else
                        false;
                    return full_coverage or mapped_full;
                },
                .set_colour_map_entries => {
                    try self.skipColourMapEntries();
                },
                .bell => {
                    // No data to read
                },
                .server_cut_text => {
                    try self.storeServerCutText();
                },
                else => {
                    log.warn("unknown server message type: {d}", .{msg_type_buf[0]});
                    return error.ProtocolError;
                },
            }
        }
    }

    /// Applies one FramebufferUpdate message. Returns true when the rects
    /// cover the entire framebuffer (every pixel just rewritten).
    fn handleFramebufferUpdate(self: *Client) ClientError!bool {
        var header: [3]u8 = undefined;
        try self.readExact(&header);
        // header[0] = padding
        const num_rects = std.mem.readInt(u16, header[1..3], .big);

        const fb = &(self.framebuffer orelse return error.FramebufferNotReady);
        const full_area: u64 = @as(u64, fb.width) * @as(u64, fb.height);
        var covered: u64 = 0;

        for (0..num_rects) |_| {
            var rect_buf: [12]u8 = undefined;
            try self.readExact(&rect_buf);
            const rect = protocol.RectHeader.decode(&rect_buf);
            covered +|= @as(u64, rect.width) * @as(u64, rect.height);

            switch (rect.encoding) {
                .raw => {
                    try encodings.decodeRaw(self, fb, rect);
                },
                else => {
                    log.warn("unsupported encoding: {}", .{rect.encoding});
                    return error.UnsupportedEncoding;
                },
            }
        }

        return covered >= full_area;
    }

    fn skipColourMapEntries(self: *Client) ClientError!void {
        var header: [5]u8 = undefined;
        try self.readExact(&header);
        const num_colours = std.mem.readInt(u16, header[3..5], .big);
        const skip_bytes = @as(usize, num_colours) * 6;
        try self.skipBytes(skip_bytes);
    }

    fn storeServerCutText(self: *Client) ClientError!void {
        var header: [7]u8 = undefined;
        try self.readExact(&header);
        const length = std.mem.readInt(u32, header[3..7], .big);
        if (length > 0 and length <= 4 * 1024 * 1024) {
            // Free previous clipboard text
            if (self.last_clipboard) |old| self.allocator.free(old);
            const buf = self.allocator.alloc(u8, length) catch {
                try self.skipBytes(length);
                return;
            };
            self.readExact(buf) catch |err| {
                self.allocator.free(buf);
                return err;
            };
            self.last_clipboard = buf;
            log.info("clipboard received: {d} bytes", .{length});
        } else {
            try self.skipBytes(length);
        }
    }

    fn skipBytes(self: *Client, count: anytype) ClientError!void {
        var remaining: usize = @intCast(count);
        var buf: [4096]u8 = undefined;
        while (remaining > 0) {
            const chunk = @min(remaining, buf.len);
            try self.readExact(buf[0..chunk]);
            remaining -= chunk;
        }
    }

    /// Wait for data on the VNC socket using kqueue (event-driven).
    /// Returns true if data is available, false on timeout or error.
    pub fn waitForData(self: *Client, timeout_ms: u32) bool {
        const EV_EOF: u16 = 0x8000;
        const kq = std.posix.kqueue() catch return false;
        defer std.posix.close(kq);

        var changelist = [_]std.posix.Kevent{.{
            .ident = @intCast(self.stream.handle),
            .filter = std.c.EVFILT.READ,
            .flags = std.c.EV.ADD | std.c.EV.ONESHOT,
            .fflags = 0,
            .data = 0,
            .udata = 0,
        }};
        var eventlist: [1]std.posix.Kevent = undefined;

        const sec: isize = @intCast(timeout_ms / 1000);
        const nsec: isize = @intCast((@as(u64, timeout_ms) % 1000) * 1_000_000);
        const timeout = std.posix.timespec{ .sec = sec, .nsec = nsec };

        const n = std.posix.kevent(kq, &changelist, &eventlist, &timeout) catch return false;
        if (n == 0) return false;
        if (eventlist[0].flags & EV_EOF != 0) return false;
        return eventlist[0].data > 0;
    }

    /// Synchronize the framebuffer with the server: request a full
    /// (non-incremental) update and keep consuming updates until one is
    /// received that completely defines the frame. Straggler responses to
    /// requests still in flight from earlier tool calls are consumed first
    /// (FIFO), so they can never be mistaken for a fresh baseline — this is
    /// what kept the first frame of vnc_capture_burst stale (#21).
    pub fn syncFullFrame(self: *Client) ClientError!void {
        try self.requestUpdate(false);
        var rounds: u32 = 0;
        while (rounds < 16) : (rounds += 1) {
            if (try self.receiveUpdate()) return;
        }
        return error.ProtocolError;
    }

    /// syncFullFrame plus a guard against cold-server blank frames: some
    /// VNC servers (TightVNC-class, poll-driven mirror) answer a new or
    /// long-idle client with an all-black frame until their desktop poll
    /// cycle catches up with the newly attached client. A screenshot's long
    /// multi-request exchange rides out the cold period, but a burst's
    /// single quick baseline does not. If the synced framebuffer is all
    /// black, wait and re-sync (3 retries). Returns false when the frame is
    /// still all-black — the caller decides how to surface that.
    pub fn syncSettledFrame(self: *Client) ClientError!bool {
        var retries: u32 = 0;
        while (true) {
            try self.syncFullFrame();
            if (!self.framebufferAllBlack()) return true;
            if (retries >= 3) return false;
            retries += 1;
            std.Thread.sleep(300 * std.time.ns_per_ms);
        }
    }

    /// True when every framebuffer pixel decodes to RGB black.
    pub fn framebufferAllBlack(self: *Client) bool {
        const fb = &(self.framebuffer orelse return false);
        for (0..fb.height) |y| {
            for (0..fb.width) |x| {
                const px = fb.getPixelRgb(@intCast(x), @intCast(y));
                if (px[0] != 0 or px[1] != 0 or px[2] != 0) return false;
            }
        }
        return true;
    }

    /// Best-effort: consume responses to requests still in flight so the
    /// connection returns quiescent — no unread bytes, nothing owed by the
    /// server — when a tool call ends. Bounded by budget_ms.
    pub fn drainInflight(self: *Client, budget_ms: u32) void {
        var timer = std.time.Timer.start() catch return;
        while (self.pending_updates.items.len > 0) {
            const elapsed: u32 = @intCast(timer.read() / std.time.ns_per_ms);
            if (elapsed >= budget_ms) break;
            if (!self.waitForData(budget_ms - elapsed)) break;
            _ = self.receiveUpdate() catch break;
        }
    }

    /// Capture the current framebuffer as a screenshot
    pub fn screenshot(self: *Client) ClientError!*const Framebuffer {
        // Strategy: request incremental updates and wait for actual server
        // data using kqueue. This adapts to TightVNC's polling cycle —
        // returns as soon as the frame stabilizes instead of blind sleeping.
        //
        // 1. Non-incremental request for baseline frame (with cold-server
        //    all-black retry guard)
        // 2. Incremental requests to flush pending screen changes
        // 3. When no more data arrives within timeout, frame is current
        _ = try self.syncSettledFrame();

        // Flush pending changes: request incremental updates until the
        // server has nothing new (kqueue timeout = frame is stable)
        var rounds: u32 = 0;
        while (rounds < 5) : (rounds += 1) {
            try self.requestUpdate(true);
            if (!self.waitForData(300)) break; // No data — frame is stable
            _ = try self.receiveUpdate();
        }

        // Final non-incremental capture for a clean definitive frame
        try self.syncFullFrame();
        self.drainInflight(100);

        return &(self.framebuffer orelse return error.FramebufferNotReady);
    }

    /// Send a key event
    pub fn sendKeyEvent(self: *Client, keysym: u32, down: bool) ClientError!void {
        var msg: [8]u8 = undefined;
        msg[0] = @intFromEnum(protocol.ClientMessageType.key_event);
        msg[1] = if (down) 1 else 0;
        msg[2] = 0; // padding
        msg[3] = 0; // padding
        std.mem.writeInt(u32, msg[4..8], keysym, .big);
        try self.writeAll(&msg);
    }

    /// Send a pointer (mouse) event
    pub fn sendPointerEvent(self: *Client, x: u16, y: u16, button_mask: u8) ClientError!void {
        var msg: [6]u8 = undefined;
        msg[0] = @intFromEnum(protocol.ClientMessageType.pointer_event);
        msg[1] = button_mask;
        std.mem.writeInt(u16, msg[2..4], x, .big);
        std.mem.writeInt(u16, msg[4..6], y, .big);
        try self.writeAll(&msg);
    }

    /// Get the last clipboard text received from the server (ServerCutText)
    pub fn getClipboard(self: *Client) ?[]const u8 {
        return self.last_clipboard;
    }

    /// Send clipboard text to remote (ClientCutText)
    pub fn sendClipboard(self: *Client, text: []const u8) ClientError!void {
        var header: [8]u8 = undefined;
        header[0] = @intFromEnum(protocol.ClientMessageType.client_cut_text);
        header[1] = 0; // padding
        header[2] = 0;
        header[3] = 0;
        std.mem.writeInt(u32, header[4..8], @intCast(text.len), .big);
        try self.writeAll(&header);
        try self.writeAll(text);
    }
};

// --- Tests ---

const TestConn = struct {
    client: Client,
    server: std.net.Stream,
    listener: std.net.Server,

    fn deinit(self: *TestConn) void {
        self.client.disconnect();
        self.server.close();
        self.listener.deinit();
    }
};

/// Loopback TCP pair standing in for a VNC server connection.
fn testConnect(allocator: std.mem.Allocator, w: u16, h: u16) !TestConn {
    const addr = try std.net.Address.parseIp4("127.0.0.1", 0);
    var listener = try addr.listen(.{});
    errdefer listener.deinit();

    const server_side = try std.net.tcpConnectToAddress(listener.listen_address);
    errdefer server_side.close();

    const accepted = try listener.accept();
    errdefer accepted.stream.close();

    var client = Client{
        .stream = accepted.stream,
        .width = w,
        .height = h,
        .allocator = allocator,
    };
    client.framebuffer = try Framebuffer.init(allocator, w, h, protocol.PixelFormat.default_rgb888);
    return .{ .client = client, .server = server_side, .listener = listener };
}

fn writeServer(s: std.net.Stream, bytes: []const u8) !void {
    try s.writeAll(bytes);
}

/// Write a raw framebuffer_update server message: one rect + pixel bytes.
fn writeUpdateMsg(s: std.net.Stream, x: u16, y: u16, w: u16, h: u16, pixels: []const u8) !void {
    var msg: [16]u8 = undefined;
    msg[0] = 0; // framebuffer_update
    msg[1] = 0; // padding
    std.mem.writeInt(u16, msg[2..4], 1, .big); // 1 rect
    std.mem.writeInt(u16, msg[4..6], x, .big);
    std.mem.writeInt(u16, msg[6..8], y, .big);
    std.mem.writeInt(u16, msg[8..10], w, .big);
    std.mem.writeInt(u16, msg[10..12], h, .big);
    std.mem.writeInt(i32, msg[12..16], 0, .big); // raw encoding
    try writeServer(s, &msg);
    try writeServer(s, pixels);
}

test "syncFullFrame skips stale straggler delta (burst first-frame regression)" {
    // Regression for vnc_capture_burst (#21): a previous tool call ended with
    // an incremental request still owed by the server. Its response (a delta)
    // must not be mistaken for the new non-incremental baseline — otherwise
    // frame 0 snapshots a stale, partially-updated framebuffer.
    const allocator = std.testing.allocator;
    var conn = try testConnect(allocator, 4, 2);
    defer conn.deinit();

    // Previous call's leftover: an incremental request, still in flight.
    try conn.client.requestUpdate(true);

    // Server responses, in wire order: the stale delta answering the old
    // request (1 pixel), then the full frame answering the new baseline.
    try writeUpdateMsg(conn.server, 0, 0, 1, 1, &.{ 0xFF, 0x00, 0x00, 0x00 });
    const full_pixels = [_]u8{0xAA} ** 32; // 4x2 px * 4bpp
    try writeUpdateMsg(conn.server, 0, 0, 4, 2, &full_pixels);

    try conn.client.syncFullFrame();

    // Every pixel came from the fresh full frame; the stale 0xFF delta at
    // (0,0) was overwritten, not used as the baseline.
    try std.testing.expectEqualSlices(u8, &full_pixels, conn.client.framebuffer.?.data);
    try std.testing.expectEqual(@as(usize, 0), conn.client.pending_updates.items.len);
}

test "syncSettledFrame retries past a cold all-black first frame" {
    // Cold-server guard: first non-incremental response is a blank frame
    // (server poll cycle hasn't caught the new client yet); the retry must
    // fetch the real frame instead of handing the caller a black image.
    const allocator = std.testing.allocator;
    var conn = try testConnect(allocator, 4, 2);
    defer conn.deinit();

    const black = [_]u8{0} ** 32;
    const real = [_]u8{0x7F} ** 32;
    try writeUpdateMsg(conn.server, 0, 0, 4, 2, &black);
    try writeUpdateMsg(conn.server, 0, 0, 4, 2, &real);

    try std.testing.expect(try conn.client.syncSettledFrame());
    try std.testing.expectEqualSlices(u8, &real, conn.client.framebuffer.?.data);
}

test "syncSettledFrame gives up with false on persistent black" {
    const allocator = std.testing.allocator;
    var conn = try testConnect(allocator, 4, 2);
    defer conn.deinit();

    const black = [_]u8{0} ** 32;
    for (0..4) |_| try writeUpdateMsg(conn.server, 0, 0, 4, 2, &black);

    try std.testing.expect(!try conn.client.syncSettledFrame());
}

test "drainInflight consumes owed responses and idle returns immediately" {
    const allocator = std.testing.allocator;
    var conn = try testConnect(allocator, 4, 2);
    defer conn.deinit();

    // Burst-tail shape: incremental request sent, response arrives late.
    try conn.client.requestUpdate(true);
    try writeUpdateMsg(conn.server, 0, 0, 1, 1, &.{ 0x55, 0x55, 0x55, 0x55 });

    conn.client.drainInflight(1000);
    try std.testing.expectEqual(@as(usize, 0), conn.client.pending_updates.items.len);

    // Nothing owed: bounded by the kqueue timeout, consumes nothing.
    conn.client.drainInflight(50);
    try std.testing.expectEqual(@as(usize, 0), conn.client.pending_updates.items.len);
}
