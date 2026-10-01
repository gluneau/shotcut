# Embedded MCP Server, Stage 3 — Design

Status: draft for review. Branch: `feature/embedded-control-server` (stages 1 and 2 are done and reviewed; see
`2026-10-01-mcp-control-server-design.md`, `2026-10-01-mcp-stage2-design.md`, `docs/mcp-server.md`).

## 1. Goal

Let an MCP client make a professional cut for four kinds of video, using only Shotcut's own features plus the
`ffmpeg`/`ffprobe` programs and the optional Whisper program that Shotcut already ships or lets the user configure:

1. talking head, interview, podcast;
2. social shorts (vertical 9:16);
3. tutorials and lessons (screen plus face);
4. highlights, montage, music-driven edits.

The client needs to HEAR and READ the footage (silence, loudness, scenes, a word-timed transcript), make precise
multi-cut edits, polish picture and sound, change the format, add text, and deliver. Stage 3 extends the stage 2
server; transport, authentication, security and the editing rules (stage 2 spec section 3) are unchanged.

One spec, three parts built and verified in order. Each part ends with a review gate and a full smoke run:

- **3A Perception and structure** (15 tools): see and hear the footage, cut precisely.
- **3B Look and sound** (13 tools): filters, audio levels, colour, speed, picture-in-picture.
- **3C Format, text and delivery** (7 tools): 9:16 and other formats, reframe, titles, captions, delivery checks, guide.

The server grows from 23 to 58 tools.

### Success criteria

1. Given a generated test file with known silent ranges and known scene cuts, `detect_silence` and `detect_scenes`
   return those ranges and cuts within a stated tolerance; `audio_levels` returns the known loudness within 1 LU.
2. With Whisper configured, `transcribe` returns word-level times for a speech file; with it not configured,
   `transcribe` returns a clear error and `transcribe` with a supplied transcript still works.
3. A client can take a source with silences, call `timeline_remove_ranges` with those silences as one undo step, and the
   resulting timeline duration equals the source minus the removed ranges (to the frame).
4. A client can build, using only tools: an interview cut (silences removed, normalized, captioned), a 9:16 short
   (format set on an empty timeline, reframed, titled, exported), a screen-plus-face layout, and a music-driven montage,
   and export each; `ffprobe` of each output matches the expected size, duration and codecs.
5. Every editing tool is undoable (Shotcut's Edit > Undo and the `undo` tool); a bad argument, a locked track, a
   refused filter, an unconfigured program or a busy job returns a clear tool error and changes nothing. No tool opens
   a dialog.
6. All stage 1 and stage 2 smoke checks still pass; new checks run headless on an isolated `--appdata`.

### Non-goals

Speaker identification, any cloud service, a mid-project format change (the tool refuses it, section 6), GPU/Movit
filters, keyframe editing beyond what the named tools expose, audio-only project editing beyond track controls,
automatic "AI" selection of best moments (the client decides; the tools give it data), proxy management, batch
export, a separate "allow editing" toggle.

## 2. Architecture

| File | Responsibility |
|------|----------------|
| `src/mcpjobs.h/.cpp` | Background job manager (section 3) |
| `src/mcptools_analysis.cpp` | 3A perception tools, `get_job` |
| `src/mcptools_structure.cpp` | 3A cutting, track and marker tools |
| `src/mcptools_filters.cpp` | 3B filter engine and recipe tools |
| `src/mcptools_format.cpp` | 3C format, reframe, titles, captions, delivery, guide |
| `src/mcptoolsutil.{h,cpp}` | Shared helpers (extended as needed) |

`McpTools::all()` concatenates the new groups. Changes to existing Shotcut code are limited to small public accessors
and, if needed, a public entry point for appending text clips and for the ffmpeg lookup (section 3). New source
files carry the GPLv3 notice and NO copyright line.

## 3. Shared foundation

**Background jobs (`McpJobs`).** Heavy tools return `{job_id}` at once; `get_job(job_id)` returns
`{job_id, kind, state:"running"|"succeeded"|"failed"|"cancelled", percent?, result?, error?, elapsed_ms}`. Heavy tools
accept `wait_ms?` (0 to 20000, default 0): block in a local event loop up to that long and return the result directly
when finished. State is captured from signals into a small struct, so it survives dock or process cleanup (the same
approach as `ExportRecord`). One analysis job per kind and source at a time; an identical request while one is running
returns the same `job_id`. Jobs are not persisted across restarts.

**Programs.** `ffmpeg` and `ffprobe` run as `QProcess` through one helper that looks where Shotcut's `FfmpegJob` and
`FfprobeJob` look (the application directory) and then falls back to `PATH`, so dev builds work. The helper never
passes a shell string, always uses an argument list, and puts a source path after `-i` only after the absolute-path
and existence checks. A missing program returns an error naming the program and where it looked.

**Cache.** Per-file results are cached in memory keyed by `(absolute path, mtime, size, tool, parameters)`; a changed
file misses the cache. `cached:true` is reported in results.

**Source time versus timeline time.** Perception tools work on SOURCE FILES (path given, or `track`+`clip` which is
resolved to the clip's source path and in/out) and report seconds in the source. When given `track`+`clip` they also
report `timeline_frames` for each range that falls inside that clip, so the client never converts by hand.

**Rules.** Every stage 2 editing rule applies: undoable only, explicit addressing, updated track returned,
no silent no-ops, no dialogs, pause playback first. Analysis tools do not touch the undo stack.

## 4. Part 3A — Perception and structure

### Perception

| Tool | Arguments | Behavior |
|------|-----------|----------|
| `analyze_media` | `path` (absolute) | `ffprobe` result: `{duration_s, video:{width,height,fps,codec,rotation,pix_fmt}, audio:{channels,sample_rate,codec}, size_bytes}`; missing streams are null. Synchronous (fast). The same fields (`width`, `height`, `fps`, `has_audio`, `rotation`) are added to `get_playlist` items and `get_timeline` clips where cheap, from MLT properties. |
| `detect_silence` | `path` or `track`+`clip`; `noise_db?` (default -35), `min_s?` (default 0.4), `wait_ms?` | `silencedetect`. Result `{ranges:[{start_s,end_s,duration_s,timeline_frames?}], speech_ranges:[...], total_silence_s}`. `speech_ranges` is the complement, so the client can keep it. |
| `audio_levels` | `path` or `track`+`clip`; `buckets?` (default 60, max 600), `wait_ms?` | `ebur128` and `astats`: `{integrated_lufs, loudness_range_lu, true_peak_dbtp, buckets:[{start_s,end_s,rms_db,peak_db}]}`. Also works on an exported file, which is the delivery loudness check. |
| `detect_scenes` | `path` or `track`+`clip`; `threshold?` (0.05 to 0.9, default 0.3), `wait_ms?` | `scdet`/scene `select`: `{scenes:[{time_s,score,timeline_frames?}]}`. |
| `transcribe` | `path` or `track`+`clip`; `language?` (default auto), `wait_ms?`; OR `transcript` (supplied, see below) | Section 5. |
| `get_overview` | `columns?` (default 4, max 8), `rows?` (default 4, max 8), `tile_width?` (default 320) | One image of the whole timeline: `columns*rows` frames evenly spaced, each labelled with its timecode and frame number, built with the same settled-frame logic as `get_frame` (several seeks, so it takes seconds; the call is refused while an export runs, since the player is shared). Returns the image plus `{positions:[...]}`. |
| `get_job` | `job_id` | Section 3. |

### Structure

| Tool | Arguments | Behavior |
|------|-----------|----------|
| `timeline_insert_range` | `path` (absolute media) or `playlist_index`; `in_s`, `out_s` (source seconds, `out_s` exclusive); `track`, `position` (absolute frame), `mode` (`insert` ripples later clips, `overwrite` replaces what is under it) | Builds an MLT producer for the file with `in`/`out` set, serializes it to XML and calls `TimelineDock::insert`/`overwrite` with that non-empty XML, so neither the clipboard nor a duration dialog is used. Validates range against the file duration, track lock and track type. One undo step. Returns the updated track. |
| `timeline_remove_ranges` | `track`, `ranges:[{start,end}]` (absolute timeline frames, `end` exclusive; up to 500), `ripple?` (default true) | Sorts and merges overlaps, validates all ranges first, then inside ONE macro splits at each boundary and removes (or lifts) the covered pieces, working from the end of the timeline backward so frames do not shift. All-or-nothing: any invalid range errors with nothing changed. Returns the updated track and `{removed_frames}`. |
| `track_add` | `type` (`video`\|`audio`), `name?` | `Timeline::AddTrackCommand`-style commands only; returns the track list. |
| `track_remove` | `track` | Refuses a track that still holds clips unless `force:true`; refuses locked. |
| `track_set` | `track`, any of `name`, `mute`, `lock`, `hide`, `composite`, and (3B) `gain_db`, `pan` | Each changed property is its own undoable command inside one macro. |
| `marker_add` | `start`, `end?`, `text`, `color?` | Via `MarkersModel` (undoable). |
| `marker_list`, `marker_remove` | none; `index` | List returns `{index,start,end,text,color}`. |

## 5. Transcription

- Uses the Whisper program and model already configured in Shotcut's Subtitles settings (`subtitles/whisperExe`,
  `subtitles/whisperModel`). No new settings are added. Not configured, or the files missing: tool error saying what
  to set in Shotcut. Never downloads anything.
- Shotcut's own subtitle path asks for an SRT only. This tool runs the same configured program with the options
  that produce word-level timing, after the source's audio is rendered to a 16 kHz mono WAV with `ffmpeg`
  (same as Shotcut's own path). The exact option names are verified against the installed program's `--help` in the
  plan's pre-flight (not assumed); if the installed build cannot give word timing the result falls back to
  segment-level timing and says `granularity:"segment"`.
- Result `{language, granularity:"word"|"segment", segments:[{start_s,end_s,text,words:[{start_s,end_s,text}]}],
  timeline_frames?}`.
- **Bring your own transcript:** `transcribe` with `transcript:{segments:[{start_s,end_s,text,words?}]}` validates
  and stores the transcript for that file (same shape as the result) so the 3C captions tool can use it. No
  Whisper needed.
- Transcripts are cached like other results. A source whose audio render fails reports the ffmpeg error tail.

## 6. Part 3B — Look and sound

### Generic filter engine

| Tool | Arguments | Behavior |
|------|-----------|----------|
| `filter_list_available` | none | The allowlist: `{service, name, kind:"audio"\|"video", params:[{name,type,min,max,default,unit}]}`. |
| `filter_list_applied` | `track`, `clip` | The clip's filters in order with their current parameters and an `index`. |
| `filter_add` | `track`, `clip`, `service`, `params?` | Adds via `AttachedFiltersModel::addService`, pushed as `Filter::AddCommand`; parameters set with defaults from the tool's own table (Shotcut keeps defaults in its QML UIs, and the tool must not set `shotcut:new`). |
| `filter_set` | `track`, `clip`, `index`, `params` | `Filter::UndoParameterCommand`, each call with a DISTINCT undo text (so repeated tweaks never merge). |
| `filter_remove` | `track`, `clip`, `index` | Undoable removal. |

Rules: only multitrack clips (a filter on a clip opened in the Source player has no undo in Shotcut, so the tool
refuses it); GPU-only services (`needsGPU`, Movit) are refused and are not in the allowlist; parameters are
type- and range-checked against the table. Allowlist and defaults are decided in the plan from Shotcut's meta
files; initial services: `volume`, `dynamic_loudness`, `lift_gamma_gain`, `brightness`, `frei0r.saturat0r`,
`frei0r.colgate`, `box_blur`, `pillar_echo`, `affine`, `crop`, speed (below).

### Recipes (plain-language tools built on the engine)

| Tool | Behavior |
|------|----------|
| `clip_set_volume` | `gain_db` (-60 to +24). One `volume` filter, replaced rather than stacked. |
| `clip_normalize` | `target_lufs` (default -16, range -30 to -6). Measures the clip's source range with the `audio_levels` code, sets the gain to reach the target, reports `{measured, applied_gain_db}`; refuses a source with no audio or silence only. |
| `track_set` (3B fields) | `gain_db`, `pan` on a track. |
| `track_set_ducking` | Music track dips under a dialogue track: uses the track mix transition's duck settings (`ChangeDuckThresholdCommand` and neighbours): `threshold_db`, `reduction_db`. |
| `clip_detach_audio` | `TimelineDock::detachAudio`-equivalent via commands; returns both tracks. |
| `clip_set_color` | `brightness`, `contrast`, `saturation`, `temperature` (each optional, normalized ranges), mapped to the services above. |
| `clip_set_transform` | `scale`, `x`, `y`, `rotation`, `crop:{l,t,r,b}`: the `affine`/`crop` filters. Used for picture-in-picture: put the screen on V1 and the face on V2 with a small scale and corner position. |
| `clip_set_blur` | `radius`; or `mode:"pad"` for the blurred-background fill (`pillar_echo`). |
| `clip_set_speed` | `speed` (0.1 to 10). Shotcut has no speed command: the tool replicates what Shotcut's own speed change does (`Timeline::UpdateCommand` with the retimed producer; audio pitch handling per the available MLT service) and the clip length changes. Exact mechanism confirmed in the plan pre-flight; if it cannot be made undoable the tool is dropped and this is recorded. |

## 7. Part 3C — Format, text and delivery

| Tool | Behavior |
|------|----------|
| `project_set_format` | `preset` (`16:9`, `9:16`, `1:1`, `4:5`) or `width`,`height`, and `fps?`. **Only when no track holds any clip**: Shotcut clears undo on a profile change and may reload producers, so a mid-project change is refused with a message to start a fresh project. Sets the profile through `MLT.profile()` setters and the preview refresh Shotcut uses. Returns the new profile. |
| `clip_reframe` | `mode` (`fill`\|`pad`), `focus_x?` (0 to 1, default 0.5) and optionally `keyframes:[{frame,focus_x}]` (max 50). `fill` crops to the project aspect ratio with a chosen focus (a follow-the-speaker crop when keyframed) using `affine`; `pad` uses the blurred-background fill. Valid on any aspect ratio mismatch; a no-op match returns an error saying so. |
| `title_add` | `text`, `track`, `position`, `duration`, `font?`, `size?`, `color?`, `bg_color?`, `x?`, `y?`. Inserts a color-generator clip with a text filter built as explicit XML via the same insert path as `timeline_insert_range`. |
| `lower_third_add` | Same with two lines and a preset position/background bar. |
| `captions_from_transcript` | Takes a transcript (from `transcribe`) and `track`, `max_words?` (default 7), `max_chars?` (default 42), `style?`; builds timed caption clips for the clip's visible range (using the timeline_frames mapping), as ONE macro. Whether captions use Shotcut's subtitle track or text clips is decided in the plan pre-flight from what the subtitle model can create undoably and burns in on export; the default is text clips, which always burn in. |
| `export_presets_list` | The presets `export_video` accepts, with codec, container and the platform they suit, from the MLT presets and `presets/encode`. |
| `get_editing_guide` | `use_case` (`interview`, `short`, `tutorial`, `montage`): returns step-by-step written workflows naming the tools to call, with default numbers (silence threshold, loudness target, caption size, safe margins). Static text kept in one source file so it can be reviewed and edited. |

Delivery checks reuse existing tools: `export_video`, `get_export_status`, then `audio_levels` and `analyze_media` on the
output.

## 8. Errors, safety and limits

- All new tool arguments are validated strictly (types, ranges, absolute existing paths, array size caps given above).
- Analysis tools read only; paths are resolved like stage 2's and refused if not regular files. They never write next to
  the media; temporary files (the WAV for transcription, overview tiles) live in a per-run directory under the system
  temp location and are deleted when the job ends.
- External programs: argument lists only, a timeout per job (default 10 minutes, kill on timeout, reported as `failed`),
  stderr tail in errors, output size caps when parsing.
- The server stays single-request. Jobs run in the background; `wait_ms` blocks within the existing `m_busy` guard
  and is capped at 20 s so a client is never stuck.
- Filter and recipe tools refuse locked tracks, blanks, transitions and (for picture filters) audio clips, and audio
  filters on video-only clips with no audio.

## 9. Verification

1. `tools/mcp-smoke-media.sh` (3A): generates test media with `ffmpeg` (tone with exact silences, coloured scene
   cuts, a speech-like file), checks tolerances (success criteria 1 and 3), `timeline_insert_range`,
   `timeline_remove_ranges` (frame-exact result, one undo restores), tracks, markers, `get_overview` image validity,
   and errors (missing program, relative path, locked track). Transcription is checked only when a Whisper program
   is configured in the test appdata (the plan builds whisper.cpp in a scratch location for this); the
   not-configured error and the supplied-transcript path are always checked.
2. `tools/mcp-smoke-polish.sh` (3B): each filter tool adds, sets, lists and removes; undo restores; GPU and
   source-clip refusals; `clip_normalize` result within 1 LU on the exported file; ducking and speed checks.
3. `tools/mcp-smoke-format.sh` (3C): `project_set_format` accepted on an empty timeline and refused with clips;
   reframe, titles and captions are checked by exported frames and `ffprobe` size; the four use-case guides listed.
4. End-to-end per use case (success criterion 4), exported and probed.
5. Stages 1 and 2 smoke scripts still pass (the helper `run-smoke.sh` runs all).

Not testable headless and recorded as residual risk: GPU filters, the visual quality of colour and reframe beyond
exported-frame spot checks, hardware encoders, Windows path handling.

## 10. Open points settled in the plan pre-flight (read-only code and tool checks, no guessing)

1. The installed Whisper program's options for word-level timing (`--help`), and what happens on this machine's
   build.
2. The exact allowlist of filter services, parameter names, ranges and defaults from Shotcut's meta files.
3. The undoable path for speed changes, for detaching audio, and for creating subtitle items versus text clips.
4. The profile change sequence for `project_set_format` and whether it is safe on an empty timeline.
5. Where `FfmpegJob`/`FfprobeJob` look for programs, and whether to expose one shared lookup function.

Each answer is recorded in the plan's ledger and may change a tool's behaviour; a tool that cannot be made
undoable and dialog-free is dropped and the drop is recorded.
