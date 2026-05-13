# Changelog

## [0.4.0] - 2026-05-13

### Added
- **`vnc_grid` tool** — overlay a labeled coordinate grid (columns A–P, rows 1–12) on a screenshot and return center pixel coordinates for every cell. Dramatically reduces the number of tool calls needed to target UI elements.
- **`vnc_manage_window` tool** — minimize, maximize, restore, or close a window by title, class name, or PID. Completes the window management suite alongside `vnc_set_active_window`.
- **Tool call timeout (R6)** — all tool executions are wrapped in a 45-second deadline. If a tool stalls (e.g., dead VNC connection), the MCP server returns an error instead of hanging the IDE indefinitely.
- **5×7 bitmap font** — built-in pixel font (A–Z, 0–9) for server-side text rendering on screenshots at configurable scale.
- **Agent instruction #11** — guidance for using `vnc_grid` in the coordinate discovery workflow.

### Changed
- **Marker contrast (#16)** — probe and click markers now render with a 2px black outline before the yellow fill. Markers are clearly visible on both light and dark backgrounds.
- **Version string** — server now correctly reports its version (was stuck at 0.1.0).

### Fixed
- Grid labels use outlined text (white on black) for readability against any background.

## [0.3.0] - 2026-05-13

### Added
- **`vnc_probe` tool** — place a yellow marker at specific coordinates on a screenshot without clicking or interacting with the remote desktop. Server-side rendering only.
- **Resolution metadata** — screenshots include "Resolution: WxH pixels" text content, giving AI agents the actual pixel dimensions for accurate coordinate computation.
- **Agent instructions #9–#10** — one-axis-at-a-time coordinate refinement; honest probe evaluation before clicking.

### Changed
- Screenshot tool description updated to warn about staleness and recommend helper tools.
- Instructions explicitly prohibit visual coordinate estimation from scaled screenshots.

## [0.2.0] - 2026-05-13

### Added
- **`vnc_set_active_window` tool** — activate/focus a window by title substring, class name, or PID. Uses `AttachThreadInput` trick to bypass Windows foreground lock.
- **`vnc_helper_clipboard_get` / `vnc_helper_clipboard_set` tools** — read/write Windows clipboard via Win32 API (CF_UNICODETEXT) for full Unicode support.
- **Single-instance enforcement** — named mutex prevents duplicate `vnc-helper.exe` processes.

### Fixed
- **Download trailing byte (#13)** — `calcSizeUpperBound` returned 1–2 extra bytes; now computes exact size from base64 padding.
- **Paste reliability (#3)** — mitigated by new helper-based clipboard tools that bypass VNC protocol limitations.
- **Focus steal (#11)** — `AttachThreadInput` workaround for `SetForegroundWindow` failures.

## [0.1.0] - 2026-05-13

### Added
- **Stale screenshot fix** — kqueue-based adaptive capture replaces blind 500ms sleep. Converges to fresh frames via incremental update requests.
- **Click marker auto-destroy** — fixed thread affinity bug (PostMessage to GUI thread for timer-based window lifecycle).
- **Stale helper socket detection (R3)** — kqueue `EVFILT_READ` with zero-timeout for instant EV_EOF detection on dead connections.

### Fixed
- Click marker windows weren't destroyed after timeout (thread affinity).
- Helper socket hang on restart (30s SO_RCVTIMEO replaced by kqueue probe).

## [0.1.0-alpha] - 2026-05-12

### Added
- Initial release: 21 MCP tools (9 VNC + 7 helper + 3 UI Automation + OCR + clipboard_get).
- Pure Zig RFB client with DES authentication.
- Windows helper agent (C, cross-compiled) with system tray, on-screen overlay, persistent connections.
- VNC tools: screenshot, click, drag, move_mouse, key_press, type_text, clipboard_set/get, paste_text, list_endpoints.
- Helper tools: cursor_position, window_list, active_window, run_command, screen_info, upload_file, download_file.
- UI Automation: ui_tree, ui_element_text, ui_click_element (via PowerShell).
- OCR: ocr_region (via PowerShell Windows.Media.Ocr).
- Visual click confirmation with yellow marker overlay.
- NSIS installer with firewall rule and startup registration.
- Forgejo Actions CI/CD pipeline.
