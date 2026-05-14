/*
 * winmcp-commands.c — Basic helper commands
 *
 * cursor_position, window_list, active_window, set_active_window,
 * manage_window, clipboard_get, clipboard_set, run_command,
 * screen_info, file_upload, file_download, click_marker
 *
 * Copyright (c) 2026, The Daniel Morante Company, Inc.
 * BSD 2-Clause License
 */

#include "winmcp.h"

/* ================================================================
 * Command: cursor_position
 * ================================================================ */

void cmd_cursor_position(SOCKET sock)
{
    POINT pt;
    GetCursorPos(&pt);

    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"status\":\"ok\",\"data\":{\"x\":%d,\"y\":%d}}",
             (int)pt.x, (int)pt.y);
    send_line(sock, buf);
}

/* ================================================================
 * Command: window_list
 * ================================================================ */

typedef struct {
    char *buf;
    int   capacity;
    int   offset;
    int   count;
    HWND  fg_hwnd;
} WinEnumCtx;

static BOOL CALLBACK enum_windows_cb(HWND hwnd, LPARAM lparam)
{
    WinEnumCtx *ctx = (WinEnumCtx *)lparam;

    if (!IsWindowVisible(hwnd)) return TRUE;

    char title[512] = {0};
    GetWindowTextA(hwnd, title, sizeof(title));
    if (title[0] == '\0') return TRUE;

    char classname[256] = {0};
    GetClassNameA(hwnd, classname, sizeof(classname));

    RECT rect;
    GetWindowRect(hwnd, &rect);

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);

    char title_esc[1024], class_esc[512];
    json_escape(title, title_esc, sizeof(title_esc));
    json_escape(classname, class_esc, sizeof(class_esc));

    if (ctx->count > 0 && ctx->offset < ctx->capacity - 1)
        ctx->buf[ctx->offset++] = ',';

    ctx->offset += snprintf(
        ctx->buf + ctx->offset, ctx->capacity - ctx->offset,
        "{\"title\":\"%s\",\"class\":\"%s\","
        "\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,"
        "\"pid\":%lu,\"foreground\":%s}",
        title_esc, class_esc,
        (int)rect.left, (int)rect.top,
        (int)(rect.right - rect.left), (int)(rect.bottom - rect.top),
        (unsigned long)pid,
        (hwnd == ctx->fg_hwnd) ? "true" : "false");

    ctx->count++;
    return TRUE;
}

void cmd_window_list(SOCKET sock)
{
    char *buf = (char *)malloc(MAX_RESPONSE);
    if (!buf) { send_error(sock, "Out of memory"); return; }

    int offset = snprintf(buf, MAX_RESPONSE,
                          "{\"status\":\"ok\",\"data\":{\"windows\":[");

    WinEnumCtx ctx;
    ctx.buf      = buf;
    ctx.capacity = MAX_RESPONSE - 16;
    ctx.offset   = offset;
    ctx.count    = 0;
    ctx.fg_hwnd  = GetForegroundWindow();

    EnumWindows(enum_windows_cb, (LPARAM)&ctx);
    snprintf(buf + ctx.offset, MAX_RESPONSE - ctx.offset, "]}}");

    send_line(sock, buf);
    free(buf);
}

/* ================================================================
 * Command: active_window
 * ================================================================ */

void cmd_active_window(SOCKET sock)
{
    HWND  hwnd = GetForegroundWindow();
    char  title[512] = {0};
    char  classname[256] = {0};
    RECT  rect = {0, 0, 0, 0};
    DWORD pid = 0;

    if (hwnd) {
        GetWindowTextA(hwnd, title, sizeof(title));
        GetClassNameA(hwnd, classname, sizeof(classname));
        GetWindowRect(hwnd, &rect);
        GetWindowThreadProcessId(hwnd, &pid);
    }

    char title_esc[1024], class_esc[512];
    json_escape(title, title_esc, sizeof(title_esc));
    json_escape(classname, class_esc, sizeof(class_esc));

    char buf[2048];
    snprintf(buf, sizeof(buf),
             "{\"status\":\"ok\",\"data\":{\"title\":\"%s\",\"class\":\"%s\","
             "\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"pid\":%lu}}",
             title_esc, class_esc,
             (int)rect.left, (int)rect.top,
             (int)(rect.right - rect.left), (int)(rect.bottom - rect.top),
             (unsigned long)pid);
    send_line(sock, buf);
}

/* ================================================================
 * Command: set_active_window
 *
 * Activate/focus a window by title substring, class name, or PID.
 * Uses SetForegroundWindow + BringWindowToTop.
 * ================================================================ */

typedef struct {
    const char *title;
    const char *classname;
    DWORD pid;
    HWND  found;
} FindWinCtx;

static BOOL CALLBACK find_window_cb(HWND hwnd, LPARAM lparam)
{
    FindWinCtx *ctx = (FindWinCtx *)lparam;
    if (!IsWindowVisible(hwnd)) return TRUE;

    if (ctx->pid) {
        DWORD wpid = 0;
        GetWindowThreadProcessId(hwnd, &wpid);
        if (wpid == ctx->pid) { ctx->found = hwnd; return FALSE; }
    }

    if (ctx->title && ctx->title[0]) {
        char wt[512] = {0};
        GetWindowTextA(hwnd, wt, sizeof(wt));
        if (wt[0] && strstr(wt, ctx->title)) { ctx->found = hwnd; return FALSE; }
    }

    if (ctx->classname && ctx->classname[0]) {
        char wc[256] = {0};
        GetClassNameA(hwnd, wc, sizeof(wc));
        if (wc[0] && strcmp(wc, ctx->classname) == 0) { ctx->found = hwnd; return FALSE; }
    }

    return TRUE;
}

void cmd_set_active_window(SOCKET sock, const char *json)
{
    char title[512] = {0};
    char classname[256] = {0};
    int  pid = 0;

    json_get_string(json, "title", title, sizeof(title));
    json_get_string(json, "class", classname, sizeof(classname));
    json_get_int(json, "pid", &pid);

    if (!title[0] && !classname[0] && !pid) {
        send_error(sock, "Provide at least one of: title, class, pid");
        return;
    }

    log_msg("set_active_window: title='%s' class='%s' pid=%d", title, classname, pid);

    FindWinCtx ctx;
    ctx.title     = title;
    ctx.classname = classname;
    ctx.pid       = (DWORD)pid;
    ctx.found     = NULL;

    EnumWindows(find_window_cb, (LPARAM)&ctx);

    if (!ctx.found) {
        send_error(sock, "No matching window found");
        return;
    }

    /* Restore if minimized */
    if (IsIconic(ctx.found))
        ShowWindow(ctx.found, SW_RESTORE);

    /* Bring to foreground — use AttachThreadInput trick to bypass
     * the Windows foreground lock (SetForegroundWindow alone fails
     * if our process doesn't own the current foreground window). */
    DWORD fg_tid = GetWindowThreadProcessId(GetForegroundWindow(), NULL);
    DWORD tgt_tid = GetWindowThreadProcessId(ctx.found, NULL);
    DWORD our_tid = GetCurrentThreadId();
    if (fg_tid != our_tid)
        AttachThreadInput(our_tid, fg_tid, TRUE);
    if (tgt_tid != our_tid && tgt_tid != fg_tid)
        AttachThreadInput(our_tid, tgt_tid, TRUE);

    SetForegroundWindow(ctx.found);
    BringWindowToTop(ctx.found);
    SetFocus(ctx.found);

    if (tgt_tid != our_tid && tgt_tid != fg_tid)
        AttachThreadInput(our_tid, tgt_tid, FALSE);
    if (fg_tid != our_tid)
        AttachThreadInput(our_tid, fg_tid, FALSE);

    /* Report what we focused */
    char wt[512] = {0}, wc[256] = {0};
    DWORD wpid = 0;
    RECT rect = {0};
    GetWindowTextA(ctx.found, wt, sizeof(wt));
    GetClassNameA(ctx.found, wc, sizeof(wc));
    GetWindowRect(ctx.found, &rect);
    GetWindowThreadProcessId(ctx.found, &wpid);

    char wt_esc[1024], wc_esc[512];
    json_escape(wt, wt_esc, sizeof(wt_esc));
    json_escape(wc, wc_esc, sizeof(wc_esc));

    char buf[2048];
    snprintf(buf, sizeof(buf),
             "{\"status\":\"ok\",\"data\":{\"title\":\"%s\",\"class\":\"%s\","
             "\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"pid\":%lu}}",
             wt_esc, wc_esc,
             (int)rect.left, (int)rect.top,
             (int)(rect.right - rect.left), (int)(rect.bottom - rect.top),
             (unsigned long)wpid);
    send_line(sock, buf);
}

/* ================================================================
 * Command: manage_window
 *
 * Minimize, maximize, restore, or close a window found by title,
 * class, or PID. Actions: minimize, maximize, restore, close.
 * ================================================================ */

void cmd_manage_window(SOCKET sock, const char *json)
{
    char title[512] = {0};
    char classname[256] = {0};
    char action[32] = {0};
    int  pid = 0;

    json_get_string(json, "title", title, sizeof(title));
    json_get_string(json, "class", classname, sizeof(classname));
    json_get_string(json, "action", action, sizeof(action));
    json_get_int(json, "pid", &pid);

    if (!action[0]) {
        send_error(sock, "action is required (minimize, maximize, restore, close)");
        return;
    }
    if (!title[0] && !classname[0] && !pid) {
        send_error(sock, "Provide at least one of: title, class, pid");
        return;
    }

    log_msg("manage_window: action='%s' title='%s' class='%s' pid=%d", action, title, classname, pid);

    FindWinCtx ctx;
    ctx.title     = title;
    ctx.classname = classname;
    ctx.pid       = (DWORD)pid;
    ctx.found     = NULL;

    EnumWindows(find_window_cb, (LPARAM)&ctx);

    if (!ctx.found) {
        send_error(sock, "No matching window found");
        return;
    }

    BOOL ok = TRUE;
    if (strcmp(action, "minimize") == 0) {
        ShowWindow(ctx.found, SW_MINIMIZE);
    } else if (strcmp(action, "maximize") == 0) {
        ShowWindow(ctx.found, SW_MAXIMIZE);
    } else if (strcmp(action, "restore") == 0) {
        ShowWindow(ctx.found, SW_RESTORE);
    } else if (strcmp(action, "close") == 0) {
        ok = PostMessageA(ctx.found, WM_CLOSE, 0, 0);
    } else {
        send_error(sock, "Unknown action (use: minimize, maximize, restore, close)");
        return;
    }

    if (!ok) {
        send_error(sock, "Window operation failed");
        return;
    }

    /* Report result */
    char wt[512] = {0}, wc[256] = {0};
    DWORD wpid = 0;
    RECT rect = {0};
    GetWindowTextA(ctx.found, wt, sizeof(wt));
    GetClassNameA(ctx.found, wc, sizeof(wc));
    GetWindowRect(ctx.found, &rect);
    GetWindowThreadProcessId(ctx.found, &wpid);

    char wt_esc[1024], wc_esc[512], act_esc[64];
    json_escape(wt, wt_esc, sizeof(wt_esc));
    json_escape(wc, wc_esc, sizeof(wc_esc));
    json_escape(action, act_esc, sizeof(act_esc));

    char buf[2048];
    snprintf(buf, sizeof(buf),
             "{\"status\":\"ok\",\"data\":{\"action\":\"%s\",\"title\":\"%s\",\"class\":\"%s\","
             "\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"pid\":%lu}}",
             act_esc, wt_esc, wc_esc,
             (int)rect.left, (int)rect.top,
             (int)(rect.right - rect.left), (int)(rect.bottom - rect.top),
             (unsigned long)wpid);
    send_line(sock, buf);
}

/* ================================================================
 * Command: clipboard_get  (Windows API, CF_UNICODETEXT)
 * ================================================================ */

void cmd_clipboard_get(SOCKET sock)
{
    if (!OpenClipboard(NULL)) {
        send_error(sock, "Failed to open clipboard");
        return;
    }

    HANDLE hData = GetClipboardData(CF_UNICODETEXT);
    if (!hData) {
        CloseClipboard();
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "{\"status\":\"ok\",\"data\":{\"text\":\"\"}}");
        send_line(sock, buf);
        return;
    }

    WCHAR *wide = (WCHAR *)GlobalLock(hData);
    if (!wide) {
        CloseClipboard();
        send_error(sock, "Failed to lock clipboard data");
        return;
    }

    /* Convert UTF-16 to UTF-8 */
    int utf8_len = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    char *utf8 = (char *)malloc(utf8_len);
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, utf8_len, NULL, NULL);

    GlobalUnlock(hData);
    CloseClipboard();

    /* JSON-escape the text */
    char *escaped = (char *)malloc(utf8_len * 6 + 1);
    json_escape(utf8, escaped, utf8_len * 6 + 1);
    free(utf8);

    char *response = (char *)malloc(MAX_RESPONSE);
    snprintf(response, MAX_RESPONSE,
             "{\"status\":\"ok\",\"data\":{\"text\":\"%s\"}}", escaped);
    send_line(sock, response);

    free(escaped);
    free(response);
}

/* ================================================================
 * Command: clipboard_set  (Windows API, CF_UNICODETEXT)
 * ================================================================ */

void cmd_clipboard_set(SOCKET sock, const char *json)
{
    /* Extract UTF-8 text from JSON */
    char *text = (char *)malloc(MAX_REQUEST);
    if (!json_get_string(json, "text", text, MAX_REQUEST)) {
        free(text);
        send_error(sock, "Missing 'text' parameter");
        return;
    }

    log_msg("clipboard_set: %d bytes", (int)strlen(text));

    /* Convert UTF-8 to UTF-16 */
    int wide_len = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, wide_len * sizeof(WCHAR));
    if (!hMem) {
        free(text);
        send_error(sock, "GlobalAlloc failed");
        return;
    }

    WCHAR *wide = (WCHAR *)GlobalLock(hMem);
    MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, wide_len);
    GlobalUnlock(hMem);
    free(text);

    if (!OpenClipboard(NULL)) {
        GlobalFree(hMem);
        send_error(sock, "Failed to open clipboard");
        return;
    }

    EmptyClipboard();
    SetClipboardData(CF_UNICODETEXT, hMem);
    CloseClipboard();
    /* Note: hMem is now owned by the clipboard — do NOT free it */

    char buf[128];
    snprintf(buf, sizeof(buf), "{\"status\":\"ok\",\"data\":{\"set\":true}}");
    send_line(sock, buf);
}

/* ================================================================
 * Command: run_command
 * ================================================================ */

void cmd_run_command(SOCKET sock, const char *json)
{
    char command[4096] = {0};
    int  timeout_ms = 30000;

    if (!json_get_string(json, "cmd", command, sizeof(command))) {
        send_error(sock, "Missing 'cmd' parameter");
        return;
    }
    json_get_int(json, "timeout", &timeout_ms);
    if (timeout_ms < 1000)   timeout_ms = 1000;
    if (timeout_ms > 300000) timeout_ms = 300000;

    log_msg("run_command: %s (timeout=%d)", command, timeout_ms);

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;

    HANDLE stdout_rd, stdout_wr, stderr_rd, stderr_wr;
    if (!CreatePipe(&stdout_rd, &stdout_wr, &sa, 0) ||
        !CreatePipe(&stderr_rd, &stderr_wr, &sa, 0)) {
        send_error(sock, "Failed to create pipes");
        return;
    }

    SetHandleInformation(stdout_rd, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stderr_rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdOutput = stdout_wr;
    si.hStdError  = stderr_wr;
    si.hStdInput  = NULL;

    char cmd_line[8192];
    snprintf(cmd_line, sizeof(cmd_line), "cmd.exe /c %s", command);

    if (!CreateProcessA(NULL, cmd_line, NULL, NULL, TRUE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        send_error(sock, "Failed to create process");
        CloseHandle(stdout_rd); CloseHandle(stdout_wr);
        CloseHandle(stderr_rd); CloseHandle(stderr_wr);
        return;
    }

    /* Close write ends — child has its own handles */
    CloseHandle(stdout_wr);
    CloseHandle(stderr_wr);

    DWORD wait_result = WaitForSingleObject(pi.hProcess, (DWORD)timeout_ms);
    int   timed_out   = (wait_result == WAIT_TIMEOUT);

    if (timed_out) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
    }

    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);

    /* Read captured output.
     * Use PeekNamedPipe to avoid blocking — child processes spawned by
     * cmd.exe (e.g. "start notepad.exe") may inherit pipe handles,
     * keeping the pipe open after cmd.exe itself exits. */
    Sleep(100);  /* let output buffer before peeking */

    char *stdout_buf = (char *)malloc(MAX_CMD_OUTPUT);
    char *stderr_buf = (char *)malloc(MAX_CMD_OUTPUT);
    DWORD bytes_read, bytes_avail;
    int stdout_len = 0, stderr_len = 0;

    while (stdout_len < MAX_CMD_OUTPUT - 1) {
        if (!PeekNamedPipe(stdout_rd, NULL, 0, NULL, &bytes_avail, NULL)
            || bytes_avail == 0)
            break;
        DWORD to_read = bytes_avail;
        if (to_read > (DWORD)(MAX_CMD_OUTPUT - stdout_len - 1))
            to_read = (DWORD)(MAX_CMD_OUTPUT - stdout_len - 1);
        if (!ReadFile(stdout_rd, stdout_buf + stdout_len, to_read,
                      &bytes_read, NULL) || bytes_read == 0)
            break;
        stdout_len += (int)bytes_read;
    }
    stdout_buf[stdout_len] = '\0';

    while (stderr_len < MAX_CMD_OUTPUT - 1) {
        if (!PeekNamedPipe(stderr_rd, NULL, 0, NULL, &bytes_avail, NULL)
            || bytes_avail == 0)
            break;
        DWORD to_read = bytes_avail;
        if (to_read > (DWORD)(MAX_CMD_OUTPUT - stderr_len - 1))
            to_read = (DWORD)(MAX_CMD_OUTPUT - stderr_len - 1);
        if (!ReadFile(stderr_rd, stderr_buf + stderr_len, to_read,
                      &bytes_read, NULL) || bytes_read == 0)
            break;
        stderr_len += (int)bytes_read;
    }
    stderr_buf[stderr_len] = '\0';

    CloseHandle(stdout_rd);
    CloseHandle(stderr_rd);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    /* Build response */
    char *out_esc = (char *)malloc(stdout_len * 6 + 1);
    char *err_esc = (char *)malloc(stderr_len * 6 + 1);
    json_escape(stdout_buf, out_esc, stdout_len * 6 + 1);
    json_escape(stderr_buf, err_esc, stderr_len * 6 + 1);

    char *response = (char *)malloc(MAX_RESPONSE);
    if (timed_out) {
        snprintf(response, MAX_RESPONSE,
                 "{\"status\":\"ok\",\"data\":{"
                 "\"stdout\":\"%s\",\"stderr\":\"%s\","
                 "\"exit_code\":%lu,\"timed_out\":true}}",
                 out_esc, err_esc, (unsigned long)exit_code);
    } else {
        snprintf(response, MAX_RESPONSE,
                 "{\"status\":\"ok\",\"data\":{"
                 "\"stdout\":\"%s\",\"stderr\":\"%s\","
                 "\"exit_code\":%lu}}",
                 out_esc, err_esc, (unsigned long)exit_code);
    }

    send_line(sock, response);

    free(stdout_buf); free(stderr_buf);
    free(out_esc);    free(err_esc);
    free(response);
}

/* ================================================================
 * Command: screen_info
 * ================================================================ */

typedef struct {
    char *buf;
    int   capacity;
    int   offset;
    int   count;
} MonitorCtx;

static BOOL CALLBACK enum_monitors_cb(HMONITOR hmon, HDC hdc,
                                      LPRECT lprc, LPARAM lparam)
{
    (void)hdc; (void)lprc;
    MonitorCtx *ctx = (MonitorCtx *)lparam;

    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    GetMonitorInfoA(hmon, &mi);

    int primary = (mi.dwFlags & MONITORINFOF_PRIMARY) ? 1 : 0;

    if (ctx->count > 0 && ctx->offset < ctx->capacity - 1)
        ctx->buf[ctx->offset++] = ',';

    ctx->offset += snprintf(
        ctx->buf + ctx->offset, ctx->capacity - ctx->offset,
        "{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"primary\":%s}",
        (int)mi.rcMonitor.left, (int)mi.rcMonitor.top,
        (int)(mi.rcMonitor.right - mi.rcMonitor.left),
        (int)(mi.rcMonitor.bottom - mi.rcMonitor.top),
        primary ? "true" : "false");

    ctx->count++;
    return TRUE;
}

void cmd_screen_info(SOCKET sock)
{
    HDC hdc = GetDC(NULL);
    int dpi = GetDeviceCaps(hdc, LOGPIXELSX);
    ReleaseDC(NULL, hdc);

    char monitors_buf[4096];
    MonitorCtx ctx;
    ctx.buf      = monitors_buf;
    ctx.capacity = (int)sizeof(monitors_buf) - 1;
    ctx.offset   = 0;
    ctx.count    = 0;

    EnumDisplayMonitors(NULL, NULL, enum_monitors_cb, (LPARAM)&ctx);
    monitors_buf[ctx.offset] = '\0';

    char response[8192];
    snprintf(response, sizeof(response),
             "{\"status\":\"ok\",\"data\":{\"monitors\":[%s],\"dpi\":%d}}",
             monitors_buf, dpi);
    send_line(sock, response);
}

/* ================================================================
 * Command: file_upload
 * ================================================================ */

/* Minimal base64 decode — returns malloc'd buffer, sets *out_len */
static unsigned char *b64_decode(const char *in, int in_len, int *out_len)
{
    static const unsigned char T[256] = {
        ['A']=0,['B']=1,['C']=2,['D']=3,['E']=4,['F']=5,['G']=6,['H']=7,
        ['I']=8,['J']=9,['K']=10,['L']=11,['M']=12,['N']=13,['O']=14,['P']=15,
        ['Q']=16,['R']=17,['S']=18,['T']=19,['U']=20,['V']=21,['W']=22,['X']=23,
        ['Y']=24,['Z']=25,['a']=26,['b']=27,['c']=28,['d']=29,['e']=30,['f']=31,
        ['g']=32,['h']=33,['i']=34,['j']=35,['k']=36,['l']=37,['m']=38,['n']=39,
        ['o']=40,['p']=41,['q']=42,['r']=43,['s']=44,['t']=45,['u']=46,['v']=47,
        ['w']=48,['x']=49,['y']=50,['z']=51,['0']=52,['1']=53,['2']=54,['3']=55,
        ['4']=56,['5']=57,['6']=58,['7']=59,['8']=60,['9']=61,['+']=62,['/']=63
    };

    int alloc_len = (in_len / 4) * 3 + 4;
    unsigned char *out = (unsigned char *)malloc(alloc_len);
    if (!out) return NULL;

    int j = 0;
    for (int i = 0; i < in_len; ) {
        unsigned int a = (i < in_len && in[i] != '=') ? T[(unsigned char)in[i]] : 0; i++;
        unsigned int b = (i < in_len && in[i] != '=') ? T[(unsigned char)in[i]] : 0; i++;
        unsigned int c = (i < in_len && in[i] != '=') ? T[(unsigned char)in[i]] : 0; i++;
        unsigned int d = (i < in_len && in[i] != '=') ? T[(unsigned char)in[i]] : 0; i++;
        unsigned int triple = (a << 18) | (b << 12) | (c << 6) | d;
        out[j++] = (unsigned char)((triple >> 16) & 0xFF);
        out[j++] = (unsigned char)((triple >> 8) & 0xFF);
        out[j++] = (unsigned char)(triple & 0xFF);
    }
    /* Adjust for padding */
    if (in_len > 0 && in[in_len - 1] == '=') j--;
    if (in_len > 1 && in[in_len - 2] == '=') j--;
    *out_len = j;
    return out;
}

void cmd_file_upload(SOCKET sock, const char *json)
{
    char path[MAX_PATH] = {0};
    char content[MAX_REQUEST] = {0};

    if (!json_get_string(json, "path", path, sizeof(path))) {
        send_error(sock, "Missing 'path' parameter");
        return;
    }
    if (!json_get_string(json, "content", content, sizeof(content))) {
        send_error(sock, "Missing 'content' parameter (base64)");
        return;
    }

    log_msg("file_upload: %s (%d bytes b64)", path, (int)strlen(content));

    int decoded_len = 0;
    unsigned char *decoded = b64_decode(content, (int)strlen(content), &decoded_len);
    if (!decoded) {
        send_error(sock, "Base64 decode failed");
        return;
    }

    HANDLE hFile = CreateFileA(path, GENERIC_WRITE, 0, NULL,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        char msg[512];
        snprintf(msg, sizeof(msg), "Failed to create file: %s (error %lu)",
                 path, (unsigned long)GetLastError());
        send_error(sock, msg);
        free(decoded);
        return;
    }

    DWORD written;
    WriteFile(hFile, decoded, (DWORD)decoded_len, &written, NULL);
    CloseHandle(hFile);
    free(decoded);

    char buf[512];
    snprintf(buf, sizeof(buf),
             "{\"status\":\"ok\",\"data\":{\"path\":\"%s\",\"bytes\":%d}}",
             path, decoded_len);
    send_line(sock, buf);
}

/* ================================================================
 * Command: file_download
 * ================================================================ */

static const char B64_CHARS[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *b64_encode(const unsigned char *in, int in_len, int *out_len)
{
    int alloc = ((in_len + 2) / 3) * 4 + 1;
    char *out = (char *)malloc(alloc);
    if (!out) return NULL;

    int j = 0;
    for (int i = 0; i < in_len; i += 3) {
        unsigned int a = in[i];
        unsigned int b = (i + 1 < in_len) ? in[i + 1] : 0;
        unsigned int c = (i + 2 < in_len) ? in[i + 2] : 0;
        unsigned int triple = (a << 16) | (b << 8) | c;

        out[j++] = B64_CHARS[(triple >> 18) & 0x3F];
        out[j++] = B64_CHARS[(triple >> 12) & 0x3F];
        out[j++] = (i + 1 < in_len) ? B64_CHARS[(triple >> 6) & 0x3F] : '=';
        out[j++] = (i + 2 < in_len) ? B64_CHARS[triple & 0x3F] : '=';
    }
    out[j] = '\0';
    *out_len = j;
    return out;
}

void cmd_file_download(SOCKET sock, const char *json)
{
    char path[MAX_PATH] = {0};

    if (!json_get_string(json, "path", path, sizeof(path))) {
        send_error(sock, "Missing 'path' parameter");
        return;
    }

    log_msg("file_download: %s", path);

    HANDLE hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        char msg[512];
        snprintf(msg, sizeof(msg), "Failed to open file: %s (error %lu)",
                 path, (unsigned long)GetLastError());
        send_error(sock, msg);
        return;
    }

    DWORD file_size = GetFileSize(hFile, NULL);
    if (file_size > 10 * 1024 * 1024) { /* 10MB limit */
        send_error(sock, "File too large (max 10MB)");
        CloseHandle(hFile);
        return;
    }

    unsigned char *file_buf = (unsigned char *)malloc(file_size);
    DWORD bytes_read;
    ReadFile(hFile, file_buf, file_size, &bytes_read, NULL);
    CloseHandle(hFile);

    int b64_len = 0;
    char *b64 = b64_encode(file_buf, (int)bytes_read, &b64_len);
    free(file_buf);

    if (!b64) {
        send_error(sock, "Base64 encode failed");
        return;
    }

    /* Build response — path needs escaping for JSON */
    char path_esc[MAX_PATH * 2];
    json_escape(path, path_esc, sizeof(path_esc));

    int resp_len = b64_len + 256;
    char *response = (char *)malloc(resp_len);
    snprintf(response, resp_len,
             "{\"status\":\"ok\",\"data\":{\"path\":\"%s\",\"bytes\":%lu,\"content\":\"%s\"}}",
             path_esc, (unsigned long)bytes_read, b64);
    send_line(sock, response);

    free(b64);
    free(response);
}
/* ================================================================
 * Command: click_marker
 *
 * Draw a bright yellow ring on screen at (x,y) that auto-destroys
 * after a short duration. Visible in VNC screenshots (no capture
 * exclusion). Used by vnc_click for visual confirmation.
 * ================================================================ */

#define MARKER_WND_CLASS  "WinMcpMarker"
#define MARKER_TIMER_ID   99
#define MARKER_RADIUS     20
#define MARKER_STROKE     3
#define MARKER_SIZE       ((MARKER_RADIUS + MARKER_STROKE) * 2 + 2)
#define MARKER_DURATION   2000  /* ms before auto-destroy */

static volatile HWND g_marker_hwnd = NULL;

static LRESULT CALLBACK marker_wnd_proc(HWND hwnd, UINT msg,
                                         WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_TIMER:
        if (wp == MARKER_TIMER_ID) {
            KillTimer(hwnd, MARKER_TIMER_ID);
            DestroyWindow(hwnd);
            g_marker_hwnd = NULL;
        }
        return 0;
    case WM_DESTROY:
        g_marker_hwnd = NULL;
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* Create and show a click marker at the given screen coordinates.
 * MUST be called from the GUI thread (via WM_CREATE_MARKER) so that
 * SetTimer/WM_TIMER auto-destroy works through the message pump. */
void create_click_marker(int cx, int cy, int duration_ms)
{
    /* Destroy previous marker if still visible */
    if (g_marker_hwnd) {
        DestroyWindow(g_marker_hwnd);
        g_marker_hwnd = NULL;
    }

    int sz = MARKER_SIZE;
    int x = cx - sz / 2;
    int y = cy - sz / 2;

    HINSTANCE hInst = GetModuleHandleA(NULL);

    /* Register class once */
    static int registered = 0;
    if (!registered) {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc   = marker_wnd_proc;
        wc.hInstance      = hInst;
        wc.lpszClassName  = MARKER_WND_CLASS;
        wc.hbrBackground  = (HBRUSH)GetStockObject(NULL_BRUSH);
        RegisterClassA(&wc);
        registered = 1;
    }

    HWND hwnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT,
        MARKER_WND_CLASS, NULL,
        WS_POPUP,
        x, y, sz, sz,
        NULL, NULL, hInst, NULL);

    if (!hwnd) return;

    /* Paint yellow ring into a 32-bit ARGB DIB */
    BITMAPINFO bmi;
    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = sz;
    bmi.bmiHeader.biHeight      = -sz;  /* top-down */
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    BYTE *bits = NULL;
    HDC screen_dc = GetDC(NULL);
    HDC mem_dc = CreateCompatibleDC(screen_dc);
    HBITMAP dib = CreateDIBSection(mem_dc, &bmi, DIB_RGB_COLORS,
                                   (void **)&bits, NULL, 0);
    HBITMAP old_bmp = (HBITMAP)SelectObject(mem_dc, dib);

    /* Clear to fully transparent */
    memset(bits, 0, (size_t)(sz * sz * 4));

    /* Draw yellow ring with per-pixel alpha */
    int center = sz / 2;
    int r_outer = MARKER_RADIUS + MARKER_STROKE;
    int r_inner = MARKER_RADIUS - MARKER_STROKE;
    for (int py = 0; py < sz; py++) {
        for (int px = 0; px < sz; px++) {
            int dx = px - center;
            int dy = py - center;
            int dist_sq = dx * dx + dy * dy;
            if (dist_sq <= r_outer * r_outer && dist_sq >= r_inner * r_inner) {
                BYTE *p = bits + (py * sz + px) * 4;
                /* Pre-multiplied alpha BGRA: bright yellow, fully opaque */
                p[0] = 0;     /* B */
                p[1] = 255;   /* G */
                p[2] = 255;   /* R */
                p[3] = 255;   /* A */
            }
        }
    }

    /* Also draw a small crosshair at center (4px) */
    for (int i = -4; i <= 4; i++) {
        /* Horizontal */
        if (center + i >= 0 && center + i < sz) {
            BYTE *p = bits + (center * sz + center + i) * 4;
            p[0] = 0; p[1] = 255; p[2] = 255; p[3] = 255;
        }
        /* Vertical */
        if (center + i >= 0 && center + i < sz) {
            BYTE *p = bits + ((center + i) * sz + center) * 4;
            p[0] = 0; p[1] = 255; p[2] = 255; p[3] = 255;
        }
    }

    /* UpdateLayeredWindow */
    POINT pt_src = {0, 0};
    POINT pt_pos = {x, y};
    SIZE wnd_sz = {sz, sz};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};

    UpdateLayeredWindow(hwnd, screen_dc, &pt_pos, &wnd_sz,
                        mem_dc, &pt_src, 0, &blend, ULW_ALPHA);

    SelectObject(mem_dc, old_bmp);
    DeleteObject(dib);
    DeleteDC(mem_dc);
    ReleaseDC(NULL, screen_dc);

    /* Show and start auto-destroy timer */
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    if (duration_ms < 500)  duration_ms = 500;
    if (duration_ms > 10000) duration_ms = 10000;
    SetTimer(hwnd, MARKER_TIMER_ID, (UINT)duration_ms, NULL);

    g_marker_hwnd = hwnd;
}

void cmd_click_marker(SOCKET sock, const char *json)
{
    int x = 0, y = 0, duration = MARKER_DURATION;

    if (!json_get_int(json, "x", &x) || !json_get_int(json, "y", &y)) {
        send_error(sock, "Missing 'x' and 'y' parameters");
        return;
    }
    json_get_int(json, "duration", &duration);

    log_msg("click_marker: x=%d y=%d duration=%d", x, y, duration);

    /* Post to the GUI thread — windows must be created on the thread
     * that runs the message pump, otherwise WM_TIMER never fires. */
    MarkerRequest *req = (MarkerRequest *)malloc(sizeof(MarkerRequest));
    if (req) {
        req->x = x;
        req->y = y;
        req->duration = duration;
        PostMessage(g_hwnd, WM_CREATE_MARKER, 0, (LPARAM)req);
    }

    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"status\":\"ok\",\"data\":{\"x\":%d,\"y\":%d,\"duration\":%d}}",
             x, y, duration);
    send_line(sock, buf);
}
