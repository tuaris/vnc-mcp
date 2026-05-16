/*
 * winmcp-input.c — Native input commands via Win32 SendInput
 *
 * mouse_click, mouse_move, mouse_drag, key_press, type_text
 *
 * Copyright (c) 2026, The Daniel Morante Company, Inc.
 * BSD 2-Clause License
 */

#include "winmcp.h"

/* ================================================================
 * Virtual Key Name → VK Code Lookup
 * ================================================================ */

typedef struct {
    const char *name;
    WORD        vk;
} VkEntry;

static const VkEntry VK_TABLE[] = {
    /* Modifiers */
    {"ctrl",        VK_CONTROL},
    {"control",     VK_CONTROL},
    {"alt",         VK_MENU},
    {"shift",       VK_SHIFT},
    {"win",         VK_LWIN},
    {"lwin",        VK_LWIN},
    {"rwin",        VK_RWIN},

    /* Navigation */
    {"return",      VK_RETURN},
    {"enter",       VK_RETURN},
    {"tab",         VK_TAB},
    {"escape",      VK_ESCAPE},
    {"esc",         VK_ESCAPE},
    {"space",       VK_SPACE},
    {"backspace",   VK_BACK},
    {"delete",      VK_DELETE},
    {"del",         VK_DELETE},
    {"insert",      VK_INSERT},
    {"ins",         VK_INSERT},
    {"home",        VK_HOME},
    {"end",         VK_END},
    {"pageup",      VK_PRIOR},
    {"page_up",     VK_PRIOR},
    {"pagedown",    VK_NEXT},
    {"page_down",   VK_NEXT},

    /* Arrow keys */
    {"up",          VK_UP},
    {"down",        VK_DOWN},
    {"left",        VK_LEFT},
    {"right",       VK_RIGHT},

    /* Function keys */
    {"f1",          VK_F1},
    {"f2",          VK_F2},
    {"f3",          VK_F3},
    {"f4",          VK_F4},
    {"f5",          VK_F5},
    {"f6",          VK_F6},
    {"f7",          VK_F7},
    {"f8",          VK_F8},
    {"f9",          VK_F9},
    {"f10",         VK_F10},
    {"f11",         VK_F11},
    {"f12",         VK_F12},

    /* Misc */
    {"capslock",    VK_CAPITAL},
    {"numlock",     VK_NUMLOCK},
    {"scrolllock",  VK_SCROLL},
    {"printscreen", VK_SNAPSHOT},
    {"print",       VK_SNAPSHOT},
    {"pause",       VK_PAUSE},
    {"apps",        VK_APPS},
    {"menu",        VK_APPS},

    {NULL, 0}
};

/* Case-insensitive match of a key name to a VK code.
 * For single printable characters (a-z, 0-9, etc.), uses VkKeyScanA. */
static WORD name_to_vk(const char *name, int *needs_shift)
{
    *needs_shift = 0;

    /* Single character? */
    if (name[0] && !name[1]) {
        char ch = name[0];
        SHORT vks = VkKeyScanA(ch);
        if (vks != -1) {
            *needs_shift = (HIBYTE(vks) & 1) ? 1 : 0;
            return LOBYTE(vks);
        }
        /* Uppercase letter: shift + lowercase VK */
        if (ch >= 'A' && ch <= 'Z') {
            *needs_shift = 1;
            return (WORD)ch;
        }
        return 0;
    }

    /* Lookup in table (case-insensitive) */
    for (int i = 0; VK_TABLE[i].name; i++) {
        if (_stricmp(name, VK_TABLE[i].name) == 0)
            return VK_TABLE[i].vk;
    }

    return 0;
}

/* ================================================================
 * Command: mouse_click
 *
 * Click at screen coordinates using SendInput.
 * {"command":"mouse_click","x":500,"y":300,"button":"left","double":false}
 * ================================================================ */

void cmd_mouse_click(SOCKET sock, const char *json)
{
    int x = 0, y = 0, dbl = 0;
    char button[16] = "left";

    if (!json_get_int(json, "x", &x) || !json_get_int(json, "y", &y)) {
        send_error(sock, "Missing 'x' and 'y' parameters");
        return;
    }
    json_get_string(json, "button", button, sizeof(button));
    json_get_int(json, "double", &dbl);

    log_msg("mouse_click: x=%d y=%d button=%s double=%d", x, y, button, dbl);

    /* Move cursor to target position */
    SetCursorPos(x, y);
    Sleep(10);

    /* Determine button flags */
    DWORD down_flag, up_flag;
    if (_stricmp(button, "right") == 0) {
        down_flag = MOUSEEVENTF_RIGHTDOWN;
        up_flag   = MOUSEEVENTF_RIGHTUP;
    } else if (_stricmp(button, "middle") == 0) {
        down_flag = MOUSEEVENTF_MIDDLEDOWN;
        up_flag   = MOUSEEVENTF_MIDDLEUP;
    } else {
        down_flag = MOUSEEVENTF_LEFTDOWN;
        up_flag   = MOUSEEVENTF_LEFTUP;
    }

    /* Single click */
    INPUT inputs[2];
    memset(inputs, 0, sizeof(inputs));
    inputs[0].type       = INPUT_MOUSE;
    inputs[0].mi.dwFlags = down_flag;
    inputs[1].type       = INPUT_MOUSE;
    inputs[1].mi.dwFlags = up_flag;
    SendInput(2, inputs, sizeof(INPUT));

    /* Double click */
    if (dbl) {
        Sleep(50);
        SendInput(2, inputs, sizeof(INPUT));
    }

    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"status\":\"ok\",\"data\":{\"x\":%d,\"y\":%d,\"button\":\"%s\",\"double\":%s}}",
             x, y, button, dbl ? "true" : "false");
    send_line(sock, buf);
}

/* ================================================================
 * Command: mouse_move
 *
 * Move cursor to screen coordinates.
 * {"command":"mouse_move","x":500,"y":300}
 * ================================================================ */

void cmd_mouse_move(SOCKET sock, const char *json)
{
    int x = 0, y = 0;

    if (!json_get_int(json, "x", &x) || !json_get_int(json, "y", &y)) {
        send_error(sock, "Missing 'x' and 'y' parameters");
        return;
    }

    log_msg("mouse_move: x=%d y=%d", x, y);

    SetCursorPos(x, y);

    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"status\":\"ok\",\"data\":{\"x\":%d,\"y\":%d}}", x, y);
    send_line(sock, buf);
}

/* ================================================================
 * Command: mouse_drag
 *
 * Click-drag from (x1,y1) to (x2,y2) with left button.
 * {"command":"mouse_drag","x1":100,"y1":200,"x2":500,"y2":300}
 * ================================================================ */

void cmd_mouse_drag(SOCKET sock, const char *json)
{
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;

    if (!json_get_int(json, "x1", &x1) || !json_get_int(json, "y1", &y1) ||
        !json_get_int(json, "x2", &x2) || !json_get_int(json, "y2", &y2)) {
        send_error(sock, "Missing x1/y1/x2/y2 parameters");
        return;
    }

    log_msg("mouse_drag: (%d,%d) -> (%d,%d)", x1, y1, x2, y2);

    /* Move to start position */
    SetCursorPos(x1, y1);
    Sleep(30);

    /* Press left button */
    INPUT down;
    memset(&down, 0, sizeof(down));
    down.type       = INPUT_MOUSE;
    down.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    SendInput(1, &down, sizeof(INPUT));
    Sleep(30);

    /* Interpolate movement in steps for smooth drag */
    int steps = 10;
    for (int i = 1; i <= steps; i++) {
        float t = (float)i / (float)steps;
        int ix = x1 + (int)((float)(x2 - x1) * t);
        int iy = y1 + (int)((float)(y2 - y1) * t);
        SetCursorPos(ix, iy);
        Sleep(15);
    }

    /* Release left button */
    INPUT up;
    memset(&up, 0, sizeof(up));
    up.type       = INPUT_MOUSE;
    up.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(1, &up, sizeof(INPUT));

    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"status\":\"ok\",\"data\":{\"x1\":%d,\"y1\":%d,\"x2\":%d,\"y2\":%d}}",
             x1, y1, x2, y2);
    send_line(sock, buf);
}

/* ================================================================
 * Command: key_press
 *
 * Press a key or combo via SendInput using virtual key codes.
 * {"command":"key_press","keys":"ctrl+c"}
 * {"command":"key_press","keys":"alt+F4"}
 * {"command":"key_press","keys":"Return"}
 * ================================================================ */

void cmd_key_press(SOCKET sock, const char *json)
{
    char keys[256] = {0};

    if (!json_get_string(json, "keys", keys, sizeof(keys))) {
        send_error(sock, "Missing 'keys' parameter");
        return;
    }

    log_msg("key_press: %s", keys);

    /* Parse "ctrl+shift+a" into parts */
    char *parts[8];
    int   part_count = 0;
    char  keys_copy[256];
    strncpy(keys_copy, keys, sizeof(keys_copy) - 1);

    char *tok = strtok(keys_copy, "+");
    while (tok && part_count < 8) {
        parts[part_count++] = tok;
        tok = strtok(NULL, "+");
    }

    if (part_count == 0) {
        send_error(sock, "No key specified");
        return;
    }

    /* Last part = main key, preceding parts = modifiers */
    WORD mod_vks[4];
    int  mod_count = 0;
    int  needs_shift;

    for (int i = 0; i < part_count - 1 && mod_count < 4; i++) {
        WORD vk = name_to_vk(parts[i], &needs_shift);
        if (vk)
            mod_vks[mod_count++] = vk;
    }

    WORD main_vk = name_to_vk(parts[part_count - 1], &needs_shift);

    /* If main key not found, try the whole string as a single named key
     * (handles case where "Return" has no '+' separator) */
    if (!main_vk && part_count == 1) {
        send_error(sock, "Unknown key");
        return;
    }
    if (!main_vk) {
        /* Try last part as single char fallback */
        send_error(sock, "Unknown key in combo");
        return;
    }

    /* If VkKeyScan indicated shift is needed and shift isn't already a modifier, add it */
    if (needs_shift) {
        int has_shift = 0;
        for (int i = 0; i < mod_count; i++) {
            if (mod_vks[i] == VK_SHIFT) { has_shift = 1; break; }
        }
        if (!has_shift && mod_count < 4)
            mod_vks[mod_count++] = VK_SHIFT;
    }

    /* Build INPUT array: mod_count downs + main down + main up + mod_count ups */
    int total = (mod_count + 1) * 2;
    INPUT *inputs = (INPUT *)calloc(total, sizeof(INPUT));
    int idx = 0;

    /* Press modifiers */
    for (int i = 0; i < mod_count; i++) {
        inputs[idx].type     = INPUT_KEYBOARD;
        inputs[idx].ki.wVk   = mod_vks[i];
        idx++;
    }

    /* Press main key */
    inputs[idx].type     = INPUT_KEYBOARD;
    inputs[idx].ki.wVk   = main_vk;
    idx++;

    /* Release main key */
    inputs[idx].type         = INPUT_KEYBOARD;
    inputs[idx].ki.wVk       = main_vk;
    inputs[idx].ki.dwFlags   = KEYEVENTF_KEYUP;
    idx++;

    /* Release modifiers in reverse */
    for (int i = mod_count - 1; i >= 0; i--) {
        inputs[idx].type         = INPUT_KEYBOARD;
        inputs[idx].ki.wVk       = mod_vks[i];
        inputs[idx].ki.dwFlags   = KEYEVENTF_KEYUP;
        idx++;
    }

    SendInput((UINT)total, inputs, sizeof(INPUT));
    free(inputs);

    char buf[256];
    char keys_esc[512];
    json_escape(keys, keys_esc, sizeof(keys_esc));
    snprintf(buf, sizeof(buf),
             "{\"status\":\"ok\",\"data\":{\"keys\":\"%s\"}}", keys_esc);
    send_line(sock, buf);
}

/* ================================================================
 * Command: type_text
 *
 * Type a string using SendInput with KEYEVENTF_UNICODE.
 * Each character is sent as a Unicode scancode — works with any
 * keyboard layout and supports full Unicode (CJK, emoji, etc.).
 * {"command":"type_text","text":"hello world"}
 * ================================================================ */

void cmd_type_text(SOCKET sock, const char *json)
{
    char *text = (char *)malloc(MAX_REQUEST);
    if (!json_get_string(json, "text", text, MAX_REQUEST)) {
        free(text);
        send_error(sock, "Missing 'text' parameter");
        return;
    }

    int text_len = (int)strlen(text);
    log_msg("type_text: %d bytes", text_len);

    /* Convert UTF-8 to UTF-16 for SendInput */
    int wide_len = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    WCHAR *wide = (WCHAR *)malloc(wide_len * sizeof(WCHAR));
    MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, wide_len);
    free(text);

    /* Allocate INPUT array: 2 events (down+up) per character.
     * Surrogate pairs need 4 events (2 per surrogate). */
    int max_events = (wide_len - 1) * 4;  /* -1 for null terminator */
    INPUT *inputs = (INPUT *)calloc(max_events, sizeof(INPUT));
    int idx = 0;

    for (int i = 0; wide[i] && idx < max_events - 1; i++) {
        WCHAR ch = wide[i];

        /* Key down */
        inputs[idx].type          = INPUT_KEYBOARD;
        inputs[idx].ki.wScan      = ch;
        inputs[idx].ki.dwFlags    = KEYEVENTF_UNICODE;
        idx++;

        /* Key up */
        inputs[idx].type          = INPUT_KEYBOARD;
        inputs[idx].ki.wScan      = ch;
        inputs[idx].ki.dwFlags    = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        idx++;
    }

    /* Send in chunks to avoid overwhelming the input queue */
    int chunk = 20;  /* 10 characters (down+up pairs) per chunk */
    for (int i = 0; i < idx; i += chunk) {
        int count = (idx - i < chunk) ? (idx - i) : chunk;
        SendInput((UINT)count, &inputs[i], sizeof(INPUT));
        if (i + chunk < idx)
            Sleep(5);  /* small delay between chunks */
    }

    free(inputs);
    free(wide);

    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"status\":\"ok\",\"data\":{\"length\":%d}}", text_len);
    send_line(sock, buf);
}
