# VNC MCP Server

A high-performance VNC remote desktop control server for the [Model Context Protocol](https://modelcontextprotocol.io/), with an optional Windows helper agent for extended capabilities.

Built in **Zig** (MCP server) and **C** (Windows helper), replacing the Node.js mcp-vnc prototype that proved too slow and limited for real-world use.

## Features

### VNC Tools (pure RFB protocol, no helper needed)

| Tool | Description |
|------|-------------|
| `vnc_screenshot` | Capture screen as JPEG. Optional delay parameter. |
| `vnc_click` | Click at (x,y). Left/right/middle button, double-click. |
| `vnc_drag` | Mouse down at (x1,y1), move to (x2,y2), mouse up. |
| `vnc_move_mouse` | Move cursor without clicking. |
| `vnc_key_press` | Press key or combo (Ctrl+C, Alt+F4, etc). |
| `vnc_type_text` | Type string via direct Unicode keysyms. |
| `vnc_clipboard_set` | Send text to remote clipboard via ClientCutText. |
| `vnc_paste_text` | Clipboard set + Ctrl+V — reliable text entry. |
| `vnc_list_endpoints` | List registered VNC endpoints. |

### Helper Tools (require vnc-helper.exe on target)

| Tool | Description |
|------|-------------|
| `vnc_cursor_position` | Get current cursor X,Y. |
| `vnc_window_list` | List open windows with titles, positions, sizes. |
| `vnc_active_window` | Get focused window info. |
| `vnc_run_command` | Execute command, return stdout/stderr/exit code. |
| `vnc_upload_file` | Transfer file to remote filesystem (max 10MB). |
| `vnc_download_file` | Retrieve file from remote filesystem (max 10MB). |
| `vnc_screen_info` | Resolution, DPI, monitor layout. |

## Architecture

```
┌─────────────────────────────┐     ┌─────────────────────────────┐
│  FreeBSD / Linux Host       │     │  Windows VM                 │
│                             │     │                             │
│  ┌───────────────────────┐  │     │  ┌───────────────────────┐  │
│  │  vnc-mcp-server       │  │ VNC │  │  TightVNC / RealVNC   │  │
│  │  (Zig, MCP stdio)     │──┼─────┼──│  (port 5900)          │  │
│  │                       │  │     │  └───────────────────────┘  │
│  │  Persistent RFB conn  │  │     │                             │
│  │  per endpoint         │  │ TCP │  ┌───────────────────────┐  │
│  │                       │──┼─────┼──│  vnc-helper.exe        │  │
│  └───────────────────────┘  │     │  │  (system tray app)     │  │
│                             │     │  │  port 9800             │  │
│  AI IDE ←── MCP stdio       │     │  └───────────────────────┘  │
└─────────────────────────────┘     └─────────────────────────────┘
```

## Building

Requires **Zig 0.15.x** and **OpenSSL** (libcrypto).

```sh
# MCP server (FreeBSD/Linux)
zig build -Doptimize=ReleaseSafe

# Windows helper (cross-compile from FreeBSD/Linux)
zig build helper
```

Outputs:
- `zig-out/bin/vnc-mcp-server` — MCP server binary (~3.5MB)
- `zig-out/bin/vnc-helper.exe` — Windows helper PE32+ GUI app (~770KB)

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
      "password_file": "/home/user/.vnc",
      "helper_port": 9800,
      "default": true
    }
  ]
}
```

Or set `VNC_MCP_CONFIG=/path/to/config.json` env var.

Add to your MCP client config:
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

```
vnc-helper.exe                  Run as tray app (default)
vnc-helper.exe -console         Run with console for debugging
vnc-helper.exe -port 9800       Set listen port
vnc-helper.exe -password-file P Read auth password from file
vnc-helper.exe install          Add to Windows startup (HKCU)
vnc-helper.exe uninstall        Remove from Windows startup
```

**Authentication:** The helper uses VNC DES challenge-response on every connection. It reads the VNC password from the Windows registry (TightVNC, RealVNC, TigerVNC, UltraVNC) or from a plaintext file via `-password-file`. The MCP server must know the same password (via `password_file` in endpoints.json).

**On-screen indicator:** When an MCP client connects, a small translucent overlay pill appears at the top-right corner showing the connection source IP and duration. It's draggable and disappears when disconnected.

## Security

- VNC connections use standard RFB DES challenge-response authentication
- Helper agent authenticates every TCP connection with the same VNC DES protocol
- No unencrypted command execution without successful auth handshake
- Constant-time password comparison prevents timing attacks
- Helper agent reads password from system registry (same as VNC server) — no plaintext config

## Dependencies

**MCP server:**
- Zig 0.15.x (build only)
- OpenSSL libcrypto (DES for VNC auth)
- stb_image_write.h (bundled, JPEG encoding)

**Helper:**
- No external dependencies (statically linked, Win32 API only)
- Cross-compiled from FreeBSD/Linux with `zig cc`

## License

BSD 2-Clause — Copyright (c) 2026, The Daniel Morante Company, Inc.
