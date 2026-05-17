# VNC MCP Server

A high-performance VNC remote desktop control server for the [Model Context Protocol](https://modelcontextprotocol.io/), with optional [WinMCP](https://pacyworld.dev/winmcp/winmcp) agent support for extended Windows capabilities.

Built in **Zig**. The MCP server runs on FreeBSD or Linux and communicates with any standard VNC server over the RFB protocol. The [WinMCP agent](https://pacyworld.dev/winmcp/winmcp) runs as a system tray app on Windows, providing OS-level features that VNC alone cannot offer.

## Features

- **34 MCP tools** — screen capture, mouse/keyboard input, clipboard, file transfer, UI automation, window management, process/service management, registry, command execution
- **Visual click confirmation** — clicks return a screenshot with a yellow marker ring at the exact click point
- **Coordinate grid** — `vnc_grid` overlays a labeled grid (A1–P12) and returns center coordinates for every cell
- **Coordinate verification** — `vnc_probe` places a marker on a screenshot without interacting with the desktop
- **Resolution metadata** — screenshots include pixel dimensions so AI agents can compute coordinates accurately
- **DES authentication** — both VNC and helper connections use challenge-response auth (reads password from VNC server registry)
- **On-screen indicator** — translucent overlay pill shows when an MCP client is connected
- **Persistent connections** — connection pools for both VNC and helper, with automatic reconnection
- **Tool call timeout** — 45-second safety net prevents IDE hangs if a tool call stalls
- **Multi-endpoint** — manage multiple remote desktops from a single MCP server instance
- **CI/CD** — Forgejo Actions builds and releases the MCP server on tag push

### VNC Tools (pure RFB protocol, no helper needed)

| Tool | Description |
|------|-------------|
| `vnc_screenshot` | Capture screen as JPEG with resolution metadata. Optional quality and delay. |
| `vnc_probe` | Place a yellow marker at coordinates on a screenshot *without* clicking or moving the mouse. |
| `vnc_grid` | Overlay a labeled coordinate grid (A1–P12) on a screenshot with cell center coordinates. |
| `vnc_click` | Click at (x,y) with visual confirmation screenshot. Left/right/middle, double-click. |
| `vnc_drag` | Click-and-drag from (x1,y1) to (x2,y2) with interpolated movement. |
| `vnc_move_mouse` | Move cursor without clicking. |
| `vnc_key_press` | Press key or combo (`ctrl+c`, `alt+F4`, `shift+a`). X11 keysym names. |
| `vnc_type_text` | Type a string via direct Unicode keysym events. |
| `vnc_clipboard_set` | Send text to remote clipboard via VNC ClientCutText (Latin-1). |
| `vnc_clipboard_get` | Read the last clipboard text received from the remote desktop via VNC ServerCutText. |
| `vnc_paste_text` | Set clipboard + Ctrl+V — reliable text entry for URLs and special characters. |
| `vnc_list_endpoints` | List registered VNC endpoints with connection status. |

### Helper Tools (require WinMCP agent on target)

| Tool | Description |
|------|-------------|
| `vnc_cursor_position` | Get current mouse cursor position. |
| `vnc_window_list` | List all visible windows with titles, positions, sizes, and PIDs. |
| `vnc_active_window` | Get the focused window's title, class, position, size, and PID. |
| `vnc_set_active_window` | Activate a window by title substring, class name, or PID. |
| `vnc_manage_window` | Minimize, maximize, restore, or close a window by title, class, or PID. |
| `vnc_run_command` | Execute a command via `cmd.exe /c`, return stdout/stderr/exit code. Configurable timeout. |
| `vnc_screen_info` | Get monitor layout, resolution, and DPI. |
| `vnc_upload_file` | Transfer a local file to the remote filesystem (max 10MB, base64 over TCP). |
| `vnc_download_file` | Retrieve a file from the remote filesystem to local disk (max 10MB). |
| `vnc_helper_clipboard_get` | Read Windows clipboard via Win32 API (full Unicode, CF_UNICODETEXT). |
| `vnc_helper_clipboard_set` | Set Windows clipboard via Win32 API (full Unicode). Use with `vnc_key_press` Ctrl+V to paste. |
| `vnc_ocr_region` | OCR a screen region using WinRT Windows.Media.Ocr via the native DLL. |
| `vnc_ui_tree` | Get the accessibility tree of the foreground window (or by PID). Configurable depth. |
| `vnc_ui_element_text` | Read text/value from a UI element by name or automation ID. |
| `vnc_ui_click_element` | Invoke the default action on a UI element (click button, toggle checkbox, etc.). |
| `vnc_registry_read` | Read a Windows registry value (REG_SZ, REG_DWORD, REG_QWORD, REG_BINARY, etc.). |
| `vnc_registry_write` | Write a registry value. Creates keys if needed. |
| `vnc_registry_list` | Enumerate subkeys and values under a registry key. |
| `vnc_list_processes` | List running processes with name, PID, parent PID, thread count. |
| `vnc_kill_process` | Terminate a process by PID or executable name. |
| `vnc_list_services` | List all Windows services with status. |
| `vnc_service_control` | Start, stop, or restart a Windows service. |

## Architecture

```
┌─────────────────────────────┐     ┌─────────────────────────────┐
│  FreeBSD / Linux Host       │     │  Windows Desktop            │
│                             │     │                             │
│  ┌───────────────────────┐  │     │  ┌───────────────────────┐  │
│  │  vnc-mcp-server       │  │ RFB │  │  VNC Server            │  │
│  │  (Zig, MCP stdio)     │──┼─────┼──│  (port 5900)           │  │
│  │                       │  │     │  └───────────────────────┘  │
│  │  Connection pools:    │  │     │                             │
│  │  • VNC (per endpoint) │  │ TCP │  ┌───────────────────────┐  │
│  │  • Helper (per endpt) │──┼─────┼──│  winmcp.exe            │  │
│  └───────────────────────┘  │     │  │  (system tray, Win32)  │  │
│                             │     │  │  port 9800 (default)   │  │
│  AI IDE ◄── MCP stdio ──►  │     │  └───────────────────────┘  │
└─────────────────────────────┘     └─────────────────────────────┘
```

UI Automation uses native COM (`IUIAutomation`) for <100ms latency. OCR uses WinRT `Windows.Media.Ocr` via the native DLL.

## Building

Requires **Zig 0.15.x** and **OpenSSL** (libcrypto).

```sh
zig build -Doptimize=ReleaseSafe
```

Outputs:
- `zig-out/bin/vnc-mcp-server` — MCP server binary (~4MB)

The WinMCP agent is built and released from its [own repository](https://pacyworld.dev/winmcp/winmcp).

## Configuration

### MCP Server

Create `~/.config/vnc-mcp/endpoints.json`:

```json
{
  "endpoints": [
    {
      "id": "win11-dev",
      "description": "Windows 11 development VM",
      "host": "192.168.1.195",
      "port": 5900,
      "password_file": "/home/user/.vnc-password",
      "helper_port": 9800,
      "default": true
    }
  ]
}
```

- `password_file` — path to a file containing the VNC password (same password the VNC server uses)
- `helper_port` — TCP port the helper agent listens on (0 or omit to disable helper tools)
- `default` — this endpoint is used when tools are called without an explicit `endpoint` parameter

Override the config path with `VNC_MCP_CONFIG=/path/to/config.json`.

Add to your MCP client config (Windsurf, VS Code, Claude Desktop, etc.):

```json
{
  "mcpServers": {
    "vnc": {
      "command": "/path/to/vnc-mcp-server",
      "args": []
    }
  }
}
```

### WinMCP Agent

The WinMCP agent is a separate project: **[winmcp/winmcp](https://pacyworld.dev/winmcp/winmcp)**

Download the installer from the [WinMCP releases page](https://pacyworld.dev/winmcp/winmcp/releases). See the WinMCP README for installation and configuration.

## Security

- VNC connections use standard RFB DES challenge-response authentication
- Helper agent authenticates every TCP connection with the same VNC DES protocol
- No command execution without a successful auth handshake
- Constant-time password comparison prevents timing attacks
- Helper reads password from the VNC server's registry keys — no separate password config needed
- `vnc_run_command` passes commands to `cmd.exe /c` (same security model as RDP or SSH)
- The on-screen indicator makes remote control sessions visible to the desktop user

## Dependencies

**MCP server:**
- Zig 0.15.x (build only)
- OpenSSL libcrypto (DES for VNC auth)
- stb_image_write.h (bundled, JPEG encoding)

## CI/CD

Forgejo Actions workflows on [pacyworld.dev](https://pacyworld.dev/pacyworld/vnc-mcp-server):

- **CI** (`ci.yml`) — builds MCP server on every push to master
- **Release** (`release.yml`) — on tag push (`v*`): builds the MCP server binary and uploads it to a Forgejo release

Release artifact:
- `vnc-mcp-server-freebsd-amd64` — MCP server binary

The WinMCP agent is built and released from its [own repository](https://pacyworld.dev/winmcp/winmcp).

## License

BSD 2-Clause — Copyright (c) 2026, The Daniel Morante Company, Inc.
