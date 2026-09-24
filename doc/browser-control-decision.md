# Browser Control — Path Decision

> Decision made 2026-09-23. Chooses the transport over which the browser
> tools operate; the MCP tool surface is designed to be backend-agnostic so
> the rejected path remains available as a future backend.

## Decision

Build **Phase 8: a direct Marionette client in C** inside the WinMCP agent,
talking to the Firefox/Bloom remote agent on `localhost:2828`.

[Issue #15](https://pacyworld.dev/pacyworld/vnc-mcp-server/issues/15)
(geckodriver/chromedriver REST proxy) stays **open** as the documented
fallback for Chrome/Edge targets.

## The two candidate paths

### A. WebDriver via geckodriver/chromedriver (issue #15)

The agent launches the driver binary — itself a W3C WebDriver HTTP server —
and proxies REST calls (session management, element find/click/type,
screenshot, execute-sync JS).

### B. Direct Marionette client (chosen)

The agent speaks Marionette's length-prefixed JSON wire protocol directly to
the browser's built-in server. First-class commands to start:
`WebDriver:ExecuteScript` (with optional `sandbox: "system"` for
chrome-privileged JS), `WebDriver:Navigate`, `WebDriver:FindElement(s)`,
`WebDriver:ElementClick`, `WebDriver:PerformActions`, `WebDriver:GetElementText`,
`WebDriver:TakeScreenshot`.

## Rationale

Fact verified in the Bloom source (Firefox ESR 153): geckodriver is itself
**only a WebDriver→Marionette translator**. `ElementClick`, `PerformActions`
(full W3C Actions pipeline), `FindElement`, and `TakeScreenshot` all run
inside the browser's own Marionette server
(`remote/marionette/{server,driver,interaction}.sys.mjs`). geckodriver
forwards to those exact commands and contributes zero input capability of
its own. Going direct removes the middleman without losing native, trusted
input synthesis.

Advantages of the direct path:

- **Attaches to the user's live browser** — real profile, logged-in
  sessions, the exact window visible over VNC. WebDriver/geckodriver always
  spawns a fresh browser process with a separate profile and cannot attach
  to a running instance. Driving the live desktop is the core mission of
  this project.
- **Zero deployment.** The Marionette server is compiled into every
  Firefox/Bloom build; enabling is one flag (`-marionette`) or pref. No
  driver binary to install or keep version-matched (chromedriver↔Chrome
  version coupling is a recurring breakage class). Bloom dev profiles can
  default it on.
- **`sandbox: "system"` chrome-privileged JS** — toolkit access strictly
  beyond content-level WebDriver JS.
- **One less process and hop:** MCP → agent → browser instead of
  MCP → agent → geckodriver → browser, on the same box class where orphaned
  helper sockets already cost us a release cycle.

## Explicitly rejected claims (recorded so they aren't repeated)

Two alleged differences were checked against the source and found false:

- *"Direct Marionette loses geckodriver-style native clicks."* **False** —
  the native click/action machinery lives in the browser's Marionette
  server; geckodriver only forwards to it.
- *"Marionette avoids the automation fingerprint."* **False** —
  `Navigator::Webdriver()` (`dom/base/Navigator.cpp`) returns true whenever
  the Marionette service or RemoteAgent is *running*. Both paths flag
  `navigator.webdriver = true` by default. If stealth ever matters, the
  lever is a Bloom-fork patch (we own the browser), not path selection.

## Remaining cons of the chosen path (accepted)

- Firefox/Bloom only — no Chrome/Edge. Accepted: the fleet runs Bloom; a
  Chrome target can be served later by a WebDriver backend behind the same
  MCP surface (issue #15 remains the design for that).
- We maintain the wire client (~300 lines of C) instead of offloading to an
  upstream driver.
- Known Bloom limitation: the DANE cert-verify path (socket process) does
  not fire under Marionette, producing false negatives. TLS/DANE testing
  keeps using interactively launched browsers; this is a test-harness
  limitation, not a control-path defect.

## MCP surface

`vnc_browser_eval` (and any later structured tools) must be written
backend-agnostic: they proxy whichever transport the agent supports.
Phase 8 lands the Marionette backend; the issue #15 path can slot in behind
the same tools without changing the MCP contract.

Phase 9 (Firefox-native RDP client) is unaffected and stays on the roadmap.
