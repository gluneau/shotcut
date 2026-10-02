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

echo "== per-clip media fields"
tool_json playlist_add "{\"path\":\"$MEDIA/tone_av.mp4\"}" >/dev/null
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

# FOOTER (later tasks insert their blocks above this line)
if [ "$failures" -eq 0 ]; then echo "All checks passed."; else echo "${failures} check(s) failed."; exit 1; fi
