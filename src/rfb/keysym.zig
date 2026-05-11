const std = @import("std");

/// Map a Unicode codepoint to an X11 keysym
/// For Latin-1 (U+0020..U+00FF), keysym == codepoint
/// For Unicode above U+00FF, keysym = codepoint + 0x01000000 (Unicode keysym range)
pub fn unicodeToKeysym(codepoint: u21) u32 {
    if (codepoint >= 0x0020 and codepoint <= 0x007E) {
        // ASCII printable — keysym matches codepoint
        return @intCast(codepoint);
    }
    if (codepoint >= 0x00A0 and codepoint <= 0x00FF) {
        // Latin-1 supplement — keysym matches codepoint
        return @intCast(codepoint);
    }
    if (codepoint == '\t') return 0xFF09; // Tab
    if (codepoint == '\n') return 0xFF0D; // Return
    if (codepoint == '\r') return 0xFF0D; // Return

    // Unicode keysym range (RFC 6143 Section 7.5.4)
    return 0x01000000 + @as(u32, codepoint);
}

/// Parse a named key string to a keysym
pub fn namedKeysym(name: []const u8) ?u32 {
    const map = .{
        .{ "Return", 0xFF0D },
        .{ "Enter", 0xFF0D },
        .{ "Tab", 0xFF09 },
        .{ "Escape", 0xFF1B },
        .{ "BackSpace", 0xFF08 },
        .{ "Backspace", 0xFF08 },
        .{ "Delete", 0xFFFF },
        .{ "Insert", 0xFF63 },
        .{ "Home", 0xFF50 },
        .{ "End", 0xFF57 },
        .{ "Page_Up", 0xFF55 },
        .{ "PageUp", 0xFF55 },
        .{ "Page_Down", 0xFF56 },
        .{ "PageDown", 0xFF56 },
        .{ "Left", 0xFF51 },
        .{ "Up", 0xFF52 },
        .{ "Right", 0xFF53 },
        .{ "Down", 0xFF54 },
        .{ "F1", 0xFFBE },
        .{ "F2", 0xFFBF },
        .{ "F3", 0xFFC0 },
        .{ "F4", 0xFFC1 },
        .{ "F5", 0xFFC2 },
        .{ "F6", 0xFFC3 },
        .{ "F7", 0xFFC4 },
        .{ "F8", 0xFFC5 },
        .{ "F9", 0xFFC6 },
        .{ "F10", 0xFFC7 },
        .{ "F11", 0xFFC8 },
        .{ "F12", 0xFFC9 },
        .{ "space", 0x0020 },
        .{ "Space", 0x0020 },
        .{ "Print", 0xFF61 },
        .{ "Scroll_Lock", 0xFF14 },
        .{ "Pause", 0xFF13 },
        .{ "Caps_Lock", 0xFFE5 },
        .{ "Num_Lock", 0xFF7F },
        .{ "Menu", 0xFF67 },
        .{ "Super_L", 0xFFEB },
        .{ "Super_R", 0xFFEC },
        .{ "Win", 0xFFEB },
        .{ "Windows", 0xFFEB },
    };

    inline for (map) |entry| {
        if (std.ascii.eqlIgnoreCase(name, entry[0])) {
            return entry[1];
        }
    }

    // Single character — treat as direct keysym
    if (name.len == 1) {
        return unicodeToKeysym(name[0]);
    }

    return null;
}

/// Modifier key name to keysym
pub fn modifierKeysym(name: []const u8) ?u32 {
    if (std.ascii.eqlIgnoreCase(name, "ctrl") or std.ascii.eqlIgnoreCase(name, "control")) {
        return 0xFFE3; // Control_L
    }
    if (std.ascii.eqlIgnoreCase(name, "alt")) {
        return 0xFFE9; // Alt_L
    }
    if (std.ascii.eqlIgnoreCase(name, "shift")) {
        return 0xFFE1; // Shift_L
    }
    if (std.ascii.eqlIgnoreCase(name, "super") or std.ascii.eqlIgnoreCase(name, "win") or std.ascii.eqlIgnoreCase(name, "meta")) {
        return 0xFFEB; // Super_L
    }
    return null;
}
