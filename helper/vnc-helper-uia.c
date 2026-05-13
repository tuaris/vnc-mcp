/*
 * vnc-helper-uia.c — UI Automation commands (PowerShell wrapper, to be replaced with native COM)
 *
 * Copyright (c) 2026, The Daniel Morante Company, Inc.
 * BSD 2-Clause License
 */

#include "vnc-helper.h"

/* ================================================================
 * Command: ui_tree
 *
 * Wrapper around vnc-uia.ps1 -Mode tree. Returns the accessibility
 * tree of the foreground window (or a specific PID).
 * ================================================================ */

void cmd_ui_tree(SOCKET sock, const char *json)
{
    int depth = 3;
    int pid = 0;

    json_get_int(json, "depth", &depth);
    json_get_int(json, "pid", &pid);

    if (depth < 1)  depth = 1;
    if (depth > 10) depth = 10;

    log_msg("ui_tree: depth=%d pid=%d", depth, pid);

    /* Locate vnc-uia.ps1 next to our exe */
    char exe_path[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, exe_path, MAX_PATH);
    char *last_sep = strrchr(exe_path, '\\');
    if (last_sep) *(last_sep + 1) = '\0';

    char ps1_path[MAX_PATH];
    snprintf(ps1_path, sizeof(ps1_path), "%svnc-uia.ps1", exe_path);

    DWORD attrs = GetFileAttributesA(ps1_path);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        send_error(sock, "vnc-uia.ps1 not found next to vnc-helper.exe");
        return;
    }

    /* Build PowerShell command line */
    char cmd_line[4096];
    if (pid > 0) {
        snprintf(cmd_line, sizeof(cmd_line),
                 "powershell.exe -ExecutionPolicy Bypass -NoProfile -NonInteractive "
                 "-File \"%s\" -Mode tree -Depth %d -Pid %d",
                 ps1_path, depth, pid);
    } else {
        snprintf(cmd_line, sizeof(cmd_line),
                 "powershell.exe -ExecutionPolicy Bypass -NoProfile -NonInteractive "
                 "-File \"%s\" -Mode tree -Depth %d",
                 ps1_path, depth);
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

    DWORD wait_result = WaitForSingleObject(pi.hProcess, 30000);
    int timed_out = (wait_result == WAIT_TIMEOUT);

    if (timed_out) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
    }

    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    Sleep(100);

    char *stdout_buf = (char *)malloc(MAX_RESPONSE);
    char *stderr_buf = (char *)malloc(MAX_CMD_OUTPUT);
    DWORD bytes_read, bytes_avail;
    int stdout_len = 0, stderr_len = 0;

    while (stdout_len < MAX_RESPONSE - 1) {
        if (!PeekNamedPipe(stdout_rd, NULL, 0, NULL, &bytes_avail, NULL)
            || bytes_avail == 0)
            break;
        DWORD to_read = bytes_avail;
        if (to_read > (DWORD)(MAX_RESPONSE - stdout_len - 1))
            to_read = (DWORD)(MAX_RESPONSE - stdout_len - 1);
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
        send_error(sock, "UI tree timed out (30s)");
        free(stdout_buf); free(stderr_buf);
        return;
    }

    if (exit_code != 0 && stdout_len == 0) {
        char *err_esc = (char *)malloc(stderr_len * 6 + 128);
        json_escape(stderr_buf, err_esc, stderr_len * 6 + 128);
        char *response = (char *)malloc(MAX_RESPONSE);
        snprintf(response, MAX_RESPONSE,
                 "{\"status\":\"error\",\"message\":\"UI automation failed (exit %lu): %s\"}",
                 (unsigned long)exit_code, err_esc);
        send_line(sock, response);
        free(err_esc); free(response);
        free(stdout_buf); free(stderr_buf);
        return;
    }

    /* The PS1 script outputs JSON directly — wrap it in our response format */
    char *response = (char *)malloc(MAX_RESPONSE);
    snprintf(response, MAX_RESPONSE,
             "{\"status\":\"ok\",\"data\":%s}",
             stdout_buf);
    send_line(sock, response);

    free(response);
    free(stdout_buf); free(stderr_buf);
}

/* ================================================================
 * Command: ui_element_text
 *
 * Wrapper around vnc-uia.ps1 -Mode text. Returns text/value of
 * a UI element found by name or automationId.
 * ================================================================ */

void cmd_ui_element_text(SOCKET sock, const char *json)
{
    char name[512] = {0};
    char automation_id[512] = {0};
    char control_type[128] = {0};

    json_get_string(json, "name", name, sizeof(name));
    json_get_string(json, "automation_id", automation_id, sizeof(automation_id));
    json_get_string(json, "control_type", control_type, sizeof(control_type));

    if (name[0] == '\0' && automation_id[0] == '\0') {
        send_error(sock, "Must specify 'name' or 'automation_id'");
        return;
    }

    log_msg("ui_element_text: name=%s aid=%s ct=%s", name, automation_id, control_type);

    char exe_path[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, exe_path, MAX_PATH);
    char *last_sep = strrchr(exe_path, '\\');
    if (last_sep) *(last_sep + 1) = '\0';

    char ps1_path[MAX_PATH];
    snprintf(ps1_path, sizeof(ps1_path), "%svnc-uia.ps1", exe_path);

    DWORD attrs = GetFileAttributesA(ps1_path);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        send_error(sock, "vnc-uia.ps1 not found next to vnc-helper.exe");
        return;
    }

    char cmd_line[4096];
    int offset = snprintf(cmd_line, sizeof(cmd_line),
                          "powershell.exe -ExecutionPolicy Bypass -NoProfile -NonInteractive "
                          "-File \"%s\" -Mode text", ps1_path);

    if (name[0])
        offset += snprintf(cmd_line + offset, sizeof(cmd_line) - offset,
                           " -Name \"%s\"", name);
    if (automation_id[0])
        offset += snprintf(cmd_line + offset, sizeof(cmd_line) - offset,
                           " -AutomationId \"%s\"", automation_id);
    if (control_type[0])
        offset += snprintf(cmd_line + offset, sizeof(cmd_line) - offset,
                           " -ControlType \"%s\"", control_type);

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
        send_error(sock, "Failed to start PowerShell");
        CloseHandle(stdout_rd); CloseHandle(stdout_wr);
        CloseHandle(stderr_rd); CloseHandle(stderr_wr);
        return;
    }

    CloseHandle(stdout_wr);
    CloseHandle(stderr_wr);

    WaitForSingleObject(pi.hProcess, 30000);

    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    Sleep(100);

    char *stdout_buf = (char *)malloc(MAX_RESPONSE);
    DWORD bytes_read, bytes_avail;
    int stdout_len = 0;

    while (stdout_len < MAX_RESPONSE - 1) {
        if (!PeekNamedPipe(stdout_rd, NULL, 0, NULL, &bytes_avail, NULL)
            || bytes_avail == 0)
            break;
        DWORD to_read = bytes_avail;
        if (to_read > (DWORD)(MAX_RESPONSE - stdout_len - 1))
            to_read = (DWORD)(MAX_RESPONSE - stdout_len - 1);
        if (!ReadFile(stdout_rd, stdout_buf + stdout_len, to_read,
                      &bytes_read, NULL) || bytes_read == 0)
            break;
        stdout_len += (int)bytes_read;
    }
    stdout_buf[stdout_len] = '\0';

    CloseHandle(stdout_rd);
    CloseHandle(stderr_rd);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    char *response = (char *)malloc(MAX_RESPONSE);
    snprintf(response, MAX_RESPONSE, "{\"status\":\"ok\",\"data\":%s}", stdout_buf);
    send_line(sock, response);

    free(response);
    free(stdout_buf);
}

/* ================================================================
 * Command: ui_click_element
 *
 * Wrapper around vnc-uia.ps1 -Mode click. Finds a UI element by
 * name/automationId and invokes its default action.
 * ================================================================ */

void cmd_ui_click_element(SOCKET sock, const char *json)
{
    char name[512] = {0};
    char automation_id[512] = {0};
    char control_type[128] = {0};

    json_get_string(json, "name", name, sizeof(name));
    json_get_string(json, "automation_id", automation_id, sizeof(automation_id));
    json_get_string(json, "control_type", control_type, sizeof(control_type));

    if (name[0] == '\0' && automation_id[0] == '\0') {
        send_error(sock, "Must specify 'name' or 'automation_id'");
        return;
    }

    log_msg("ui_click_element: name=%s aid=%s ct=%s", name, automation_id, control_type);

    char exe_path[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, exe_path, MAX_PATH);
    char *last_sep = strrchr(exe_path, '\\');
    if (last_sep) *(last_sep + 1) = '\0';

    char ps1_path[MAX_PATH];
    snprintf(ps1_path, sizeof(ps1_path), "%svnc-uia.ps1", exe_path);

    DWORD attrs = GetFileAttributesA(ps1_path);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        send_error(sock, "vnc-uia.ps1 not found next to vnc-helper.exe");
        return;
    }

    char cmd_line[4096];
    int offset = snprintf(cmd_line, sizeof(cmd_line),
                          "powershell.exe -ExecutionPolicy Bypass -NoProfile -NonInteractive "
                          "-File \"%s\" -Mode click", ps1_path);

    if (name[0])
        offset += snprintf(cmd_line + offset, sizeof(cmd_line) - offset,
                           " -Name \"%s\"", name);
    if (automation_id[0])
        offset += snprintf(cmd_line + offset, sizeof(cmd_line) - offset,
                           " -AutomationId \"%s\"", automation_id);
    if (control_type[0])
        offset += snprintf(cmd_line + offset, sizeof(cmd_line) - offset,
                           " -ControlType \"%s\"", control_type);

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
        send_error(sock, "Failed to start PowerShell");
        CloseHandle(stdout_rd); CloseHandle(stdout_wr);
        CloseHandle(stderr_rd); CloseHandle(stderr_wr);
        return;
    }

    CloseHandle(stdout_wr);
    CloseHandle(stderr_wr);

    WaitForSingleObject(pi.hProcess, 30000);

    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    Sleep(100);

    char *stdout_buf = (char *)malloc(MAX_RESPONSE);
    DWORD bytes_read, bytes_avail;
    int stdout_len = 0;

    while (stdout_len < MAX_RESPONSE - 1) {
        if (!PeekNamedPipe(stdout_rd, NULL, 0, NULL, &bytes_avail, NULL)
            || bytes_avail == 0)
            break;
        DWORD to_read = bytes_avail;
        if (to_read > (DWORD)(MAX_RESPONSE - stdout_len - 1))
            to_read = (DWORD)(MAX_RESPONSE - stdout_len - 1);
        if (!ReadFile(stdout_rd, stdout_buf + stdout_len, to_read,
                      &bytes_read, NULL) || bytes_read == 0)
            break;
        stdout_len += (int)bytes_read;
    }
    stdout_buf[stdout_len] = '\0';

    CloseHandle(stdout_rd);
    CloseHandle(stderr_rd);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    char *response = (char *)malloc(MAX_RESPONSE);
    snprintf(response, MAX_RESPONSE, "{\"status\":\"ok\",\"data\":%s}", stdout_buf);
    send_line(sock, response);

    free(response);
    free(stdout_buf);
}
