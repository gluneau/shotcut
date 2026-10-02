#!/usr/bin/env bash
# Perception and structure smoke test for the Shotcut MCP server (stage 3, part 3A).
# Usage: MCP_TOKEN=<token> [MCP_PORT=7468] [MCP_APPDATA=<dir>] tools/mcp-smoke-media.sh
# Needs: bash, curl, jq, ffmpeg, ffprobe. Run it against a FRESH, throwaway Shotcut (nothing
# loaded, empty playlist, no timeline): it aborts otherwise unless MCP_SMOKE_FORCE=1. See
# docs/mcp-server.md for the headless recipe.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=tools/mcp-smoke-lib.sh
. "$HERE/mcp-smoke-lib.sh"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fresh_guard

MEDIA="$("$HERE/mcp-fixtures.sh" "$TMP/media")" || { echo "ffmpeg could not create the fixtures"; exit 2; }

echo "== get_job"
expect_eq "tools/list has get_job" true "$(rpc '{"jsonrpc":"2.0","id":3,"method":"tools/list"}' | jq -r '[.result.tools[].name] | contains(["get_job"])')"
expect_eq "get_job: missing job_id is an error" true "$(tool_is_error get_job '{}')"
expect_eq "get_job: negative job_id is an error" true "$(tool_is_error get_job '{"job_id":-1}')"
expect_eq "get_job: string job_id is an error" true "$(tool_is_error get_job '{"job_id":"1"}')"
expect_eq "get_job: fractional job_id is an error" true "$(tool_is_error get_job '{"job_id":1.5}')"
expect_eq "get_job: unknown job_id is an error" true "$(tool_is_error get_job '{"job_id":999999}')"
expect_eq "get_job: unknown job_id message" "No such job." "$(tool_text get_job '{"job_id":999999}')"

echo "== analyze_media"
expect_eq "tools/list has analyze_media" true "$(rpc '{"jsonrpc":"2.0","id":3,"method":"tools/list"}' | jq -r '[.result.tools[].name] | contains(["analyze_media"])')"
analyze() { # PATH: the result JSON, following get_job when the call returns a job
  local out job
  out="$(tool_json analyze_media "$(jq -nc --arg p "$1" '{path:$p}')")"
  job="$(echo "$out" | jq -r '.job_id // empty')"
  if [ -n "$job" ]; then out="$(wait_job "$job" 30 | jq -c '.result')"; fi
  echo "$out"
}
A="$(analyze "$MEDIA/tone_av.mp4")"
expect_eq "analyze_media tone_av: path" "$MEDIA/tone_av.mp4" "$(echo "$A" | jq -r '.path')"
expect_eq "analyze_media tone_av: width" 320 "$(echo "$A" | jq -r '.video.width')"
expect_eq "analyze_media tone_av: height" 240 "$(echo "$A" | jq -r '.video.height')"
expect_eq "analyze_media tone_av: fps" 25 "$(echo "$A" | jq -r '.video.fps')"
expect_eq "analyze_media tone_av: codec" h264 "$(echo "$A" | jq -r '.video.codec')"
expect_eq "analyze_media tone_av: rotation" 0 "$(echo "$A" | jq -r '.video.rotation')"
expect_eq "analyze_media tone_av: pix_fmt" yuv420p "$(echo "$A" | jq -r '.video.pix_fmt')"
expect_eq "analyze_media tone_av: audio channels" 1 "$(echo "$A" | jq -r '.audio.channels')"
expect_eq "analyze_media tone_av: audio sample_rate" 16000 "$(echo "$A" | jq -r '.audio.sample_rate')"
expect_eq "analyze_media tone_av: duration within 0.1 of 10" true "$(echo "$A" | jq -r '(.duration_s - 10) | fabs < 0.1')"
expect_eq "analyze_media tone_av: size_bytes matches the file" "$(stat -c %s "$MEDIA/tone_av.mp4")" "$(echo "$A" | jq -r '.size_bytes')"
expect_eq "analyze_media noaudio: audio is null" null "$(analyze "$MEDIA/noaudio.mp4" | jq -c '.audio')"
expect_eq "analyze_media noaudio: video present" 320 "$(analyze "$MEDIA/noaudio.mp4" | jq -r '.video.width')"
expect_eq "analyze_media audioonly: video is null" null "$(analyze "$MEDIA/audioonly.wav" | jq -c '.video')"
expect_eq "analyze_media audioonly: audio present" 1 "$(analyze "$MEDIA/audioonly.wav" | jq -r '.audio.channels')"
expect_eq "analyze_media space name works" 320 "$(analyze "$MEDIA/space name.mp4" | jq -r '.video.width')"
printf 'not media\n' > "$TMP/note.txt"
expect_eq "analyze_media: relative path is an error" true "$(tool_is_error analyze_media '{"path":"tone_av.mp4"}')"
expect_eq "analyze_media: missing path is an error" true "$(tool_is_error analyze_media '{}')"
expect_eq "analyze_media: non-string path is an error" true "$(tool_is_error analyze_media '{"path":5}')"
expect_eq "analyze_media: missing file is an error" true "$(tool_is_error analyze_media "{\"path\":\"$MEDIA/nope.mp4\"}")"
expect_eq "analyze_media: directory is an error" true "$(tool_is_error analyze_media "{\"path\":\"$MEDIA\"}")"
expect_eq "analyze_media: resource path is an error" true "$(tool_is_error analyze_media '{"path":":/icons/shotcut-logo-64.png"}')"
note_job="$(tool_json analyze_media "{\"path\":\"$TMP/note.txt\"}" 2>/dev/null | jq -r '.job_id // empty')"
if [ -n "$note_job" ]; then
  expect_eq "analyze_media: a text file fails the job" failed "$(wait_job "$note_job" 30 | jq -r '.state')"
else
  expect_eq "analyze_media: a text file is an error" true "$(tool_is_error analyze_media "{\"path\":\"$TMP/note.txt\"}")"
fi

echo "== get_overview without a timeline"
expect_eq "get_overview: nothing loaded is an error" true "$(tool_is_error get_overview '{}')"
expect_eq "get_overview: no timeline message" "There is no timeline; add clips with timeline_add_playlist first." "$(tool_text get_overview '{}')"

echo "== per-clip media fields"
tool_json playlist_add "{\"path\":\"$MEDIA/tone_av.mp4\"}" >/dev/null
expect_eq "get_overview: a clip in the player but no timeline is an error" true "$(tool_is_error get_overview '{}')"
P="$(tool_json get_playlist '{}')"
expect_eq "get_playlist item width" 320 "$(echo "$P" | jq -r '.items[0].width')"
expect_eq "get_playlist item height" 240 "$(echo "$P" | jq -r '.items[0].height')"
expect_eq "get_playlist item has_audio" true "$(echo "$P" | jq -r '.items[0].has_audio')"
expect_eq "get_playlist item fps" 25 "$(echo "$P" | jq -r '.items[0].fps')"
tool_json playlist_add "{\"path\":\"$MEDIA/noaudio.mp4\"}" >/dev/null
expect_eq "get_playlist noaudio item has_audio false" false "$(tool_json get_playlist '{}' | jq -r '.items[1].has_audio')"
tool_json timeline_add_playlist '{}' >/dev/null
T="$(tool_json get_timeline '{}')"
expect_eq "get_timeline clip width" 320 "$(echo "$T" | jq -r '[.tracks[].clips[] | select(.kind=="clip")][0].width')"
expect_eq "get_timeline clip height" 240 "$(echo "$T" | jq -r '[.tracks[].clips[] | select(.kind=="clip")][0].height')"
expect_eq "get_timeline clip has_audio" true "$(echo "$T" | jq -r '[.tracks[].clips[] | select(.kind=="clip")][0].has_audio')"
expect_eq "get_timeline clip fps" 25 "$(echo "$T" | jq -r '[.tracks[].clips[] | select(.kind=="clip")][0].fps')"
expect_eq "get_timeline blanks and transitions have no width" 0 "$(echo "$T" | jq -r '[.tracks[].clips[] | select(.kind!="clip" and has("width"))] | length')"

echo "== detect_silence"
expect_eq "tools/list has detect_silence" true "$(rpc '{"jsonrpc":"2.0","id":3,"method":"tools/list"}' | jq -r '[.result.tools[].name] | contains(["detect_silence"])')"
# job_result TOOL ARGS: the result JSON, following get_job when the call returns a job
job_result() {
  local out job
  out="$(tool_json "$1" "$2")"
  job="$(echo "$out" | jq -r '.job_id // empty')"
  if [ -n "$job" ]; then out="$(wait_job "$job" 60 | jq -c '.result')"; fi
  echo "$out"
}
SIL="{\"path\":\"$MEDIA/silence.wav\"}"
S="$(tool_json detect_silence "$(jq -c '. + {wait_ms:20000}' <<<"$SIL")")"
expect_eq "detect_silence wait_ms: result in the same call" 2 "$(echo "$S" | jq -r '.ranges | length')"
expect_eq "detect_silence: path" "$MEDIA/silence.wav" "$(echo "$S" | jq -r '.path')"
expect_eq "detect_silence: range 1 start near 2.0" true "$(echo "$S" | jq -r '(.ranges[0].start_s - 2.0) | fabs < 0.15')"
expect_eq "detect_silence: range 1 end near 4.0" true "$(echo "$S" | jq -r '(.ranges[0].end_s - 4.0) | fabs < 0.15')"
expect_eq "detect_silence: range 2 start near 7.0" true "$(echo "$S" | jq -r '(.ranges[1].start_s - 7.0) | fabs < 0.15')"
expect_eq "detect_silence: range 2 end near 8.5" true "$(echo "$S" | jq -r '(.ranges[1].end_s - 8.5) | fabs < 0.15')"
expect_eq "detect_silence: total_silence_s near 3.5" true "$(echo "$S" | jq -r '(.total_silence_s - 3.5) | fabs < 0.3')"
expect_eq "detect_silence: three speech_ranges" 3 "$(echo "$S" | jq -r '.speech_ranges | length')"
expect_eq "detect_silence: in_s 0" 0 "$(echo "$S" | jq -r '.in_s')"
expect_eq "detect_silence: out_s near 10" true "$(echo "$S" | jq -r '(.out_s - 10) | fabs < 0.1')"
expect_eq "detect_silence: speech 1 starts at in_s" true "$(echo "$S" | jq -r '.speech_ranges[0].start_s < 0.01')"
expect_eq "detect_silence: speech 3 ends at out_s" true "$(echo "$S" | jq -r '(.speech_ranges[2].end_s - .out_s) | fabs < 0.01')"
expect_eq "detect_silence: path source has no timeline_frames" 0 "$(echo "$S" | jq -r '[.ranges[] | select(has("timeline_frames"))] | length')"
expect_eq "detect_silence: first wait_ms call is not cached" null "$(echo "$S" | jq -c '.cached // null')"
expect_eq "detect_silence: identical second call is cached" true "$(tool_json detect_silence "$SIL" | jq -r '.cached')"
expect_eq "detect_silence: cached call returns the ranges at once" 2 "$(tool_json detect_silence "$SIL" | jq -r '.ranges | length')"
SJ="$(tool_json detect_silence "$(jq -c '. + {noise_db:-36}' <<<"$SIL")")"
expect_eq "detect_silence without wait_ms returns a job" true "$(echo "$SJ" | jq -r 'has("job_id")')"
expect_eq "detect_silence: get_job yields the ranges" 2 "$(wait_job "$(echo "$SJ" | jq -r '.job_id')" 60 | jq -r '.result.ranges | length')"
expect_eq "detect_silence min_s 3: no ranges" 0 "$(job_result detect_silence "$(jq -c '. + {min_s:3}' <<<"$SIL")" | jq -r '.ranges | length')"
expect_eq "detect_silence min_s 3: one speech range" 1 "$(job_result detect_silence "$(jq -c '. + {min_s:3}' <<<"$SIL")" | jq -r '.speech_ranges | length')"
NA="{\"path\":\"$MEDIA/noaudio.mp4\"}"
expect_eq "detect_silence noaudio: error" true "$(tool_is_error detect_silence "$NA")"
expect_eq "detect_silence noaudio: message" "$MEDIA/noaudio.mp4 has no audio stream." "$(tool_text detect_silence "$NA")"
expect_eq "detect_silence: noise_db 5 is an error" true "$(tool_is_error detect_silence "$(jq -c '. + {noise_db:5}' <<<"$SIL")")"
expect_eq "detect_silence: noise_db -95 is an error" true "$(tool_is_error detect_silence "$(jq -c '. + {noise_db:-95}' <<<"$SIL")")"
expect_eq "detect_silence: string noise_db is an error" true "$(tool_is_error detect_silence "$(jq -c '. + {noise_db:"-35"}' <<<"$SIL")")"
expect_eq "detect_silence: min_s 0 is an error" true "$(tool_is_error detect_silence "$(jq -c '. + {min_s:0}' <<<"$SIL")")"
expect_eq "detect_silence: min_s 31 is an error" true "$(tool_is_error detect_silence "$(jq -c '. + {min_s:31}' <<<"$SIL")")"
expect_eq "detect_silence: wait_ms -1 is an error" true "$(tool_is_error detect_silence "$(jq -c '. + {wait_ms:-1}' <<<"$SIL")")"
expect_eq "detect_silence: wait_ms 20001 is an error" true "$(tool_is_error detect_silence "$(jq -c '. + {wait_ms:20001}' <<<"$SIL")")"
expect_eq "detect_silence: fractional wait_ms is an error" true "$(tool_is_error detect_silence "$(jq -c '. + {wait_ms:1.5}' <<<"$SIL")")"
expect_eq "detect_silence: path and track is an error" true "$(tool_is_error detect_silence "$(jq -c '. + {track:0}' <<<"$SIL")")"
expect_eq "detect_silence: neither path nor track is an error" true "$(tool_is_error detect_silence '{}')"
expect_eq "detect_silence: track without clip is an error" true "$(tool_is_error detect_silence '{"track":0}')"
expect_eq "detect_silence: relative path is an error" true "$(tool_is_error detect_silence '{"path":"silence.wav"}')"
expect_eq "detect_silence: missing file is an error" true "$(tool_is_error detect_silence "{\"path\":\"$MEDIA/nope.wav\"}")"
TS="$(tool_json get_timeline '{}')"
expect_eq "detect_silence clip: tone_av is track 0 clip 0" tone_av.mp4 "$(basename "$(echo "$TS" | jq -r '.tracks[0].clips[0].path // .tracks[0].clips[0].name')")"
C="$(job_result detect_silence '{"track":0,"clip":0}')"
expect_eq "detect_silence clip: two ranges" 2 "$(echo "$C" | jq -r '.ranges | length')"
expect_eq "detect_silence clip: path is the source file" "$MEDIA/tone_av.mp4" "$(echo "$C" | jq -r '.path')"
expect_eq "detect_silence clip: range 1 source start unchanged" true "$(echo "$C" | jq -r '(.ranges[0].start_s - 2.0) | fabs < 0.15')"
expect_eq "detect_silence clip: range 1 timeline start 50" true "$(echo "$C" | jq -r '(.ranges[0].timeline_frames[0].start - 50) | fabs <= 3')"
expect_eq "detect_silence clip: range 1 timeline end 100" true "$(echo "$C" | jq -r '(.ranges[0].timeline_frames[0].end - 100) | fabs <= 3')"
expect_eq "detect_silence clip: range 2 timeline start 175" true "$(echo "$C" | jq -r '(.ranges[1].timeline_frames[0].start - 175) | fabs <= 3')"
expect_eq "detect_silence clip: range 2 timeline end 212" true "$(echo "$C" | jq -r '(.ranges[1].timeline_frames[0].end - 212) | fabs <= 3')"
expect_eq "detect_silence clip: speech ranges carry timeline_frames" true "$(echo "$C" | jq -r '[.speech_ranges[] | has("timeline_frames")] | all')"

echo "== detect_silence and audio_levels on a trimmed clip"
tool_json timeline_trim '{"track":0,"clip":0,"edge":"in","delta":25,"ripple":true}' >/dev/null
expect_eq "trimmed clip: in is 25" 25 "$(tool_json get_timeline '{}' | jq -r '.tracks[0].clips[0].in')"
TC="$(job_result detect_silence '{"track":0,"clip":0}')"
expect_eq "trimmed clip: in_s about 1.0" true "$(echo "$TC" | jq -r '(.in_s - 1.0) | fabs < 0.01')"
expect_eq "trimmed clip: out_s about 10" true "$(echo "$TC" | jq -r '(.out_s - 10) | fabs < 0.1')"
expect_eq "trimmed clip: two ranges" 2 "$(echo "$TC" | jq -r '.ranges | length')"
expect_eq "trimmed clip: range 1 in source time about 2.0-4.0" true "$(echo "$TC" | jq -r '((.ranges[0].start_s - 2.0) | fabs < 0.15) and ((.ranges[0].end_s - 4.0) | fabs < 0.15)')"
expect_eq "trimmed clip: range 2 in source time about 7.0-8.5" true "$(echo "$TC" | jq -r '((.ranges[1].start_s - 7.0) | fabs < 0.15) and ((.ranges[1].end_s - 8.5) | fabs < 0.15)')"
expect_eq "trimmed clip: range 1 timeline frames 25-75" true "$(echo "$TC" | jq -r '((.ranges[0].timeline_frames[0].start - 25) | fabs <= 3) and ((.ranges[0].timeline_frames[0].end - 75) | fabs <= 3)')"
expect_eq "trimmed clip: range 2 timeline frames 150-187" true "$(echo "$TC" | jq -r '((.ranges[1].timeline_frames[0].start - 150) | fabs <= 3) and ((.ranges[1].timeline_frames[0].end - 187) | fabs <= 3)')"
expect_eq "trimmed clip: first speech range starts at in_s" true "$(echo "$TC" | jq -r '(.speech_ranges[0].start_s - 1.0) | fabs < 0.01')"
TV="$(job_result audio_levels '{"track":0,"clip":0,"buckets":10}')"
expect_eq "trimmed clip levels: in_s about 1.0" true "$(echo "$TV" | jq -r '(.in_s - 1.0) | fabs < 0.01')"
expect_eq "trimmed clip levels: out_s about 10" true "$(echo "$TV" | jq -r '(.out_s - 10) | fabs < 0.1')"
expect_eq "trimmed clip levels: first bucket starts at in_s" true "$(echo "$TV" | jq -r '(.buckets[0].start_s - 1.0) | fabs < 0.01')"
expect_eq "trimmed clip levels: first bucket (1.0-1.9 s) is tone" true "$(echo "$TV" | jq -r '(.buckets[0].rms_db + 23) | fabs < 3')"
expect_eq "trimmed clip levels: third bucket (2.8-3.7 s) is silent" true "$(echo "$TV" | jq -r '.buckets[2].rms_db < -80')"
tool_json timeline_trim '{"track":0,"clip":0,"edge":"in","delta":50,"ripple":true}' >/dev/null
TD="$(job_result detect_silence '{"track":0,"clip":0}')"
expect_eq "clip trimmed into the first silence: in_s about 3.0" true "$(echo "$TD" | jq -r '(.in_s - 3.0) | fabs < 0.01')"
expect_eq "clip trimmed into the first silence: range 1 is clipped to start at in_s" true "$(echo "$TD" | jq -r '(.ranges[0].start_s - 3.0) | fabs < 0.01')"
expect_eq "clip trimmed into the first silence: range 1 ends about 4.0" true "$(echo "$TD" | jq -r '(.ranges[0].end_s - 4.0) | fabs < 0.15')"
expect_eq "clip trimmed into the first silence: range 1 timeline frames 0-25" true "$(echo "$TD" | jq -r '((.ranges[0].timeline_frames[0].start) | fabs <= 3) and ((.ranges[0].timeline_frames[0].end - 25) | fabs <= 3)')"
expect_eq "clip trimmed into the first silence: range 2 timeline frames 101-137" true "$(echo "$TD" | jq -r '((.ranges[1].timeline_frames[0].start - 101) | fabs <= 3) and ((.ranges[1].timeline_frames[0].end - 137) | fabs <= 3)')"
expect_eq "clip trimmed into the first silence: no speech before the first range" 2 "$(echo "$TD" | jq -r '.speech_ranges | length')"

echo "== detect_silence on a clip of a locked track"
# No tool locks a track yet (Task 10 adds track_set), so lock it in a saved copy of the project
# and reopen that.
LOCKED="$TMP/locked.mlt"
tool_json save_project "{\"path\":\"$LOCKED\"}" >/dev/null
PL="$(grep -o '<track producer="[^"]*"' "$LOCKED" | grep -v background | head -1 | sed 's/.*producer="//; s/"//')"
sed -i "0,/<playlist id=\"$PL\"[^>]*>/s//&\n  <property name=\"shotcut:lock\">1<\/property>/" "$LOCKED"
tool_json open_project "{\"path\":\"$LOCKED\",\"discard_changes\":true}" >/dev/null
sleep 2
expect_eq "locked clip: track 0 is locked after reopening" true "$(tool_json get_timeline '{}' | jq -r '.tracks[0].locked')"
L="$(job_result detect_silence '{"track":0,"clip":0}')"
expect_eq "locked clip: detect_silence works on a locked track" 2 "$(echo "$L" | jq -r '.ranges | length')"
expect_eq "locked clip: timeline_frames of the trimmed clip" true "$(echo "$L" | jq -r '(.ranges[1].timeline_frames[0].start - 101) | fabs <= 3')"

echo "== audio_levels"
expect_eq "tools/list has audio_levels" true "$(rpc '{"jsonrpc":"2.0","id":3,"method":"tools/list"}' | jq -r '[.result.tools[].name] | contains(["audio_levels"])')"
V="$(tool_json audio_levels "$(jq -c '. + {wait_ms:20000}' <<<"$SIL")")"
expect_eq "audio_levels wait_ms: result in the same call" 60 "$(echo "$V" | jq -r '.buckets | length')"
expect_eq "audio_levels: path" "$MEDIA/silence.wav" "$(echo "$V" | jq -r '.path')"
expect_eq "audio_levels: integrated_lufs finite in -45..-15" true "$(echo "$V" | jq -r '.integrated_lufs > -45 and .integrated_lufs < -15')"
expect_eq "audio_levels: loudness_range_lu is a number" number "$(echo "$V" | jq -r '.loudness_range_lu | type')"
expect_eq "audio_levels: true_peak_dbtp within 1.5 of -20" true "$(echo "$V" | jq -r '(.true_peak_dbtp + 20) | fabs < 1.5')"
expect_eq "audio_levels: first bucket starts at in_s" true "$(echo "$V" | jq -r '.buckets[0].start_s < 0.001')"
expect_eq "audio_levels: last bucket ends at out_s" true "$(echo "$V" | jq -r '(.buckets[-1].end_s - .out_s) | fabs < 0.01')"
# 60 buckets over 10 s: bucket i covers i/6 .. (i+1)/6 s; silence 2.05-4.03 and 7.04-8.51
expect_eq "audio_levels: buckets inside silence are below -80 dB" true "$(echo "$V" | jq -r '[.buckets[] | select(.start_s > 2.3 and .end_s < 3.8) | .rms_db < -80] | (length > 0) and all')"
expect_eq "audio_levels: buckets inside second silence are below -80 dB" true "$(echo "$V" | jq -r '[.buckets[] | select(.start_s > 7.3 and .end_s < 8.3) | .rms_db < -80] | (length > 0) and all')"
expect_eq "audio_levels: buckets inside tone are within 3 of -23 dB rms" true "$(echo "$V" | jq -r '[.buckets[] | select(.start_s > 0.2 and .end_s < 1.8) | (.rms_db + 23) | fabs < 3] | (length > 0) and all')"
expect_eq "audio_levels: tone bucket peak near -20 dB" true "$(echo "$V" | jq -r '[.buckets[] | select(.start_s > 0.2 and .end_s < 1.8) | (.peak_db + 20) | fabs < 1.5] | all')"
expect_eq "audio_levels: identical second call is cached" true "$(tool_json audio_levels "$SIL" | jq -r '.cached')"
expect_eq "audio_levels buckets 7: length" 7 "$(job_result audio_levels "$(jq -c '. + {buckets:7}' <<<"$SIL")" | jq -r '.buckets | length')"
expect_eq "audio_levels buckets 600: length" 600 "$(job_result audio_levels "$(jq -c '. + {buckets:600}' <<<"$SIL")" | jq -r '.buckets | length')"
expect_eq "audio_levels buckets 1: length" 1 "$(job_result audio_levels "$(jq -c '. + {buckets:1}' <<<"$SIL")" | jq -r '.buckets | length')"
expect_eq "audio_levels without wait_ms returns a job" true "$(tool_json audio_levels "$(jq -c '. + {buckets:9}' <<<"$SIL")" | jq -r 'has("job_id")')"
expect_eq "audio_levels noaudio: error" true "$(tool_is_error audio_levels "$NA")"
expect_eq "audio_levels noaudio: message" "$MEDIA/noaudio.mp4 has no audio stream." "$(tool_text audio_levels "$NA")"
expect_eq "audio_levels: buckets 0 is an error" true "$(tool_is_error audio_levels "$(jq -c '. + {buckets:0}' <<<"$SIL")")"
expect_eq "audio_levels: buckets 601 is an error" true "$(tool_is_error audio_levels "$(jq -c '. + {buckets:601}' <<<"$SIL")")"
expect_eq "audio_levels: fractional buckets is an error" true "$(tool_is_error audio_levels "$(jq -c '. + {buckets:2.5}' <<<"$SIL")")"
expect_eq "audio_levels: wait_ms 20001 is an error" true "$(tool_is_error audio_levels "$(jq -c '. + {wait_ms:20001}' <<<"$SIL")")"
expect_eq "audio_levels: path and clip is an error" true "$(tool_is_error audio_levels "$(jq -c '. + {clip:0}' <<<"$SIL")")"
expect_eq "audio_levels: neither path nor track is an error" true "$(tool_is_error audio_levels '{}')"
VC="$(job_result audio_levels '{"track":0,"clip":0,"buckets":10}')"
expect_eq "audio_levels clip: 10 buckets" 10 "$(echo "$VC" | jq -r '.buckets | length')"
expect_eq "audio_levels clip (locked track): in_s about 3.0 (trimmed)" true "$(echo "$VC" | jq -r '(.in_s - 3.0) | fabs < 0.01')"
# a 600 s file must be bucketed over its whole length, not truncated by the output cap
ffmpeg -v error -nostdin -y -f lavfi -i "sine=frequency=440:duration=600:sample_rate=16000" "$TMP/long.wav"
VL="$(job_result audio_levels "$(jq -nc --arg p "$TMP/long.wav" '{path:$p,buckets:10,wait_ms:20000}')")"
expect_eq "audio_levels 600 s: last bucket ends near 600" true "$(echo "$VL" | jq -r '(.buckets[-1].end_s - 600) | fabs < 0.5')"
expect_eq "audio_levels 600 s: last bucket carries signal (sine 0.125 = -21 dB rms)" true "$(echo "$VL" | jq -r '(.buckets[-1].rms_db + 21) | fabs < 3')"

echo "== detect_scenes"
expect_eq "tools/list has detect_scenes" true "$(rpc '{"jsonrpc":"2.0","id":3,"method":"tools/list"}' | jq -r '[.result.tools[].name] | contains(["detect_scenes"])')"
SC="{\"path\":\"$MEDIA/scenes.mp4\"}"
D="$(tool_json detect_scenes "$(jq -c '. + {wait_ms:20000}' <<<"$SC")")"
expect_eq "detect_scenes wait_ms: exactly two scenes" 2 "$(echo "$D" | jq -r '.scenes | length')"
expect_eq "detect_scenes: path" "$MEDIA/scenes.mp4" "$(echo "$D" | jq -r '.path')"
expect_eq "detect_scenes: scene 1 near 3.0" true "$(echo "$D" | jq -r '(.scenes[0].time_s - 3.0) | fabs < 0.1')"
expect_eq "detect_scenes: scene 2 near 6.0" true "$(echo "$D" | jq -r '(.scenes[1].time_s - 6.0) | fabs < 0.1')"
expect_eq "detect_scenes: scores are 0..1" true "$(echo "$D" | jq -r '[.scenes[].score | . > 0.3 and . <= 1] | all')"
expect_eq "detect_scenes: hard cut scores 1" true "$(echo "$D" | jq -r '.scenes[1].score == 1')"
expect_eq "detect_scenes: scores have at most 3 decimals" true "$(echo "$D" | jq -r '[.scenes[].score | (. * 1000 | round) / 1000 == .] | all')"
expect_eq "detect_scenes: in_s 0" 0 "$(echo "$D" | jq -r '.in_s')"
expect_eq "detect_scenes: out_s near 9" true "$(echo "$D" | jq -r '(.out_s - 9) | fabs < 0.1')"
expect_eq "detect_scenes: no bogus scene at the start" true "$(echo "$D" | jq -r '[.scenes[] | select(.time_s < 1)] | length == 0')"
expect_eq "detect_scenes: path source has no timeline_frames" 0 "$(echo "$D" | jq -r '[.scenes[] | select(has("timeline_frames"))] | length')"
expect_eq "detect_scenes: first wait_ms call is not cached" null "$(echo "$D" | jq -c '.cached // null')"
expect_eq "detect_scenes: identical second call is cached" true "$(tool_json detect_scenes "$SC" | jq -r '.cached')"
expect_eq "detect_scenes: cached call returns the scenes at once" 2 "$(tool_json detect_scenes "$SC" | jq -r '.scenes | length')"
DJ="$(tool_json detect_scenes "$(jq -c '. + {threshold:0.31}' <<<"$SC")")"
expect_eq "detect_scenes without wait_ms returns a job" true "$(echo "$DJ" | jq -r 'has("job_id")')"
expect_eq "detect_scenes: get_job yields the scenes" 2 "$(wait_job "$(echo "$DJ" | jq -r '.job_id')" 60 | jq -r '.result.scenes | length')"
# the 3 s cut scores 0.64 and the 6 s cut 1.0
expect_eq "detect_scenes threshold 0.9: only the hard cut at 6 s" true "$(job_result detect_scenes "$(jq -c '. + {threshold:0.9}' <<<"$SC")" | jq -r '(.scenes | length == 1) and ((.scenes[0].time_s - 6.0) | fabs < 0.1)')"
expect_eq "detect_scenes threshold 0.6: both cuts" 2 "$(job_result detect_scenes "$(jq -c '. + {threshold:0.6}' <<<"$SC")" | jq -r '.scenes | length')"
expect_eq "detect_scenes threshold 0.05: still two scenes" 2 "$(job_result detect_scenes "$(jq -c '. + {threshold:0.05}' <<<"$SC")" | jq -r '.scenes | length')"
SPC="{\"path\":\"$MEDIA/space name.mp4\"}"
expect_eq "detect_scenes space name works" 2 "$(job_result detect_scenes "$(jq -c '. + {wait_ms:20000}' <<<"$SPC")" | jq -r '.scenes | length')"
AO="{\"path\":\"$MEDIA/audioonly.wav\"}"
expect_eq "detect_scenes audioonly: error" true "$(tool_is_error detect_scenes "$AO")"
expect_eq "detect_scenes audioonly: message" "$MEDIA/audioonly.wav has no video stream." "$(tool_text detect_scenes "$AO")"
expect_eq "detect_scenes: threshold 0.04 is an error" true "$(tool_is_error detect_scenes "$(jq -c '. + {threshold:0.04}' <<<"$SC")")"
expect_eq "detect_scenes: threshold 0.91 is an error" true "$(tool_is_error detect_scenes "$(jq -c '. + {threshold:0.91}' <<<"$SC")")"
expect_eq "detect_scenes: string threshold is an error" true "$(tool_is_error detect_scenes "$(jq -c '. + {threshold:"0.3"}' <<<"$SC")")"
expect_eq "detect_scenes: wait_ms -1 is an error" true "$(tool_is_error detect_scenes "$(jq -c '. + {wait_ms:-1}' <<<"$SC")")"
expect_eq "detect_scenes: wait_ms 20001 is an error" true "$(tool_is_error detect_scenes "$(jq -c '. + {wait_ms:20001}' <<<"$SC")")"
expect_eq "detect_scenes: fractional wait_ms is an error" true "$(tool_is_error detect_scenes "$(jq -c '. + {wait_ms:1.5}' <<<"$SC")")"
expect_eq "detect_scenes: path and track is an error" true "$(tool_is_error detect_scenes "$(jq -c '. + {track:0}' <<<"$SC")")"
expect_eq "detect_scenes: neither path nor track is an error" true "$(tool_is_error detect_scenes '{}')"
expect_eq "detect_scenes: relative path is an error" true "$(tool_is_error detect_scenes '{"path":"scenes.mp4"}')"
expect_eq "detect_scenes: missing file is an error" true "$(tool_is_error detect_scenes "{\"path\":\"$MEDIA/nope.mp4\"}")"

echo "== detect_scenes on clips"
# Reopen the project of the locked block without its lock, put scenes.mp4 on the timeline.
UNLOCKED="$TMP/unlocked.mlt"
grep -v 'shotcut:lock' "$LOCKED" > "$UNLOCKED"
tool_json open_project "{\"path\":\"$UNLOCKED\",\"discard_changes\":true}" >/dev/null
sleep 2
tool_json playlist_add "{\"path\":\"$MEDIA/scenes.mp4\"}" >/dev/null
tool_json timeline_add_playlist '{}' >/dev/null
scene_clip() { # the index of the last scenes.mp4 clip on track 0
  tool_json get_timeline '{}' | jq -r '[.tracks[0].clips | to_entries[] | select(.value.kind=="clip" and ((.value.path // .value.name) | test("scenes.mp4$")))] | last | .key'
}
clip_start() { tool_json get_timeline '{}' | jq -r ".tracks[0].clips[$1].start"; }
SI="$(scene_clip)"
S0="$(clip_start "$SI")"
SCC="$(job_result detect_scenes "{\"track\":0,\"clip\":$SI}")"
expect_eq "detect_scenes clip: path is the source file" "$MEDIA/scenes.mp4" "$(echo "$SCC" | jq -r '.path')"
expect_eq "detect_scenes clip: two scenes" 2 "$(echo "$SCC" | jq -r '.scenes | length')"
expect_eq "detect_scenes clip: source time 3.0 and 6.0" true "$(echo "$SCC" | jq -r '((.scenes[0].time_s - 3.0) | fabs < 0.1) and ((.scenes[1].time_s - 6.0) | fabs < 0.1)')"
expect_eq "detect_scenes clip: scene 1 timeline frame is clip start + 75" true "$(echo "$SCC" | jq -r "(.scenes[0].timeline_frames - $S0 - 75) | fabs <= 2")"
expect_eq "detect_scenes clip: scene 2 timeline frame is clip start + 150" true "$(echo "$SCC" | jq -r "(.scenes[1].timeline_frames - $S0 - 150) | fabs <= 2")"
expect_eq "detect_scenes clip: timeline_frames is a number" number "$(echo "$SCC" | jq -r '.scenes[0].timeline_frames | type')"
tool_json timeline_trim "{\"track\":0,\"clip\":$SI,\"edge\":\"in\",\"delta\":25,\"ripple\":true}" >/dev/null
expect_eq "detect_scenes trimmed: in is 25" 25 "$(tool_json get_timeline '{}' | jq -r ".tracks[0].clips[$SI].in")"
S1="$(clip_start "$SI")"
SCT="$(job_result detect_scenes "{\"track\":0,\"clip\":$SI}")"
expect_eq "detect_scenes trimmed: in_s about 1.0" true "$(echo "$SCT" | jq -r '(.in_s - 1.0) | fabs < 0.01')"
expect_eq "detect_scenes trimmed: two scenes" 2 "$(echo "$SCT" | jq -r '.scenes | length')"
expect_eq "detect_scenes trimmed: source time stays 3.0 and 6.0" true "$(echo "$SCT" | jq -r '((.scenes[0].time_s - 3.0) | fabs < 0.1) and ((.scenes[1].time_s - 6.0) | fabs < 0.1)')"
expect_eq "detect_scenes trimmed: scene 1 timeline frame is clip start + 50" true "$(echo "$SCT" | jq -r "(.scenes[0].timeline_frames - $S1 - 50) | fabs <= 2")"
expect_eq "detect_scenes trimmed: scene 2 timeline frame is clip start + 125" true "$(echo "$SCT" | jq -r "(.scenes[1].timeline_frames - $S1 - 125) | fabs <= 2")"
tool_json timeline_trim "{\"track\":0,\"clip\":$SI,\"edge\":\"in\",\"delta\":75,\"ripple\":true}" >/dev/null
SCE="$(job_result detect_scenes "{\"track\":0,\"clip\":$SI}")"
expect_eq "detect_scenes clip trimmed past the first cut: one scene" 1 "$(echo "$SCE" | jq -r '.scenes | length')"
expect_eq "detect_scenes clip trimmed past the first cut: in_s about 4.0" true "$(echo "$SCE" | jq -r '(.in_s - 4.0) | fabs < 0.01')"
expect_eq "detect_scenes clip trimmed past the first cut: scene at 6.0" true "$(echo "$SCE" | jq -r '(.scenes[0].time_s - 6.0) | fabs < 0.1')"
expect_eq "detect_scenes clip trimmed past the first cut: no scene at the clip start" true "$(echo "$SCE" | jq -r '[.scenes[] | select(.time_s < 4.5)] | length == 0')"

echo "== detect_scenes on a clip of a locked track"
LOCKED2="$TMP/locked2.mlt"
tool_json save_project "{\"path\":\"$LOCKED2\"}" >/dev/null
PL2="$(grep -o '<track producer="[^"]*"' "$LOCKED2" | grep -v background | head -1 | sed 's/.*producer="//; s/"//')"
sed -i "0,/<playlist id=\"$PL2\"[^>]*>/s//&\n  <property name=\"shotcut:lock\">1<\/property>/" "$LOCKED2"
tool_json open_project "{\"path\":\"$LOCKED2\",\"discard_changes\":true}" >/dev/null
sleep 2
expect_eq "locked scenes clip: track 0 is locked after reopening" true "$(tool_json get_timeline '{}' | jq -r '.tracks[0].locked')"
SL="$(job_result detect_scenes "{\"track\":0,\"clip\":$(scene_clip)}")"
expect_eq "locked scenes clip: detect_scenes works on a locked track" 1 "$(echo "$SL" | jq -r '.scenes | length')"
expect_eq "locked scenes clip: timeline_frames present" number "$(echo "$SL" | jq -r '.scenes[0].timeline_frames | type')"

echo "== transcribe: supplied transcripts"
expect_eq "tools/list has transcribe" true "$(rpc '{"jsonrpc":"2.0","id":3,"method":"tools/list"}' | jq -r '[.result.tools[].name] | contains(["transcribe"])')"
AOP="$MEDIA/audioonly.wav"
# supply PATH TRANSCRIPT_JSON: the transcribe arguments that store TRANSCRIPT_JSON for PATH
supply() { jq -nc --arg p "$1" --argjson t "$2" '{path:$p,transcript:$t}'; }
TW='{"segments":[{"start_s":0.5,"end_s":2.0,"text":" Hello there ","words":[{"start_s":0.5,"end_s":1.0,"text":"Hello"},{"start_s":1.1,"end_s":2.0,"text":"there"}]},{"start_s":4.0,"end_s":6.5,"text":"General Kenobi","words":[{"start_s":4.0,"end_s":5.0,"text":"General"},{"start_s":5.0,"end_s":6.5,"text":"Kenobi"}]}]}'
ST="$(tool_json transcribe "$(supply "$AOP" "$TW")")"
expect_eq "transcribe supplied: accepted with word granularity" word "$(echo "$ST" | jq -r '.granularity')"
expect_eq "transcribe supplied: path" "$AOP" "$(echo "$ST" | jq -r '.path')"
expect_eq "transcribe supplied: two segments" 2 "$(echo "$ST" | jq -r '.segments | length')"
expect_eq "transcribe supplied: text is trimmed" "Hello there" "$(echo "$ST" | jq -r '.segments[0].text')"
SR="$(tool_json transcribe "$(jq -nc --arg p "$AOP" '{path:$p}')")"
expect_eq "transcribe path only: returns the supplied transcript" word "$(echo "$SR" | jq -r '.granularity')"
expect_eq "transcribe path only: segment times" "0.5 2 4 6.5" "$(echo "$SR" | jq -r '[.segments[] | .start_s, .end_s] | map(tostring) | join(" ")')"
expect_eq "transcribe path only: word times" "0.5 1 1.1 2 4 5 5 6.5" "$(echo "$SR" | jq -r '[.segments[].words[] | .start_s, .end_s] | map(tostring) | join(" ")')"
expect_eq "transcribe path only: word texts" "Hello there General Kenobi" "$(echo "$SR" | jq -r '[.segments[].words[].text] | join(" ")')"
expect_eq "transcribe path only: supplied flag" true "$(echo "$SR" | jq -r '.supplied')"
expect_eq "transcribe path only: no job_id" false "$(echo "$SR" | jq -r 'has("job_id")')"
TSEG='{"segments":[{"start_s":0,"end_s":1,"text":"one","words":[{"start_s":0,"end_s":1,"text":"one"}]},{"start_s":1,"end_s":2,"text":"two"}]}'
expect_eq "transcribe supplied: a segment without words gives segment granularity" segment "$(tool_json transcribe "$(supply "$TMP/note.txt" "$TSEG")" | jq -r '.granularity')"
expect_eq "transcribe supplied: stored per path (note.txt)" segment "$(tool_json transcribe "$(jq -nc --arg p "$TMP/note.txt" '{path:$p}')" | jq -r '.granularity')"
expect_eq "transcribe supplied: other path keeps its own transcript" word "$(tool_json transcribe "$(jq -nc --arg p "$AOP" '{path:$p}')" | jq -r '.granularity')"
bad_transcript() { # NAME TRANSCRIPT_JSON
  expect_eq "transcribe supplied invalid: $1" true "$(tool_is_error transcribe "$(supply "$AOP" "$2")")"
}
bad_transcript "empty segments" '{"segments":[]}'
bad_transcript "no segments" '{}'
bad_transcript "segments not an array" '{"segments":{}}'
bad_transcript "transcript not an object" '[]'
bad_transcript "start_s equals end_s" '{"segments":[{"start_s":1,"end_s":1,"text":"a"}]}'
bad_transcript "start_s after end_s" '{"segments":[{"start_s":2,"end_s":1,"text":"a"}]}'
bad_transcript "negative start_s" '{"segments":[{"start_s":-1,"end_s":1,"text":"a"}]}'
bad_transcript "end_s beyond 86400" '{"segments":[{"start_s":0,"end_s":86400.5,"text":"a"}]}'
bad_transcript "string start_s" '{"segments":[{"start_s":"0","end_s":1,"text":"a"}]}'
bad_transcript "missing end_s" '{"segments":[{"start_s":0,"text":"a"}]}'
bad_transcript "empty text" '{"segments":[{"start_s":0,"end_s":1,"text":""}]}'
bad_transcript "blank text" '{"segments":[{"start_s":0,"end_s":1,"text":"   "}]}'
bad_transcript "missing text" '{"segments":[{"start_s":0,"end_s":1}]}'
bad_transcript "text over 2000 chars" "$(jq -nc '{segments:[{start_s:0,end_s:1,text:("a" * 2001)}]}')"
bad_transcript "word before the segment" '{"segments":[{"start_s":1,"end_s":2,"text":"a","words":[{"start_s":0.5,"end_s":1.5,"text":"a"}]}]}'
bad_transcript "word after the segment" '{"segments":[{"start_s":1,"end_s":2,"text":"a","words":[{"start_s":1.5,"end_s":2.5,"text":"a"}]}]}'
bad_transcript "words decreasing" '{"segments":[{"start_s":0,"end_s":3,"text":"a b","words":[{"start_s":1,"end_s":2,"text":"a"},{"start_s":0.5,"end_s":1,"text":"b"}]}]}'
bad_transcript "word start_s after end_s" '{"segments":[{"start_s":0,"end_s":3,"text":"a","words":[{"start_s":2,"end_s":1,"text":"a"}]}]}'
bad_transcript "word with empty text" '{"segments":[{"start_s":0,"end_s":3,"text":"a","words":[{"start_s":1,"end_s":2,"text":""}]}]}'
bad_transcript "words not an array" '{"segments":[{"start_s":0,"end_s":3,"text":"a","words":{}}]}'
bad_transcript "unsorted segments" '{"segments":[{"start_s":2,"end_s":3,"text":"b"},{"start_s":0,"end_s":1,"text":"a"}]}'
# built by jq itself: a transcript this large does not fit in one command-line argument
many() { jq -nc --arg p "$1" --argjson n "$2" '{path:$p,transcript:{segments:[range($n) | {start_s:., end_s:(. + 0.5), text:"w"}]}}'; }
expect_eq "transcribe supplied invalid: 5001 segments" true "$(tool_is_error transcribe "$(many "$AOP" 5001)")"
expect_eq "transcribe supplied: 5000 segments accepted" 5000 "$(tool_json transcribe "$(many "$TMP/note.txt" 5000)" | jq -r '.segments | length')"
expect_eq "transcribe supplied: overlapping segments accepted" segment "$(tool_json transcribe "$(supply "$TMP/note.txt" '{"segments":[{"start_s":0,"end_s":2,"text":"a"},{"start_s":1,"end_s":3,"text":"b"}]}')" | jq -r '.granularity')"
expect_eq "transcribe supplied invalid: transcript with track and clip" true "$(tool_is_error transcribe "$(jq -nc --argjson t "$TW" '{track:0,clip:0,transcript:$t}')")"
expect_eq "transcribe supplied invalid: transcript with path and track" true "$(tool_is_error transcribe "$(supply "$AOP" "$TW" | jq -c '. + {track:0}')")"
expect_eq "transcribe supplied invalid: transcript with path and clip" true "$(tool_is_error transcribe "$(supply "$AOP" "$TW" | jq -c '. + {clip:0}')")"
expect_eq "transcribe supplied invalid: transcript with language" true "$(tool_is_error transcribe "$(supply "$AOP" "$TW" | jq -c '. + {language:"en"}')")"
expect_eq "transcribe supplied invalid: transcript with wait_ms" true "$(tool_is_error transcribe "$(supply "$AOP" "$TW" | jq -c '. + {wait_ms:0}')")"
expect_eq "transcribe supplied invalid: transcript without path" true "$(tool_is_error transcribe "{\"transcript\":$TW}")"
expect_eq "transcribe supplied invalid: relative path" true "$(tool_is_error transcribe "$(supply "audioonly.wav" "$TW")")"
expect_eq "transcribe supplied invalid: missing file" true "$(tool_is_error transcribe "$(supply "$MEDIA/nope.wav" "$TW")")"
expect_eq "transcribe: a rejected transcript keeps the stored one" "Hello there General Kenobi" "$(tool_json transcribe "$(jq -nc --arg p "$AOP" '{path:$p}')" | jq -r '[.segments[].words[].text] | join(" ")')"
expect_eq "transcribe: language english is an error" true "$(tool_is_error transcribe "$(jq -nc --arg p "$MEDIA/tone_av.mp4" '{path:$p,language:"english"}')")"
expect_eq "transcribe: three-letter language fra is an error" true "$(tool_is_error transcribe "$(jq -nc --arg p "$MEDIA/tone_av.mp4" '{path:$p,language:"fra"}')")"
expect_eq "transcribe: one-letter language is an error" true "$(tool_is_error transcribe "$(jq -nc --arg p "$MEDIA/tone_av.mp4" '{path:$p,language:"e"}')")"
expect_eq "transcribe supplied: a zero-length word is accepted" word "$(tool_json transcribe "$(supply "$TMP/note.txt" '{"segments":[{"start_s":1,"end_s":2,"text":"a b","words":[{"start_s":1,"end_s":1,"text":"a"},{"start_s":1,"end_s":2,"text":"b"}]}]}')" | jq -r '.granularity')"
bad_transcript "zero-length segment" '{"segments":[{"start_s":1,"end_s":1,"text":"a","words":[{"start_s":1,"end_s":1,"text":"a"}]}]}'
expect_eq "transcribe: language EN is an error" true "$(tool_is_error transcribe "$(jq -nc --arg p "$MEDIA/tone_av.mp4" '{path:$p,language:"EN"}')")"
expect_eq "transcribe: numeric language is an error" true "$(tool_is_error transcribe "$(jq -nc --arg p "$MEDIA/tone_av.mp4" '{path:$p,language:5}')")"
expect_eq "transcribe: wait_ms 20001 is an error" true "$(tool_is_error transcribe "$(jq -nc --arg p "$MEDIA/tone_av.mp4" '{path:$p,wait_ms:20001}')")"
expect_eq "transcribe: neither path nor track is an error" true "$(tool_is_error transcribe '{}')"
expect_eq "transcribe: path and track is an error" true "$(tool_is_error transcribe "$(jq -nc --arg p "$MEDIA/tone_av.mp4" '{path:$p,track:0}')")"

echo "== transcribe: Whisper"
TONE="$(jq -nc --arg p "$MEDIA/tone_av.mp4" '{path:$p}')"
# A job id from a new, uncached detect_silence call (to prove that no job was created in between).
new_job_id() { tool_json detect_silence "$(jq -c ". + {noise_db:$1}" <<<"$SIL")" | jq -r '.job_id'; }
tmp_dirs() { ls -d "${TMPDIR:-/tmp}"/shotcut-transcribe-* 2>/dev/null | wc -l; }
TD0="$(tmp_dirs)"
J1="$(new_job_id -41)"
NC="$(tool_text transcribe "$TONE")"
J2="$(new_job_id -42)"
if [ "${NC#Whisper is not configured}" != "$NC" ]; then
  expect_eq "transcribe not configured: is an error" true "$(tool_is_error transcribe "$TONE")"
  expect_eq "transcribe not configured: says to supply a transcript" true "$(echo "$NC" | grep -q 'supply a transcript' && echo true || echo false)"
  expect_eq "transcribe not configured: names the Speech to Text settings" true "$(echo "$NC" | grep -q 'Speech to Text' && echo true || echo false)"
  expect_eq "transcribe not configured: no job was created" "$((J1 + 1))" "$J2"
  expect_eq "transcribe not configured: clip source too" true "$(tool_text transcribe '{"track":0,"clip":0,"language":"fr"}' | grep -q '^Whisper is not configured' && echo true || echo false)"
  echo "SKIP (no whisper): Whisper transcription checks (export WHISPER_EXE and WHISPER_MODEL to the harness)"
else
  echo "SKIP (whisper configured): not-configured checks"
  # the call above started a job on tone_av.mp4
  expect_eq "transcribe tone_av: the job ends" true "$(wait_job "$(echo "$NC" | jq -r '.job_id')" 120 | jq -r '.state != "running"')"
  JFK_SRC="${JFK_WAV:-$HOME/GitHub/whisper.cpp/samples/jfk.wav}"
  if [ ! -f "$JFK_SRC" ]; then
    fail "transcribe whisper" "$JFK_SRC is missing (set JFK_WAV)"
  else
    cp "$JFK_SRC" "$MEDIA/jfk.wav"
    JF="$(jq -nc --arg p "$MEDIA/jfk.wav" '{path:$p}')"
    W="$(job_result transcribe "$(jq -c '. + {wait_ms:20000}' <<<"$JF")")"
    norm='ascii_downcase | gsub("[^a-z ]"; "") | gsub(" +"; " ")'
    expect_eq "transcribe jfk: granularity word" word "$(echo "$W" | jq -r '.granularity')"
    expect_eq "transcribe jfk: language en" en "$(echo "$W" | jq -r '.language')"
    expect_eq "transcribe jfk: path" "$MEDIA/jfk.wav" "$(echo "$W" | jq -r '.path')"
    expect_eq "transcribe jfk: text has the famous line" true "$(echo "$W" | jq -r "[.segments[].text] | join(\" \") | $norm | contains(\"ask not what your country can do for you\")")"
    expect_eq "transcribe jfk: at least 15 words" true "$(echo "$W" | jq -r '[.segments[].words[]] | length >= 15')"
    expect_eq "transcribe jfk: words rebuild each segment's text" true "$(echo "$W" | jq -r '[.segments[] | ([.words[].text] | join(" ") | gsub(" +"; " ")) == (.text | gsub(" +"; " "))] | all')"
    expect_eq "transcribe jfk: no special tokens in words" 0 "$(echo "$W" | jq -r '[.segments[].words[] | select(.text | test("^\\[_|^$"))] | length')"
    expect_eq "transcribe jfk: words inside their segment" true "$(echo "$W" | jq -r '[.segments[] as $s | $s.words[] | .start_s >= $s.start_s and .end_s <= $s.end_s and .start_s <= .end_s] | all')"
    expect_eq "transcribe jfk: word starts non-decreasing" true "$(echo "$W" | jq -r '[.segments[].words[].start_s] | . == sort')"
    expect_eq "transcribe jfk: span within the 11 s file" true "$(echo "$W" | jq -r '(.segments[0].start_s >= 0) and (.segments[-1].end_s <= 11.05)')"
    expect_eq "transcribe jfk: path source has no timeline_frames" 0 "$(echo "$W" | jq -r '[.segments[] | select(has("timeline_frames"))] | length')"
    expect_eq "transcribe jfk: \"country\" is near 5.7 s" true "$(echo "$W" | jq -r '[.segments[].words[] | select(.text | test("^country"))][0].start_s | (. - 5.74) | fabs < 0.5')"
    expect_eq "transcribe jfk: punctuation does not stretch a word (Americans, ends near 2.11 s)" true "$(echo "$W" | jq -r '[.segments[].words[] | select(.text | test("^Americans"))][0].end_s | (. - 2.11) | fabs < 0.3')"
    expect_eq "transcribe jfk: no word is longer than 2 s" true "$(echo "$W" | jq -r '[.segments[].words[] | .end_s - .start_s < 2] | all')"
    expect_eq "transcribe jfk: the result round-trips as a supplied transcript" word "$(tool_json transcribe "$(jq -c --arg p "$TMP/note.txt" '{path:$p,transcript:{segments:.segments}}' <<<"$W")" | jq -r '.granularity')"
    expect_eq "transcribe jfk: first call is not cached" null "$(echo "$W" | jq -c '.cached // null')"
    expect_eq "transcribe jfk: second call is cached" true "$(tool_json transcribe "$JF" | jq -r '.cached')"
    sleep 0.5
    expect_eq "transcribe jfk: the temporary folder is removed" "$TD0" "$(tmp_dirs)"
    COUNTRY="$(echo "$W" | jq -r '[.segments[].words[] | select(.text | test("^country"))][0].start_s')"

    ZZ="$(tool_json transcribe "$(jq -c '. + {language:"zz",wait_ms:20000}' <<<"$JF")" 2>/dev/null)"
    [ -n "$ZZ" ] && ZZ="$(wait_job "$(echo "$ZZ" | jq -r '.job_id')" 60 | jq -r '.error')"
    [ -n "$ZZ" ] || ZZ="$(tool_text transcribe "$(jq -c '. + {language:"zz",wait_ms:20000}' <<<"$JF")")"
    expect_eq "transcribe language zz: the error has whisper-cli's message" true "$(echo "$ZZ" | grep -q "unknown language 'zz'" && echo true || echo false)"
    ffmpeg -v error -nostdin -y -f lavfi -i anullsrc=r=16000:cl=mono -t 3 "$MEDIA/hush.wav"
    HU="$(job_result transcribe "$(jq -nc --arg p "$MEDIA/hush.wav" '{path:$p,wait_ms:20000}')")"
    expect_eq "transcribe silence: no segments, word granularity" "word 0" "$(echo "$HU" | jq -r '"\(.granularity) \(.segments | length)"')"
    expect_eq "transcribe noaudio: error" true "$(tool_is_error transcribe "$(jq -nc --arg p "$MEDIA/noaudio.mp4" '{path:$p}')")"
    expect_eq "transcribe noaudio: message" "$MEDIA/noaudio.mp4 has no audio stream." "$(tool_text transcribe "$(jq -nc --arg p "$MEDIA/noaudio.mp4" '{path:$p}')")"

    # jfk.wav on the timeline, trimmed by 2 s at its start
    tool_json open_project "{\"path\":\"$UNLOCKED\",\"discard_changes\":true}" >/dev/null
    sleep 2
    tool_json playlist_add "{\"path\":\"$MEDIA/jfk.wav\"}" >/dev/null
    tool_json timeline_add_playlist '{}' >/dev/null
    jfk_clip() {
      tool_json get_timeline '{}' | jq -r '[.tracks[0].clips | to_entries[] | select(.value.kind=="clip" and ((.value.path // .value.name) | test("jfk.wav$")))] | last | .key'
    }
    JI="$(jfk_clip)"
    tool_json timeline_trim "{\"track\":0,\"clip\":$JI,\"edge\":\"in\",\"delta\":50,\"ripple\":true}" >/dev/null
    expect_eq "transcribe trimmed clip: in is 50" 50 "$(tool_json get_timeline '{}' | jq -r ".tracks[0].clips[$JI].in")"
    J0="$(clip_start "$JI")"
    WT="$(job_result transcribe "{\"track\":0,\"clip\":$JI,\"wait_ms\":20000,\"language\":\"en\"}")"
    expect_eq "transcribe trimmed clip: in_s about 2.0" true "$(echo "$WT" | jq -r '(.in_s - 2.0) | fabs < 0.01')"
    expect_eq "transcribe trimmed clip: out_s about 11" true "$(echo "$WT" | jq -r '(.out_s - 11) | fabs < 0.05')"
    expect_eq "transcribe trimmed clip: granularity word" word "$(echo "$WT" | jq -r '.granularity')"
    expect_eq "transcribe trimmed clip: text has the second half" true "$(echo "$WT" | jq -r "[.segments[].text] | join(\" \") | $norm | contains(\"ask what you can do for your country\")")"
    expect_eq "transcribe trimmed clip: all times in source seconds from in_s" true "$(echo "$WT" | jq -r '[.segments[] | .start_s, .end_s, (.words[] | .start_s, .end_s)] | all(. >= 1.999 and . <= 11.05)')"
    expect_eq "transcribe trimmed clip: \"country\" stays at its source time" true "$(echo "$WT" | jq -r "[.segments[].words[] | select(.text | test(\"^country\"))][0].start_s | (. - $COUNTRY) | fabs < 0.5")"
    expect_eq "transcribe trimmed clip: words inside their segment" true "$(echo "$WT" | jq -r '[.segments[] as $s | $s.words[] | .start_s >= $s.start_s and .end_s <= $s.end_s] | all')"
    expect_eq "transcribe trimmed clip: every segment and word has timeline_frames" true "$(echo "$WT" | jq -r '[.segments[] | has("timeline_frames"), (.words[] | has("timeline_frames"))] | all')"
    expect_eq "transcribe trimmed clip: word timeline frames = clip start + (start_s - 2) * 25" true "$(echo "$WT" | jq -r "[.segments[].words[] | select(.timeline_frames | length > 0) | (.timeline_frames[0].start - ($J0 + (.start_s * 25 | round) - 50)) | fabs <= 1] | (length > 10) and all")"
    expect_eq "transcribe trimmed clip: segment timeline frames start at the clip" true "$(echo "$WT" | jq -r "(.segments[0].timeline_frames[0].start - ($J0 + (.segments[0].start_s * 25 | round) - 50)) | fabs <= 1")"
    sleep 0.5
    expect_eq "transcribe trimmed clip: the temporary folder is removed" "$TD0" "$(tmp_dirs)"

    LOCKED3="$TMP/locked3.mlt"
    tool_json save_project "{\"path\":\"$LOCKED3\"}" >/dev/null
    PL3="$(grep -o '<track producer="[^"]*"' "$LOCKED3" | grep -v background | head -1 | sed 's/.*producer="//; s/"//')"
    sed -i "0,/<playlist id=\"$PL3\"[^>]*>/s//&\n  <property name=\"shotcut:lock\">1<\/property>/" "$LOCKED3"
    tool_json open_project "{\"path\":\"$LOCKED3\",\"discard_changes\":true}" >/dev/null
    sleep 2
    expect_eq "transcribe locked clip: track 0 is locked" true "$(tool_json get_timeline '{}' | jq -r '.tracks[0].locked')"
    WL="$(job_result transcribe "{\"track\":0,\"clip\":$(jfk_clip),\"wait_ms\":20000,\"language\":\"en\"}")"
    expect_eq "transcribe locked clip: works on a locked track" word "$(echo "$WL" | jq -r '.granularity')"
    expect_eq "transcribe locked clip: in_s about 2.0" true "$(echo "$WL" | jq -r '(.in_s - 2.0) | fabs < 0.01')"
  fi
fi

echo "== get_overview"
expect_eq "tools/list has get_overview" true "$(rpc '{"jsonrpc":"2.0","id":3,"method":"tools/list"}' | jq -r '[.result.tools[].name] | contains(["get_overview"])')"
# The unlocked project from the scenes block, emptied, then scenes.mp4 alone on track 0:
# red frames 0-74, green 75-149, blue 150-224.
tool_json open_project "{\"path\":\"$UNLOCKED\",\"discard_changes\":true}" >/dev/null
sleep 2
for _ in $(seq 1 20); do
  K="$(tool_json get_timeline '{}' | jq -r '[.tracks[0].clips | to_entries[] | select(.value.kind!="blank")] | last | .key // empty')"
  [ -z "$K" ] && break
  tool_json timeline_remove "{\"track\":0,\"clip\":$K}" >/dev/null
done
expect_eq "get_overview: an empty timeline is an error" true "$(tool_is_error get_overview '{}')"
expect_eq "get_overview: empty timeline message" "The timeline is empty." "$(tool_text get_overview '{}')"
tool_json playlist_add "{\"path\":\"$MEDIA/scenes.mp4\"}" >/dev/null
tool_json timeline_add_playlist '{}' >/dev/null
for _ in $(seq 1 20); do
  K="$(tool_json get_timeline '{}' | jq -r '[.tracks[0].clips | to_entries[] | select(.value.kind!="blank" and (((.value.path // .value.name) | test("/scenes.mp4$")) | not))] | first | .key // empty')"
  [ -z "$K" ] && break
  tool_json timeline_remove "{\"track\":0,\"clip\":$K}" >/dev/null
done
for _ in $(seq 1 5); do # extra scenes.mp4 clips, if any
  K="$(tool_json get_timeline '{}' | jq -r '[.tracks[0].clips | to_entries[] | select(.value.kind!="blank")] | if length > 1 then last.key else empty end')"
  [ -z "$K" ] && break
  tool_json timeline_remove "{\"track\":0,\"clip\":$K}" >/dev/null
done
expect_eq "get_overview fixture: scenes.mp4 alone, frames 0-224" "1 0 225 225" "$(tool_json get_timeline '{}' | jq -r '"\([.tracks[0].clips[] | select(.kind!="blank")] | length) \(.tracks[0].clips[0].start) \(.tracks[0].clips[0].duration) \(.duration)"')"
ov_call() { call_tool get_overview "$1" > "$TMP/ov.json"; jq -r '.result.content[0].data // empty' "$TMP/ov.json" | base64 -d > "$2"; }
ov_info() { jq -c '.result.content[1].text | fromjson' "$TMP/ov.json"; }
img_size() { ffprobe -v error -select_streams v:0 -show_entries stream=width,height -of csv=p=0:s=x "$1"; }
gf_call() { call_tool get_frame "$1" | jq -r '.result.content[0].data' | base64 -d > "$2"; }
# gray_rows FILE W H X Y: the luma of that rectangle, one line of W values per row
gray_rows() { ffmpeg -v error -i "$1" -vf "crop=$2:$3:$4:$5,format=gray" -f rawvideo -pix_fmt gray - | od -An -v -tu1 -w"$2"; }
bright_count() { gray_rows "$@" | awk '{for (i = 1; i <= NF; i++) if ($i > 200) n++} END {print n + 0}'; }
# bright_cols FILE W H X Y: per column, # when any pixel is brighter than 200, else .
bright_cols() { gray_rows "$@" | awk -v w="$2" '{for (i = 1; i <= NF; i++) if ($i > 200) b[i] = 1} END {for (i = 1; i <= w; i++) printf "%s", (b[i] ? "#" : "."); print ""}'; }
# label_ok COLS: the label starts at the left edge (timecode drawn) and its widest inner gap,
# the space between timecode and frame number, is at least 5 px (the parts do not overlap).
label_ok() {
  local cols="$1" first gap
  first="$(echo "$cols" | sed 's/#.*//' | tr -d '\n' | wc -c)"
  gap="$(echo "$cols" | sed 's/^\.*//; s/\.*$//' | grep -o '\.\+' | awk '{if (length > m) m = length} END {print m + 0}')"
  if [ "$first" -lt 10 ] && [ "$gap" -ge 5 ]; then echo true; else echo "false(first=$first gap=$gap $cols)"; fi
}
# rgb_class FILE W H X Y: R, G, B or ? for the mean colour of that rectangle
rgb_class() {
  local hex r g b
  hex="$(ffmpeg -v error -i "$1" -vf "crop=$2:$3:$4:$5,scale=1:1" -f rawvideo -pix_fmt rgb24 - | xxd -p)"
  r=$((16#${hex:0:2})); g=$((16#${hex:2:2})); b=$((16#${hex:4:2}))
  if [ "$r" -gt 150 ] && [ "$g" -lt 90 ] && [ "$b" -lt 90 ]; then echo R
  elif [ "$g" -gt 150 ] && [ "$r" -lt 90 ] && [ "$b" -lt 90 ]; then echo G
  elif [ "$b" -gt 150 ] && [ "$r" -lt 90 ] && [ "$g" -lt 90 ]; then echo B
  else echo "?($hex)"; fi
}
gf_call '{"position":40}' "$TMP/gf40a.jpg"
tool_json pause '{"position":37}' >/dev/null
T0=$(date +%s%N)
ov_call '{}' "$TMP/ov.jpg"
T1=$(date +%s%N)
echo "      (get_overview 4x4 took $(( (T1 - T0) / 1000000 )) ms)"
expect_eq "get_overview: not an error" false "$(jq -r '.result.isError' "$TMP/ov.json")"
expect_eq "get_overview: image content" image "$(jq -r '.result.content[0].type' "$TMP/ov.json")"
expect_eq "get_overview: jpeg by default" image/jpeg "$(jq -r '.result.content[0].mimeType' "$TMP/ov.json")"
expect_eq "get_overview: JPEG signature" ffd8 "$(head -c 2 "$TMP/ov.jpg" | xxd -p)"
expect_eq "get_overview: size 4*320+2*3 by 4*240+2*3" 1286x966 "$(img_size "$TMP/ov.jpg")"
OI="$(ov_info)"
expect_eq "get_overview: text reports columns rows tile_width" "4 4 320" "$(echo "$OI" | jq -r '"\(.columns) \(.rows) \(.tile_width)"')"
expect_eq "get_overview: 16 positions" 16 "$(echo "$OI" | jq -r '.positions | length')"
expect_eq "get_overview: positions from 0 to the last frame" "0 224" "$(echo "$OI" | jq -r '"\(.positions[0]) \(.positions[-1])"')"
expect_eq "get_overview: positions strictly increasing" true "$(echo "$OI" | jq -r '[.positions as $p | range(1; $p | length) | $p[.] > $p[. - 1]] | all')"
expect_eq "get_overview: positions are round(i*224/15)" "0,15,30,45,60,75,90,105,119,134,149,164,179,194,209,224" "$(echo "$OI" | jq -r '.positions | join(",")')"
CLASSES=""
for i in $(seq 0 15); do
  CLASSES="$CLASSES$(rgb_class "$TMP/ov.jpg" 160 120 $(( (i % 4) * 322 + 80 )) $(( (i / 4) * 242 + 40 )))"
done
expect_eq "get_overview: tile colours follow the clip (red, green, blue)" RRRRRGGGGGGBBBBB "$CLASSES"
expect_eq "get_overview: label bar darkens the bottom of the first tile" true "$(c="$(rgb_class "$TMP/ov.jpg" 300 6 10 230)"; [ "$c" != R ] && echo true || echo false)"
# Label text: white glyphs (luma > 200) in the bottom 16 rows of the first tile's bar (13 px font,
# 21 px bar on a 240 px tile); a PNG so that JPEG does not blur thin glyphs. Needs fonts.
if [ "$(fc-list 2>/dev/null | wc -l)" -gt 0 ]; then
  ov_call '{"columns":2,"rows":1,"format":"png"}' "$TMP/ovlabel.png"
  expect_eq "get_overview: the label bar holds text (at least 40 bright pixels)" true "$([ "$(bright_count "$TMP/ovlabel.png" 320 16 0 222)" -ge 40 ] && echo true || echo false)"
else
  echo "SKIP  get_overview label text (no fonts installed)"
fi
ST="$(tool_json get_state '{}')"
expect_eq "get_overview: playhead restored" 37 "$(echo "$ST" | jq -r '.position')"
expect_eq "get_overview: still paused" false "$(echo "$ST" | jq -r '.playing')"
gf_call '{"position":40}' "$TMP/gf40b.jpg"
expect_eq "get_frame after get_overview: same position gives the identical image" true "$([ "$(md5sum < "$TMP/gf40a.jpg")" = "$(md5sum < "$TMP/gf40b.jpg")" ] && echo true || echo false)"
expect_eq "get_frame after get_overview: frame 40 is red" R "$(rgb_class "$TMP/gf40b.jpg" 100 100 110 70)"
gf_call '{"position":200}' "$TMP/gf200.jpg"
expect_eq "get_frame after get_overview: frame 200 is blue" B "$(rgb_class "$TMP/gf200.jpg" 100 100 110 70)"
gf_call '{"position":100}' "$TMP/gf100.jpg"
expect_eq "get_frame after get_overview: frame 100 is green" G "$(rgb_class "$TMP/gf100.jpg" 100 100 110 70)"
ov_call '{}' "$TMP/ov2.jpg"
expect_eq "get_overview: a second call gives the same image" true "$([ "$(md5sum < "$TMP/ov.jpg")" = "$(md5sum < "$TMP/ov2.jpg")" ] && echo true || echo false)"
expect_eq "get_overview: playhead stays where get_frame left it" 100 "$(tool_json get_state '{}' | jq -r '.position')"
ov_call '{"columns":2,"rows":1}' "$TMP/ov21.jpg"
expect_eq "get_overview 2x1: two positions" "0,224" "$(ov_info | jq -r '.positions | join(",")')"
expect_eq "get_overview 2x1: size" 642x240 "$(img_size "$TMP/ov21.jpg")"
expect_eq "get_overview 2x1: red then blue" RB "$(rgb_class "$TMP/ov21.jpg" 160 120 80 40)$(rgb_class "$TMP/ov21.jpg" 160 120 402 40)"
ov_call '{"columns":1,"rows":1,"tile_width":96,"format":"png"}' "$TMP/ov11.png"
expect_eq "get_overview 1x1: one position at 0" 0 "$(ov_info | jq -r '.positions | join(",")')"
expect_eq "get_overview png: mime" image/png "$(jq -r '.result.content[0].mimeType' "$TMP/ov.json")"
expect_eq "get_overview png: signature" 89504e47 "$(head -c 4 "$TMP/ov11.png" | xxd -p)"
expect_eq "get_overview png: tile_width 96 gives 96x72" 96x72 "$(img_size "$TMP/ov11.png")"
T0=$(date +%s%N)
ov_call '{"columns":8,"rows":8,"tile_width":96}' "$TMP/ov88.jpg"
T1=$(date +%s%N)
echo "      (get_overview 8x8 took $(( (T1 - T0) / 1000000 )) ms)"
expect_eq "get_overview 8x8: 64 positions" 64 "$(ov_info | jq -r '.positions | length')"
expect_eq "get_overview 8x8: size" 782x590 "$(img_size "$TMP/ov88.jpg")"
ov_call '{"columns":8,"rows":8,"tile_width":640}' "$TMP/ovbig.jpg"
expect_eq "get_overview 8x8 at 640: tile_width reduced to keep the sheet within 4096 px" 510 "$(ov_info | jq -r '.tile_width')"
expect_eq "get_overview 8x8 at 640: size 8*510+14 by 8*383+14" 4094x3078 "$(img_size "$TMP/ovbig.jpg")"
for bad in '{"columns":9}' '{"columns":0}' '{"rows":0}' '{"rows":9}' '{"tile_width":50}' '{"tile_width":641}' '{"format":"gif"}' '{"columns":"4"}' '{"columns":2.5}' '{"tile_width":null}' '{"format":5}'; do
  expect_eq "get_overview: $bad is an error" true "$(tool_is_error get_overview "$bad")"
done
expect_eq "get_overview: columns message" "columns must be an integer from 1 to 8." "$(tool_text get_overview '{"columns":9}')"
tool_json play '{}' >/dev/null
sleep 0.5
ov_call '{"columns":2,"rows":2,"tile_width":96}' "$TMP/ovp.jpg"
expect_eq "get_overview while playing: succeeds" false "$(jq -r '.result.isError' "$TMP/ov.json")"
expect_eq "get_overview while playing: still playing afterwards" true "$(tool_json get_state '{}' | jq -r '.playing')"
tool_json pause '{"position":10}' >/dev/null
# Fewer frames than tiles: a 5-frame timeline reduces 4x4 to 5 tiles in 4 columns and 2 rows.
tool_json timeline_trim '{"track":0,"clip":0,"edge":"out","frame":5,"ripple":true}' >/dev/null
expect_eq "get_overview short fixture: 5 frames" 5 "$(tool_json get_timeline '{}' | jq -r '.duration')"
ov_call '{}' "$TMP/ovs.jpg"
expect_eq "get_overview 5 frames: positions 0-4" "0,1,2,3,4" "$(ov_info | jq -r '.positions | join(",")')"
expect_eq "get_overview 5 frames: 4 columns, 2 rows reported" "4 2" "$(ov_info | jq -r '"\(.columns) \(.rows)"')"
expect_eq "get_overview 5 frames: size" 1286x482 "$(img_size "$TMP/ovs.jpg")"
tool_json undo '{}' >/dev/null
expect_eq "get_overview short fixture: undo restores 225 frames" 225 "$(tool_json get_timeline '{}' | jq -r '.duration')"
if [ -n "${MCP_APPDATA:-}" ]; then
  mkdir -p "$MCP_APPDATA/presets/encode"
  printf '%s\n' f=mp4 vcodec=libx264 preset=slower crf=18 width=1280 height=720 acodec=aac ab=128k \
    meta.preset.extension=mp4 > "$MCP_APPDATA/presets/encode/mcp-slow"
  OVJOB="$(tool_json export_video "{\"path\":\"$TMP/ov-export.mp4\",\"preset\":\"mcp-slow\"}" | jq -r '.job_id')"
  expect_eq "get_overview while an export runs: error" true "$(tool_is_error get_overview '{"columns":1,"rows":1}')"
  expect_eq "get_overview while an export runs: message" "An export is running; get_overview uses the player, so try again when it has finished." "$(tool_text get_overview '{"columns":1,"rows":1}')"
  expect_eq "get_overview: the export was still running during those checks" true "$(tool_json get_export_status "{\"job_id\":$OVJOB}" | jq '.state | IN("pending","running")')"
  for _ in $(seq 1 300); do
    case "$(tool_json get_export_status "{\"job_id\":$OVJOB}" | jq -r '.state')" in succeeded|failed|stopped|removed) break ;; esac
    sleep 1
  done
  expect_eq "get_overview after the export: works again" false "$(tool_is_error get_overview '{"columns":1,"rows":1}')"
else
  echo "SKIP  get_overview while an export runs (needs MCP_APPDATA for the slow preset)"
fi

echo "== get_overview on a portrait (9:16) timeline"
# No tool sets the video mode yet (Task 17), so make the project 180x320 9:16 in a saved copy.
PORTRAIT="$TMP/portrait.mlt"
tool_json save_project "{\"path\":\"$PORTRAIT\"}" >/dev/null
sed -i -E '/<profile/{s/ width="[0-9]+"/ width="180"/;s/ height="[0-9]+"/ height="320"/;s/display_aspect_num="[0-9]+"/display_aspect_num="9"/;s/display_aspect_den="[0-9]+"/display_aspect_den="16"/;s/sample_aspect_num="[0-9]+"/sample_aspect_num="1"/;s/sample_aspect_den="[0-9]+"/sample_aspect_den="1"/}' "$PORTRAIT"
tool_json open_project "{\"path\":\"$PORTRAIT\",\"discard_changes\":true}" >/dev/null
sleep 2
expect_eq "portrait: get_frame is 180x320" "180 320" "$(call_tool get_frame '{"position":10}' | jq -r '.result.content[1].text | fromjson | "\(.width) \(.height)"')"
ov_call '{"columns":4,"rows":2,"tile_width":96,"format":"png"}' "$TMP/ovp96.png"
expect_eq "portrait 4x2 at 96: size 4*96+6 by 2*171+2" 390x344 "$(img_size "$TMP/ovp96.png")"
# The last tile (frame 224, the widest label); bottom 18 rows hold the 18 px bar of a 10 px font.
expect_eq "portrait 96x171 tiles: timecode and frame number do not overlap" true "$(label_ok "$(bright_cols "$TMP/ovp96.png" 96 18 294 325)")"
ov_call '{"format":"png"}' "$TMP/ovp320.png"
expect_eq "portrait 4x4 at 320: size 4*320+6 by 4*569+6" 1286x2282 "$(img_size "$TMP/ovp320.png")"
expect_eq "portrait 320x569 tiles: timecode and frame number do not overlap" true "$(label_ok "$(bright_cols "$TMP/ovp320.png" 320 18 966 2264)")"
ov_call '{"columns":8,"rows":8,"tile_width":640}' "$TMP/ovpbig.jpg"
expect_eq "portrait 8x8 at 640: tile_width reduced by the height limit" 287 "$(ov_info | jq -r '.tile_width')"
expect_eq "portrait 8x8 at 640: size 8*287+14 by 8*510+14" 2310x4094 "$(img_size "$TMP/ovpbig.jpg")"

echo "== timeline_insert_range"
expect_eq "tools/list has timeline_insert_range" true "$(rpc '{"jsonrpc":"2.0","id":3,"method":"tools/list"}' | jq -r '[.result.tools[].name] | contains(["timeline_insert_range"])')"
# An empty project (what Shotcut saves with nothing loaded) gives a session with no timeline.
EMPTY="$TMP/empty.mlt"
cat > "$EMPTY" <<'XML'
<?xml version="1.0" encoding="utf-8"?>
<mlt LC_NUMERIC="C" producer="playlist0">
  <profile description="320x240 25 fps" width="320" height="240" progressive="1" sample_aspect_num="1" sample_aspect_den="1" display_aspect_num="4" display_aspect_den="3" frame_rate_num="25" frame_rate_den="1" colorspace="601"/>
  <playlist id="playlist0"/>
</mlt>
XML
tool_json open_project "{\"path\":\"$EMPTY\",\"discard_changes\":true}" >/dev/null
sleep 1
TL0="$(tool_json get_timeline '{}')"
expect_eq "insert_range: the session has no timeline" 0 "$(echo "$TL0" | jq '.tracks | length')"
ins() { tool_json timeline_insert_range "$1"; }
clipf() { jq -c "[.clips[] | select(.kind==\"clip\") | [.start, .duration, .in, .out]]"; }
tlclips() { tool_json get_timeline '{}' | jq -c '.tracks[0]' | clipf; }
R="$(ins "{\"path\":\"$MEDIA/scenes.mp4\",\"in_s\":3.0,\"out_s\":6.0,\"track\":0,\"position\":0}")"
expect_eq "insert_range on an empty session: inserted start and duration" "0 75" "$(echo "$R" | jq -r '"\(.inserted.start) \(.inserted.duration)"')"
expect_eq "insert_range on an empty session: one clip, duration 75, in 75" "[[0,75,75,149]]" "$(echo "$R" | jq -c '.track' | clipf)"
expect_eq "insert_range: the created track is a video track" video "$(echo "$R" | jq -r '.track.type')"
TL1="$(tool_json get_timeline '{}')"
expect_eq "insert_range: get_timeline agrees" "[[0,75,75,149]]" "$(echo "$TL1" | jq -c '.tracks[0]' | clipf)"
expect_eq "insert_range: get_timeline has one track and duration 75" "1 75" "$(echo "$TL1" | jq -r '"\(.tracks | length) \(.duration)"')"
expect_eq "insert_range: the clip is scenes.mp4" "$MEDIA/scenes.mp4" "$(echo "$TL1" | jq -r '[.tracks[0].clips[] | select(.kind=="clip")][0].path')"
U="$(tool_json undo '{}')"
expect_eq "insert_range: one undo step with its own text" "Insert range into track" "$(echo "$U" | jq -r '.text')"
expect_eq "insert_range: undo restores the session without a timeline" "$TL0" "$(tool_json get_timeline '{}')"
tool_json redo '{}' >/dev/null
expect_eq "insert_range: redo re-applies" "$TL1" "$(tool_json get_timeline '{}')"
tool_json seek '{"position":5}' >/dev/null
TL1="$(tool_json get_timeline '{}')"
expect_eq "insert_range: playhead at 5 before the overwrite" 5 "$(echo "$TL1" | jq '.position')"

R="$(ins "{\"path\":\"$MEDIA/scenes.mp4\",\"in_s\":0,\"out_s\":1,\"track\":0,\"position\":10,\"mode\":\"overwrite\"}")"
expect_eq "overwrite: inserted start and duration" "10 25" "$(echo "$R" | jq -r '"\(.inserted.start) \(.inserted.duration)"')"
expect_eq "overwrite: splits the clip around the new one" "[[0,10,75,84],[10,25,0,24],[35,40,110,149]]" "$(echo "$R" | jq -c '.track' | clipf)"
TL2="$(tool_json get_timeline '{}')"
expect_eq "overwrite: get_timeline agrees" "[[0,10,75,84],[10,25,0,24],[35,40,110,149]]" "$(echo "$TL2" | jq -c '.tracks[0]' | clipf)"
expect_eq "overwrite: total duration unchanged" 75 "$(echo "$TL2" | jq '.duration')"
expect_eq "overwrite: the playhead does not move" 5 "$(echo "$TL2" | jq '.position')"
expect_eq "overwrite: undo text" "Overwrite range onto track" "$(tool_json undo '{}' | jq -r '.text')"
expect_eq "overwrite: one undo restores the previous get_timeline" "$TL1" "$(tool_json get_timeline '{}')"
tool_json redo '{}' >/dev/null
expect_eq "overwrite: redo re-applies" "$TL2" "$(tool_json get_timeline '{}')"

R="$(ins "{\"path\":\"$MEDIA/scenes.mp4\",\"in_s\":0,\"out_s\":2,\"track\":0,\"position\":25}")"
expect_eq "insert into a clip: inserted start and duration" "25 50" "$(echo "$R" | jq -r '"\(.inserted.start) \(.inserted.duration)"')"
expect_eq "insert into a clip: splits it and ripples later clips" "[[0,10,75,84],[10,15,0,14],[25,50,0,49],[75,10,15,24],[85,40,110,149]]" "$(echo "$R" | jq -c '.track' | clipf)"
TL3="$(tool_json get_timeline '{}')"
expect_eq "insert into a clip: get_timeline agrees" "[[0,10,75,84],[10,15,0,14],[25,50,0,49],[75,10,15,24],[85,40,110,149]]" "$(echo "$TL3" | jq -c '.tracks[0]' | clipf)"
expect_eq "insert into a clip: total duration grows by exactly 50" 125 "$(echo "$TL3" | jq '.duration')"
expect_eq "insert into a clip: the playhead does not move" 5 "$(echo "$TL3" | jq '.position')"
tool_json undo '{}' >/dev/null
expect_eq "insert into a clip: one undo restores the previous get_timeline" "$TL2" "$(tool_json get_timeline '{}')"
tool_json redo '{}' >/dev/null
expect_eq "insert into a clip: redo re-applies" "$TL3" "$(tool_json get_timeline '{}')"

ffmpeg -loglevel error -y -f lavfi -i "color=c=white:size=64x64" -frames:v 1 "$TMP/still.png"
S="{\"path\":\"$MEDIA/scenes.mp4\",\"in_s\":0,\"out_s\":1,\"track\":0,\"position\":0}"
with() { echo "$S" | jq -c "$1"; }
ierr() { tool_is_error timeline_insert_range "$1"; }
expect_eq "insert_range: relative path is an error" true "$(ierr "$(with '.path="scenes.mp4"')")"
expect_eq "insert_range: missing file is an error" true "$(ierr "$(with ".path=\"$MEDIA/nope.mp4\"")")"
expect_eq "insert_range: range beyond the file is an error" true "$(ierr "$(with '.in_s=8 | .out_s=9.5')")"
expect_eq "insert_range: in_s == out_s is an error" true "$(ierr "$(with '.in_s=2 | .out_s=2')")"
expect_eq "insert_range: in_s > out_s is an error" true "$(ierr "$(with '.in_s=3 | .out_s=2')")"
expect_eq "insert_range: negative in_s is an error" true "$(ierr "$(with '.in_s=-1')")"
expect_eq "insert_range: missing out_s with a path is an error" true "$(ierr "$(with 'del(.out_s)')")"
expect_eq "insert_range: negative position is an error" true "$(ierr "$(with '.position=-1')")"
expect_eq "insert_range: fractional position is an error" true "$(ierr "$(with '.position=1.5')")"
expect_eq "insert_range: missing position is an error" true "$(ierr "$(with 'del(.position)')")"
expect_eq "insert_range: unknown track is an error" true "$(ierr "$(with '.track=7')")"
expect_eq "insert_range: missing track is an error" true "$(ierr "$(with 'del(.track)')")"
expect_eq "insert_range: mode ripple is an error" true "$(ierr "$(with '.mode="ripple"')")"
expect_eq "insert_range: playlist_index out of range is an error" true "$(ierr '{"playlist_index":0,"track":0,"position":0}')"
expect_eq "insert_range: negative playlist_index is an error" true "$(ierr '{"playlist_index":-1,"track":0,"position":0}')"
expect_eq "insert_range: an image is an error" true "$(ierr "$(with ".path=\"$TMP/still.png\"")")"
expect_eq "insert_range: a Qt resource path is an error" true "$(ierr "$(with '.path=":/icons/shotcut-logo-64.png"')")"
expect_eq "insert_range: a .mlt file is an error" true "$(ierr "$(with ".path=\"$EMPTY\"")")"
expect_eq "insert_range: string in_s is an error" true "$(ierr "$(with '.in_s="0"')")"
expect_eq "insert_range: string out_s is an error" true "$(ierr "$(with '.out_s="1"')")"
expect_eq "insert_range: string track is an error" true "$(ierr "$(with '.track="0"')")"
expect_eq "insert_range: string position is an error" true "$(ierr "$(with '.position="0"')")"
expect_eq "insert_range: string playlist_index is an error" true "$(ierr '{"playlist_index":"0","track":0,"position":0}')"
expect_eq "insert_range: both path and playlist_index is an error" true "$(ierr "$(with '.playlist_index=0')")"
expect_eq "insert_range: neither path nor playlist_index is an error" true "$(ierr "$(with 'del(.path)')")"
expect_eq "insert_range: the errors changed nothing" "$TL3" "$(tool_json get_timeline '{}')"

R="$(ins "{\"path\":\"$MEDIA/audioonly.wav\",\"in_s\":0,\"out_s\":1,\"track\":0,\"position\":125}")"
expect_eq "insert_range: audio-only media on a video track is allowed (as in Shotcut)" "125 25" "$(echo "$R" | jq -r '"\(.inserted.start) \(.inserted.duration)"')"
tool_json undo '{}' >/dev/null
expect_eq "insert_range: undo of the audio insert" "$TL3" "$(tool_json get_timeline '{}')"

# Lock track 0 in a saved copy and reopen it (no tool locks a track yet; Task 10 adds track_set).
IR_UNLOCKED="$TMP/ir-unlocked.mlt"
IR_LOCKED="$TMP/ir-locked.mlt"
tool_json save_project "{\"path\":\"$IR_UNLOCKED\"}" >/dev/null
cp "$IR_UNLOCKED" "$IR_LOCKED"
PL4="$(grep -o '<track producer="[^"]*"' "$IR_LOCKED" | grep -v background | head -1 | sed 's/.*producer="//; s/"//')"
sed -i "0,/<playlist id=\"$PL4\"[^>]*>/s//&\n  <property name=\"shotcut:lock\">1<\/property>/" "$IR_LOCKED"
tool_json open_project "{\"path\":\"$IR_LOCKED\",\"discard_changes\":true}" >/dev/null
sleep 1
expect_eq "insert_range locked: track 0 is locked after reopening" true "$(tool_json get_timeline '{}' | jq -r '.tracks[0].locked')"
LTL="$(tool_json get_timeline '{}')"
expect_eq "insert_range: locked track is an error" true "$(ierr "$S")"
expect_eq "insert_range: overwrite on a locked track is an error" true "$(ierr "$(with '.mode="overwrite"')")"
expect_eq "insert_range: locked track unchanged" "$LTL" "$(tool_json get_timeline '{}')"

tool_json open_project "{\"path\":\"$IR_UNLOCKED\",\"discard_changes\":true}" >/dev/null
sleep 1
tool_json playlist_add "{\"path\":\"$MEDIA/scenes.mp4\"}" >/dev/null
UTL="$(tool_json get_timeline '{}')"
R="$(ins '{"playlist_index":0,"track":0,"position":0}')"
expect_eq "insert_range playlist_index: the item's own range by default" "0 225" "$(echo "$R" | jq -r '"\(.inserted.start) \(.inserted.duration)"')"
expect_eq "insert_range playlist_index: clip frames" "[0,225,0,224]" "$(echo "$R" | jq -c '.track' | clipf | jq -c '.[0]')"
expect_eq "insert_range playlist_index: later clips ripple by 225" 350 "$(tool_json get_timeline '{}' | jq '.duration')"
tool_json undo '{}' >/dev/null
expect_eq "insert_range playlist_index: undo restores" "$UTL" "$(tool_json get_timeline '{}')"
R="$(ins '{"playlist_index":0,"in_s":1,"out_s":2,"track":0,"position":0,"mode":"overwrite"}')"
expect_eq "insert_range playlist_index with in_s/out_s" "[0,25,25,49]" "$(echo "$R" | jq -c '.track' | clipf | jq -c '.[0]')"
tool_json undo '{}' >/dev/null
expect_eq "insert_range playlist_index: only in_s is an error" true "$(ierr '{"playlist_index":0,"in_s":1,"track":0,"position":0}')"
expect_eq "insert_range playlist_index: index 1 is out of range" true "$(ierr '{"playlist_index":1,"track":0,"position":0}')"
expect_eq "get_timeline settings report ripple_markers" false "$(tool_json get_timeline '{}' | jq -r '.settings.ripple_markers')"
R="$(ins "{\"path\":\"$MEDIA/scenes.mp4\",\"in_s\":0,\"out_s\":1,\"track\":0,\"position\":0}")"
expect_eq "insert_range: result reports ripple (insert, default settings)" '{"all_tracks":false,"markers":false}' "$(echo "$R" | jq -c '.ripple')"
tool_json undo '{}' >/dev/null
R="$(ins "{\"path\":\"$MEDIA/scenes.mp4\",\"in_s\":0,\"out_s\":1,\"track\":0,\"position\":0,\"mode\":\"overwrite\"}")"
expect_eq "insert_range: result reports ripple (overwrite never ripples)" '{"all_tracks":false,"markers":false}' "$(echo "$R" | jq -c '.ripple')"
tool_json undo '{}' >/dev/null
expect_eq "insert_range: a range shorter than one frame is an error" true "$(ierr "$(with '.in_s=1 | .out_s=1.01')")"
expect_eq "insert_range: position more than an hour after the track end is an error" true "$(ierr "$(with '.position=100000000')")"
expect_eq "insert_range: the new errors changed nothing" "$UTL" "$(tool_json get_timeline '{}')"
# UTL track 0: clips 0-9, 10-24, 25-74, 75-84, 85-124 (duration 125)
R="$(ins "{\"path\":\"$MEDIA/scenes.mp4\",\"in_s\":0,\"out_s\":1,\"track\":0,\"position\":124}")"
expect_eq "insert_range on the track's last frame lands one frame later (appended)" "125 25" "$(echo "$R" | jq -r '"\(.inserted.start) \(.inserted.duration)"')"
expect_eq "insert_range on the last frame: last clip untouched" "[85,40,110,149]" "$(echo "$R" | jq -c '.track' | clipf | jq -c '.[-2]')"
tool_json undo '{}' >/dev/null
R="$(ins "{\"path\":\"$MEDIA/scenes.mp4\",\"in_s\":0,\"out_s\":1,\"track\":0,\"position\":200}")"
expect_eq "insert_range beyond the end: starts at position" "200 25" "$(echo "$R" | jq -r '"\(.inserted.start) \(.inserted.duration)"')"
expect_eq "insert_range beyond the end: a 75-frame gap before it" "blank 125 75" "$(echo "$R" | jq -r '.track.clips[-2] | "\(.kind) \(.start) \(.duration)"')"
tool_json undo '{}' >/dev/null
R="$(ins "{\"path\":\"$MEDIA/scenes.mp4\",\"in_s\":0,\"out_s\":1,\"track\":0,\"position\":75,\"mode\":\"overwrite\"}")"
expect_eq "overwrite at a clip boundary: covers from the boundary on" "[[0,10,75,84],[10,15,0,14],[25,50,0,49],[75,25,0,24],[100,25,125,149]]" "$(echo "$R" | jq -c '.track' | clipf)"
tool_json undo '{}' >/dev/null
expect_eq "insert_range: undo of the edge cases restores the timeline" "$UTL" "$(tool_json get_timeline '{}')"

# A transition joins clip 0 (0-9) and clip 1 (10-24): 5 frames.
tool_json timeline_add_transition '{"track":0,"clip":1,"duration":5}' >/dev/null
TR="$(tool_json get_timeline '{}' | jq -c '[.tracks[0].clips[] | select(.kind=="transition")][0]')"
TS="$(echo "$TR" | jq '.start')"; TD="$(echo "$TR" | jq '.duration')"
expect_eq "insert_range transition fixture: 5 frames" 5 "$TD"
TTL="$(tool_json get_timeline '{}')"
expect_eq "insert_range: insert inside a transition is an error" true "$(ierr "$(with ".position=$((TS + 1))")")"
expect_eq "insert_range: insert inside a transition message" true "$(tool_text timeline_insert_range "$(with ".position=$((TS + 2))")" | grep -q 'inside a transition' && echo true || echo false)"
expect_eq "insert_range: overwrite inside a transition is an error" true "$(ierr "$(with ".position=$((TS + 1)) | .mode=\"overwrite\"")")"
expect_eq "insert_range: overwrite covering the start of a transition is an error" true "$(ierr "$(with ".position=$((TS - 2)) | .in_s=0 | .out_s=0.16 | .mode=\"overwrite\"")")"
expect_eq "insert_range: the transition refusals changed nothing" "$TTL" "$(tool_json get_timeline '{}')"
tool_json undo '{}' >/dev/null
expect_eq "insert_range: transition fixture undone" "$UTL" "$(tool_json get_timeline '{}')"

# A speed-changed playlist item (timewarp), from a hand-written project (no tool sets speed yet).
TW="$TMP/timewarp.mlt"
cat > "$TW" <<XML
<?xml version="1.0" encoding="utf-8"?>
<mlt LC_NUMERIC="C" producer="main_bin">
  <profile description="320x240 25 fps" width="320" height="240" progressive="1" sample_aspect_num="1" sample_aspect_den="1" display_aspect_num="4" display_aspect_den="3" frame_rate_num="25" frame_rate_den="1" colorspace="601"/>
  <producer id="producer0" in="0" out="111">
    <property name="length">112</property>
    <property name="resource">2:$MEDIA/scenes.mp4</property>
    <property name="mlt_service">timewarp</property>
    <property name="shotcut:producer">avformat</property>
    <property name="warp_speed">2</property>
    <property name="warp_resource">$MEDIA/scenes.mp4</property>
  </producer>
  <playlist id="main_bin">
    <property name="xml_retain">1</property>
    <entry producer="producer0" in="0" out="111"/>
  </playlist>
</mlt>
XML
tool_json open_project "{\"path\":\"$TW\",\"discard_changes\":true}" >/dev/null
sleep 1
expect_eq "insert_range timewarp fixture: one playlist item" 1 "$(tool_json get_playlist '{}' | jq '.items | length')"
expect_eq "insert_range: a speed-changed playlist item is refused" true "$(tool_text timeline_insert_range '{"playlist_index":0,"track":0,"position":0}' | grep -q 'speed change' && echo true || echo false)"
expect_eq "insert_range: the refusal creates no timeline" 0 "$(tool_json get_timeline '{}' | jq '.tracks | length')"

echo "== timeline_remove_ranges"
expect_eq "tools/list has timeline_remove_ranges" true "$(rpc '{"jsonrpc":"2.0","id":3,"method":"tools/list"}' | jq -r '[.result.tools[].name] | contains(["timeline_remove_ranges"])')"
expect_eq "remove_ranges: no timeline is an error" true "$(tool_is_error timeline_remove_ranges '{"tracks":[0],"ranges":[{"start":0,"end":10}]}')"
tool_json open_project "{\"path\":\"$EMPTY\",\"discard_changes\":true}" >/dev/null
sleep 1
tool_json timeline_insert_range "{\"path\":\"$MEDIA/tone_av.mp4\",\"in_s\":0,\"out_s\":10,\"track\":0,\"position\":0}" >/dev/null
RR0="$(tool_json get_timeline '{}')"
expect_eq "remove_ranges fixture: one 250-frame clip" "[[0,250,0,249]]" "$(echo "$RR0" | jq -c '.tracks[0]' | clipf)"
rr() { tool_json timeline_remove_ranges "$1"; }
rerr() { tool_is_error timeline_remove_ranges "$1"; }
rrtrack() { jq -c '.tracks[0]' | clipf; }
tldur() { tool_json get_timeline '{}' | jq '.duration'; }

R="$(rr '{"tracks":[0],"ranges":[{"start":50,"end":100},{"start":175,"end":212}]}')"
expect_eq "remove_ranges ripple: result clips" "[[0,50,0,49],[50,75,100,174],[125,38,212,249]]" "$(echo "$R" | rrtrack)"
expect_eq "remove_ranges ripple: removed_frames" '{"0":87}' "$(echo "$R" | jq -c '.removed_frames')"
expect_eq "remove_ranges ripple: ranges_applied" '[{"end":100,"start":50},{"end":212,"start":175}]' "$(echo "$R" | jq -c '.ranges_applied')"
expect_eq "remove_ranges ripple: result reports ripple flags" '{"all_tracks":false,"markers":false}' "$(echo "$R" | jq -c '.ripple')"
expect_eq "remove_ranges ripple: one track in the result" 1 "$(echo "$R" | jq '.tracks | length')"
RR1="$(tool_json get_timeline '{}')"
expect_eq "remove_ranges ripple: duration 250-50-37" 163 "$(echo "$RR1" | jq '.duration')"
expect_eq "remove_ranges ripple: get_timeline agrees" "[[0,50,0,49],[50,75,100,174],[125,38,212,249]]" "$(echo "$RR1" | rrtrack)"
U="$(tool_json undo '{}')"
expect_eq "remove_ranges: one undo step with its own text" "Remove 2 range(s)" "$(echo "$U" | jq -r '.text')"
expect_eq "remove_ranges: ONE undo restores the timeline byte for byte" "$RR0" "$(tool_json get_timeline '{}')"
tool_json redo '{}' >/dev/null
expect_eq "remove_ranges: redo re-applies" "$RR1" "$(tool_json get_timeline '{}')"
tool_json undo '{}' >/dev/null

R="$(rr '{"tracks":[0],"ranges":[{"start":60,"end":80},{"start":50,"end":70},{"start":200,"end":210}]}')"
expect_eq "remove_ranges unsorted/overlapping: merged" '[{"end":80,"start":50},{"end":210,"start":200}]' "$(echo "$R" | jq -c '.ranges_applied')"
expect_eq "remove_ranges unsorted/overlapping: clips" "[[0,50,0,49],[50,120,80,199],[170,40,210,249]]" "$(echo "$R" | rrtrack)"
expect_eq "remove_ranges unsorted/overlapping: duration 210" 210 "$(tldur)"
tool_json undo '{}' >/dev/null
R="$(rr '{"tracks":[0],"ranges":[{"start":50,"end":60},{"start":60,"end":70}]}')"
expect_eq "remove_ranges: touching ranges merge" '[{"end":70,"start":50}]' "$(echo "$R" | jq -c '.ranges_applied')"
expect_eq "remove_ranges: touching ranges undo text" "Remove 1 range(s)" "$(tool_json undo '{}' | jq -r '.text')"
R="$(rr '{"tracks":[0],"ranges":[{"start":0,"end":10},{"start":240,"end":250}]}')"
expect_eq "remove_ranges touching the clip start and end" "[[0,230,10,239]]" "$(echo "$R" | rrtrack)"
expect_eq "remove_ranges touching start and end: duration 230" 230 "$(tldur)"
tool_json undo '{}' >/dev/null
R="$(rr '{"tracks":[0],"ranges":[{"start":240,"end":400}]}')"
expect_eq "remove_ranges partly beyond the end: clamped" '[{"end":250,"start":240}]' "$(echo "$R" | jq -c '.ranges_applied')"
expect_eq "remove_ranges partly beyond the end: removed_frames" '{"0":10}' "$(echo "$R" | jq -c '.removed_frames')"
expect_eq "remove_ranges partly beyond the end: duration 240" 240 "$(tldur)"
tool_json undo '{}' >/dev/null
R="$(rr '{"tracks":[0],"ranges":[{"start":100,"end":120},{"start":300,"end":400}]}')"
expect_eq "remove_ranges: a range past the end is dropped" '[{"end":120,"start":100}]' "$(echo "$R" | jq -c '.ranges_applied')"
tool_json undo '{}' >/dev/null
expect_eq "remove_ranges: the undos restore the fixture" "$RR0" "$(tool_json get_timeline '{}')"

R="$(rr '{"tracks":[0],"ranges":[{"start":50,"end":100}],"ripple":false}')"
expect_eq "remove_ranges ripple:false: clips keep their positions" "[[0,50,0,49],[100,150,100,249]]" "$(echo "$R" | rrtrack)"
expect_eq "remove_ranges ripple:false: a 50-frame blank" "blank 50 50" "$(echo "$R" | jq -r '.tracks[0].clips[1] | "\(.kind) \(.start) \(.duration)"')"
expect_eq "remove_ranges ripple:false: ripple flags off" '{"all_tracks":false,"markers":false}' "$(echo "$R" | jq -c '.ripple')"
expect_eq "remove_ranges ripple:false: duration kept" 250 "$(tldur)"
RRL="$(tool_json get_timeline '{}')"
R="$(rr '{"tracks":[0],"ranges":[{"start":40,"end":120}]}')"
expect_eq "remove_ranges ripple across a gap: clips" "[[0,40,0,39],[40,130,120,249]]" "$(echo "$R" | rrtrack)"
expect_eq "remove_ranges ripple across a gap: duration 170" 170 "$(tldur)"
tool_json undo '{}' >/dev/null
expect_eq "remove_ranges ripple across a gap: one undo" "$RRL" "$(tool_json get_timeline '{}')"
R="$(rr '{"tracks":[0],"ranges":[{"start":60,"end":110}]}')"
expect_eq "remove_ranges ripple from inside a gap: clips" "[[0,50,0,49],[60,140,110,249]]" "$(echo "$R" | rrtrack)"
expect_eq "remove_ranges ripple from inside a gap: 10 frames of gap left" "blank 50 10" "$(echo "$R" | jq -r '.tracks[0].clips[1] | "\(.kind) \(.start) \(.duration)"')"
expect_eq "remove_ranges ripple from inside a gap: duration 200" 200 "$(tldur)"
tool_json undo '{}' >/dev/null
expect_eq "remove_ranges ripple from inside a gap: one undo" "$RRL" "$(tool_json get_timeline '{}')"
R="$(rr '{"tracks":[0],"ranges":[{"start":55,"end":95}],"ripple":false}')"
expect_eq "remove_ranges ripple:false inside a gap changes nothing" "$RRL" "$(tool_json get_timeline '{}')"
expect_eq "remove_ranges ripple:false inside a gap adds no undo step" "$RR0" "$(tool_json undo '{}' >/dev/null; tool_json get_timeline '{}')"
expect_eq "remove_ranges: back to the fixture" "$RR0" "$(tool_json get_timeline '{}')"
R="$(rr '{"tracks":[0],"ranges":[{"start":240,"end":250}],"ripple":false}')"
expect_eq "remove_ranges ripple:false at the end: Shotcut drops the trailing gap" 240 "$(tldur)"
tool_json undo '{}' >/dev/null
R="$(rr '{"tracks":[0],"ranges":[{"start":0,"end":250}]}')"
expect_eq "remove_ranges of everything: no clip left" "[]" "$(echo "$R" | rrtrack)"
tool_json undo '{}' >/dev/null
expect_eq "remove_ranges: back to the fixture again" "$RR0" "$(tool_json get_timeline '{}')"

expect_eq "remove_ranges: a range entirely beyond the end is an error" true "$(rerr '{"tracks":[0],"ranges":[{"start":300,"end":400}]}')"
expect_eq "remove_ranges: beyond the end message" "No range lies inside the timeline." "$(tool_text timeline_remove_ranges '{"tracks":[0],"ranges":[{"start":250,"end":400}]}')"
RS='{"tracks":[0],"ranges":[{"start":50,"end":100}]}'
rwith() { echo "$RS" | jq -c "$1"; }
expect_eq "remove_ranges: 501 ranges is an error" true "$(rerr "$(jq -nc '{tracks:[0],ranges:[range(501) | {start:(.*2),end:(.*2+1)}]}')")"
expect_eq "remove_ranges: 500 ranges is accepted" false "$(rerr "$(jq -nc '{tracks:[0],ranges:[range(500) | {start:(.*600),end:(.*600+1)}]}')")"
tool_json undo '{}' >/dev/null
expect_eq "remove_ranges: start == end is an error" true "$(rerr "$(rwith '.ranges[0].end=50')")"
expect_eq "remove_ranges: start > end is an error" true "$(rerr "$(rwith '.ranges[0].end=40')")"
expect_eq "remove_ranges: negative start is an error" true "$(rerr "$(rwith '.ranges[0].start=-10')")"
expect_eq "remove_ranges: fractional end is an error" true "$(rerr "$(rwith '.ranges[0].end=99.5')")"
expect_eq "remove_ranges: string start is an error" true "$(rerr "$(rwith '.ranges[0].start="50"')")"
expect_eq "remove_ranges: missing end is an error" true "$(rerr "$(rwith 'del(.ranges[0].end)')")"
expect_eq "remove_ranges: a range as an array is an error" true "$(rerr "$(rwith '.ranges=[[50,100]]')")"
expect_eq "remove_ranges: a bad range after a good one is an error" true "$(rerr "$(rwith '.ranges+=[{"start":5,"end":1}]')")"
expect_eq "remove_ranges: empty ranges is an error" true "$(rerr "$(rwith '.ranges=[]')")"
expect_eq "remove_ranges: missing ranges is an error" true "$(rerr "$(rwith 'del(.ranges)')")"
expect_eq "remove_ranges: string ranges is an error" true "$(rerr "$(rwith '.ranges="50-100"')")"
expect_eq "remove_ranges: duplicate tracks is an error" true "$(rerr "$(rwith '.tracks=[0,0]')")"
expect_eq "remove_ranges: 9 tracks is an error" true "$(rerr "$(rwith '.tracks=[0,1,2,3,4,5,6,7,8]')")"
expect_eq "remove_ranges: unknown track is an error" true "$(rerr "$(rwith '.tracks=[5]')")"
expect_eq "remove_ranges: negative track is an error" true "$(rerr "$(rwith '.tracks=[-1]')")"
expect_eq "remove_ranges: string track is an error" true "$(rerr "$(rwith '.tracks=["0"]')")"
expect_eq "remove_ranges: tracks as a number is an error" true "$(rerr "$(rwith '.tracks=0')")"
expect_eq "remove_ranges: empty tracks is an error" true "$(rerr "$(rwith '.tracks=[]')")"
expect_eq "remove_ranges: missing tracks is an error" true "$(rerr "$(rwith 'del(.tracks)')")"
expect_eq "remove_ranges: string ripple is an error" true "$(rerr "$(rwith '.ripple="no"')")"
expect_eq "remove_ranges: the errors changed nothing" "$RR0" "$(tool_json get_timeline '{}')"

# A transition joins two clips: ripple-remove [50,100) first, then 5 frames between clip 0 and 1.
rr '{"tracks":[0],"ranges":[{"start":50,"end":100}]}' >/dev/null
tool_json timeline_add_transition '{"track":0,"clip":1,"duration":5}' >/dev/null
TR="$(tool_json get_timeline '{}' | jq -c '[.tracks[0].clips[] | select(.kind=="transition")][0]')"
TS="$(echo "$TR" | jq '.start')"; TE="$(( TS + $(echo "$TR" | jq '.duration') ))"
expect_eq "remove_ranges transition fixture: 5 frames" 5 "$(echo "$TR" | jq '.duration')"
RRT="$(tool_json get_timeline '{}')"
expect_eq "remove_ranges: a range crossing a transition is an error" true "$(rerr "{\"tracks\":[0],\"ranges\":[{\"start\":$((TS + 1)),\"end\":$((TE + 10))}]}")"
expect_eq "remove_ranges: the transition error names it" true "$(tool_text timeline_remove_ranges "{\"tracks\":[0],\"ranges\":[{\"start\":$((TS - 10)),\"end\":$((TS + 2))}]}" | grep -q 'transition' && echo true || echo false)"
expect_eq "remove_ranges: a range ending at a transition is an error" true "$(rerr "{\"tracks\":[0],\"ranges\":[{\"start\":10,\"end\":$TS}]}")"
expect_eq "remove_ranges: a range starting at a transition's end is an error" true "$(rerr "{\"tracks\":[0],\"ranges\":[{\"start\":$TE,\"end\":$((TE + 10))}]}")"
expect_eq "remove_ranges: a bad range with good ones is an error" true "$(rerr "{\"tracks\":[0],\"ranges\":[{\"start\":0,\"end\":5},{\"start\":$((TS + 1)),\"end\":$((TS + 2))},{\"start\":150,\"end\":160}]}")"
expect_eq "remove_ranges: the transition refusals changed nothing" "$RRT" "$(tool_json get_timeline '{}')"
R="$(rr '{"tracks":[0],"ranges":[{"start":140,"end":150}]}')"
expect_eq "remove_ranges: a range away from the transition works" "$(( $(echo "$RRT" | jq '.duration') - 10 ))" "$(tldur)"
tool_json undo '{}' >/dev/null
expect_eq "remove_ranges: undo next to a transition" "$RRT" "$(tool_json get_timeline '{}')"
tool_json undo '{}' >/dev/null
tool_json undo '{}' >/dev/null
expect_eq "remove_ranges: transition fixture undone" "$RR0" "$(tool_json get_timeline '{}')"

# Lock track 0 in a saved copy and reopen it (Task 10 adds track_set).
RR_UNLOCKED="$TMP/rr-unlocked.mlt"
RR_LOCKED="$TMP/rr-locked.mlt"
tool_json save_project "{\"path\":\"$RR_UNLOCKED\"}" >/dev/null
cp "$RR_UNLOCKED" "$RR_LOCKED"
PL5="$(grep -o '<track producer="[^"]*"' "$RR_LOCKED" | grep -v background | head -1 | sed 's/.*producer="//; s/"//')"
sed -i "0,/<playlist id=\"$PL5\"[^>]*>/s//&\n  <property name=\"shotcut:lock\">1<\/property>/" "$RR_LOCKED"
tool_json open_project "{\"path\":\"$RR_LOCKED\",\"discard_changes\":true}" >/dev/null
sleep 1
expect_eq "remove_ranges locked: track 0 is locked after reopening" true "$(tool_json get_timeline '{}' | jq -r '.tracks[0].locked')"
RRK="$(tool_json get_timeline '{}')"
expect_eq "remove_ranges: locked track is an error" true "$(rerr "$RS")"
expect_eq "remove_ranges: locked track (ripple:false) is an error" true "$(rerr "$(rwith '.ripple=false')")"
expect_eq "remove_ranges: locked track unchanged" "$RRK" "$(tool_json get_timeline '{}')"

# Two tracks (V1 and A1, written by hand: no tool adds a track before Task 10).
two_track() { # FILE AUDIO_OUT
cat > "$1" <<XML
<?xml version="1.0" encoding="utf-8"?>
<mlt LC_NUMERIC="C" producer="main_bin">
  <profile description="320x240 25 fps" width="320" height="240" progressive="1" sample_aspect_num="1" sample_aspect_den="1" display_aspect_num="4" display_aspect_den="3" frame_rate_num="25" frame_rate_den="1" colorspace="601"/>
  <playlist id="main_bin"><property name="xml_retain">1</property></playlist>
  <producer id="black" in="0" out="249"><property name="length">250</property><property name="resource">0</property><property name="mlt_service">color</property><property name="set.test_audio">0</property></producer>
  <playlist id="background"><entry producer="black" in="0" out="249"/></playlist>
  <chain id="chain0" out="249"><property name="length">250</property><property name="resource">$MEDIA/tone_av.mp4</property><property name="mlt_service">avformat-novalidate</property></chain>
  <chain id="chain1" out="249"><property name="length">250</property><property name="resource">$MEDIA/tone_av.mp4</property><property name="mlt_service">avformat-novalidate</property></chain>
  <playlist id="playlist0"><property name="shotcut:video">1</property><property name="shotcut:name">V1</property><entry producer="chain0" in="0" out="249"/></playlist>
  <playlist id="playlist1"><property name="shotcut:audio">1</property><property name="shotcut:name">A1</property><entry producer="chain1" in="0" out="$2"/></playlist>
  <tractor id="tractor0" in="0" out="249">
    <property name="shotcut">1</property>
    <track producer="background"/>
    <track producer="playlist0"/>
    <track producer="playlist1" hide="video"/>
    <transition id="transition0"><property name="a_track">0</property><property name="b_track">1</property><property name="mlt_service">mix</property><property name="always_active">1</property><property name="sum">1</property></transition>
    <transition id="transition1"><property name="a_track">0</property><property name="b_track">2</property><property name="mlt_service">mix</property><property name="always_active">1</property><property name="sum">1</property></transition>
  </tractor>
</mlt>
XML
}
two_track "$TMP/two-equal.mlt" 249
tool_json open_project "{\"path\":\"$TMP/two-equal.mlt\",\"discard_changes\":true}" >/dev/null
sleep 1
TT0="$(tool_json get_timeline '{}')"
expect_eq "remove_ranges two tracks fixture: V1 and A1 of 250 frames" "video [[0,250,0,249]] audio [[0,250,0,249]]" "$(echo "$TT0" | jq -r '[.tracks[] | "\(.type) \(. | [.clips[] | select(.kind=="clip") | [.start,.duration,.in,.out]] | tojson)"] | join(" ")')"
R="$(rr '{"tracks":[0,1],"ranges":[{"start":50,"end":100},{"start":175,"end":212}]}')"
expect_eq "remove_ranges two tracks: video clips" "[[0,50,0,49],[50,75,100,174],[125,38,212,249]]" "$(echo "$R" | jq -c '.tracks[0]' | clipf)"
expect_eq "remove_ranges two tracks: audio clips aligned" "[[0,50,0,49],[50,75,100,174],[125,38,212,249]]" "$(echo "$R" | jq -c '.tracks[1]' | clipf)"
expect_eq "remove_ranges two tracks: result track indexes" "[0,1]" "$(echo "$R" | jq -c '[.tracks[].index]')"
expect_eq "remove_ranges two tracks: removed_frames" '{"0":87,"1":87}' "$(echo "$R" | jq -c '.removed_frames')"
expect_eq "remove_ranges two tracks: duration 163" 163 "$(tldur)"
expect_eq "remove_ranges two tracks: one undo step" "Remove 2 range(s)" "$(tool_json undo '{}' | jq -r '.text')"
expect_eq "remove_ranges two tracks: one undo restores both" "$TT0" "$(tool_json get_timeline '{}')"
R="$(rr '{"tracks":[1,0],"ranges":[{"start":0,"end":25}]}')"
expect_eq "remove_ranges two tracks: result follows the listed order" "[1,0]" "$(echo "$R" | jq -c '[.tracks[].index]')"
tool_json undo '{}' >/dev/null
R="$(rr '{"tracks":[0],"ranges":[{"start":0,"end":10}]}')"
expect_eq "remove_ranges: an unlisted track is untouched (no ripple of other tracks)" "$(echo "$TT0" | jq -c '.tracks[1]')" "$(tool_json get_timeline '{}' | jq -c '.tracks[1]')"
expect_eq "remove_ranges: the listed track rippled" "[[0,240,10,249]]" "$(echo "$R" | jq -c '.tracks[0]' | clipf)"
tool_json undo '{}' >/dev/null
expect_eq "remove_ranges: unlisted-track case undone" "$TT0" "$(tool_json get_timeline '{}')"

two_track "$TMP/two-diff.mlt" 199
tool_json open_project "{\"path\":\"$TMP/two-diff.mlt\",\"discard_changes\":true}" >/dev/null
sleep 1
TD0="$(tool_json get_timeline '{}')"
R="$(rr '{"tracks":[0,1],"ranges":[{"start":150,"end":230}]}')"
expect_eq "remove_ranges tracks of different lengths: removed_frames per track" '{"0":80,"1":50}' "$(echo "$R" | jq -c '.removed_frames')"
expect_eq "remove_ranges tracks of different lengths: video" "[[0,150,0,149],[150,20,230,249]]" "$(echo "$R" | jq -c '.tracks[0]' | clipf)"
expect_eq "remove_ranges tracks of different lengths: audio" "[[0,150,0,149]]" "$(echo "$R" | jq -c '.tracks[1]' | clipf)"
tool_json undo '{}' >/dev/null
R="$(rr '{"tracks":[0,1],"ranges":[{"start":210,"end":300}]}')"
expect_eq "remove_ranges: a range past the shorter track only cuts the longer one" '{"0":40,"1":0}' "$(echo "$R" | jq -c '.removed_frames')"
expect_eq "remove_ranges: the shorter track is unchanged" "$(echo "$TD0" | jq -c '.tracks[1]')" "$(echo "$R" | jq -c '.tracks[1]')"
tool_json undo '{}' >/dev/null
expect_eq "remove_ranges: different-length case undone" "$TD0" "$(tool_json get_timeline '{}')"
expect_eq "remove_ranges: the user's ripple-all-tracks setting is unchanged" false "$(tool_json get_timeline '{}' | jq -r '.settings.ripple_all_tracks')"

# FOOTER (later tasks insert their blocks above this line)
if [ "$failures" -eq 0 ]; then echo "All checks passed."; else echo "${failures} check(s) failed."; exit 1; fi
