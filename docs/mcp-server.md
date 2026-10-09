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

## Stage 3A: perception and structure

Stage 3A adds 15 tools (38 in all) so a client can hear and read footage, look at the whole
timeline at once and cut it by frame ranges, and manage tracks and markers.

| Tool | What it does | Key arguments |
|------|--------------|---------------|
| `get_job` | Status and result of a background analysis job | `job_id` |
| `analyze_media` | Probes a media file with ffprobe | `path` |
| `detect_silence` | Silent and spoken stretches of a source's audio | source, `noise_db?`, `min_s?`, `wait_ms?` |
| `audio_levels` | Loudness (LUFS, LRA, true peak) and per-slice RMS/peak levels | source, `buckets?`, `wait_ms?` |
| `detect_scenes` | Scene changes (cuts) in a source's video | source, `threshold?`, `wait_ms?` |
| `transcribe` | Speech to text with word times (Whisper), or stores a supplied transcript | source, `language?`, `wait_ms?`; or `path` + `transcript` |
| `get_overview` | One contact-sheet image of the whole timeline | `columns?`, `rows?`, `tile_width?`, `format?` |
| `timeline_insert_range` | Puts a range of a file on a track at a frame (insert or overwrite) | `path` or `playlist_index`, `in_s`, `out_s`, `track`, `position`, `mode?` |
| `timeline_remove_ranges` | Cuts the same frame ranges out of several tracks | `tracks`, `ranges`, `ripple?` (default true) |
| `track_add` | Adds an empty video or audio track | `type`, `name?` |
| `track_remove` | Removes a track | `track`, `force?` |
| `track_set` | Sets name, mute, lock, hide, composite, gain_db | `track` plus at least one property |
| `marker_add` | Adds a point or range marker | `start`, `end?`, `text`, `color?` |
| `marker_list` | Lists markers in index order | none |
| `marker_remove` | Removes a marker | `index` |

"Source" means exactly one of `path` (absolute path of an existing file) or `track` and `clip` (a
timeline clip: only its trimmed range is analysed; a locked track is fine). All result times are
seconds in the SOURCE file. For a clip source, results also carry `timeline_frames`: timeline
frame numbers (`[{start, end}]` with `end` exclusive, or a single frame for `detect_scenes`),
empty or omitted when a time does not map onto the clip, and always for a clip with a speed
change.

### Background jobs

The analysis tools run external programs in the background. `wait_ms` (integer 0 to 20000,
default 0) makes the call wait for the result; without it, or when the job is slower, the call
returns `{job_id, state}` at once. Poll it with `get_job {job_id}`, which returns `{job_id, kind,
state, percent?, result?, error?, elapsed_ms}`; `state` is `running`, `succeeded`, `failed` or
`cancelled`, `result` is present once it succeeded and `error` (with the end of the program's
output) once it failed. An identical call while the first still runs returns the same job. Jobs
live in memory until Shotcut quits, also after the project is closed; beyond 200 jobs the
oldest FINISHED ones are dropped (running jobs are never dropped, and at most 4 run). A job is
killed after 600 s (`transcribe`: 600 s plus the length of the audio, at most 4 h).
`analyze_media` always waits up to 5 s. The other analysis tools probe the source with ffprobe
first (the same probe as `analyze_media`, cached per file), so the first call on a file may take
up to 10 s while ffprobe runs, even with `wait_ms` 0.

At most 4 jobs run at once, and at most one of them is a `transcribe`: a call that would start
another returns the error "Too many analysis jobs are running; wait for one with get_job." (or "A
transcription is already running; wait for it with get_job."). A cached result and a call
identical to a running job (which returns that job) are never refused.

Programs are looked up in Shotcut's application folder first, then on `PATH`: `ffmpeg` and
`ffprobe` (both ship with Shotcut). A missing program is a tool error. Arguments are passed as a
list, never through a shell. Temporary files (`transcribe`'s decoded audio) are deleted when the
job ends.

Results are cached in memory, keyed by the file's path, size and modification time, the tool and
its parameters (and for a clip, its position and trim, since `timeline_frames` depend on them). A
cached result comes back at once with `"cached": true`. The cache keeps the 200 most recent
results, has no size budget and is lost when Shotcut quits. Editing a file in place without
changing its size or modification time is not noticed.

### Analysis tools

- `analyze_media {path}` returns `{path, duration_s, size_bytes, video, audio}`: `video` is
  `{width, height, fps, codec, rotation, pix_fmt}` or null, `audio` is `{channels, sample_rate,
  codec}` or null; first video and first audio stream only; `rotation` is degrees as ffprobe
  reports it (may be negative). `duration_s` is null when unknown. Errors: relative or missing
  path, a directory, a Qt resource path, a file that is not media (the job fails). Stage 3A also
  adds `width`, `height`, `has_audio`, `rotation` and `fps` of the source to clips in
  `get_playlist` and `get_timeline` (omitted when unknown).
- `detect_silence` (`noise_db` -90 to -10, default -35; `min_s` 0.05 to 30, default 0.4)
  returns `{path, in_s, out_s, ranges, speech_ranges, total_silence_s}`; `ranges` are the silent
  stretches and `speech_ranges` the rest of `[in_s, out_s]`, each `{start_s, end_s,
  duration_s, timeline_frames?}`. Boundaries are approximate (about 0.1 s). A silence running
  to the end is closed at `out_s`. A source without audio is an error.
- `audio_levels` (`buckets` 1 to 600, default 60) returns `{path, in_s, out_s, integrated_lufs,
  loudness_range_lu, true_peak_dbtp, buckets:[{start_s, end_s, rms_db, peak_db}]}`. Levels are
  dBFS with a floor of -120; `integrated_lufs` reads -70 for silence (ebur128's floor). A source
  without audio, or longer than 7200 s, is an error (analyse a clip range instead).
- `detect_scenes` (`threshold` 0.05 to 0.9, default 0.3) returns `{path, in_s, out_s,
  scenes:[{time_s, score, timeline_frames?}]}`; `time_s` is the first frame of the new scene,
  `score` (0 to 1, 3 decimals) how different it is from the frame before. A hard cut scores
  about 1, a change between similar pictures less (the fixture's red-to-green cut scores 0.64),
  so a high threshold misses soft cuts. A cut on the very first frame of the analysed range is
  never reported (there is no frame before it). A source without video is an error.
- `get_overview` (`columns` and `rows` 1 to 8, default 4; `tile_width` 96 to 640, default 320;
  `format` `jpeg` or `png`) returns one image of `columns*rows` frames evenly spaced from frame
  0 to the last frame, each labelled with its timecode and frame number, plus `{columns, rows,
  tile_width, positions}` with the values actually used. Limits: a timeline with fewer frames
  than tiles gets one tile per frame; `tile_width` is reduced (not below 96) so neither side of
  the image exceeds 4096 px; every tile waits at most 3 s for its frame and the whole sheet at
  most 45 s, else an error. The playhead is put back and playback resumes; a source clip shown
  in the player is replaced by the timeline. Refused with no timeline, an empty timeline, or
  while an export started with `export_video` runs; stopped with an error if the timeline
  changes meanwhile.

### transcribe and Whisper

`transcribe` runs the Whisper.cpp program and model configured in Shotcut: Settings are in the
Subtitles panel > Speech to Text... > Configuration (the `whisper-cli` executable and a GGML model
file; nothing is downloaded). When either is missing the call returns "Whisper is not
configured" at once, without starting a job. The audio is decoded with ffmpeg to 16 kHz mono and
passed to `whisper-cli ... -ojf`; GPU use follows the dialog's Use GPU option, and a failed run is
not retried (the error carries the program's message and, with the GPU on, a hint to turn it
off). `language` is `auto` (default) or a Whisper code (`en`, `fr`, ...; also `haw`, `yue`);
whisper-cli itself rejects unknown codes.

The result is `{path, in_s, out_s, language, granularity, segments:[{start_s, end_s, text,
words:[{start_s, end_s, text}], timeline_frames?}]}`. `granularity` is `word`, or `segment` when
Whisper gave no word times. Words lie inside their segment, in order, and may have `start_s ==
end_s`. Punctuation tokens (ASCII, and Unicode punctuation such as an ellipsis or an em dash)
join a word's text without extending its end time. A segment that has no length once clamped to
the analysed range is dropped. Audio without speech gives `segments: []`.

Bring your own transcript instead: `transcribe {path, transcript}` (nothing else) stores
`transcript` = `{segments:[{start_s, end_s, text, words?}]}` for that file: 1 to 5000 segments
sorted by `start_s`, `0 <= start_s < end_s <= 86400`, non-empty text of at most 2000
characters (Unicode code points, so an emoji counts once), words (at most 2000 per segment)
inside their segment and in order. It is returned with `"supplied": true` by later `transcribe
{path}` calls and wins over Whisper results. A track and clip call always runs Whisper. Supplied
transcripts are never evicted, but at most 200 files may hold one: storing for a 201st file is an
error, replacing the transcript of a file that has one is allowed. The last 50 Whisper results
are also kept per file for later tools.

### Structure tools

- `timeline_insert_range` puts `[in_s, out_s)` of a media file (`path`) or of a playlist item
  (`playlist_index`, whose own in/out is the default range; its filters are not carried over and
  an item with a speed change is refused) on `track` at the absolute frame `position`, as one
  undo step, and returns `{track, inserted:{start, duration}, ripple:{all_tracks, markers}}`.
  `mode` `insert` (default) splits a clip at `position` and pushes later items right; whether
  other tracks and markers also move follows Shotcut's own ripple settings, reported in
  `ripple` and in `get_timeline`'s `settings`. `overwrite` replaces what is under the range and
  never ripples. With no timeline, `track` must be 0 and the first video track is created, plus
  an audio track A1 when Shotcut's Timeline > Automatically Add Tracks setting is on, as
  `timeline_add_playlist` does; both are part of the same undo step. Refused:
  locked tracks, still images, generators, project files, a range beyond the file or shorter
  than one frame, a position more than one hour after the end of the track, a position inside a
  transition and an overwrite covering part of one. Inserting exactly at a transition's start
  puts the new clip before the transition, which then plays after the new clip instead of at the
  end of the previous clip; inserting exactly at its end puts the new clip between the
  transition and the next clip. An overwrite covering a whole transition replaces it.
- `timeline_remove_ranges` cuts the same `ranges` (absolute frames `{start, end}`, `end`
  exclusive, up to 500) out of every listed track (1 to 8) as one undo step, and returns
  `{tracks, removed_frames, ranges_applied, ripple}`. Ranges are sorted, overlapping or touching
  ones merged, clamped to the end of the longest listed track and dropped past it. With `ripple`
  true (default) each range is closed on every listed track, gaps included, so the tracks stay
  aligned; unlisted tracks are never touched, whatever Shotcut's ripple-all-tracks setting is
  (the tool turns it off while it works and restores it). Markers follow Shotcut's
  ripple-markers setting: markers inside a range are deleted and later ones shift once, with the
  track that loses the most. With `ripple` false the clips are lifted and gaps remain. A range
  that overlaps or touches a transition is refused before anything changes (keep a one-frame
  margin).

  Work cap: at most 120 cuts per call (ranges after merging times listed tracks, for example 60
  ranges on 2 tracks); more is refused up front with nothing changed. Every split and remove
  makes Shotcut snapshot the whole track for undo and a split reopens the media, so the cost
  grows faster than the number of cuts (on a debug build 120 cuts take about 12 s and memory
  grows by roughly 0.6 GB). 120 is the previous cap, re-measured against the undo-snapshot race
  described below with the preview guard in place (see there for the numbers). For more ranges
  send chunks from the END of the timeline to the start, so ripple never shifts a range you have
  not sent yet (or re-read `get_timeline` between chunks); each call is one undo step, and
  `save_project` first is a good idea.
- `track_add {type, name?}` adds an empty track as one undo step and returns `{tracks, added}`.
  A video track goes on top (index 0, so every index shifts by one), an audio track at the
  bottom. A track head selected in the timeline is deselected, as Shotcut's own Add Track does.
- `track_remove {track, force?}` removes a track as one undo step; a track holding clips needs
  `force: true`. Undo restores its clips, filters, name, mute, hide, gain and compositing on/off
  (not a custom blend mode). The bottom video track is refused: a timeline needs one, and
  Shotcut's own Remove Track, when undone with other video tracks present, reconnects their
  blend transitions to the wrong tracks. Remove its clips with `timeline_remove_ranges` instead.
- `track_set` sets `name`, `mute`, `lock`, `hide` and `composite` (video only; the bottom video
  track cannot composite) and `gain_db` (-70 to 24, any track) as one undo step. Values are set,
  not toggled; a call that changes nothing adds no undo step. A locked track only accepts
  `lock: false` (optionally with other properties, applied after unlocking). A gain with
  keyframes is refused.
- Names (tracks, 1 to 64) and marker texts (1 to 200) count characters as Unicode code points
  and must not be blank.
- `marker_add {start, end?, text, color?}` adds a point or range marker (inclusive frames as in
  Shotcut; `start` and `end` at most one hour of frames after the end of the timeline; `color`
  `#rrggbb`) and returns `{index, marker, markers_count}`. Markers are listed by `marker_list`
  in the order they were added, not by time. `marker_remove {index}` removes one; later indexes
  shift down.

`get_timeline` tracks now also report `gain_db` and, for video tracks, `composite`.

### Known upstream issue: undo snapshots can be corrupted

Two related Shotcut/MLT thread-safety problems were found while testing; the MCP server works
around the first one.

- **Undo-snapshot corruption (the path the mitigation targets).** Shotcut writes an undo
  snapshot of the track (and sometimes of the clip) with every split or remove, on the GUI
  thread, while the preview thread may be rendering those same clips; rendering rewrites their
  `meta.media.*` properties, and MLT's XML writer can then copy freed memory into the snapshot.
  Undoing such a step aborts a debug build ("Missing snapshot for restored clip"); in a release
  build the code skips the clip, so it would silently disappear or come back with missing
  properties (inferred from the code, not measured). A long `timeline_remove_ranges` call
  (hundreds of commands in one burst, repeated by its redo) makes it much more likely: without
  the guard, 2 of 20 debug runs of a 120-cut call plus undo, redo and undo aborted.
- **Render-thread crash in the Shotcut window.** Pressing Shift+S (Split All Tracks At
  Playhead) every 150 ms during playback crashed 3 of 10 debug sessions of about 135 splits with
  a SIGSEGV in the preview render thread (MLT property and producer state used by two threads).
  This is a related MLT race in plain GUI use; the MCP server does not touch that path.

The MCP server's preview guard: `timeline_remove_ranges`, `timeline_insert_range`,
`track_remove` and the `undo`/`redo` tools pause playback and block the preview refresh while
they run, then refresh once. For large work, `timeline_remove_ranges` always and `undo`/`redo`
of a step with more than 8 commands (a `timeline_remove_ranges` call of 3 or more cuts), the
preview is also fed a black stand-in at the same position instead of the timeline, because
Shotcut still seeks the player after every duration change and that seek alone renders the
clips being edited; the timeline is reconnected afterwards. Guards do not nest (only the
outermost acts).

Measured on a debug build (a 120-cut call plus undo, redo and undo per run): blocking the
refresh alone left a corrupt snapshot in about 3-7% of runs wherever 30 or more runs were made,
at every cap from 20 to 120 cuts, so a lower cap does not help; with the stand-in, the final
code gave no abort and no corrupt snapshot in 32 consecutive runs (95% upper bound about 9%;
earlier, 75 runs pooled over two stand-in variants also gave none). This narrows the race but
cannot rule it out. Call `save_project` before large cuts and send them in chunks.

Residual risks:

- Undo/Redo and History jumps in the Shotcut window are not guarded.
- Small MCP edits (`timeline_insert_range`, `track_remove`, small undo/redo steps) get only the
  refresh block, not the stand-in, so they keep a smaller exposure; so do the other edit tools.
- Inserting into an empty timeline creates the timeline inside the guard, while the player still
  shows the old producer; nothing is swapped then.
- Shotcut's autosave runs on a worker thread and may serialize the project while a guarded call
  is half done.
- GPU (Movit) processing mode was not tested.
- During a guarded large call the preview shows black, the scopes freeze and the audio device is
  restarted (the preview consumer is stopped and started twice).
- Unverified: a `seeked` signal emitted by a redone command might move the playhead during the
  guard.

### Known limits and residual risks (stage 3A)

- Undoing an overwrite onto an empty or short track leaves the extra gap that Shotcut's own
  Overwrite command creates (the same happens in the GUI).
- Removing a track whose gain has keyframes and undoing brings the keyframes back in an
  ordinary Gain/Volume filter (Shotcut's Remove Track copies filters without the track-gain
  marker): it sounds the same, but `gain_db` then reports 0.
- Proxy clips may report the proxy's dimensions in `get_playlist`/`get_timeline`.
- `get_overview` does not detect an export started from the Shotcut window (those use melt, not
  the player), and uses the player while it runs.
- A clip with a speed change has no `timeline_frames`, and its `in_s`/`out_s` are its trimmed
  frames divided by the frame rate, not true source seconds.
- Memory grows with every split `timeline_remove_ranges` makes (each split reopens the media);
  a long session of many cuts can use several GB on a debug build.
- The analysis cache and Whisper results have no size budget beyond their entry counts.

## Stage 3B: filters

Stage 3B starts with five generic filter tools (43 tools in all) that add, change and remove
filters on TIMELINE clips:

| Tool | What it does | Key arguments |
|------|--------------|---------------|
| `filter_list_available` | The allowlist below with parameters, ranges, defaults and units | none |
| `filter_list_applied` | A clip's user filters in processing order | `track`, `clip` |
| `filter_add` | Adds an allowlisted filter after the clip's existing filters | `track`, `clip`, `filter`, `params?` |
| `filter_set` | Changes parameters of an allowlisted filter | `track`, `clip`, `index`, `params` |
| `filter_remove` | Removes any listed filter (undo puts it back in place) | `track`, `clip`, `index` |

`filter_list_applied` returns `{filters:[{index, id, service, disabled, params}]}`; the edit tools
return `{index, track}` (the track as `get_timeline` reports it). `filter_list_applied` lists
the clip's user filters in MLT (processing) order: hidden internal filters (such as the audio
level meter) are never listed and keep their place, and chain links, which Shotcut's Filters
panel lists first, are not included, so its indexes can differ from the panel's rows. Filters are
appended in call order; Shotcut's panel sorts video filters before audio ones when it adds them,
which gives the same result because they act on different streams.

### The allowlist

Parameters, ranges and defaults are those of Shotcut's own filter UIs (`src/qml/filters/`): the
tool sets the defaults itself, because Shotcut applies them only when its UI first shows a new
filter. Missing params take the defaults; unknown names, wrong types and values out of range are
errors. A rect is `{x, y, w, h}` in pixels of the video mode (stored as `"x y w h 1"`); its
default `fit` is the clip's picture fitted into the frame and centered.

| id | MLT service | Kind | Parameters (range, default, unit) | Set automatically |
|----|-------------|------|-----------------------------------|-------------------|
| `gain` | `volume` | audio | `level` -70..24, 0 dB | |
| `normalize_one_pass` | `dynamic_loudness` | audio | `target_loudness` -50..-10, -23 LUFS; `window` 2..600, 10 s; `max_gain` 0..30, 15 dB; `min_gain` -30..0, -15 dB; `max_rate` 0.5..9, 3 dB/s | `discontinuity_reset` 1 |
| `brightness` | `brightness` | video | `level` 0..2, 1 (1 = unchanged) | `rgb_only` 1 |
| `color_grading` | `lift_gamma_gain` | video | `lift_r/g/b` -1..1, 0; `gamma_r/g/b` and `gain_r/g/b` 0.5..2, 1 (raw MLT values; Shotcut's wheels show other numbers) | `shotcut:filter_version` 1 |
| `contrast` | `lift_gamma_gain` (`shotcut:filter` `contrast`) | video | `gamma_r/g/b` and `gain_r/g/b` 0..2, 1 (raw MLT values; Shotcut's Contrast slider at level L sets every gain to L x 2 and every gamma to (1 - L) x 2) | |
| `saturation` | `frei0r.saturat0r` | video | `saturation` 0..3, 1 (a multiplier; stored as multiplier x 0.125 in property `0`) | `threads` 0 |
| `white_balance` | `frei0r.colgate` | video | `neutral_color` `#rrggbb`, `#7f7f7f`; `temperature_k` 1000..15000, 6500 K (stored as K / 15000 in property `1`) | `threads` 0 |
| `box_blur` | `box_blur` | video | `hradius`, `vradius` integers 0..100, 5 px | |
| `blur_pad` | `pillar_echo` | video | `blur` 0..10, 4 %; `rect` rect, fit | |
| `size_position` | `affine` | video | `rect` (property `transition.rect`) rect, fit; `rotation` (property `transition.fix_rotate_x`) -360..360, 0 degrees | `transition.fill` 1, `transition.distort` 0, `transition.valign` middle, `transition.halign` center, `background` `color:#00000000`, `transition.threads` 0 |
| `crop` | `crop` | video, once per clip | `left`, `right` integers 0..video width; `top`, `bottom` 0..video height; 0 px (left + right and top + bottom must leave a picture) | `use_profile` 1, `center` 0, `center_bias` 0 |

A row whose MLT service is missing (for example the frei0r plugins are not installed) is listed
under `unavailable` with the reason, and cannot be added. GPU-only (Movit) filters are never
offered.

`filter_list_applied` reports allowlisted filters with the tool-facing names and units above (a
value with keyframes is shown as its raw MLT string). Other filters report their Shotcut id (or
MLT service) and empty `params`; `filter_set` refuses them, `filter_remove` removes them.

### Rules

- Every call is ONE undo step with its own text: `Add <label> filter`, `Set <label>:
  name=value, ...` (two `filter_set` calls are always two undo steps, even with equal values) and
  `Remove <label> filter`, where label is Shotcut's name of the filter (for example "Gain /
  Volume").
- The tools work by `(track, clip)` through the timeline model, never through the Filters panel:
  they do not change the selection, and the clip opened in the Source player cannot be targeted
  (Shotcut has no undo for filters there).
- `shotcut:filter` is set to the id Shotcut uses (the filter UI's objectName, for example
  `audioGain`, `affineSizePosition`, `blur_pad`) and left out when Shotcut has none (Shotcut
  then identifies the filter by its MLT service), so the Filters panel shows tool-made filters
  under their real names. `_shotcut:newFilter` is never set, so opening the filter in the panel
  does not reset its values to the UI's defaults.
- A clip's filter is attached to the clip's parent producer, as in Shotcut, with the clip's in
  and out; it follows the clip when it is moved or trimmed.
- The undo commands find the filter again by the clip's `(track, clip)` and the filter's index
  at every undo and redo (checking its service and `shotcut:filter`), because Shotcut's undo of
  many timeline edits (move, insert, transitions, ripple removes, trims, splits) rebuilds the
  clip and its filters from saved XML.
- Refused: a locked track (`filter_list_applied` works on one), a blank or a transition, an audio
  filter on a clip whose media has no audio stream (clips with no audio information at all, such
  as images and colour clips, accept audio filters as in Shotcut), a picture filter on an
  audio-only clip or on an audio track, a second filter of a kind that allows only one (`crop`;
  Shotcut's GPU crop counts), an index not in `filter_list_applied`.

### Known limits and residual risks (stage 3B filters)

- Keyframes are not edited: `filter_set` refuses a parameter that has keyframes.
- Filters on the clip opened in the Source player, on tracks and on the timeline output are not
  reachable through the tools. The commands can target the timeline output (for the caption
  burn-in of a later stage), but no tool uses that path yet, so it is untested.
- If the Filters panel shows a filter that `filter_set` changes, the panel reloads it; other
  panel state (the selected filter) may reset when a tool adds or removes a filter of the clip it
  shows.

### Audio recipes

Four plain-language audio tools (47 tools in all) built on the filter engine and Shotcut's own
timeline commands:

| Tool | What it does | Key arguments |
|------|--------------|---------------|
| `clip_set_volume` | Sets a clip's volume with one `gain` filter | `track`, `clip`, `gain_db` |
| `clip_normalize` | Measures a clip's loudness and sets its volume to reach a target | `track`, `clip`, `target_lufs?`, `wait_ms?` |
| `track_set_ducking` | Shotcut's automatic track audio ducking | `track`, `threshold_db?`, `reduction_db?`, `fade_in_ms?`, `fade_out_ms?`, `enabled?` |
| `clip_detach_audio` | Shotcut's Detach Audio | `track`, `clip` |

`clip_set_volume`: `gain_db` -70 to 24. It changes the clip's first enabled `gain` filter (an
`audioGain` volume filter; Shotcut's fade-in, fade-out and mute filters use the same MLT service
but are never touched) or adds one; `gain_db: 0` removes it, so a volume never stacks. One undo
step, `Set clip volume to <n> dB` or `Reset clip volume to 0 dB`. A call that would change nothing
(the same value again, or 0 on a clip without a gain filter) pushes no undo step and returns
`changed: false`. Result `{track, gain_db, changed, index?}`, `index` being the gain filter's
`filter_list_applied` index while one exists. Refused: a locked track, a blank or transition, a
clip without audio, a gain filter with keyframes.

`clip_normalize`: `target_lufs` -30 to -6 (default -16), `wait_ms` 0 to 20000 (default 0). It
measures the clip's source range with the `audio_levels` code (EBU R128 integrated loudness of the
clip's own audio, before any filter; the measurement is cached and shared with `audio_levels` of
the same clip) and sets the clip's volume to `target - measured`, rounded to 0.01 dB and clamped
to -70..24, with `clip_set_volume`'s logic and the undo text `Normalize to <t> LUFS`.

- When the measurement is done within `wait_ms` (or cached) the call returns `{measured_lufs,
  applied_gain_db, target_lufs, changed, clip, track, index?}` and the edit is already made
  (`clip` and `track` say where the clip was found, see below).
- Otherwise it returns `{job_id, state: "running", measure_job_id}` at once (`measure_job_id` is
  the `audio_levels` job). The gain is then applied AUTOMATICALLY when the measurement ends:
  `get_job(job_id)` shows `succeeded` with the same result object, or `failed` with the reason,
  and the change is an ordinary undo step made at that moment: it lands on top of whatever the
  user or other tools did meanwhile and, like any edit, discards the redo history. The
  application waits until no tool call is running and no modal dialog or popup menu is open
  (`McpTools::runWhenIdle`, checked every 100 ms), so it never interleaves with another tool's
  undo macro or runs inside a waiting call's event loop (the smoke test checks the order of two
  overlapping normalizations).
- Before editing, the call checks that the timeline it measured is still open: a timeline that
  was closed, reopened or replaced by another project fails with "The project was closed or
  replaced while measuring" (a counter of timeline creations and closings; saving the project,
  also under a new name, does not count). It then finds the clip again by identity, not by index:
  the same in, out and source file, and the same start, or, if it was moved, the same Shotcut
  clip id (the parent producer's `_shotcut:uuid`, stamped at call time as Shotcut's undo helper
  does; moves keep it). A match on the original track wins; exactly one match is required. So a
  split of an earlier clip, a new video track shifting the rows, or a move of the clip itself do
  not matter, while a trim, a removal or a replacement of the clip fails with "The clip changed
  while measuring". In both failure cases nothing is edited.
- A computed gain of exactly 0.00 dB removes an existing gain filter of the clip (as
  `gain_db: 0` does); the indexes of the clip's later filters then shift down by one.
- Refused: a locked track (also when it is locked by the time the measurement ends), a blank or
  transition, a clip without audio, a clip with a speed change (its source range is not
  measured correctly), and a silent source: `audio_levels` reports -70 LUFS for silence, and a
  measurement at or below -70 LUFS is an error.
- Not taken into account: fades, the track's gain, other filters, and Shotcut's own loudness
  normalization filters. The smoke test exports a normalized 10 s clip of the gated tone fixture
  (440 Hz with silent gaps; ebur128 gates the silence out of both measurements) and measures the
  export: -16.1 LUFS for a -16 target (measured -24.2 LUFS, gain 8.2 dB).

`track_set_ducking`: makes a track (usually music) dip while the tracks above it are louder than
the threshold, through the track's audio `mix` transition, with Shotcut's
`ChangeDuckThresholdCommand` and `ChangeTransitionPropertyCommand` (`duck_attenuation`,
`duck_fade_in`, `duck_fade_out`) in ONE undo step (`Set track <n> audio ducking`, or `Disable
track <n> audio ducking` with `enabled: false`). Defaults and ranges are those of Shotcut's
Properties panel: `threshold_db` -70..0 (-12), `reduction_db` -70..0 (-12), `fade_in_ms` and
`fade_out_ms` 0..5000 (1500 and 250). Result `{track, ducking:{enabled, threshold_db,
reduction_db, fade_in_ms, fade_out_ms, duck_threshold}}`, `duck_threshold` read back from the
transition (0 when disabled). Refused: track 0 (the top track, as in Shotcut), a locked track, a
track without an audio mix transition ("Track <n> has no audio mix transition."), and an MLT
older than 7.41. MLT's mix transition gained ducking in 7.41 and Shotcut shows the controls only
when built against it; THIS build uses MLT 7.36.1, where the tool returns "Audio ducking needs MLT
7.41 or newer (this is 7.36.1)." and changes nothing, so the editing path is untested here. The
check is made at run time (`mlt_version_get_int()`), after the track checks. Shotcut's ducking
commands look the transition up in the player's producer, so the tool switches the player to the
timeline when it shows a source clip. Undo of an `enabled: false` call leaves ducking OFF:
Shotcut's `ChangeDuckThresholdCommand::undo` reads `shotcut:duck_enabled` after its own redo
changed it (Shotcut's own enable checkbox has the same bug). Undo of an enabling call restores the
previous state.

`clip_detach_audio`: Shotcut's Detach Audio (`TimelineDock::detachAudio`, no dialog, explicit
indexes) as ONE undo step `Detach audio`: the video clip keeps its place with its audio switched
off and its audio filters removed; an audio-only copy of the same range, with the audio filters,
goes to the first audio track that is blank over the whole range, else to a new audio track added
at the bottom (a new, empty audio track does not count: Shotcut keeps a one-frame blank on it).
Result `{tracks, audio_track}`: every track as `get_timeline` reports it (re-read indexes) and the
track that got the audio. Refused: an audio track, a locked track, a locked target audio track
(Shotcut itself does not check locks here), a blank or transition, a clip without audio (also a
clip whose audio was already detached). Speed-changed clips work. Shotcut selects the video clip
afterwards.

### Known limits and residual risks (audio recipes)

- `clip_normalize` measures the clip's source, not the mix: track gain, fades, ducking and other
  filters are ignored. The gain is computed once: trimming the clip later does not update it.
- A background `clip_normalize` application runs from the GUI event loop when no tool call is
  running. The idle check sees only QWidget modal dialogs and popups: a QML context menu, a
  native file dialog or a Shotcut operation that spins its own event loop without a modal
  QWidget could, in principle, see the edit land in the middle of it.
- A queued application still runs when the MCP server is stopped in the meantime (the job then
  ends as usual; it is only visible again through `get_job` once the server runs).
- A clip removed and put back with the same file, trim and start (without its Shotcut clip id)
  is taken for the same clip.
- `track_set_ducking` is untested end to end (MLT 7.36.1 here). Besides the `enabled: false` undo
  above, Shotcut's `ChangeDuckThresholdCommand::undo` leaves its `shotcut:duck_enabled` and
  `shotcut:duck_threshold` shadow properties on the transition, and
  `ChangeTransitionPropertyCommand::undo` writes 0 for a property that was not set before. All
  upstream behaviour, not compensated by the tool.
- Test coverage gaps: no test of the "no audio mix transition" error (Shotcut gives every track
  one), no test of `clip_normalize` refused by the 4-running-jobs cap, the ducking edit path
  (needs MLT 7.41), and the failure branches of the filter commands' undo (a failed detach or
  re-attach cannot be provoked through the tools).
- `clip_detach_audio` uses Shotcut's command, whose undo snapshots are subject to the upstream
  snapshot race described in stage 3A; the tool wraps it in the same preview guard as the other
  structural edits.

### Picture recipes

Three plain-language picture tools (50 tools in all) built on the filter engine. Each call is ONE
undo step, changes the clip's first filter of each kind it needs or adds one (repeated calls
update rather than stack), and returns `{filters:[{id, index}], track}` with the ids and
indexes `filter_list_applied` reports. All three take picture filters only: refused are a
locked track, a blank or transition, an audio track or an audio-only clip, a value with
keyframes, unknown argument names, and filters this Shotcut cannot use (GPU-only rows are never
offered). Any refusal changes nothing (a failure inside the call rolls the whole call back).

| Tool | What it does | Key arguments |
|------|--------------|---------------|
| `clip_set_color` | Brightness, contrast, saturation and colour temperature | `track`, `clip`, `brightness?`, `contrast?`, `saturation?`, `temperature_k?` |
| `clip_set_transform` | Size, position, rotation and crop | `track`, `clip`, `scale?`, `x?`, `y?`, `rotation?`, `crop?` |
| `clip_set_blur` | Box blur or a blurred pad behind the picture | `track`, `clip`, `mode`, `radius?` |

`clip_set_color` (undo text `Set clip color: name=value, ...`; give at least one value):

| Argument | Range (unchanged) | Filter and MLT value |
|----------|-------------------|----------------------|
| `brightness` | -1..1 (0) | `brightness`, `level` = 1 + brightness |
| `contrast` | -1..1 (0) | `contrast` (`lift_gamma_gain`, `shotcut:filter` `contrast`): Shotcut's slider level L = (contrast + 1) / 2, every `gain_*` = L x 2, every `gamma_*` = (1 - L) x 2 (`src/qml/filters/contrast/ui.qml`, `gainFactor`/`gammaFactor` 2) |
| `saturation` | 0..3 (1; 0 is grey) | `saturation` (`frei0r.saturat0r`), property `0` = saturation x 0.125 |
| `temperature_k` | 2000..15000 (6500) | `white_balance` (`frei0r.colgate`), property `1` = kelvin / 15000 |

`saturation` and `temperature_k` need MLT's frei0r plugins; without them the call is refused
with the reason ("The saturation filter is not available: this MLT has no frei0r.saturat0r
service."); the smoke test then checks that refusal and prints SKIP for their value and
exported-frame checks.

`clip_set_transform` (undo text `Set clip transform: scale=..., x=..., y=..., rotation=...,
crop left=... top=...`, naming the given values; give at least one):

- `scale` (0.05..8, 1 = the picture fitted into the frame), `x`, `y` (pixels of the video mode:
  the picture's centre offset from the frame centre, default 0) and `rotation` (-360..360
  degrees) set `size_position` (the `affine` filter) in Shotcut's Fit mode (`transition.fill` 0,
  `transition.distort` 0: the picture keeps its shape inside the rect; the transparent
  background shows lower tracks, or black on the bottom track).
- The rect is the `fit` rect of the allowlist (the clip's picture fitted into the frame with the
  formula of `SizePositionUI.qml`) scaled about its centre and moved by `(x, y)`:
  `w = fit.w x scale`, `h = fit.h x scale`, `x = fit.x + (fit.w - w) / 2 + x`, `y = fit.y +
  (fit.h - h) / 2 + y`. When any of `scale`, `x`, `y` is given the missing ones take their
  defaults; `rotation` alone keeps the current rect.
- `crop` `{left, right, top, bottom}` (integer pixels of the video mode, only the given sides
  change) sets `crop` (Crop: Source); left + right and top + bottom must leave a picture.
- When both are new, the crop is added before the size filter. Filters are appended after the
  clip's existing ones (Shotcut does the same), so a crop added to a clip that already has a
  size filter processes after it.
- Picture-in-picture on a 1920x1080 project: `{scale: 0.3, x: 560, y: -300}` on the upper
  track's clip (a 576x324 picture near the top-right corner).

`clip_set_blur`: `mode` `box` sets `box_blur` with `hradius` = `vradius` = `radius` (integer
0..100, default 5; undo text `Set clip blur: box radius=<n>`); `mode` `pad` sets `blur_pad`
(`pillar_echo`) with `blur` 4 and the default fit rect (undo text `Set clip blur: pad`;
`radius` is refused). A clip that already has that blur gets it replaced with these values.

The smoke test checks the MLT values read back through `filter_list_applied` and the saved
project, the undo steps, and exported frames of a red 1 s clip: `brightness: -0.5` lowers the
luma average (ffmpeg `signalstats` YAVG), `saturation: 0` gives equal R, G and B means (skipped
without frei0r), and `scale: 0.5` leaves the corners black with the centre red.

### Known limits (picture recipes)

- The fit rect uses the clip's media size and sample aspect ratio (`meta.media.width`,
  `meta.media.height`, `meta.media.sample_aspect_num/den`), falling back to the video mode's
  aspect; a source with rotation metadata is not taken into account.
- The tools never add keyframes and refuse values that have them.
- Brightness, contrast and saturation are separate filters applied in the order they were
  added, as in Shotcut.

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

Stage 3A lets the token make Shotcut read any file the user can read with ffprobe, ffmpeg and the
configured Whisper program (only results come back, never the file's bytes), keep transcripts and
analysis results in memory, and write short-lived temporary files (decoded audio) in the system
temp folder. Programs get argument lists, never a shell command line.

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

Stage 3A has two scripts, each needing its own FRESH throwaway Shotcut like the stage 2 script:

    MCP_TOKEN=<token> [MCP_PORT=<port>] [MCP_APPDATA=<scratch dir>] tools/mcp-smoke-media.sh
    MCP_TOKEN=<token> [MCP_PORT=<port>] tools/mcp-smoke-ripple.sh

They need bash, curl, jq, ffmpeg, ffprobe, xxd, md5sum and base64; `tools/mcp-fixtures.sh` (run by
the scripts) generates the test media with ffmpeg. `mcp-smoke-media.sh` covers every stage 3A
tool and takes a few minutes on a debug build. `MCP_APPDATA` enables the "get_overview while an
export runs" checks (they write a slow custom preset there, as stage 2 does). Its Whisper checks
depend on the Whisper program configured in that Shotcut (pre-seed a `[subtitles]` section with
`whisperExe=<path of whisper-cli>` and `whisperModel=<path of a GGML model>` in `shotcut.ini`):

- not configured: it checks the "Whisper is not configured" error and skips the rest;
- configured with a real whisper-cli: it transcribes whisper.cpp's `samples/jfk.wav` (set
  `JFK_WAV` if it is not at `~/GitHub/whisper.cpp/samples/jfk.wav`);
- configured with `tools/mcp-fake-whisper.sh` (any existing file as the model): it checks the
  parser on a canned transcript (Unicode punctuation, a segment clamped to nothing), no model
  needed.

Run it in all three setups. `mcp-smoke-ripple.sh` checks that `timeline_remove_ranges` overrides
the user's ripple settings and puts them back: pre-seed a `[timeline]` section with
`rippleAllTracks=true` and `rippleMarkers=true` in that Shotcut's `shotcut.ini`. Without them it
prints SKIP and exits 2, which is not a pass. Each script ends with "All checks passed." (exit
code 0) when it passes.

Stage 3B has its own script, also needing a FRESH throwaway Shotcut:

    MCP_TOKEN=<token> [MCP_PORT=<port>] tools/mcp-smoke-polish.sh

It needs bash, curl, jq and ffmpeg. It builds a timeline from the generated fixtures, adds,
changes and removes filters with their undo and redo, saves and reopens a copy of the project,
and checks the refusals. The audio recipe checks export a normalized clip (`x264-medium`, needs
`melt-7` as for stage 2) and run several background `clip_normalize` jobs on generated 2400 s
and 3600 s files (about 7 and 10 s each on a debug build); if a measurement ends before the script
can change the clip, that check prints SKIP. The picture recipe checks export four 1 s clips and read pixels with
ffmpeg and od; without frei0r the saturation and temperature checks print SKIP.

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
mode, proxies, generators and titles, filters outside the stage 3B allowlist, subtitles, two-pass
encoding and export ranges.
