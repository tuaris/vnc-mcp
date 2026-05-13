# VNC MCP Server

A high-performance VNC remote desktop control server for the [Model Context Protocol](https://modelcontextprotocol.io/), with an optional Windows helper agent for extended capabilities.

Built in **Zig** (MCP server) and **C** (Windows helper). The MCP server runs on FreeBSD or Linux and communicates with any standard VNC server over the RFB protocol. The helper agent runs as a system tray app on Windows, providing OS-level features that VNC alone cannot offer.

## Features

- **25 MCP tools** — screen capture, mouse/keyboard input, clipboard, file transfer, OCR, UI automation, command execution
- **Visual click confirmation** — clicks return a screenshot with a yellow marker ring at the exact click point
- **Coordinate verification** — `vnc_probe` places a marker on a screenshot without interacting with the desktop
- **Resolution metadata** — screenshots include pixel dimensions so AI agents can compute coordinates accurately
- **DES authentication** — both VNC and helper connections use challenge-response auth (reads password from VNC server registry)
- **On-screen indicator** — translucent overlay pill shows when an MCP client is connected
- **Persistent connections** — connection pools for both VNC and helper, with automatic reconnection
- **NSIS installer** — Windows installer for the helper agent (firewall rule, startup registration, uninstaller)
- **Multi-endpoint** — manage multiple remote desktops from a single MCP server instance
- **CI/CD** — Forgejo Actions builds MCP server, cross-compiles helper, and packages NSIS installer on tag push

### VNC Tools (pure RFB protocol, no helper needed)

| Tool | Description |
|------|-------------|
| `vnc_screenshot` | Capture screen as JPEG with resolution metadata. Optional quality and delay. |
| `vnc_probe` | Place a yellow marker at coordinates on a screenshot *without* clicking or moving the mouse. |
| `vnc_click` | Click at (x,y) with visual confirmation screenshot. Left/right/middle, double-click. |
| `vnc_drag` | Click-and-drag from (x1,y1) to (x2,y2) with interpolated movement. |
| `vnc_move_mouse` | Move cursor without clicking. |
| `vnc_key_press` | Press key or combo (`ctrl+c`, `alt+F4`, `shift+a`). X11 keysym names. |
| `vnc_type_text` | Type a string via direct Unicode keysym events. |
| `vnc_clipboard_set` | Send text to remote clipboard via VNC ClientCutText (Latin-1). |
| `vnc_clipboard_get` | Read the last clipboard text received from the remote desktop via VNC ServerCutText. |
| `vnc_paste_text` | Set clipboard + Ctrl+V — reliable text entry for URLs and special characters. |
| `vnc_list_endpoints` | List registered VNC endpoints with connection status. |

### Helper Tools (require vnc-helper.exe on target)

| Tool | Description |
|------|-------------|
| `vnc_cursor_position` | Get current mouse cursor position. |
| `vnc_window_list` | List all visible windows with titles, positions, sizes, and PIDs. |
| `vnc_active_window` | Get the focused window's title, class, position, size, and PID. |
| `vnc_set_active_window` | Activate a window by title substring, class name, or PID. |
| `vnc_run_command` | Execute a command via `cmd.exe /c`, return stdout/stderr/exit code. Configurable timeout. |
| `vnc_screen_info` | Get monitor layout, resolution, and DPI. |
| `vnc_upload_file` | Transfer a local file to the remote filesystem (max 10MB, base64 over TCP). |
| `vnc_download_file` | Retrieve a file from the remote filesystem to local disk (max 10MB). |
| `vnc_helper_clipboard_get` | Read Windows clipboard via Win32 API (full Unicode, CF_UNICODETEXT). |
| `vnc_helper_clipboard_set` | Set Windows clipboard via Win32 API (full Unicode). Use with `vnc_key_press` Ctrl+V to paste. |
| `vnc_ocr_region` | OCR a screen region using Windows.Media.Ocr. Returns recognized text. Multi-language. |
| `vnc_ui_tree` | Get the accessibility tree of the foreground window (or by PID). Configurable depth. |
| `vnc_ui_element_text` | Read text/value from a UI element by name or automation ID. |
| `vnc_ui_click_element` | Invoke the default action on a UI element (click button, toggle checkbox, etc.). |

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
│  │  • Helper (per endpt) │──┼─────┼──│  vnc-helper.exe        │  │
│  └───────────────────────┘  │     │  │  (system tray, Win32)  │  │
│                             │     │  │  port 9800 (default)   │  │
│  AI IDE ◄── MCP stdio ──►  │     │  │  + vnc-ocr.ps1         │  │
│                             │     │  │  + vnc-uia.ps1         │  │
└─────────────────────────────┘     │  └───────────────────────┘  │
                                    └─────────────────────────────┘
```

The helper agent delegates OCR to `vnc-ocr.ps1` (Windows.Media.Ocr) and UI Automation to `vnc-uia.ps1` (System.Windows.Automation) — both ship with the installer.

## Building

Requires **Zig 0.15.x** and **OpenSSL** (libcrypto).

```sh
# MCP server (FreeBSD/Linux)
zig build -Doptimize=ReleaseSafe

# Windows helper (cross-compile from FreeBSD/Linux)
zig build helper
```

Outputs:
- `zig-out/bin/vnc-mcp-server` — MCP server binary (~4MB)
- `zig-out/bin/vnc-helper.exe` — Windows helper PE32+ GUI app

The NSIS installer is built by CI (requires a Linux environment with `makensis`). See `.forgejo/workflows/release.yml`.

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

### Windows Helper

**Recommended:** Use the NSIS installer (`vnc-helper-<version>-setup.exe` from the [releases page](https://pacyworld.dev/pacyworld/vnc-mcp-server/releases)). It handles installation, firewall rules, startup registration, and includes the OCR and UI Automation scripts.

**Manual usage:**

```
vnc-helper.exe                  Run as tray app (default port 9800)
vnc-helper.exe -console         Run with console window for debugging
vnc-helper.exe -port 9800       Set listen port
vnc-helper.exe -password-file P Read auth password from file (instead of registry)
vnc-helper.exe install          Add to Windows startup (HKCU\...\Run)
vnc-helper.exe uninstall        Remove from Windows startup
```

**Authentication:** The helper uses VNC DES challenge-response on every TCP connection. It reads the VNC password from the Windows registry (TightVNC, RealVNC, TigerVNC, UltraVNC — checked in order) or from a plaintext file via `-password-file`. The MCP server must have the same password configured via `password_file` in endpoints.json.

**On-screen indicator:** When an MCP client connects, a small translucent overlay pill appears at the top-right corner showing the connection source IP and duration. It's draggable and disappears when disconnected.

**Single-instance:** Only one vnc-helper.exe process runs at a time (enforced via a named mutex). Launching a second instance silently exits.

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

**Helper agent:**
- No external dependencies (statically linked Win32 API)
- Cross-compiled from FreeBSD/Linux with `zig cc`
- PowerShell 5.1+ (ships with Windows 10/11) for OCR and UI Automation scripts

## CI/CD

Forgejo Actions workflows on [pacyworld.dev](https://pacyworld.dev/pacyworld/vnc-mcp-server):

- **CI** (`ci.yml`) — builds MCP server and helper on every push to master
- **Release** (`release.yml`) — on tag push (`v*`): builds binaries, provisions a Linux jail for NSIS, packages the installer, and uploads all three artifacts to a Forgejo release

Release artifacts:
- `vnc-mcp-server-freebsd-amd64` — MCP server binary
- `vnc-helper-windows-amd64.exe` — standalone helper binary
- `vnc-helper-<version>-setup.exe` — NSIS installer (includes OCR/UIA scripts, firewall rule, startup)

## License

BSD 2-Clause — Copyright (c) 2026, The Daniel Morante Company, Inc.
