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

# FOOTER (later tasks insert their blocks above this line)
if [ "$failures" -eq 0 ]; then echo "All checks passed."; else echo "${failures} check(s) failed."; exit 1; fi
