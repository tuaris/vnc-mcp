/*
 * winmcp.c — WinMCP agent for VNC MCP Server
 *
 * Provides local system info (cursor, windows, screen) and command execution
 * over a simple JSON-over-TCP protocol. Runs as a system tray application.
 *
 * Cross-compile from FreeBSD:
 *   zig build helper
 * Or manually:
 *   zig cc -target x86_64-windows-gnu -O2 -mwindows -o winmcp.exe \
 *     helper/winmcp.c helper/winmcp-auth.c helper/winmcp-commands.c \
 *     helper/winmcp-registry.c helper/winmcp-process.c \
 *     helper/winmcp-ocr.c helper/winmcp-uia.c \
 *     -lws2_32 -lshell32 -luser32 -lgdi32 -ladvapi32
 *
 * Usage:
 *   winmcp.exe              Run as tray app (default)
 *   winmcp.exe -console     Run with console output for debugging
 *   winmcp.exe -port 9800   Set listen port (default: 9800)
 *   winmcp.exe install      Add to Windows startup (HKCU Run key)
 *   winmcp.exe uninstall    Remove from Windows startup
 *
 * Protocol: newline-delimited JSON over TCP
 *   Request:  {"command":"cursor_position"}\n
 *   Response: {"status":"ok","data":{...}}\n
 *
 * Copyright (c) 2026, The Daniel Morante Company, Inc.
 * BSD 2-Clause License
 */

#include "winmcp.h"
#include <stdarg.h>

/* Forward declarations */
static void overlay_show(void);
static void overlay_hide(void);

/* ================================================================
 * Globals
 * ================================================================ */

int  g_port    = DEFAULT_PORT;
int  g_console = 0;
int  g_running = 1;
HWND g_hwnd    = NULL;
NOTIFYICONDATAA g_nid;
HICON g_icon_normal    = NULL;
HICON g_icon_connected = NULL;
volatile LONG g_client_count = 0;  /* active client connections */
char g_client_ip[64]     = {0};  /* last connected client IP */
DWORD g_connect_time     = 0;    /* GetTickCount at connect */
CRITICAL_SECTION g_cs;           /* protects g_client_ip/g_connect_time */
char g_password[9]       = {0};  /* VNC password (max 8 chars) */
int  g_auth_enabled      = 0;
const char *g_password_file = NULL;
volatile DWORD g_overlay_linger_until = 0; /* GetTickCount deadline for auto-hide */

WmcpNative g_native = {0};  /* native DLL function pointers */


/* ================================================================
 * Native DLL Loader
 * ================================================================ */

#define NATIVE_DLL_NAME "winmcp-native.dll"

int native_dll_load(void)
{
    /* Try loading from same directory as the executable */
    char dll_path[MAX_PATH];
    GetModuleFileNameA(NULL, dll_path, MAX_PATH);

    /* Replace executable name with DLL name */
    char *slash = strrchr(dll_path, '\\');
    if (slash)
        strcpy(slash + 1, NATIVE_DLL_NAME);
    else
        strcpy(dll_path, NATIVE_DLL_NAME);

    g_native.handle = LoadLibraryA(dll_path);
    if (!g_native.handle) {
        /* Try bare name (searches PATH, system dirs) */
        g_native.handle = LoadLibraryA(NATIVE_DLL_NAME);
    }

    if (!g_native.handle) {
        log_msg("Native DLL not found (optional): %s", dll_path);
        return 0;
    }

    g_native.version    = (wmcp_version_fn)GetProcAddress(g_native.handle, "wmcp_version");
    g_native.screenshot = (wmcp_screenshot_fn)GetProcAddress(g_native.handle, "wmcp_screenshot");
    g_native.ocr_region = (wmcp_ocr_region_fn)GetProcAddress(g_native.handle, "wmcp_ocr_region");

    /* Log what we found */
    char ver_buf[256] = {0};
    if (g_native.version && g_native.version(ver_buf, sizeof(ver_buf)) == 0) {
        log_msg("Native DLL loaded: %s", ver_buf);
    } else {
        log_msg("Native DLL loaded (version query failed)");
    }

    return 1;
}

void native_dll_unload(void)
{
    if (g_native.handle) {
        FreeLibrary(g_native.handle);
        memset(&g_native, 0, sizeof(g_native));
    }
}


/* ================================================================
 * JSON Helpers
 * ================================================================ */

/* Escape a C string for safe embedding inside a JSON string value. */
int json_escape(const char *in, char *out, int out_max)
{
    int j = 0;
    for (int i = 0; in[i] && j < out_max - 7; i++) {
        unsigned char c = (unsigned char)in[i];
        if      (c == '"')  { out[j++] = '\\'; out[j++] = '"';  }
        else if (c == '\\') { out[j++] = '\\'; out[j++] = '\\'; }
        else if (c == '\n') { out[j++] = '\\'; out[j++] = 'n';  }
        else if (c == '\r') { out[j++] = '\\'; out[j++] = 'r';  }
        else if (c == '\t') { out[j++] = '\\'; out[j++] = 't';  }
        else if (c < 0x20)  { j += snprintf(out + j, out_max - j, "\\u%04x", c); }
        else                { out[j++] = (char)c; }
    }
    out[j] = '\0';
    return j;
}

/* Extract a string value for a given key from flat JSON.
 * Handles basic escape sequences in the value. */
int json_get_string(const char *json, const char *key,
                           char *out, int out_max)
{
    char pat[256];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return 0;
    p += strlen(pat);

    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    if (*p != '"') return 0;
    p++;

    int i = 0;
    while (*p && i < out_max - 1) {
        if (*p == '\\' && *(p + 1)) {
            p++;
            switch (*p) {
            case '"':  out[i++] = '"';  break;
            case '\\': out[i++] = '\\'; break;
            case 'n':  out[i++] = '\n'; break;
            case 'r':  out[i++] = '\r'; break;
            case 't':  out[i++] = '\t'; break;
            case '/':  out[i++] = '/';  break;
            default:   out[i++] = *p;   break;
            }
        } else if (*p == '"') {
            break;
        } else {
            out[i++] = *p;
        }
        p++;
    }
    out[i] = '\0';
    return 1;
}

/* Extract an integer value for a given key from flat JSON. */
int json_get_int(const char *json, const char *key, int *out)
{
    char pat[256];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    if (*p == '"') return 0;   /* it's a string, not a number */
    *out = atoi(p);
    return 1;
}

/* ================================================================
 * Logging
 * ================================================================ */

void log_msg(const char *fmt, ...)
{
    if (!g_console) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[winmcp] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    fflush(stderr);
    va_end(ap);
}

/* ================================================================
 * Network Helpers
 * ================================================================ */

void send_line(SOCKET sock, const char *json)
{
    send(sock, json, (int)strlen(json), 0);
    send(sock, "\n", 1, 0);
}

void send_error(SOCKET sock, const char *msg)
{
    char buf[1024];
    char escaped[512];
    json_escape(msg, escaped, sizeof(escaped));
    snprintf(buf, sizeof(buf),
             "{\"status\":\"error\",\"message\":\"%s\"}", escaped);
    send_line(sock, buf);
}

/* ================================================================
 * Client Handler
 * ================================================================ */

/* Read one newline-delimited request from the socket.
 * Returns bytes read (>0) on success, 0 on clean disconnect, -1 on error/timeout. */
static int read_request(SOCKET sock, char *buf, int buf_size)
{
    int total = 0;
    memset(buf, 0, buf_size);

    while (total < buf_size - 1) {
        int n = recv(sock, buf + total, buf_size - total - 1, 0);
        if (n <= 0) return (total > 0 && strchr(buf, '\n')) ? total : (n == 0 ? 0 : -1);
        total += n;
        buf[total] = '\0';
        if (strchr(buf, '\n')) break;
    }

    /* Strip trailing whitespace / newline */
    while (total > 0 && (buf[total - 1] == '\n' ||
                         buf[total - 1] == '\r' ||
                         buf[total - 1] == ' ')) {
        buf[--total] = '\0';
    }

    return total;
}

/* Persistent client handler: reads multiple requests per connection.
 * Each request/response is newline-delimited JSON.
 * An optional "id" field in the request is echoed in the response. */
static void handle_client(SOCKET sock)
{
    char request[MAX_REQUEST];

    while (g_running) {
        int n = read_request(sock, request, sizeof(request));
        if (n <= 0) break;  /* disconnect or error */

        log_msg("request: %s", request);

        char command[64] = {0};
        if (!json_get_string(request, "command", command, sizeof(command))) {
            send_error(sock, "Missing 'command' field");
            continue;
        }

        if      (strcmp(command, "cursor_position") == 0) cmd_cursor_position(sock);
        else if (strcmp(command, "window_list")     == 0) cmd_window_list(sock);
        else if (strcmp(command, "active_window")   == 0) cmd_active_window(sock);
        else if (strcmp(command, "set_active_window") == 0) cmd_set_active_window(sock, request);
        else if (strcmp(command, "manage_window")   == 0) cmd_manage_window(sock, request);
        else if (strcmp(command, "clipboard_get")   == 0) cmd_clipboard_get(sock);
        else if (strcmp(command, "clipboard_set")   == 0) cmd_clipboard_set(sock, request);
        else if (strcmp(command, "run_command")     == 0) cmd_run_command(sock, request);
        else if (strcmp(command, "screen_info")     == 0) cmd_screen_info(sock);
        else if (strcmp(command, "file_upload")     == 0) cmd_file_upload(sock, request);
        else if (strcmp(command, "file_download")   == 0) cmd_file_download(sock, request);
        else if (strcmp(command, "ocr_region")      == 0) cmd_ocr_region(sock, request);
        else if (strcmp(command, "ui_tree")         == 0) cmd_ui_tree(sock, request);
        else if (strcmp(command, "ui_element_text") == 0) cmd_ui_element_text(sock, request);
        else if (strcmp(command, "ui_click_element")== 0) cmd_ui_click_element(sock, request);
        else if (strcmp(command, "click_marker")   == 0) cmd_click_marker(sock, request);
        else if (strcmp(command, "registry_read") == 0) cmd_registry_read(sock, request);
        else if (strcmp(command, "registry_write")== 0) cmd_registry_write(sock, request);
        else if (strcmp(command, "registry_list") == 0) cmd_registry_list(sock, request);
        else if (strcmp(command, "process_list")  == 0) cmd_process_list(sock);
        else if (strcmp(command, "process_kill")  == 0) cmd_process_kill(sock, request);
        else if (strcmp(command, "service_list")  == 0) cmd_service_list(sock);
        else if (strcmp(command, "service_control")== 0) cmd_service_control(sock, request);
        else if (strcmp(command, "screenshot")    == 0) cmd_screenshot(sock, request);
        else if (strcmp(command, "mouse_click") == 0) cmd_mouse_click(sock, request);
        else if (strcmp(command, "mouse_move")  == 0) cmd_mouse_move(sock, request);
        else if (strcmp(command, "mouse_drag")  == 0) cmd_mouse_drag(sock, request);
        else if (strcmp(command, "key_press")   == 0) cmd_key_press(sock, request);
        else if (strcmp(command, "type_text")   == 0) cmd_type_text(sock, request);
        else {
            char msg[128];
            snprintf(msg, sizeof(msg), "Unknown command: %s", command);
            send_error(sock, msg);
        }
    }
}

/* ================================================================
 * Client Worker Thread (one per connection)
 * ================================================================ */

typedef struct {
    SOCKET sock;
    char   ip[64];
    int    port;
} ClientCtx;

static DWORD WINAPI client_thread(LPVOID param)
{
    ClientCtx *ctx = (ClientCtx *)param;
    LONG prev;

    /* Track connection — cancel any pending linger hide */
    prev = InterlockedIncrement(&g_client_count);
    g_overlay_linger_until = 0;
    EnterCriticalSection(&g_cs);
    strncpy(g_client_ip, ctx->ip, sizeof(g_client_ip) - 1);
    g_connect_time = GetTickCount();
    LeaveCriticalSection(&g_cs);

    /* Switch tray icon to connected state + show overlay */
    if (g_icon_connected && g_hwnd) {
        g_nid.hIcon = g_icon_connected;
        snprintf(g_nid.szTip, sizeof(g_nid.szTip),
                 "WinMCP (CONNECTED: %s)", ctx->ip);
        Shell_NotifyIconA(NIM_MODIFY, &g_nid);
    }
    overlay_show();

    log_msg("client connected from %s:%d", ctx->ip, ctx->port);

    if (auth_client(ctx->sock))
        handle_client(ctx->sock);
    closesocket(ctx->sock);

    log_msg("client disconnected: %s:%d", ctx->ip, ctx->port);

    /* Decrement — start linger countdown if no more clients */
    prev = InterlockedDecrement(&g_client_count);
    if (prev == 0) {
        /* Don't hide immediately — set linger deadline.
         * The overlay timer (1s) will auto-hide after OVERLAY_LINGER_MS. */
        g_overlay_linger_until = GetTickCount() + OVERLAY_LINGER_MS;
    }

    free(ctx);
    return 0;
}

/* ================================================================
 * TCP Server Thread (accept loop)
 * ================================================================ */

static DWORD WINAPI server_thread(LPVOID param)
{
    (void)param;

    SOCKET listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock == INVALID_SOCKET) {
        log_msg("socket() failed: %d", WSAGetLastError());
        return 1;
    }

    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR,
               (const char *)&opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons((u_short)g_port);

    if (bind(listen_sock, (struct sockaddr *)&addr, sizeof(addr))
            == SOCKET_ERROR) {
        log_msg("bind() failed on port %d: %d", g_port, WSAGetLastError());
        closesocket(listen_sock);
        return 1;
    }

    if (listen(listen_sock, 5) == SOCKET_ERROR) {
        log_msg("listen() failed: %d", WSAGetLastError());
        closesocket(listen_sock);
        return 1;
    }

    log_msg("listening on port %d", g_port);

    while (g_running) {
        struct sockaddr_in client_addr;
        int client_len = sizeof(client_addr);
        SOCKET client_sock = accept(listen_sock,
                                    (struct sockaddr *)&client_addr,
                                    &client_len);

        if (client_sock == INVALID_SOCKET) {
            if (g_running)
                log_msg("accept() failed: %d", WSAGetLastError());
            continue;
        }

        /* Enable TCP keepalive to detect dead persistent connections */
        int keepalive = 1;
        setsockopt(client_sock, SOL_SOCKET, SO_KEEPALIVE,
                   (const char *)&keepalive, sizeof(keepalive));

        /* Spawn worker thread for this client */
        ClientCtx *ctx = (ClientCtx *)malloc(sizeof(ClientCtx));
        if (!ctx) {
            log_msg("malloc failed for client context");
            closesocket(client_sock);
            continue;
        }
        ctx->sock = client_sock;
        strncpy(ctx->ip, inet_ntoa(client_addr.sin_addr), sizeof(ctx->ip) - 1);
        ctx->ip[sizeof(ctx->ip) - 1] = '\0';
        ctx->port = ntohs(client_addr.sin_port);

        HANDLE ht = CreateThread(NULL, 0, client_thread, ctx, 0, NULL);
        if (ht) {
            CloseHandle(ht);  /* detach — thread runs independently */
        } else {
            log_msg("CreateThread failed: %lu", (unsigned long)GetLastError());
            closesocket(client_sock);
            free(ctx);
        }
    }

    closesocket(listen_sock);
    return 0;
}

/* ================================================================
 * System Tray
 * ================================================================ */

#define TRAY_WND_CLASS "WinMcpTray"

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg,
                                 WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_TRAYICON:
        if (lp == WM_RBUTTONUP || lp == WM_LBUTTONUP) {
            POINT pt;
            GetCursorPos(&pt);
            HMENU menu = CreatePopupMenu();
            AppendMenuA(menu, MF_STRING | MF_GRAYED, IDM_ABOUT,
                        "WinMCP v" WINMCP_VERSION);
            AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
            AppendMenuA(menu, MF_STRING, IDM_EXIT, "Exit");
            SetForegroundWindow(hwnd);
            TrackPopupMenu(menu, TPM_RIGHTALIGN | TPM_BOTTOMALIGN,
                           pt.x, pt.y, 0, hwnd, NULL);
            DestroyMenu(menu);
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_EXIT:
            g_running = 0;
            Shell_NotifyIconA(NIM_DELETE, &g_nid);
            PostQuitMessage(0);
            break;
        case IDM_ABOUT: {
            char about[512];
            snprintf(about, sizeof(about),
                     "WinMCP Agent v" WINMCP_VERSION "\n"
                     "Part of vnc-mcp-server\n\n"
                     "Listening on port %d\n\n"
                     "Copyright (c) 2026,\n"
                     "The Daniel Morante Company, Inc.",
                     g_port);
            MessageBoxA(NULL, about, "WinMCP",
                        MB_OK | MB_ICONINFORMATION);
            break;
        }
        }
        return 0;

    case WM_CREATE_MARKER: {
        MarkerRequest *req = (MarkerRequest *)lp;
        if (req) {
            create_click_marker(req->x, req->y, req->duration);
            free(req);
        }
        return 0;
    }

    case WM_DESTROY:
        g_running = 0;
        Shell_NotifyIconA(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void setup_tray(HINSTANCE hInstance)
{
    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance      = hInstance;
    wc.lpszClassName  = TRAY_WND_CLASS;
    RegisterClassA(&wc);

    g_hwnd = CreateWindowA(TRAY_WND_CLASS, "WinMCP", 0,
                           0, 0, 0, 0,
                           HWND_MESSAGE, NULL, hInstance, NULL);

    memset(&g_nid, 0, sizeof(g_nid));
    g_nid.cbSize           = sizeof(g_nid);
    g_nid.hWnd             = g_hwnd;
    g_nid.uID              = 1;
    g_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    /* Load custom icons from embedded resources */
    g_icon_normal = LoadIconA(hInstance, MAKEINTRESOURCEA(IDI_TRAY_NORMAL));
    g_icon_connected = LoadIconA(hInstance, MAKEINTRESOURCEA(IDI_TRAY_CONNECTED));
    if (!g_icon_normal)
        g_icon_normal = LoadIconA(NULL, IDI_APPLICATION);
    if (!g_icon_connected)
        g_icon_connected = g_icon_normal;

    g_nid.hIcon            = g_icon_normal;
    snprintf(g_nid.szTip, sizeof(g_nid.szTip),
             "WinMCP (port %d)", g_port);

    Shell_NotifyIconA(NIM_ADD, &g_nid);
}

/* ================================================================
 * On-Screen Connection Indicator Overlay
 *
 * A small translucent pill at top-right of screen showing:
 *   "● 192.168.1.233 (0:05)"
 * Appears when a client connects, hides when all disconnect.
 * WS_EX_TOPMOST + WS_EX_TOOLWINDOW + WS_EX_LAYERED (click-through)
 * ================================================================ */

#define OVERLAY_WND_CLASS "WinMcpOverlay"
#define OVERLAY_TIMER_ID  42
#define OVERLAY_WIDTH     260
#define OVERLAY_HEIGHT    30
#define OVERLAY_MARGIN    8
#define OVERLAY_BG_ALPHA  210  /* 0-255 background translucency */

/* WDA_EXCLUDEFROMCAPTURE: hide overlay from screen capture (Win10 2004+) */
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

static HWND  g_overlay_hwnd = NULL;

/* Paint the overlay into a 32-bit ARGB DIB and call UpdateLayeredWindow.
 * This gives per-pixel alpha: the background is semi-transparent while
 * the text is rendered at full opacity — no washed-out appearance. */
static void overlay_repaint(HWND hwnd)
{
    if (!hwnd) return;

    int w = OVERLAY_WIDTH, h = OVERLAY_HEIGHT;

    /* Create 32-bit ARGB DIB section */
    BITMAPINFO bmi;
    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = w;
    bmi.bmiHeader.biHeight      = -h;  /* top-down */
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    BYTE *bits = NULL;
    HDC screen_dc = GetDC(NULL);
    HDC mem_dc = CreateCompatibleDC(screen_dc);
    HBITMAP dib = CreateDIBSection(mem_dc, &bmi, DIB_RGB_COLORS,
                                   (void **)&bits, NULL, 0);
    HBITMAP old_bmp = (HBITMAP)SelectObject(mem_dc, dib);

    /* Clear to transparent */
    memset(bits, 0, (size_t)(w * h * 4));

    /* Fill rounded rect background with per-pixel alpha */
    {
        int radius = 10;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                /* Check if pixel is inside the rounded rect */
                int inside = 1;
                /* Top-left corner */
                if (x < radius && y < radius) {
                    int dx = radius - x - 1, dy = radius - y - 1;
                    if (dx * dx + dy * dy > radius * radius) inside = 0;
                }
                /* Top-right corner */
                else if (x >= w - radius && y < radius) {
                    int dx = x - (w - radius), dy = radius - y - 1;
                    if (dx * dx + dy * dy > radius * radius) inside = 0;
                }
                /* Bottom-left corner */
                else if (x < radius && y >= h - radius) {
                    int dx = radius - x - 1, dy = y - (h - radius);
                    if (dx * dx + dy * dy > radius * radius) inside = 0;
                }
                /* Bottom-right corner */
                else if (x >= w - radius && y >= h - radius) {
                    int dx = x - (w - radius), dy = y - (h - radius);
                    if (dx * dx + dy * dy > radius * radius) inside = 0;
                }

                if (inside) {
                    BYTE *p = bits + (y * w + x) * 4;
                    /* Pre-multiplied alpha BGRA */
                    BYTE a = OVERLAY_BG_ALPHA;
                    p[0] = (BYTE)(30 * a / 255);   /* B */
                    p[1] = (BYTE)(30 * a / 255);   /* G */
                    p[2] = (BYTE)(30 * a / 255);   /* R */
                    p[3] = a;                       /* A */
                }
            }
        }
    }

    /* Build text: show IP and duration while connected or lingering */
    char text[128] = {0};
    EnterCriticalSection(&g_cs);
    if (g_client_ip[0] && g_connect_time) {
        DWORD elapsed = (GetTickCount() - g_connect_time) / 1000;
        int mins = (int)(elapsed / 60);
        int secs = (int)(elapsed % 60);
        if (g_client_count > 0) {
            snprintf(text, sizeof(text), "Connected: %s (%d:%02d)",
                     g_client_ip, mins, secs);
        } else {
            snprintf(text, sizeof(text), "Last: %s (%d:%02d)",
                     g_client_ip, mins, secs);
        }
    }
    LeaveCriticalSection(&g_cs);

    if (text[0]) {
        /* Two-pass text rendering: GDI DrawText doesn't write alpha,
         * so draw text on a separate all-zero surface to isolate text
         * pixels, then composite onto the main DIB at full opacity. */
        BYTE *text_bits = NULL;
        HDC text_dc = CreateCompatibleDC(screen_dc);
        HBITMAP text_dib = CreateDIBSection(text_dc, &bmi, DIB_RGB_COLORS,
                                            (void **)&text_bits, NULL, 0);
        HBITMAP text_old_bmp = (HBITMAP)SelectObject(text_dc, text_dib);
        memset(text_bits, 0, (size_t)(w * h * 4));

        SetBkMode(text_dc, TRANSPARENT);
        SetTextColor(text_dc, RGB(180, 255, 180));

        HFONT font = CreateFontA(16, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                 DEFAULT_PITCH | FF_SWISS, "Segoe UI");
        HFONT old_font = (HFONT)SelectObject(text_dc, font);

        RECT text_rc = {12, 0, w - 8, h};
        DrawTextA(text_dc, text, -1, &text_rc,
                  DT_SINGLELINE | DT_VCENTER | DT_LEFT);

        SelectObject(text_dc, old_font);
        DeleteObject(font);

        /* Composite: any text_bits pixel with non-zero RGB is text.
         * Write it into the main DIB as fully opaque. */
        for (int i = 0; i < w * h; i++) {
            BYTE *tp = text_bits + i * 4;
            if (tp[0] | tp[1] | tp[2]) {
                BYTE *dp = bits + i * 4;
                dp[0] = tp[0];  /* B */
                dp[1] = tp[1];  /* G */
                dp[2] = tp[2];  /* R */
                dp[3] = 255;    /* fully opaque */
            }
        }

        SelectObject(text_dc, text_old_bmp);
        DeleteObject(text_dib);
        DeleteDC(text_dc);
    }

    /* UpdateLayeredWindow with per-pixel alpha */
    POINT pt_src = {0, 0};
    SIZE sz = {w, h};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};

    POINT pt_pos;
    RECT win_rc;
    GetWindowRect(hwnd, &win_rc);
    pt_pos.x = win_rc.left;
    pt_pos.y = win_rc.top;

    UpdateLayeredWindow(hwnd, screen_dc, &pt_pos, &sz,
                        mem_dc, &pt_src, 0, &blend, ULW_ALPHA);

    SelectObject(mem_dc, old_bmp);
    DeleteObject(dib);
    DeleteDC(mem_dc);
    ReleaseDC(NULL, screen_dc);
}

static LRESULT CALLBACK overlay_wnd_proc(HWND hwnd, UINT msg,
                                          WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_TIMER:
        if (wp == OVERLAY_TIMER_ID) {
            overlay_repaint(hwnd);
            /* Auto-hide after linger period with no active clients */
            if (g_client_count == 0 && g_overlay_linger_until != 0 &&
                GetTickCount() >= g_overlay_linger_until) {
                g_overlay_linger_until = 0;
                ShowWindow(hwnd, SW_HIDE);
                /* Restore tray icon to idle */
                if (g_icon_normal && g_hwnd) {
                    g_nid.hIcon = g_icon_normal;
                    snprintf(g_nid.szTip, sizeof(g_nid.szTip),
                             "WinMCP (port %d)", g_port);
                    Shell_NotifyIconA(NIM_MODIFY, &g_nid);
                }
            }
        }
        return 0;

    case WM_NCHITTEST:
        /* Allow dragging the overlay by its client area */
        return HTCAPTION;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void overlay_create(HINSTANCE hInstance)
{
    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = overlay_wnd_proc;
    wc.hInstance      = hInstance;
    wc.lpszClassName  = OVERLAY_WND_CLASS;
    wc.hbrBackground  = (HBRUSH)GetStockObject(NULL_BRUSH);
    RegisterClassA(&wc);

    /* Position at top-right of primary monitor */
    int screen_w = GetSystemMetrics(SM_CXSCREEN);
    int x = screen_w - OVERLAY_WIDTH - OVERLAY_MARGIN;
    int y = OVERLAY_MARGIN;

    g_overlay_hwnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
        OVERLAY_WND_CLASS, "VNC Connection",
        WS_POPUP,
        x, y, OVERLAY_WIDTH, OVERLAY_HEIGHT,
        NULL, NULL, hInstance, NULL);

    /* Hide overlay from screen capture / VNC (Win10 2004+) */
    SetWindowDisplayAffinity(g_overlay_hwnd, WDA_EXCLUDEFROMCAPTURE);

    /* 1-second timer for duration updates + per-pixel alpha repaint */
    SetTimer(g_overlay_hwnd, OVERLAY_TIMER_ID, 1000, NULL);
}

static void overlay_show(void)
{
    if (g_overlay_hwnd) {
        ShowWindow(g_overlay_hwnd, SW_SHOWNOACTIVATE);
        overlay_repaint(g_overlay_hwnd);  /* immediate content */
    }
}

static void overlay_hide(void)
{
    if (g_overlay_hwnd)
        ShowWindow(g_overlay_hwnd, SW_HIDE);
}

/* ================================================================
 * Registry Install / Uninstall
 * ================================================================ */

#define REG_RUN_KEY    "Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define REG_VALUE_NAME "WinMCP"

static void do_install(void)
{
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);

    char value[MAX_PATH + 32];
    snprintf(value, sizeof(value), "\"%s\" -port %d", path, g_port);

    HKEY hkey;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, REG_RUN_KEY, 0,
                      KEY_SET_VALUE, &hkey) == ERROR_SUCCESS) {
        RegSetValueExA(hkey, REG_VALUE_NAME, 0, REG_SZ,
                       (const BYTE *)value, (DWORD)strlen(value) + 1);
        RegCloseKey(hkey);
        printf("Installed to startup: %s\n", value);
    } else {
        fprintf(stderr, "Failed to open registry key\n");
    }
}

static void do_uninstall(void)
{
    HKEY hkey;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, REG_RUN_KEY, 0,
                      KEY_SET_VALUE, &hkey) == ERROR_SUCCESS) {
        RegDeleteValueA(hkey, REG_VALUE_NAME);
        RegCloseKey(hkey);
        printf("Removed from startup\n");
    } else {
        fprintf(stderr, "Failed to open registry key\n");
    }
}

/* ================================================================
 * Entry Point — WinMain (GUI subsystem, no console by default)
 * ================================================================ */

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow)
{
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    /* Parse command line using Win32 API */
    int argc = 0;
    LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &argc);

    for (int i = 1; i < argc; i++) {
        if (wcscmp(wargv[i], L"-console") == 0) {
            g_console = 1;
        } else if (wcscmp(wargv[i], L"-port") == 0 && i + 1 < argc) {
            g_port = _wtoi(wargv[++i]);
        } else if (wcscmp(wargv[i], L"-password-file") == 0 && i + 1 < argc) {
            /* Convert wide string to narrow for password file path */
            static char pw_path[MAX_PATH];
            WideCharToMultiByte(CP_UTF8, 0, wargv[++i], -1,
                                pw_path, sizeof(pw_path), NULL, NULL);
            g_password_file = pw_path;
        } else if (wcscmp(wargv[i], L"install") == 0) {
            AllocConsole();
            freopen("CONOUT$", "w", stdout);
            do_install();
            LocalFree(wargv);
            return 0;
        } else if (wcscmp(wargv[i], L"uninstall") == 0) {
            AllocConsole();
            freopen("CONOUT$", "w", stdout);
            do_uninstall();
            LocalFree(wargv);
            return 0;
        } else if (wcscmp(wargv[i], L"-h") == 0 ||
                   wcscmp(wargv[i], L"--help") == 0) {
            AllocConsole();
            freopen("CONOUT$", "w", stdout);
            printf("WinMCP Agent v" WINMCP_VERSION "\n\n"
                   "Usage: winmcp.exe [options]\n"
                   "  -console              Show console output\n"
                   "  -port PORT            Listen port (default: %d)\n"
                   "  -password-file PATH   Read VNC password from file\n"
                   "  install               Add to Windows startup\n"
                   "  uninstall             Remove from Windows startup\n\n"
                   "Authentication: reads VNC password from the Windows\n"
                   "registry (TightVNC, RealVNC, TigerVNC, UltraVNC) or\n"
                   "from -password-file. Uses VNC DES challenge-response.\n",
                   DEFAULT_PORT);
            LocalFree(wargv);
            return 0;
        }
    }
    LocalFree(wargv);

    /* Console mode: allocate a console for debugging output */
    if (g_console) {
        AllocConsole();
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    }

    /* Single-instance enforcement via named mutex */
    HANDLE hMutex = CreateMutexA(NULL, FALSE, "WinMcpSingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (g_console) {
            fprintf(stderr, "WinMCP is already running. Exiting.\n");
        } else {
            MessageBoxA(NULL, "WinMCP is already running.",
                        "WinMCP", MB_OK | MB_ICONINFORMATION);
        }
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }

    /* Initialize critical section for client tracking */
    InitializeCriticalSection(&g_cs);

    /* Initialize Winsock */
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        if (g_console) fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }

    log_msg("WinMCP v" WINMCP_VERSION " starting on port %d", g_port);

    /* Initialize authentication (password file or registry) */
    init_auth();

    /* Load native DLL for DXGI/OCR capabilities (optional) */
    native_dll_load();

    /* Start TCP server in background thread */
    CreateThread(NULL, 0, server_thread, NULL, 0, NULL);

    /* Setup system tray icon and connection overlay */
    setup_tray(hInstance);
    overlay_create(hInstance);

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    native_dll_unload();
    DeleteCriticalSection(&g_cs);
    WSACleanup();
    return 0;
}
