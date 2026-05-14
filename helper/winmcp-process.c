/*
 * winmcp-process.c — Process and service management commands
 *
 * process_list, process_kill, service_list, service_control
 *
 * Copyright (c) 2026, The Daniel Morante Company, Inc.
 * BSD 2-Clause License
 */

#include "winmcp.h"

/* ================================================================
 * Command: process_list
 *
 * List running processes with name, PID, and memory usage.
 * ================================================================ */

void cmd_process_list(SOCKET sock)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        send_error(sock, "CreateToolhelp32Snapshot failed");
        return;
    }

    char *response = (char *)malloc(MAX_RESPONSE);
    int off = snprintf(response, MAX_RESPONSE,
                       "{\"status\":\"ok\",\"data\":{\"processes\":[");

    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);
    int count = 0;

    if (Process32First(snap, &pe)) {
        do {
            char name_esc[512];
            json_escape(pe.szExeFile, name_esc, sizeof(name_esc));

            if (count > 0 && off < MAX_RESPONSE - 2) response[off++] = ',';
            off += snprintf(response + off, MAX_RESPONSE - off,
                            "{\"name\":\"%s\",\"pid\":%lu,\"parent_pid\":%lu,\"threads\":%lu}",
                            name_esc,
                            (unsigned long)pe.th32ProcessID,
                            (unsigned long)pe.th32ParentProcessID,
                            (unsigned long)pe.cntThreads);
            count++;
            if (off >= MAX_RESPONSE - 256) break;  /* safety */
        } while (Process32Next(snap, &pe));
    }

    CloseHandle(snap);
    snprintf(response + off, MAX_RESPONSE - off, "]}}");
    send_line(sock, response);
    free(response);
}

/* ================================================================
 * Command: process_kill
 *
 * Kill a process by PID or name.
 * ================================================================ */

void cmd_process_kill(SOCKET sock, const char *json)
{
    int pid = 0;
    char name[256] = {0};

    json_get_int(json, "pid", &pid);
    json_get_string(json, "name", name, sizeof(name));

    if (!pid && !name[0]) {
        send_error(sock, "Provide 'pid' or 'name' to kill");
        return;
    }

    log_msg("process_kill: pid=%d name=%s", pid, name[0] ? name : "(none)");

    /* If name given without PID, find first matching PID */
    if (!pid && name[0]) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32 pe;
            pe.dwSize = sizeof(pe);
            if (Process32First(snap, &pe)) {
                do {
                    if (_stricmp(pe.szExeFile, name) == 0) {
                        pid = (int)pe.th32ProcessID;
                        break;
                    }
                } while (Process32Next(snap, &pe));
            }
            CloseHandle(snap);
        }
        if (!pid) {
            send_error(sock, "No process found with that name");
            return;
        }
    }

    HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, (DWORD)pid);
    if (!hProc) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Failed to open process %d (error %lu)",
                 pid, (unsigned long)GetLastError());
        send_error(sock, msg);
        return;
    }

    if (!TerminateProcess(hProc, 1)) {
        char msg[256];
        snprintf(msg, sizeof(msg), "TerminateProcess failed (error %lu)",
                 (unsigned long)GetLastError());
        CloseHandle(hProc);
        send_error(sock, msg);
        return;
    }

    CloseHandle(hProc);

    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"status\":\"ok\",\"data\":{\"killed_pid\":%d}}", pid);
    send_line(sock, buf);
}

/* ================================================================
 * Command: service_list
 *
 * List Windows services with name, display name, status, start type.
 * ================================================================ */

static const char *svc_state_name(DWORD state)
{
    switch (state) {
    case SERVICE_STOPPED:          return "stopped";
    case SERVICE_START_PENDING:    return "start_pending";
    case SERVICE_STOP_PENDING:     return "stop_pending";
    case SERVICE_RUNNING:          return "running";
    case SERVICE_CONTINUE_PENDING: return "continue_pending";
    case SERVICE_PAUSE_PENDING:    return "pause_pending";
    case SERVICE_PAUSED:           return "paused";
    default:                       return "unknown";
    }
}

void cmd_service_list(SOCKET sock)
{
    SC_HANDLE scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_ENUMERATE_SERVICE);
    if (!scm) {
        char msg[256];
        snprintf(msg, sizeof(msg), "OpenSCManager failed (error %lu)",
                 (unsigned long)GetLastError());
        send_error(sock, msg);
        return;
    }

    DWORD bytes_needed = 0, svc_count = 0, resume = 0;
    EnumServicesStatusA(scm, SERVICE_WIN32, SERVICE_STATE_ALL,
                        NULL, 0, &bytes_needed, &svc_count, &resume);

    ENUM_SERVICE_STATUSA *svcs = (ENUM_SERVICE_STATUSA *)malloc(bytes_needed);
    if (!EnumServicesStatusA(scm, SERVICE_WIN32, SERVICE_STATE_ALL,
                             svcs, bytes_needed, &bytes_needed,
                             &svc_count, &resume)) {
        free(svcs);
        CloseServiceHandle(scm);
        send_error(sock, "EnumServicesStatus failed");
        return;
    }

    char *response = (char *)malloc(MAX_RESPONSE);
    int off = snprintf(response, MAX_RESPONSE,
                       "{\"status\":\"ok\",\"data\":{\"services\":[");

    for (DWORD i = 0; i < svc_count; i++) {
        char name_esc[512], disp_esc[1024];
        json_escape(svcs[i].lpServiceName, name_esc, sizeof(name_esc));
        json_escape(svcs[i].lpDisplayName, disp_esc, sizeof(disp_esc));

        if (i > 0 && off < MAX_RESPONSE - 2) response[off++] = ',';
        off += snprintf(response + off, MAX_RESPONSE - off,
                        "{\"name\":\"%s\",\"display_name\":\"%s\",\"status\":\"%s\"}",
                        name_esc, disp_esc,
                        svc_state_name(svcs[i].ServiceStatus.dwCurrentState));
        if (off >= MAX_RESPONSE - 256) break;
    }

    snprintf(response + off, MAX_RESPONSE - off, "]}}");

    CloseServiceHandle(scm);
    free(svcs);
    send_line(sock, response);
    free(response);
}

/* ================================================================
 * Command: service_control
 *
 * Start, stop, or restart a Windows service by name.
 * ================================================================ */

void cmd_service_control(SOCKET sock, const char *json)
{
    char name[256] = {0};
    char action[32] = {0};

    if (!json_get_string(json, "name", name, sizeof(name))) {
        send_error(sock, "Missing 'name' parameter");
        return;
    }
    if (!json_get_string(json, "action", action, sizeof(action))) {
        send_error(sock, "Missing 'action' parameter (start, stop, restart)");
        return;
    }

    log_msg("service_control: name=%s action=%s", name, action);

    SC_HANDLE scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!scm) {
        char msg[256];
        snprintf(msg, sizeof(msg), "OpenSCManager failed (error %lu)",
                 (unsigned long)GetLastError());
        send_error(sock, msg);
        return;
    }

    SC_HANDLE svc = OpenServiceA(scm, name,
                                  SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (!svc) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Service '%s' not found (error %lu)",
                 name, (unsigned long)GetLastError());
        CloseServiceHandle(scm);
        send_error(sock, msg);
        return;
    }

    BOOL ok = TRUE;
    SERVICE_STATUS ss;

    if (strcmp(action, "stop") == 0) {
        ok = ControlService(svc, SERVICE_CONTROL_STOP, &ss);
    } else if (strcmp(action, "start") == 0) {
        ok = StartServiceA(svc, 0, NULL);
    } else if (strcmp(action, "restart") == 0) {
        /* Stop then start */
        if (ControlService(svc, SERVICE_CONTROL_STOP, &ss)) {
            /* Wait up to 10s for stop */
            for (int i = 0; i < 20; i++) {
                Sleep(500);
                if (QueryServiceStatus(svc, &ss) && ss.dwCurrentState == SERVICE_STOPPED)
                    break;
            }
        }
        ok = StartServiceA(svc, 0, NULL);
    } else {
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        send_error(sock, "Unknown action (use: start, stop, restart)");
        return;
    }

    if (!ok) {
        DWORD err = GetLastError();
        /* Not an error if already in desired state */
        if (!((strcmp(action, "stop") == 0 && err == ERROR_SERVICE_NOT_ACTIVE) ||
              (strcmp(action, "start") == 0 && err == ERROR_SERVICE_ALREADY_RUNNING))) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Service %s '%s' failed (error %lu)",
                     action, name, (unsigned long)err);
            CloseServiceHandle(svc);
            CloseServiceHandle(scm);
            send_error(sock, msg);
            return;
        }
    }

    /* Query final status */
    QueryServiceStatus(svc, &ss);
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);

    char name_esc[512], act_esc[64];
    json_escape(name, name_esc, sizeof(name_esc));
    json_escape(action, act_esc, sizeof(act_esc));

    char buf[512];
    snprintf(buf, sizeof(buf),
             "{\"status\":\"ok\",\"data\":{\"name\":\"%s\",\"action\":\"%s\","
             "\"current_status\":\"%s\"}}",
             name_esc, act_esc, svc_state_name(ss.dwCurrentState));
    send_line(sock, buf);
}
