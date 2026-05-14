/*
 * winmcp.h — Shared declarations for WinMCP agent modules
 *
 * Copyright (c) 2026, The Daniel Morante Company, Inc.
 * BSD 2-Clause License
 */

#ifndef WINMCP_H
#define WINMCP_H

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tlhelp32.h>

#define WINMCP_VERSION "0.2.0"
#define DEFAULT_PORT       9800
#define MAX_REQUEST        65536
#define MAX_RESPONSE       (2 * 1024 * 1024)
#define MAX_CMD_OUTPUT     (512 * 1024)

#include "resource.h"

#define WM_TRAYICON       (WM_USER + 1)
#define WM_CREATE_MARKER  (WM_APP + 1)   /* worker thread -> GUI thread */
#define IDM_EXIT     1001
#define IDM_ABOUT    1002

typedef struct {
    int x, y, duration;
} MarkerRequest;

/* ================================================================
 * Globals (defined in winmcp.c)
 * ================================================================ */

extern int  g_port;
extern int  g_console;
extern int  g_running;
extern HWND g_hwnd;
extern NOTIFYICONDATAA g_nid;
extern HICON g_icon_normal;
extern HICON g_icon_connected;
extern volatile LONG g_client_count;
extern char g_client_ip[64];
extern DWORD g_connect_time;
extern CRITICAL_SECTION g_cs;
extern char g_password[9];
extern int  g_auth_enabled;
extern const char *g_password_file;
extern volatile DWORD g_overlay_linger_until;

#define OVERLAY_LINGER_MS 30000

/* ================================================================
 * Native DLL (winmcp-native.dll) — loaded at startup via LoadLibrary
 * ================================================================ */

/* Function pointer types matching winmcp-native.h exports */
typedef int (*wmcp_version_fn)(char *out, int out_max);
typedef int (*wmcp_screenshot_fn)(int x, int y, int w, int h,
                                  unsigned char *jpeg_out, int jpeg_max,
                                  int *jpeg_len, int quality);
typedef int (*wmcp_ocr_region_fn)(int x, int y, int w, int h,
                                  const char *lang,
                                  char *text_out, int text_max);

typedef struct {
    HMODULE             handle;          /* LoadLibrary handle (NULL if not loaded) */
    wmcp_version_fn     version;
    wmcp_screenshot_fn  screenshot;
    wmcp_ocr_region_fn  ocr_region;
} WmcpNative;

extern WmcpNative g_native;

/* Load/unload the native DLL. Returns 1 if loaded, 0 if not found. */
int  native_dll_load(void);
void native_dll_unload(void);

/* ================================================================
 * Utility functions (defined in winmcp.c)
 * ================================================================ */

void log_msg(const char *fmt, ...);
void send_line(SOCKET sock, const char *json);
void send_error(SOCKET sock, const char *msg);
int  json_escape(const char *in, char *out, int out_max);
int  json_get_string(const char *json, const char *key, char *out, int out_max);
int  json_get_int(const char *json, const char *key, int *out);

/* ================================================================
 * Auth (defined in winmcp-auth.c)
 * ================================================================ */

void init_auth(void);
int  auth_client(SOCKET sock);

/* ================================================================
 * Commands (defined in winmcp-commands.c)
 * ================================================================ */

void cmd_cursor_position(SOCKET sock);
void cmd_window_list(SOCKET sock);
void cmd_active_window(SOCKET sock);
void cmd_set_active_window(SOCKET sock, const char *json);
void cmd_manage_window(SOCKET sock, const char *json);
void cmd_clipboard_get(SOCKET sock);
void cmd_clipboard_set(SOCKET sock, const char *json);
void cmd_run_command(SOCKET sock, const char *json);
void cmd_screen_info(SOCKET sock);
void cmd_file_upload(SOCKET sock, const char *json);
void cmd_file_download(SOCKET sock, const char *json);
void cmd_click_marker(SOCKET sock, const char *json);

void cmd_screenshot(SOCKET sock, const char *json);

/* Click marker creation (must be called from GUI thread) */
void create_click_marker(int cx, int cy, int duration_ms);

/* ================================================================
 * Registry (defined in winmcp-registry.c)
 * ================================================================ */

void cmd_registry_read(SOCKET sock, const char *json);
void cmd_registry_write(SOCKET sock, const char *json);
void cmd_registry_list(SOCKET sock, const char *json);

/* ================================================================
 * Process & Service (defined in winmcp-process.c)
 * ================================================================ */

void cmd_process_list(SOCKET sock);
void cmd_process_kill(SOCKET sock, const char *json);
void cmd_service_list(SOCKET sock);
void cmd_service_control(SOCKET sock, const char *json);

/* ================================================================
 * OCR (defined in winmcp-ocr.c)
 * ================================================================ */

void cmd_ocr_region(SOCKET sock, const char *json);

/* ================================================================
 * UI Automation (defined in winmcp-uia.c)
 * ================================================================ */

void cmd_ui_tree(SOCKET sock, const char *json);
void cmd_ui_element_text(SOCKET sock, const char *json);
void cmd_ui_click_element(SOCKET sock, const char *json);

#endif /* WINMCP_H */
