# Agent Protocol Specification

Version: 1.0 (v0.5.0)

## Overview

The VNC MCP Server communicates with a remote agent (the "helper") over a
persistent TCP connection using a simple newline-delimited JSON protocol.
The agent runs on the target machine and provides native OS API access that
the MCP server cannot obtain through VNC alone.

This document specifies the wire protocol, authentication handshake, and
all supported commands with their request/response formats.

## Transport

- **Protocol:** TCP
- **Default port:** 9800
- **Framing:** Newline-delimited JSON (each message is a single line
  terminated by `\n`)
- **Encoding:** UTF-8
- **Connection model:** Persistent — multiple request/response pairs per
  connection. The MCP server reuses the connection across tool calls.
- **Concurrency:** Requests are serialized by the MCP server (mutex).
  The agent processes one request at a time per connection.
- **Max request size:** 64 KB
- **Max response size:** 2 MB (4 MB read limit on MCP server side)

## Authentication

Authentication is **optional** and uses VNC DES challenge-response
(RFC 6143 Section 7.2.2). It occurs immediately after TCP connection,
before any JSON messages.

### Handshake

```
Agent                           MCP Server
  |                                 |
  |--- 16-byte challenge --------->|
  |                                 |  (DES-encrypt challenge with password)
  |<-- 16-byte response -----------|
  |                                 |
  |--- 4-byte result (BE u32) ---->|
  |    0x00000000 = OK              |
  |    0x00000001 = Failed          |
```

The password is sourced from:
- **Agent side:** VNC server registry key (TightVNC, RealVNC, TigerVNC)
  or a plaintext password file (`-password-file` flag)
- **MCP server side:** `password_file` field in the endpoint configuration

If no password is configured on the agent, authentication is skipped
entirely — the connection proceeds directly to the JSON command phase.

### DES Key Convention

VNC DES uses LSB-first bit ordering (the d3des convention). The password
is truncated or zero-padded to exactly 8 bytes. No bit reversal is
applied — the raw password bytes are used as the DES key.

## Request Format

Every request is a JSON object with a `"command"` field:

```json
{"command": "command_name", ...parameters}
```

## Response Format

Every response is a JSON object with a `"status"` field:

**Success:**
```json
{"status": "ok", "data": { ...result }}
```

**Error:**
```json
{"status": "error", "message": "Human-readable error description"}
```

---

## Commands

### Cursor & Input

#### cursor_position

Get the current mouse cursor position.

**Request:**
```json
{"command": "cursor_position"}
```

**Response:**
```json
{"status": "ok", "data": {"x": 500, "y": 300}}
```

#### click_marker

Display a temporary visual marker at the specified coordinates.
The marker is a yellow ring rendered as a transparent overlay window.
It auto-destroys after the specified duration.

**Request:**
```json
{"command": "click_marker", "x": 500, "y": 300, "duration": 2000}
```

| Field | Type | Required | Default | Description |
|-------|------|----------|---------|-------------|
| x | int | yes | | X coordinate |
| y | int | yes | | Y coordinate |
| duration | int | no | 2000 | Display duration in milliseconds |

**Response:**
```json
{"status": "ok", "data": {"x": 500, "y": 300}}
```

---

### Window Management

#### window_list

List all visible top-level windows.

**Request:**
```json
{"command": "window_list"}
```

**Response:**
```json
{"status": "ok", "data": {"windows": [
  {"title": "Untitled - Notepad", "class": "Notepad",
   "x": 100, "y": 200, "w": 800, "h": 600, "pid": 1234}
]}}
```

#### active_window

Get the currently focused/foreground window.

**Request:**
```json
{"command": "active_window"}
```

**Response:**
```json
{"status": "ok", "data": {"title": "Untitled - Notepad", "class": "Notepad",
  "x": 100, "y": 200, "w": 800, "h": 600, "pid": 1234}}
```

#### set_active_window

Bring a window to the foreground by title, class, or PID.
Uses `AttachThreadInput` to bypass the Windows foreground lock.

**Request:**
```json
{"command": "set_active_window", "title": "Notepad"}
```

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| title | string | no | Window title substring (partial match) |
| class | string | no | Window class name (exact match) |
| pid | int | no | Process ID |

At least one of `title`, `class`, or `pid` must be specified.

**Response:**
```json
{"status": "ok", "data": {"title": "Untitled - Notepad", "class": "Notepad",
  "x": 100, "y": 200, "w": 800, "h": 600, "pid": 1234}}
```

#### manage_window

Minimize, maximize, restore, or close a window.

**Request:**
```json
{"command": "manage_window", "action": "minimize", "title": "Notepad"}
```

| Field | Type | Required | Values |
|-------|------|----------|--------|
| action | string | yes | `minimize`, `maximize`, `restore`, `close` |
| title | string | no | Window title substring |
| class | string | no | Window class name |
| pid | int | no | Process ID |

**Response:**
```json
{"status": "ok", "data": {"action": "minimize",
  "title": "Untitled - Notepad", "class": "Notepad", "pid": 1234}}
```

---

### Clipboard

#### clipboard_get

Read the current Windows clipboard text (CF_UNICODETEXT via Win32 API).

**Request:**
```json
{"command": "clipboard_get"}
```

**Response:**
```json
{"status": "ok", "data": {"text": "clipboard contents"}}
```

#### clipboard_set

Set the Windows clipboard text.

**Request:**
```json
{"command": "clipboard_set", "text": "text to place on clipboard"}
```

**Response:**
```json
{"status": "ok", "data": {"length": 28}}
```

---

### Command Execution

#### run_command

Execute a command via `cmd.exe /c` and return stdout, stderr, and exit
code. Uses `PeekNamedPipe` for non-blocking reads to handle child
processes that inherit pipe handles.

**Request:**
```json
{"command": "run_command", "cmd": "dir C:\\", "timeout": 30000}
```

| Field | Type | Required | Default | Description |
|-------|------|----------|---------|-------------|
| cmd | string | yes | | Command passed to `cmd.exe /c` |
| timeout | int | no | 30000 | Max wait in milliseconds |

**Response:**
```json
{"status": "ok", "data": {
  "stdout": "...", "stderr": "...", "exit_code": 0}}
```

---

### Screen

#### screen_info

Get display resolution, DPI, and monitor information.

**Request:**
```json
{"command": "screen_info"}
```

**Response:**
```json
{"status": "ok", "data": {
  "width": 1918, "height": 968, "dpi": 96, "scale_percent": 100}}
```

---

### File Transfer

#### file_upload

Write a base64-encoded file to a path on the agent machine.
Maximum file size: 10 MB.

**Request:**
```json
{"command": "file_upload", "path": "C:\\tmp\\file.txt",
 "content": "SGVsbG8gV29ybGQ="}
```

**Response:**
```json
{"status": "ok", "data": {"path": "C:\\tmp\\file.txt", "bytes": 11}}
```

#### file_download

Read a file from the agent machine and return its contents as base64.
Maximum file size: 10 MB.

**Request:**
```json
{"command": "file_download", "path": "C:\\tmp\\file.txt"}
```

**Response:**
```json
{"status": "ok", "data": {"path": "C:\\tmp\\file.txt",
 "content": "SGVsbG8gV29ybGQ=", "bytes": 11}}
```

---

### Registry

#### registry_read

Read a Windows registry value. Supports REG_SZ, REG_DWORD, REG_QWORD,
REG_BINARY, REG_MULTI_SZ, and REG_EXPAND_SZ.

**Request:**
```json
{"command": "registry_read", "key": "HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
 "value": "ProductName"}
```

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| key | string | yes | Registry key path (HKLM, HKCU, HKCR, HKU, HKCC) |
| value | string | no | Value name (omit for default value) |

**Response:**
```json
{"status": "ok", "data": {"key": "...", "value": "ProductName",
 "type": "REG_SZ", "data": "Windows 11 Pro"}}
```

#### registry_write

Write a registry value. Creates the key if it does not exist.

**Request:**
```json
{"command": "registry_write", "key": "HKCU\\SOFTWARE\\MyApp",
 "value": "Setting1", "type": "REG_SZ", "data": "hello"}
```

| Field | Type | Required | Values |
|-------|------|----------|--------|
| key | string | yes | Registry key path |
| value | string | no | Value name |
| type | string | no | `REG_SZ` (default), `REG_DWORD`, `REG_EXPAND_SZ` |
| data | string/int | yes | Value data |

**Response:**
```json
{"status": "ok", "data": {"key": "...", "value": "Setting1", "type": "REG_SZ"}}
```

#### registry_list

Enumerate subkeys and values under a registry key.

**Request:**
```json
{"command": "registry_list", "key": "HKLM\\SOFTWARE\\Microsoft"}
```

**Response:**
```json
{"status": "ok", "data": {"key": "...",
  "subkeys": ["DirectX", "Windows"],
  "values": [
    {"name": "InstallDate", "type": "REG_DWORD", "size": 4}
  ]}}
```

---

### Process Management

#### process_list

List all running processes.

**Request:**
```json
{"command": "process_list"}
```

**Response:**
```json
{"status": "ok", "data": {"processes": [
  {"name": "notepad.exe", "pid": 1234, "ppid": 5678, "threads": 4}
]}}
```

#### process_kill

Terminate a process by PID or executable name.

**Request:**
```json
{"command": "process_kill", "pid": 1234}
```

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| pid | int | no | Process ID to terminate |
| name | string | no | Executable name (case-insensitive, first match) |

**Response:**
```json
{"status": "ok", "data": {"pid": 1234, "name": "notepad.exe"}}
```

---

### Service Management

#### service_list

List all Windows services.

**Request:**
```json
{"command": "service_list"}
```

**Response:**
```json
{"status": "ok", "data": {"services": [
  {"name": "wuauserv", "display_name": "Windows Update",
   "status": "running"}
]}}
```

Status values: `running`, `stopped`, `start_pending`, `stop_pending`,
`continue_pending`, `pause_pending`, `paused`.

#### service_control

Start, stop, or restart a Windows service.

**Request:**
```json
{"command": "service_control", "name": "wuauserv", "action": "restart"}
```

| Field | Type | Required | Values |
|-------|------|----------|--------|
| name | string | yes | Service name (not display name) |
| action | string | yes | `start`, `stop`, `restart` |

**Response:**
```json
{"status": "ok", "data": {"name": "wuauserv", "action": "restart",
 "status": "running"}}
```

---

### OCR

#### ocr_region

Capture a screen region and recognize text using Windows.Media.Ocr.
Currently implemented via PowerShell (vnc-ocr.ps1); native WinRT
implementation planned.

**Request:**
```json
{"command": "ocr_region", "x": 100, "y": 200, "w": 400, "h": 50,
 "lang": "en-US"}
```

| Field | Type | Required | Default | Description |
|-------|------|----------|---------|-------------|
| x | int | yes | | Left edge of capture region |
| y | int | yes | | Top edge of capture region |
| w | int | yes | | Width (1–4096) |
| h | int | yes | | Height (1–4096) |
| lang | string | no | system | BCP 47 language tag |

**Response:**
```json
{"status": "ok", "data": {"text": "Recognized text here"}}
```

---

### UI Automation

All UI Automation commands use the COM `IUIAutomation` interface directly.
Searches are performed against the desktop root element using
`TreeScope_Descendants`.

#### ui_tree

Get the accessibility tree of the foreground window (or a specific
process). Uses the Control View walker.

**Request:**
```json
{"command": "ui_tree", "depth": 3, "pid": 0}
```

| Field | Type | Required | Default | Description |
|-------|------|----------|---------|-------------|
| depth | int | no | 3 | Max tree depth (1–10) |
| pid | int | no | 0 | Process ID (0 = foreground window) |

**Response:**
```json
{"status": "ok", "data": {
  "name": "Untitled - Notepad",
  "controlType": "Window",
  "automationId": "",
  "className": "Notepad",
  "isEnabled": true,
  "x": 100, "y": 200, "w": 800, "h": 600,
  "children": [ ... ]
}}
```

Each node contains: `name`, `controlType`, `automationId`, `className`,
`isEnabled`, `x`, `y`, `w`, `h`, and optionally `children`.

Control type values: `Button`, `CheckBox`, `ComboBox`, `Edit`,
`ListItem`, `MenuItem`, `RadioButton`, `Text`, `TreeItem`, `Window`,
`Pane`, `TitleBar`, `MenuBar`, `Menu`, `Tab`, `TabItem`, `Group`,
`List`, `DataGrid`, `Document`, `ToolBar`, `ToolTip`, `Tree`,
`ScrollBar`, `Slider`, `StatusBar`, `ProgressBar`, `Hyperlink`,
`Image`, `Header`, `HeaderItem`, `Table`, `Separator`, `SplitButton`,
`Spinner`, `Thumb`, `Custom`, `SemanticZoom`, `AppBar`, `Calendar`,
`DataItem`, `Unknown`.

#### ui_element_text

Find an element and return its text content. Tries ValuePattern, then
TextPattern (DocumentRange), then falls back to the Name property.

**Request:**
```json
{"command": "ui_element_text", "name": "Text Editor",
 "automation_id": "", "control_type": "Edit"}
```

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| name | string | no | Element name (exact match) |
| automation_id | string | no | Automation ID (exact match) |
| control_type | string | no | Control type filter |

At least one of `name` or `automation_id` must be specified.

**Response:**
```json
{"status": "ok", "data": {
  "text": "Hello world",
  "element": { "name": "...", "controlType": "Edit", ... }
}}
```

#### ui_click_element

Find an element and invoke its default action. Tries patterns in order:

1. **InvokePattern** — buttons, links, menu items
2. **TogglePattern** — checkboxes
3. **SelectionItemPattern** — radio buttons, list items
4. **ExpandCollapsePattern** — dropdowns, tree items (toggles state)
5. **SetFocus** — fallback if no interaction pattern available

**Request:**
```json
{"command": "ui_click_element", "name": "File",
 "control_type": "MenuItem"}
```

Parameters are the same as `ui_element_text`.

**Response:**
```json
{"status": "ok", "data": {
  "action": "invoked",
  "element": { "name": "File", "controlType": "MenuItem", ... }
}}
```

Action values: `invoked`, `toggled`, `selected`, `expanded`,
`collapsed`, `focused`, `none`.

---

## Connection Lifecycle

1. **TCP connect** to agent on configured port
2. **Authentication** (if password configured): challenge-response
3. **Command loop**: send request JSON + `\n`, read response JSON + `\n`
4. **Disconnect**: close TCP socket

The MCP server maintains a persistent connection with:
- **kqueue-based liveness check** (EV_EOF detection) before each request
- **Auto-reconnect** on connection failure (one retry with fresh connection)
- **30-second SO_RCVTIMEO** on the socket to prevent indefinite blocking
- **Mutex serialization** — one request in flight at a time

The agent accepts multiple concurrent TCP connections but processes
requests sequentially per connection. The agent maintains a system tray
icon and connection status overlay.

## Future: Screen Capture & Input Commands

The following commands are planned to enable the agent to fully replace
VNC for Windows targets:

- `screenshot` — DXGI Desktop Duplication capture, return base64 JPEG
- `mouse_click` — SendInput mouse click at coordinates
- `mouse_move` — SendInput mouse move
- `mouse_drag` — SendInput mouse drag
- `key_press` — SendInput keyboard input
- `type_text` — SendInput character sequence

These additions would make the agent a complete Windows remote control
solution, with VNC reserved for cross-platform or agent-less scenarios.
