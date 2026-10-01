# MCP server

Shotcut can run a local [Model Context Protocol](https://modelcontextprotocol.io) server so an
MCP client (for example Claude Code) can control it. It is off by default.

## Enable

1. Settings > MCP Server > Enable. The server listens on `127.0.0.1:8765` only.
2. Settings > MCP Server > Copy Token to Clipboard.
3. Connect a client, for example:

       claude mcp add --transport http shotcut http://127.0.0.1:8765/mcp \
         --header "Authorization: Bearer <token>"

The port is read from `mcpServer/port` (1024-65535, default 8765) in Shotcut's settings file when
the server starts, so toggle Enable off and on after changing it. An invalid value silently falls
back to 8765. "Regenerate Token" invalidates
the old token immediately.

The server speaks MCP over HTTP with a handshake: `initialize` is required first. Supported
protocol versions are 2025-11-25, 2025-06-18, 2025-03-26 and 2024-11-05. The stateless 2026-07-28
revision is not supported.

## Tools

| Tool | What it does |
|------|--------------|
| `get_state` | Project path, modified flag, loaded/playing, position, duration, fps |
| `open_project` | Open a project or media file (absolute path of an existing file) |
| `save_project` | Save the project, or write a copy |
| `play`, `pause`, `seek` | Transport control (frame numbers) |
| `list_actions`, `trigger_action` | List and trigger a fixed allowlist of 17 player actions |

`open_project` refuses if the project has unsaved changes unless `discard_changes` is true, and
refuses if an auto-save file exists for the project (`.mlt`/`.xml` project files only).
`discard_changes` applies only when a project file (`.mlt`/`.xml`, case-sensitive) is opened or no
project is loaded; opening a media file while a timeline or playlist project is loaded neither
needs it nor touches the project or its modified flag. It returns an error (and restores the modified
flag) if Shotcut did not actually open the file. Rare project-repair or compatibility dialogs inside
Shotcut's open code can still appear for unusual project files and block the request until they
are answered; call `get_state` to confirm the result.

`save_project` without `path` saves to the current file (error if the project is untitled; Shotcut
may show its own save-error dialog if the write itself fails). With `path` (absolute, must end in
`.mlt`) it writes a copy and leaves the current file and the modified flag unchanged.

`seek` is clamped to the last frame (the timeline length when a timeline project is open). Whether
playback continues after a seek follows Shotcut's "pause after seek" player setting; `pause` always
stops playback.

`trigger_action` accepts only these player actions (also returned by `list_actions`):
`playerPlayPauseAction`, `playerPauseAction`, `playerRewindAction`, `playerFastForwardAction`,
`playerSeekStartAction`, `playerSeekEndAction`, `playerNextFrameAction`,
`playerPreviousFrameAction`, `playerForwardOneSecondAction`, `playerBackwardOneSecondAction`,
`playerForwardFiveSecondsAction`, `playerBackwardFiveSecondsAction`,
`playerForwardTenSecondsAction`, `playerBackwardTenSecondsAction`, `playerSkipNextAction`,
`playerSkipPreviousAction`, `playerLoopAction`.

## Security

Off by default; loopback only; every request needs the bearer token; requests carrying an `Origin`
header or a foreign `Host` header are rejected, which blocks web pages and DNS rebinding. The token
is stored unencrypted in the settings file.

## Testing

    MCP_TOKEN=<token> tools/mcp-smoke.sh

Needs curl and jq, plus ffmpeg for the media checks; takes roughly 25 s. The test opens and replaces
whatever is loaded in the target Shotcut, so run it against a throwaway session (the headless
recipe below); it aborts if the project has unsaved changes unless `MCP_SMOKE_FORCE=1` is set.
Shotcut can be run
headless for it: pre-seed `<scratch dir>/shotcut.ini` with a `[mcpServer]` section containing
`enabled=true` and a `token=` value, then start
`xvfb-run -a build/cc-debug-linux/src/shotcut --appdata <scratch dir>`.

Manual checks (they need a modified project, which the tools cannot create):

- With an edited project loaded, call `open_project` with `discard_changes: true` on a broken `.mlt`.
  It must error, the project must stay modified, and its file under `<appdata>/autosave` must still
  exist and keep being updated.
- Open a media file into a loaded timeline: no discard is needed and the modified flag is unchanged.
