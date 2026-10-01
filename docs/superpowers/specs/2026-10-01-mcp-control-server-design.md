# Embedded MCP Control Server — Design

Status: draft for review. Branch: `feature/embedded-control-server`.

## 1. Goal

Let an MCP client (for example Claude Code) control a running Shotcut instance
through the Model Context Protocol, served directly by Shotcut over local HTTP.

Scope is staged:

- **Stage 1 (this spec):** transport and project control.
- **Stage 2 (future spec):** timeline editing and filters.
- **Later:** export (needs a new non-UI entry point on `EncodeDock`).

### Success criteria (stage 1)

1. With the server enabled, `claude mcp add --transport http shotcut http://127.0.0.1:<port>/mcp --header "Authorization: Bearer <token>"` connects and lists the stage 1 tools.
2. Each stage 1 tool behaves as specified in section 5 against a running Shotcut.
3. Requests without a valid token, with an `Origin` header, or with a foreign `Host` header are rejected and never reach a tool.
4. The server is off by default and adds no new Qt dependencies.
5. No stage 1 tool opens a dialog in the common cases. `open_project` pre-checks the two common causes (modified project, stale auto-save) and fails instead. Residual: `MainWindow::open()` can still show project-repair, processing-mode or incompatible-version dialogs for unusual project files. Those block the request until the user answers; the server reports "busy" meanwhile. This cannot be removed without refactoring `open()`, which is out of scope.

### Non-goals (stage 1)

Timeline or filter edits, export, SSE streaming, `Mcp-Session-Id` sessions,
multiple simultaneous in-flight requests, remote (non-loopback) access, TLS,
event push or subscriptions.

## 2. Architecture

```
MCP client --HTTP POST /mcp (JSON-RPC 2.0)--> McpServer --> tool table --> MAIN / MLT / Actions
                                              (inside Shotcut, GUI thread)
```

New files (added by hand to `add_executable` in `src/CMakeLists.txt`, in the
alphabetical `foo.cpp foo.h` convention):

| File | Responsibility |
|------|----------------|
| `src/mcpserver.h/.cpp` | `QTcpServer` listener, HTTP parsing, auth/Origin/Host checks, MCP JSON-RPC dispatch. No editor knowledge. |
| `src/mcptools.h/.cpp` | Tool table: name, description, JSON input schema, handler. Handlers call existing `MLT`, `MAIN`, `Actions` APIs. |

Small hooks in existing files:

- `src/settings.h/.cpp`: `mcpServerEnabled` (default false), `mcpServerPort` (default 8765), `mcpServerToken`.
- `src/mainwindow.ui/.cpp`: a checkable "MCP Server" item in the Settings menu, plus a way to view/copy/regenerate the token.
- `src/CMakeLists.txt`: the new source files. `Qt6::Network` is already linked; no CMake dependency changes.

Everything runs on the GUI thread. No threads, no marshaling.

Editor mutations must go through the existing undoable path
(`MAIN.undoStack()` commands or dock slots), never through raw
`MultitrackModel` mutators. Stage 1 tools do not edit the timeline, so this
constrains stage 2.

## 3. Security

Threat model: other local processes, and web pages in the user's browser
(cross-site requests, DNS rebinding), reaching a port that can drive the editor.

- **Loopback only.** Listen on `QHostAddress::LocalHost`.
- **Bearer token.** `Authorization: Bearer <token>` required on every request.
  Token is 32 bytes from `QRandomGenerator::system()`, hex-encoded, generated on
  first enable, stored in `ShotcutSettings` (plaintext `QSettings`, like other
  settings), regenerable from the Settings menu. Compared in constant time.
- **Origin check.** Any request carrying an `Origin` header is rejected (403).
  Browsers always send one; MCP clients and `curl` do not.
- **Host check.** `Host` must be `127.0.0.1:<port>` or `localhost:<port>`
  (403 otherwise).
- **Off by default.** Enabling is an explicit user action.
- **Path rules.** `open_project` and `save_project` accept absolute paths only.
- **Dialog safety.** Stage 1 tools avoid modal dialogs where the code allows it (success criterion 5, section 5).

## 4. HTTP and MCP surface

HTTP (hand-rolled on `QTcpServer`, HTTP/1.1, `Connection: close`):

- `POST /mcp`, `Content-Type: application/json` -> `200 application/json` for
  requests; `202` with no body for notifications.
- `GET /mcp` -> `405` (no optional SSE stream in stage 1).
- Any other path -> `404`. Malformed request -> `400` and the socket closes.
- Limits: headers <= 8 KB, body <= 1 MB, 10 s read timeout, one request per
  connection. Order of checks: size/format, Host, Origin, token, JSON parse.
- JSON-RPC batching is not supported; a batch array gets an error.

MCP methods:

- `initialize`: negotiate `protocolVersion`. If the client's version is one we
  support, echo it; otherwise reply with our latest. Capabilities: `tools` only.
  The exact supported-version list is fixed at implementation time by checking
  the current MCP specification.
- `notifications/initialized`: accepted, no reply (202).
- `ping`: empty result.
- `tools/list`, `tools/call`.
- Unknown method -> JSON-RPC error `-32601`. Bad params -> `-32602`.

Concurrency: one tool call in flight. A second request that arrives while a
call is running gets JSON-RPC error `-32000` "busy". Tool-level failures
return a normal result with `isError: true` and a text message, per MCP.

## 5. Stage 1 tools

All handlers run on the GUI thread and must not block on a dialog.

| Tool | Input | Behavior |
|------|-------|----------|
| `get_state` | none | Returns project path (or null), modified flag, playhead position, duration, fps, playing flag. |
| `play` | `speed?` (number, default 1.0) | `MLT.play(speed)`. Error if nothing is loaded. |
| `pause` | `position?` (int frame) | `MLT.pause(position)`. |
| `seek` | `position` (int frame, required) | Clamped to valid range; uses the timeline-aware seek when a timeline project is open. |
| `open_project` | `path` (absolute), `discard_changes?` (bool, default false) | Fails with a clear message if the file does not exist, if the project is modified and `discard_changes` is not true, or if a stale auto-save file exists for `path`. With `discard_changes: true` the modified flag is cleared before opening so the unsaved-changes prompt does not appear. |
| `save_project` | `path?` (absolute) | Without `path`: saves to the current file via `MAIN.on_actionSave_triggered()`; fails if the project is untitled or the target is not writable (checked first, so the "unable to write" dialog never appears). With `path`: `path` must be absolute and end in `.mlt` (so a client cannot overwrite arbitrary files); writes a copy with `MAIN.saveXML(path)`; the current file name and modified flag are unchanged. |
| `list_actions` | none | Returns the allowed `Actions` keys with display name, enabled and checked state. |
| `trigger_action` | `key`, `checked?` (bool, for checkable actions) | Only keys on a fixed allowlist in `mcptools.cpp`. Unknown or non-allowlisted keys return an error. |

The `trigger_action` allowlist exists because many registered actions open
modal dialogs. Allowlist entries are chosen by auditing each action's slot, and
start small.

## 6. Lifecycle

- Constructed by `MainWindow`, parented to it; started when the setting is
  enabled, stopped when it is disabled or when the MainWindow is destroyed
  (the destructor stops it).
- Bind failure (port in use): show a status message and turn the setting off.
- The port is read from the settings file when the server starts; changing it
  requires toggling Enable off and on.

## 7. Verification

The repo has no C++ unit tests, so verification is by exercise:

1. `tools/mcp-smoke.sh` (curl): missing token, wrong token, `Origin` present,
   bad `Host`, oversized body, `GET /mcp`, `initialize`, `tools/list`, and one
   `tools/call` each for `get_state` and `seek`. The script lives in the repo
   under `tools/`.
2. Manual: connect Claude Code to the running build and call every stage 1 tool.
3. Existing JS tests still pass (`node src/qml/export-edl/test-node.js`,
   `node src/qml/export-chapters/test-node.js`), as a regression check only.

Build notes for this machine: MLT 7.36.1 is installed at `~/mlt-install`
(built without Movit, vid.stab, RtAudio, Rubberband, XINE, JACK Rack); configure
with `PKG_CONFIG_PATH=$HOME/mlt-install/lib/pkgconfig cmake --preset cc-debug-linux`.
ClangFormat 14 is not installed, so formatting is matched by hand.

## 8. Resolved decisions

1. `save_project` with a `path` writes a copy via `MAIN.saveXML(path)` and does
   not change the current file or modified flag (section 5).
2. Initial `trigger_action` allowlist, all player navigation actions whose
   slots open no dialog: `playerPlayPauseAction`, `playerPauseAction`,
   `playerRewindAction`, `playerFastForwardAction`, `playerSeekStartAction`,
   `playerSeekEndAction`, `playerNextFrameAction`, `playerPreviousFrameAction`,
   `playerForwardOneSecondAction`, `playerBackwardOneSecondAction`,
   `playerForwardFiveSecondsAction`, `playerBackwardFiveSecondsAction`,
   `playerForwardTenSecondsAction`, `playerBackwardTenSecondsAction`,
   `playerSkipNextAction`, `playerSkipPreviousAction`, `playerLoopAction`.
   Excluded: `playerSetJumpAction` and `playerSetPositionAction` (may open
   input dialogs) and `playerSetInAction`/`playerSetOutAction` (edits).
3. Supported MCP protocol versions: `2025-06-18`, `2025-03-26`, `2024-11-05`,
   plus any newer version found when the plan's verification step checks the
   current MCP specification.
4. The port is configurable through the settings file only (no UI field).
   The Settings menu offers: enable toggle, copy token, regenerate token.
