# Embedded MCP Server, Stage 2 — Design

Status: draft for review. Branch: `feature/embedded-control-server` (stage 1 is done and reviewed; see
`2026-10-01-mcp-control-server-design.md`, `docs/mcp-server.md`).

## 1. Goal

Let an MCP client (for example Claude Code) edit a video end to end in a running Shotcut: add clips to
the playlist, put them on the timeline, cut, trim, add transitions, SEE the result, and render an MP4.
Stage 2 extends the stage 1 server; transport, authentication and security (stage 1 spec section 3)
are unchanged.

### Success criteria

1. A client can, using only tools: add three media files to the playlist, put all playlist clips on the
   timeline, split one clip, add a transition between two clips, fetch a frame image at a chosen
   position, export an H.264/AAC MP4, and poll the export until it succeeds. The result plays and has
   the expected duration and codecs (checked with `ffprobe` in the headless smoke test).
2. Every editing tool is undoable with Shotcut's own Edit > Undo, and the `undo`/`redo` tools work.
3. A bad argument, a locked track, an invalid trim, an overlapping move, a missing file or a busy
   exporter returns a clear tool error and changes nothing. No tool ever opens a dialog.
4. `get_frame` returns a valid image the client can look at, including frames with timeline filters and
   transitions applied.
5. All stage 1 smoke checks still pass; the new checks run headless on an isolated `--appdata`.

### Non-goals

Filters other than fades, keyframes, titles/generators, subtitles, markers, copy/paste, track
add/remove/rename tools, audio-only editing, proxy management, batch export, two-pass or hardware
encoding, GPU-mode (Movit) rendering, export range selection, a separate "allow editing" toggle.

## 2. Architecture

Same `McpServer`; only the tool set grows. `src/mcptools.cpp` is split by area so no file grows past
what a reviewer can hold:

| File | Responsibility |
|------|----------------|
| `src/mcptools.cpp` | Registry (`McpTools::all()` concatenates the groups), stage 1 tools, result builders |
| `src/mcptoolsutil.h` | Shared helpers (argument parsing, `isLoaded()`, `timeline()`, track/clip JSON builders) |
| `src/mcptools_frame.cpp` | `get_frame` |
| `src/mcptools_timeline.cpp` | Playlist and timeline read/edit tools, `undo`, `redo` |
| `src/mcptools_export.cpp` | `export_video`, `get_export_status` |

`McpTools::imageResult(bytes, mimeType, text)` builds `{"content":[{"type":"image","data":<base64>,
"mimeType":...},{"type":"text","text":...}],"isError":false}`. The HTTP reply path already handles large
bodies (only requests are size-limited).

Changes to existing Shotcut code are limited to: small public accessors on `MainWindow`, a public
`EncodeDock::exportToFile()` and a quiet mode for `EncodeDock::createMeltJob()` (section 5).

## 3. Rules every editing tool follows

1. **Undoable only.** Edits push `Timeline::*Command` / `Playlist::*Command` on `MAIN.undoStack()`.
   They never call `MultitrackModel` mutators or the drag-gesture `TimelineDock::trimClipIn/Out` and
   `moveClip` overloads (those write to the model without a command or apply later through queued signals).
2. **Explicit addressing, no implicit UI state.** Tools take `(track, clip)` indexes as reported by
   `get_timeline`, or an absolute timeline frame. They do not depend on selection, current track,
   playhead or clipboard; where a Shotcut slot needs them the tool uses the command directly or sets
   them explicitly first.
3. **Indexes shift.** Every edit result includes the updated track summary (same shape as one `tracks[]`
   entry of `get_timeline`) so clients never guess. Track 0 is the TOPMOST video track; the tool
   descriptions and `get_timeline` say so, and `get_timeline` reports each track's `name` ("V1", "A1").
4. **No silent no-ops.** Locked tracks, blank/transition targets, invalid trims, overlapping moves and
   invalid transitions are validated first (`isTrackLocked`, `trimClipInValid/OutValid`,
   `addTransitionValid`, overlap checks) and return `isError`. Success is confirmed by comparing the
   track before and after where Shotcut's call returns `void`.
5. **No dialogs.** A tool must not call code that can show a modal dialog; where unavoidable it is
   pre-checked (as in stage 1) or given a quiet mode (export). The one known modal is the progress
   dialog `LongUiTask` inside `AppendCommand`; it spins the event loop but waits for no input, and the
   existing `m_busy` guard rejects other `tools/call` requests meanwhile. Tool calls are also refused while a user dialog is open (`QApplication::activeModalWidget()`), so a tool never runs inside the user's modal loop.
6. **Playback.** Edit tools pause the player first (as Shotcut's own "add to timeline" does).

## 4. Tools

All arguments are validated strictly (types, integer frame numbers >= 0, absolute paths). Frame numbers
are timeline frames unless stated. Results are compact JSON text unless stated.

### See

| Tool | Arguments | Behavior |
|------|-----------|----------|
| `get_frame` | `position?` (frame), `max_width?` (64-1920, default 640), `format?` (`jpeg` default, quality 80, or `png`) | Returns an image content item plus a text item `{position,width,height,format,proxy}`. Without `position`: the currently displayed frame via `VideoWidget::image()` (consumer output, so timeline filters and transitions are included; synchronous, no playhead move). With `position`: clamp to the last frame, pause, move the playhead like `seek`, then wait up to 3 s in a local event loop for the first displayed frame whose position equals the requested TIMELINE frame, re-pushing a consumer refresh every 200 ms (the display lags a seek by one frame and `requestImage()` is consumed by whichever frame shows next, so it can deliver a stale picture; a match shown before an edit made just before the call is not trusted either, so the third matching display, after refreshes issued by this call, is the one copied). On timeout return `isError` "frame not ready". Output size: height from the profile display aspect ratio (`MLT.profile().dar()`), scaled down to `max_width` with smooth scaling. `proxy:true` when the frame came from a proxy. Error when nothing is loaded. |

### Read

| Tool | Arguments | Result |
|------|-----------|--------|
| `get_playlist` | none | `{fps, items:[{index,name,path,in,out,duration}]}` from `MAIN.playlist()` and `clip_info(i)` (the real playlist order; `path` is the real source via `ProxyManager::resource`, made absolute against the project folder). `items` is empty when there is no playlist. |
| `get_timeline` | none | `{fps, position, duration, settings:{allow_transitions,ripple_all_tracks}, tracks:[{index,name,type:"video"\|"audio",locked,muted,hidden,clips:[{index,kind:"clip"\|"blank"\|"transition",start,duration,in,out,name,path,fade_in,fade_out}]}]}`. `in`/`out` are source frames, `out` inclusive, `duration = out-in+1`. `tracks` is empty when no timeline exists (`MAIN.isMultitrackValid()` false). |

### Playlist and timeline editing

| Tool | Arguments | Behavior |
|------|-----------|----------|
| `playlist_add` | `path` (absolute existing media file) | Opens the file as an MLT producer on the project profile, pushes `Playlist::AppendCommand`, returns the `get_playlist` result. Errors: not absolute / missing / not a file / MLT cannot open it / is a project file (`.mlt`/`.xml`, any case; use `open_project`) / is a generator or producer without a length. Media files and still images are accepted; a still image is added with Shotcut's default image duration. Resource (`:`-prefixed) paths are rejected as in stage 1. |
| `timeline_add_playlist` | `track?` (default: the lowest-numbered video track "V1", created if no timeline exists) | Copies every playlist item into a fresh `Mlt::Playlist`, pauses the player, and calls `TimelineDock::appendFromPlaylist(playlist, skipProxy=false, emptyTrack=false)` for that track. ALWAYS appends after existing clips on the track, as one undo step. Errors: empty playlist, locked track, unknown track. Returns `{appended:n, track:<summary>}`. Selection afterwards is whatever Shotcut selects (do not rely on it). |
| `timeline_split` | `track`, `position` (absolute frame, strictly inside a clip) | Resolves the clip with `clipIndexAtPosition`, errors for blank/transition/locked, calls `TimelineDock::split`. Returns the updated track. |
| `timeline_trim` | `track`, `clip`, `edge` (`in`\|`out`), exactly one of `frame` (absolute timeline frame the edge moves to) or `delta` (frames; for `in` positive moves the in-point later, for `out` positive shortens the clip), `ripple?` (default false) | Computes the delta from `frame`, validates with `trimClipInValid`/`trimClipOutValid`, pushes `TrimClipInCommand`/`TrimClipOutCommand` directly (they apply immediately). Errors for blank/transition/locked/invalid. Returns the updated track. |
| `timeline_remove` | `track`, `clip`, `ripple?` (default true) | `ripple:true` uses `TimelineDock::remove` (closes the gap, other tracks ripple per Shotcut's own ripple settings), `ripple:false` uses `lift` (leaves a blank). Refuses blanks and transition items. Success is confirmed by comparing clip counts. Returns the updated track. |
| `timeline_move` | `track`, `clip`, `to_start` (absolute frame), `to_track?` (default same track) | Builds `Timeline::MoveClipCommand` directly with `positionDelta = to_start - start`, `trackDelta`, `ripple=false`; a zero move is a no-op success. Rejects overlaps with a neighbour or transition, locked source or destination tracks, and video/audio track-type mismatch. Returns the affected track(s). |
| `timeline_add_transition` | `track`, `clip` (the INCOMING, later clip), `duration` (frames, >= 1 and shorter than both clips) | Requires the previous item to be an adjacent plain clip (blank-separated is allowed by Shotcut but refused here for clarity), same track, neither side a transition or adjustment clip. `position = start(clip) - duration`; validates with `MultitrackModel::addTransitionValid`, pushes `Timeline::AddTransitionCommand`. The transition is Shotcut's default dissolve (`luma`) with a `mix` audio cross-fade. The incoming clip must not already have a transition after it. Pushed with `ripple = true`, so the two clips overlap by `duration` and later clips move earlier by `duration` (the timeline gets shorter by that amount, no gap opens; other tracks ripple per Shotcut's own `timelineRippleAllTracks`, markers per `timelineRippleMarkers`). Returns the updated track and the transition's clip index. |
| `timeline_fade` | `track`, `clip`, `fade_in?`, `fade_out?` (frames; 0 removes; at least one required) | Pushes the fade commands through `TimelineDock::fadeIn/fadeOut` after validating clip kind and lock. One call is one undo step (both fades in one macro). Returns the updated track. |
| `undo`, `redo` | none | `MAIN.undoStack()->undo()/redo()`. Returns `{text, can_undo, can_redo}`; error if there is nothing to undo/redo. Note this also undoes edits the user made in the GUI. |

### Export

| Tool | Arguments | Behavior |
|------|-----------|----------|
| `export_video` | `path` (absolute), `preset?` (default `x264-medium`), `overwrite?` (default false) | Starts a background render and returns immediately `{job_id, output, preset}`. Source: the timeline if one exists, else the current playlist bin (Shotcut's "Current Playlist Bin"; with a bin selected only that bin exports), else the loaded clip. Preconditions, each a tool error: something loaded; no export already running; absolute path; extension matches the preset's `meta.preset.extension`; parent folder exists and is writable; free space at least 1 GiB; file does not exist unless `overwrite:true`; output path not a symbolic link or hard link, and not media used by the project (checked by path and by file identity, also in quiet-mode `createMeltJob`) or in the autosave folder; the preset exists; the melt binary (`melt-7` on Linux, `melt` on Windows/macOS) exists where `MeltJob` looks for it (error text says how to fix it). Uses `EncodeDock::exportToFile` (section 5). |
| `get_export_status` | `job_id?` (default: the latest export started through MCP) | `{job_id, state:"pending"\|"running"\|"paused"\|"succeeded"\|"failed"\|"stopped"\|"removed"\|"idle", percent, output, output_exists, output_bytes, exit_code, elapsed_ms, error}` where `removed` means the job was taken out of the Jobs dock before it finished, `output_exists`/`output_bytes` describe the file on disk now, `elapsed_ms` is the run time so far (the total once finished) and `error` is the last ~2 KB of the job log when failed. State is captured in a small struct from the job's `progressUpdated`/`finished` signals (idempotent; `MeltJob` can emit `finished` twice), so it survives the Jobs dock removing finished jobs; the live `AbstractJob` is held in a `QPointer`. Job ids are an increasing counter owned by the tool module, never queue row numbers. |

## 5. Export design (the only non-additive change)

- Add a public `bool EncodeDock::exportToFile(const QString &presetName, const QString &path,
  QString &error)`. It checks the preconditions that need dock state, calls `resetOptions()`, loads the
  preset into the widgets the way `on_presetsTree_clicked` does (stock presets from the MLT repository
  `consumer/avformat/<name>`, custom presets from the app data `presets/encode` folder), selects the
  source ("timeline" / "playlist", meaning the current playlist bin / "clip") on the "export from" combo, then builds the job with
  `createMeltJob(...)` and `JOBS.add(job)`.
- `createMeltJob` gains a quiet mode (a parameter or an error-string out-parameter) so its
  `QMessageBox` calls (duplicate job target, target inside the project, not writable) become returned
  errors instead of modals; default behavior for the GUI is unchanged.
- The tool does none of: file dialog, low-disk Yes/No prompt, missing-files dialog, "analysis needed"
  prompt. It replaces them with explicit checks (writable folder, free space, no overwrite, the target
  not in the project) and documents that missing source files fail the render with a log message.
- `MainWindow` gets a small public forwarder (or an `encodeDock()` accessor); `m_encodeDock` stays
  private.
- Rendering runs through the existing `MeltJob` (`melt-7`, `-progress2`), serially in the job queue, with
  the Jobs dock showing the row. Preset values for stock `x264-medium`: `f=mp4`, `vcodec=libx264`,
  `acodec=aac`, `crf=18`, `ab=256k`, `preset=medium`, `vprofile=high`, `movflags=+faststart`; it
  inherits the project's resolution and frame rate.
- **Dev-tree melt:** `MeltJob` runs `<application dir>/melt-7`, which does not exist in the dev build
  tree (`build/cc-debug-linux/src/`). Stage 2 does not change `MeltJob`; the dev setup step is
  `ln -s ~/mlt-install/bin/melt-7 build/cc-debug-linux/src/melt-7`, documented in
  `docs/mcp-server.md`, and the tool checks for the binary and says so.

## 6. Security and safety

Unchanged transport and auth. New capabilities and their limits: tools modify the open project (all
undoable) and `export_video` writes one file: the path must be absolute, extension-matched to the
preset, must not already exist unless `overwrite:true`, and cannot be inside the project. No tool
deletes files. A client that holds the token can therefore change the open project and create files in
folders the user can write; this is the same trust the token already grants (open/save files) and is
stated in `docs/mcp-server.md`. There is deliberately no second toggle (resolved decision 3).

## 7. Verification

The smoke script grows (headless, isolated `--appdata`, port 18999). Fixtures: three short ffmpeg
`testsrc` clips of different lengths/colors, with audio (a sine) so audio paths are exercised.

1. Per-tool argument/state/lock/validation errors and success results.
2. The chain: `playlist_add` x3 -> `get_playlist` (3 items) -> `timeline_add_playlist` ->
   `get_timeline` (clips in order, starts contiguous, total duration = sum) -> `timeline_split`
   (clip count +1, durations add up) -> `timeline_trim` -> `timeline_add_transition` (overlap, shorter
   duration) -> `timeline_fade` -> `timeline_move` -> `undo` x N back to the post-add state and `redo`.
3. `get_frame`: the result decodes (JPEG magic bytes, requested width, aspect ratio from the profile);
   frames at two different positions differ (colors from different clips); PNG option works.
4. Export: `export_video` -> poll `get_export_status` until `succeeded` -> `ffprobe` shows h264 + aac,
   duration within a tolerance of the timeline duration, size > 0; error cases (existing file without
   `overwrite`, unwritable folder, unknown preset, nothing loaded, second export while running, wrong
   extension, missing melt binary).
5. No dialog appears and no request hangs (every call returns within a bounded time).

Human-only checks (listed in the docs): playing the exported file, visual fidelity versus Shotcut's own
export, GPU mode, a project with proxies, the Jobs dock row, and that the GUI export dialog still works
unchanged after the `createMeltJob` quiet-mode change.

## 8. Resolved decisions and known risks

Decisions (user-approved): 1) `get_frame` is JPEG 640 px wide by default, PNG optional. 2)
`timeline_add_playlist` appends to the end of the chosen track, default V1. 3) No separate
editing/export toggle. 4) Export extension must match the preset, paths absolute. 5) `playlist_add` is
included so playlists can be built (and tested) through the tools.

Known risks to resolve during implementation (each has a test or a fallback in the plan):
- Whether MLT's `mix` needs spare source frames beyond the cut for a transition (unverified; the
  transition tests use real clip pairs and the tool returns an error if Shotcut rejects the transition).
- The frame right after a seek may still be the old one (handled by the wait/timeout in `get_frame`).
- GPU/Movit frames and proxy frames in `get_frame` (reported via `proxy`; GPU mode is a non-goal).
- `moveClip`'s signal-based path and `MoveClipCommand` behavior on overlap were read, not run; the
  tool refuses overlaps rather than relying on it.
- Export under xvfb relies on the CPU path (`Settings.playerGPU()` off).

## 9. Build order (informative; the plan fixes it)

1. Split `mcptools.cpp`, shared helpers, `imageResult` (no behavior change; stage 1 checks stay green).
2. `get_frame`.
3. `get_playlist`, `get_timeline`, `playlist_add`.
4. `timeline_add_playlist`, `timeline_split`, `timeline_remove`.
5. `timeline_trim`, `timeline_move`, `undo`, `redo`.
6. `timeline_add_transition`, `timeline_fade`.
7. Export: `EncodeDock::exportToFile`, quiet mode, tools, dev-tree melt note.
8. Docs and the end-to-end smoke chain.
