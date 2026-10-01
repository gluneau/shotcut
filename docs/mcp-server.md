# MCP server

Shotcut can run a local [Model Context Protocol](https://modelcontextprotocol.io) server so an
MCP client (for example Claude Code) can control it. It is off by default.

## Enable

1. Settings > MCP Server > Enable. The server listens on `127.0.0.1:7468` only.
2. Settings > MCP Server > Copy Token to Clipboard.
3. Connect a client, for example:

       claude mcp add --transport http shotcut http://127.0.0.1:7468/mcp \
         --header "Authorization: Bearer <token>"

The port is read from `mcpServer/port` (1024-65535, default 7468) in Shotcut's settings file when
the server starts, so toggle Enable off and on after changing it. An invalid value silently falls
back to 7468. "Regenerate Token" invalidates
the old token immediately.

If the port is busy at startup or when you tick Enable, Shotcut shows a status message, unchecks
the menu item and keeps the saved setting, so it retries on the next launch. To stop retrying, free
the port or change `mcpServer/port`.

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

## Stage 2: seeing, editing and exporting

Stage 2 adds 15 tools (23 in all) so a client can build a playlist, edit the timeline, look at the
result and render it.

| Tool | What it does | Key arguments |
|------|--------------|---------------|
| `get_frame` | Returns an image of a frame | `position?`, `max_width?` (64-1920, default 640), `format?` (`jpeg` default, `png`) |
| `get_playlist` | Lists playlist items | none |
| `get_timeline` | Lists tracks and their clips, blanks and transitions | none |
| `playlist_add` | Appends a media file to the playlist | `path` |
| `timeline_add_playlist` | Appends all playlist items to a video track | `track?` (default V1) |
| `timeline_split` | Splits a clip | `track`, `position` (strictly inside the clip) |
| `timeline_remove` | Removes a clip (ripple) or lifts it (leaves a blank) | `track`, `clip`, `ripple?` (default true) |
| `timeline_trim` | Moves a clip's in or out edge | `track`, `clip`, `edge`, one of `frame` or `delta`, `ripple?` (default false) |
| `timeline_move` | Moves a clip to a start frame, optionally to another track | `track`, `clip`, `to_start`, `to_track?` |
| `timeline_add_transition` | Adds Shotcut's default dissolve between two touching clips | `track`, `clip` (the incoming, later clip), `duration` |
| `timeline_fade` | Sets a clip's fade-in and/or fade-out length (0 removes) | `track`, `clip`, `fade_in?`, `fade_out?` |
| `undo`, `redo` | Undo or redo the last edit in Shotcut's history | none |
| `export_video` | Renders the project to a video file as a background job | `path`, `preset?`, `overwrite?` |
| `get_export_status` | Progress and result of an export | `job_id?` (default: the latest) |

Positions and `to_start` are absolute timeline frames; `frame` in `timeline_trim` is the absolute
timeline frame the edge moves to. Tool arguments are validated strictly and no stage 2 tool
opens a dialog.

### Editing rules

- Every edit is undoable with Shotcut's own Edit > Undo and with the `undo`/`redo` tools. These
  also undo and redo edits made in the Shotcut window.
- Indexes shift after a split, remove or transition. Every edit returns the updated track (same
  shape as one `tracks[]` entry of `get_timeline`), so use that instead of guessing.
- Track 0 is the TOPMOST video track; `get_timeline` reports each track's `name` ("V1", "A1").
- These are refused with an error and change nothing: locked tracks; blank or transition items
  where a clip is needed; invalid trims (including an extension longer than the neighbouring
  gap); overlapping moves; moving a clip that is joined to a transition; a transition whose
  incoming clip follows a blank, a transition or nothing, or already has a transition after it.
- `playlist_add` accepts media files and still images (added with Shotcut's default image
  duration), not project files (use `open_project`) or generators. With nothing loaded, the first
  file also becomes the loaded clip, as in Shotcut's Add Files. When the window has unsaved
  changes and nothing is loaded it returns an error instead of asking in a dialog.
- `timeline_add_playlist` appends all playlist items after the existing clips of the track in one
  undo step. The default track is V1, and the timeline is created if there is none.
- Each `timeline_trim` is one undo step.
- `timeline_add_transition` uses Shotcut's default dissolve with an audio cross-fade, with
  ripple: the clips overlap by `duration`, so later clips on the track move earlier by `duration`
  and the timeline gets shorter. Other tracks ripple according to Shotcut's own ripple setting.
- `timeline_fade` sets both fades in one undo step; a fade cannot be longer than the clip.

### get_frame

Returns an image content item (JPEG, 640 px wide by default, or PNG) plus a text item with
`position`, `width`, `height`, `format` and `proxy`. Images are never upscaled. Without `position`
it returns the frame shown now. With `position` it pauses, moves the playhead like `seek` (clamped
to the last frame) and waits for the displayed frame at that timeline frame, up to 3 s; on timeout
it returns an error rather than a wrong picture.

The width and height follow the player: with the player's preview scale or in GPU mode the frame
can be smaller than the project resolution, and `width`/`height` report what you got. Known limit:
the settling rule waits for a few displayed frames (more when Player > Realtime is off), so a stale
picture is theoretically possible with Realtime off or a very loaded GUI.

### Export

`export_video` renders the timeline (else the current playlist bin, else the loaded clip) as a
background job that shows as a row in the Jobs dock, and returns `{job_id, output, preset}` at once. The
preset defaults to `x264-medium` (H.264 + AAC, `.mp4`, the project's resolution and frame rate).
It can be any stock preset name, or a custom preset by plain file name from
`<appdata>/presets/encode`; paths are not accepted. Preconditions, each a tool error:

- something is loaded and no export is already running (one at a time);
- `path` is absolute, normalized (no `./`, `../`, doubled slashes) and not a symbolic link or
  directory, and its extension matches the preset's;
- the folder exists and is writable, with at least 1 GiB free;
- the file does not exist, unless `overwrite: true`;
- the melt program exists next to the Shotcut binary. It is named `melt-7` on Linux and `melt` on
  Windows and macOS. In a Linux dev tree link it in (adjust to your MLT prefix):

      ln -s <mlt prefix>/bin/melt-7 build/cc-debug-linux/src/melt-7

Safety: an export never overwrites media used by the project: timeline clips, playlist items, the
loaded clip and the project file, checked by path and by file identity. A hard-linked target and
anything in the autosave folder are also refused. The guard has a blind spot: files referenced only
inside filters or nested project clips are not seen, so do not pass `overwrite: true` for a file
you did not create.

Side effects: the preset overwrites the Export panel's settings and its "export from" choice
(the panel's hardware-encoder and proxy/preview-scale choices may still apply). The export skips
the missing-files check and the analysis prompts, so analysis filters are not run. Dual-pass
presets are not honored (the render is single pass). Parallel processing follows the Export
panel's setting.

`get_export_status` returns `{job_id, state, percent, output, output_exists, output_bytes,
exit_code, elapsed_ms, error}`. `state` is one of `pending`, `running`, `paused`, `succeeded`,
`failed`, `stopped`, `removed` (taken out of the Jobs dock before it finished) or `idle` (no
export started yet). `output_exists` says whether the output file is on disk now, `output_bytes`
is its size (0 if absent) and `elapsed_ms` is the run time so far, or the total once the job has
finished. `exit_code` is the raw process code; a render that reached 99% or more counts as
succeeded. `error` is the end of the render log when it failed.

## Security

Off by default; loopback only; every request needs the bearer token; requests carrying an `Origin`
header or a foreign `Host` header are rejected, which blocks web pages and DNS rebinding. The token
is stored unencrypted in the settings file.

There is no connection cap and no idle timeout after a reply, so each pending connection can hold
up to about 1 MB for up to 10 s. That is acceptable because the server is loopback only and
token-gated.

Stage 2 widens what the token allows. It can now edit the open project (every edit is undoable)
and write one video file to any folder the user can write, never over media used by the project
(see Export, with the caveat described there). `export_video`'s `preset` can only name a stock
preset or a plain custom preset file, not a path. Tool calls are refused while a Shotcut dialog is
open (close it and try again).

## Testing

    MCP_TOKEN=<token> tools/mcp-smoke.sh

Needs curl and jq, plus ffmpeg for the media checks; takes roughly 25 s. The test opens and replaces
whatever is loaded in the target Shotcut, so run it against a throwaway session (the headless
recipe below); it aborts if the project has unsaved changes unless `MCP_SMOKE_FORCE=1` is set.
Shotcut can be run
headless for it: pre-seed `<scratch dir>/shotcut.ini` with a `[mcpServer]` section containing
`enabled=true` and a `token=` value, then start
`xvfb-run -a build/cc-debug-linux/src/shotcut --appdata <scratch dir>`.
To avoid the default port also add `port=<test port>` (for example 18999) and pass
`MCP_PORT=<test port>` to the scripts.

Stage 2 has its own script:

    MCP_TOKEN=<token> [MCP_PORT=<port>] [MCP_APPDATA=<scratch dir>] tools/mcp-smoke-edit.sh

It needs bash, curl, jq, ffmpeg, ffprobe, xxd, md5sum and base64, and a FRESH throwaway Shotcut:
it adds clips, edits the timeline, exports a video, and creates symlinks and hard links in its
temp dir. It aborts unless the session is fresh (nothing loaded, not modified), or
`MCP_SMOKE_FORCE=1` is set. The export checks need `melt-7` (see Export). Set `MCP_APPDATA` to the
`--appdata` directory of the Shotcut under test: the script then writes a deliberately slow custom
preset there and uses it for deterministic checks of "a second export is refused while one runs"
and "a custom preset is applied". Without it those two checks race the fast render and can flake.
Both scripts must pass: every check prints PASS and the script ends with "All checks passed."
(exit code 0).

Manual checks (they need a modified project, which the tools cannot create):

- With an edited project loaded, wait for the first autosave tick (the file under
  `<appdata>/autosave` only exists after it), then call `open_project` with `discard_changes: true` on a broken `.mlt`.
  It must error, the project must stay modified, and its file under `<appdata>/autosave` must still
  exist and keep being updated.
- Open a media file into a loaded timeline: no discard is needed and the modified flag is unchanged.

Manual checks for stage 2 (GUI only; the headless script cannot cover them):

- Play the exported file, and compare it visually with Shotcut's own export of the same project.
- Lock a track in the GUI, then call each edit tool on it: all must refuse.
- Open a project with proxies, then call `get_frame` and `export_video`.
- The Jobs dock shows a sensible row for an MCP export.
- Edit > Undo shows sensible names for the tool-made edits.
- The GUI export dialog still works unchanged.
- GPU mode: `get_frame` and `export_video` (not tested headless).
- A timeline with several video tracks: index 0 is the topmost.
- `get_frame` while playing, and with a reduced preview scale.
- Remove a running MCP export job from the Jobs dock: `get_export_status` reports `removed`.
- Rename or remove the dev-tree `melt-7` link: `export_video` reports a clear error (or a failed
  job with the start-failure text).
- With a dialog open (for example Settings), a tool call returns "A dialog is open in Shotcut".

## Known limits

Not covered by the tools or not tested headless: locked tracks (refused, tested by hand only), GPU
mode, proxies, generators and titles, filters other than fades, markers, subtitles, two-pass
encoding and export ranges.
