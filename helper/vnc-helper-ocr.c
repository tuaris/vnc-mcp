/*
 * vnc-helper-ocr.c — OCR command (PowerShell wrapper, to be replaced with native WinRT)
 *
 * Copyright (c) 2026, The Daniel Morante Company, Inc.
 * BSD 2-Clause License
 */

#include "vnc-helper.h"

/* ================================================================
 * Command: ocr_region
 *
 * Thin wrapper around vnc-ocr.ps1 (deployed alongside this exe).
 * Captures a screen region and returns OCR-recognized text.
 * ================================================================ */

void cmd_ocr_region(SOCKET sock, const char *json)
{
    int x = 0, y = 0, w = 0, h = 0;
    char lang[64] = {0};

    if (!json_get_int(json, "x", &x) || !json_get_int(json, "y", &y) ||
        !json_get_int(json, "w", &w) || !json_get_int(json, "h", &h)) {
        send_error(sock, "Missing x, y, w, h parameters");
        return;
    }

    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) {
        send_error(sock, "Invalid region dimensions (1-4096)");
        return;
    }

    json_get_string(json, "lang", lang, sizeof(lang));

    log_msg("ocr_region: x=%d y=%d w=%d h=%d lang=%s", x, y, w, h,
            lang[0] ? lang : "(default)");

    /* Locate vnc-ocr.ps1 next to our exe */
    char exe_path[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, exe_path, MAX_PATH);

    /* Strip filename to get directory */
    char *last_sep = strrchr(exe_path, '\\');
    if (last_sep) *(last_sep + 1) = '\0';

    char ps1_path[MAX_PATH];
    snprintf(ps1_path, sizeof(ps1_path), "%svnc-ocr.ps1", exe_path);

    /* Check script exists */
    DWORD attrs = GetFileAttributesA(ps1_path);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        send_error(sock, "vnc-ocr.ps1 not found next to vnc-helper.exe");
        return;
    }

    /* Build PowerShell command line */
    char cmd_line[4096];
    if (lang[0]) {
        snprintf(cmd_line, sizeof(cmd_line),
                 "powershell.exe -ExecutionPolicy Bypass -NoProfile -NonInteractive "
                 "-File \"%s\" -X %d -Y %d -W %d -H %d -Lang \"%s\"",
                 ps1_path, x, y, w, h, lang);
    } else {
        snprintf(cmd_line, sizeof(cmd_line),
                 "powershell.exe -ExecutionPolicy Bypass -NoProfile -NonInteractive "
                 "-File \"%s\" -X %d -Y %d -W %d -H %d",
                 ps1_path, x, y, w, h);
    }

    /* Execute with captured output */
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

    if (!CreateProcessA(NULL, cmd_line, NULL, NULL, TRUE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Failed to start PowerShell (error %lu)",
                 (unsigned long)GetLastError());
        send_error(sock, msg);
        CloseHandle(stdout_rd); CloseHandle(stdout_wr);
        CloseHandle(stderr_rd); CloseHandle(stderr_wr);
        return;
    }

    CloseHandle(stdout_wr);
    CloseHandle(stderr_wr);

    /* 30 second timeout for OCR (PS startup + WinRT init can be slow) */
    DWORD wait_result = WaitForSingleObject(pi.hProcess, 30000);
    int timed_out = (wait_result == WAIT_TIMEOUT);

    if (timed_out) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
    }

    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);

    Sleep(100);

    /* Read stdout (OCR text) */
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

    if (timed_out) {
        send_error(sock, "OCR timed out (30s)");
        free(stdout_buf); free(stderr_buf);
        return;
    }

    if (exit_code != 0) {
        char *err_esc = (char *)malloc(stderr_len * 6 + 128);
        json_escape(stderr_buf, err_esc, stderr_len * 6 + 128);
        char *response = (char *)malloc(MAX_RESPONSE);
        snprintf(response, MAX_RESPONSE,
                 "{\"status\":\"error\",\"message\":\"OCR failed (exit %lu): %s\"}",
                 (unsigned long)exit_code, err_esc);
        send_line(sock, response);
        free(err_esc); free(response);
        free(stdout_buf); free(stderr_buf);
        return;
    }

    /* Build success response with recognized text */
    char *text_esc = (char *)malloc(stdout_len * 6 + 1);
    json_escape(stdout_buf, text_esc, stdout_len * 6 + 1);

    char *response = (char *)malloc(MAX_RESPONSE);
    snprintf(response, MAX_RESPONSE,
             "{\"status\":\"ok\",\"data\":{\"text\":\"%s\"}}",
             text_esc);
    send_line(sock, response);

    free(text_esc); free(response);
    free(stdout_buf); free(stderr_buf);
}
