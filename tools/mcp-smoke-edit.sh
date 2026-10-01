#!/usr/bin/env bash
# Edit / see / export smoke test for the Shotcut MCP server (stage 2).
# Usage: MCP_TOKEN=<token> [MCP_PORT=7468] tools/mcp-smoke-edit.sh
# Needs: bash, curl, jq, ffmpeg, ffprobe, base64, xxd. Run it against a FRESH, throwaway Shotcut
# (nothing loaded, empty playlist, no timeline): it adds clips, edits the timeline and exports
# a video, and it aborts otherwise unless MCP_SMOKE_FORCE=1. See docs/mcp-server.md for the
# headless recipe.
set -u

PORT="${MCP_PORT:-7468}"
TOKEN="${MCP_TOKEN:?set MCP_TOKEN to the token from Settings > MCP Server}"
URL="http://127.0.0.1:${PORT}/mcp"
AUTH=(-H "Authorization: Bearer ${TOKEN}")
JSON=(-H 'Content-Type: application/json')
failures=0

pass() { echo "PASS  $1"; }
fail() { echo "FAIL  $1 -- $2"; failures=$((failures + 1)); }
expect_eq() { if [ "$2" = "$3" ]; then pass "$1"; else fail "$1" "expected '$2', got '$3'"; fi; }

rpc() { curl -s --max-time 60 -X POST "$URL" "${AUTH[@]}" "${JSON[@]}" -d "$1"; }
call_tool() {
  rpc "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"$1\",\"arguments\":$2}}"
}
tool_is_error() { call_tool "$1" "$2" | jq -r '.result.isError'; }
# Payload of a tool that returns JSON text.
tool_json() { call_tool "$1" "$2" | jq -c '.result.content[0].text | fromjson'; }
# Error text of a failing tool call.
tool_text() { call_tool "$1" "$2" | jq -r '.result.content[0].text'; }

# wait_state TOOL ARGS JQ_FILTER WANT: poll up to 10 s until the filter equals WANT; prints the last value.
wait_state() {
  local value=""
  for _ in $(seq 1 50); do
    value="$(tool_json "$1" "$2" | jq -r "$3")"
    [ "$value" = "$4" ] && break
    sleep 0.2
  done
  echo "$value"
}

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Fresh-session guard (fails closed).
STATE="$(tool_json get_state '{}' 2>/dev/null)" || STATE=""
if [ "${MCP_SMOKE_FORCE:-0}" != 1 ]; then
  if ! echo "$STATE" | jq -e '.loaded == false and .modified == false' >/dev/null 2>&1; then
    echo "Refusing to run: the target Shotcut is not a fresh session (set MCP_SMOKE_FORCE=1 to override)."
    exit 2
  fi
fi

# Fixtures: three short clips with audio, different content and lengths (75, 100, 125 frames at 25 fps).
make_clip() { # name seconds generator frequency
  ffmpeg -loglevel error -y -f lavfi -i "$3=duration=$2:size=320x240:rate=25" \
    -f lavfi -i "sine=frequency=$4:duration=$2" -pix_fmt yuv420p -c:v libx264 -c:a aac -shortest \
    "$TMP/$1.mp4"
}
make_clip a 3 testsrc 440 && make_clip b 4 smptebars 550 && make_clip c 5 rgbtestsrc 660 \
  || { echo "ffmpeg could not create the fixtures"; exit 2; }
CLIP_A="$TMP/a.mp4"; CLIP_B="$TMP/b.mp4"; CLIP_C="$TMP/c.mp4"

echo "== get_frame"
expect_eq "tools/list has get_frame" true "$(rpc '{"jsonrpc":"2.0","id":3,"method":"tools/list"}' | jq -r '[.result.tools[].name] | contains(["get_frame"])')"
expect_eq "get_frame: nothing loaded is an error" true "$(tool_is_error get_frame '{}')"

# The first playlist_add runs on the empty session: the file also becomes the loaded clip.
expect_eq "playlist_add a on the empty session" false "$(tool_is_error playlist_add "{\"path\":\"$CLIP_A\"}")"
expect_eq "clip loaded by the first playlist_add" true "$(wait_state get_state '{}' '.loaded' true)"
expect_eq "profile follows the first file (fps)" 25 "$(tool_json get_state '{}' | jq '.fps')"
expect_eq "playlist has one item after the first add" 1 "$(tool_json get_playlist '{}' | jq '.items | length')"

frame_to_file() { call_tool get_frame "$1" | jq -r '.result.content[0].data' | base64 -d > "$2"; }
expect_eq "get_frame: image content" image "$(call_tool get_frame '{}' | jq -r '.result.content[0].type')"
expect_eq "get_frame: jpeg by default" image/jpeg "$(call_tool get_frame '{}' | jq -r '.result.content[0].mimeType')"
frame_to_file '{"position":10}' "$TMP/f10.jpg"
frame_to_file '{"position":60}' "$TMP/f60.jpg"
expect_eq "get_frame: JPEG signature" ffd8ff "$(head -c 3 "$TMP/f10.jpg" | xxd -p)"
expect_eq "get_frame: native width kept (no upscaling)" 320 "$(ffprobe -v error -select_streams v:0 -show_entries stream=width -of csv=p=0 "$TMP/f10.jpg")"
expect_eq "get_frame: frames at 10 and 60 differ" true "$([ "$(md5sum < "$TMP/f10.jpg")" != "$(md5sum < "$TMP/f60.jpg")" ] && echo true || echo false)"
frame_to_file '{"position":10}' "$TMP/f10b.jpg"
expect_eq "get_frame: position 10 again matches the first position 10 (no lag)" true "$([ "$(md5sum < "$TMP/f10.jpg")" = "$(md5sum < "$TMP/f10b.jpg")" ] && echo true || echo false)"
expect_eq "get_frame: position 10 again differs from position 60" true "$([ "$(md5sum < "$TMP/f10b.jpg")" != "$(md5sum < "$TMP/f60.jpg")" ] && echo true || echo false)"
expect_eq "get_frame: text item reports the position" 60 "$(call_tool get_frame '{"position":60}' | jq -r '.result.content[1].text | fromjson | .position')"
expect_eq "get_frame: max_width shrinks" 100 "$(call_tool get_frame '{"position":10,"max_width":100}' | jq -r '.result.content[0].data' | base64 -d > "$TMP/small.jpg"; ffprobe -v error -select_streams v:0 -show_entries stream=width -of csv=p=0 "$TMP/small.jpg")"
call_tool get_frame '{"position":10,"format":"png"}' | jq -r '.result.content[0].data' | base64 -d > "$TMP/f.png"
expect_eq "get_frame: PNG signature" 89504e47 "$(head -c 4 "$TMP/f.png" | xxd -p)"
expect_eq "get_frame: png mime" image/png "$(call_tool get_frame '{"format":"png"}' | jq -r '.result.content[0].mimeType')"
expect_eq "get_frame: position past the end is clamped" false "$(tool_is_error get_frame '{"position":99999}')"
expect_eq "get_frame: negative position is an error" true "$(tool_is_error get_frame '{"position":-1}')"
expect_eq "get_frame: string position is an error" true "$(tool_is_error get_frame '{"position":"a"}')"
expect_eq "get_frame: max_width too small is an error" true "$(tool_is_error get_frame '{"max_width":10}')"
expect_eq "get_frame: max_width too large is an error" true "$(tool_is_error get_frame '{"max_width":5000}')"
expect_eq "get_frame: bad format is an error" true "$(tool_is_error get_frame '{"format":"gif"}')"

echo "== playlist and timeline (read)"
expect_eq "get_timeline: no tracks yet" 0 "$(tool_json get_timeline '{}' | jq '.tracks | length')"
expect_eq "get_timeline: fps" 25 "$(tool_json get_timeline '{}' | jq '.fps')"

expect_eq "playlist_add: relative path is an error" true "$(tool_is_error playlist_add '{"path":"a.mp4"}')"
expect_eq "playlist_add: missing file is an error" true "$(tool_is_error playlist_add "{\"path\":\"$TMP/missing.mp4\"}")"
expect_eq "playlist_add: directory is an error" true "$(tool_is_error playlist_add "{\"path\":\"$TMP\"}")"
expect_eq "playlist_add: resource path is an error" true "$(tool_is_error playlist_add '{"path":":/a.mp4"}')"
echo "not media" > "$TMP/junk.mp4"
expect_eq "playlist_add: a non-media file is an error" true "$(tool_is_error playlist_add "{\"path\":\"$TMP/junk.mp4\"}")"
expect_eq "get_playlist unchanged after the errors" 1 "$(tool_json get_playlist '{}' | jq '.items | length')"

expect_eq "playlist_add b" false "$(tool_is_error playlist_add "{\"path\":\"$CLIP_B\"}")"
RES="$(tool_json playlist_add "{\"path\":\"$CLIP_C\"}")"
expect_eq "playlist_add returns the whole playlist" 3 "$(echo "$RES" | jq '.items | length')"
expect_eq "get_playlist: three items" 3 "$(tool_json get_playlist '{}' | jq '.items | length')"
expect_eq "get_playlist: durations in order" "75,100,125" "$(tool_json get_playlist '{}' | jq -r '[.items[].duration] | join(",")')"
expect_eq "get_playlist: paths in order" "$CLIP_A,$CLIP_B,$CLIP_C" "$(tool_json get_playlist '{}' | jq -r '[.items[].path] | join(",")')"
expect_eq "get_playlist: indexes" "0,1,2" "$(tool_json get_playlist '{}' | jq -r '[.items[].index] | join(",")')"

echo "== timeline: add, split, remove"
expect_eq "timeline_add_playlist: unknown track is an error" true "$(tool_is_error timeline_add_playlist '{"track":7}')"
expect_eq "timeline_add_playlist: negative track is an error" true "$(tool_is_error timeline_add_playlist '{"track":-1}')"
expect_eq "timeline_add_playlist" false "$(tool_is_error timeline_add_playlist '{}')"
expect_eq "timeline_add_playlist: unknown track is an error (timeline exists)" true "$(tool_is_error timeline_add_playlist '{"track":7}')"
expect_eq "timeline_add_playlist: unchanged after the error" 3 "$(tool_json get_timeline '{}' | jq '.tracks[0].clips | length')"
TL="$(tool_json get_timeline '{}')"
expect_eq "timeline has a video track" video "$(echo "$TL" | jq -r '.tracks[0].type')"
expect_eq "timeline: three clips" 3 "$(echo "$TL" | jq '[.tracks[0].clips[] | select(.kind=="clip")] | length')"
expect_eq "timeline: contiguous starts" "0,75,175" "$(echo "$TL" | jq -r '[.tracks[0].clips[].start] | join(",")')"
expect_eq "timeline: total duration is the sum" 300 "$(echo "$TL" | jq '.duration')"
expect_eq "timeline: paths in playlist order" "$CLIP_A,$CLIP_B,$CLIP_C" "$(echo "$TL" | jq -r '[.tracks[0].clips[].path] | join(",")')"

expect_eq "split: position at a clip edge is an error" true "$(tool_is_error timeline_split '{"track":0,"position":75}')"
expect_eq "split: position past the end is an error" true "$(tool_is_error timeline_split '{"track":0,"position":999}')"
expect_eq "split: unknown track is an error" true "$(tool_is_error timeline_split '{"track":5,"position":10}')"
expect_eq "split: string position is an error" true "$(tool_is_error timeline_split '{"track":0,"position":"x"}')"
expect_eq "split: unchanged after the errors" 3 "$(tool_json get_timeline '{}' | jq '.tracks[0].clips | length')"
SPLIT="$(tool_json timeline_split '{"track":0,"position":125}')"
expect_eq "split: result is the updated track" 4 "$(echo "$SPLIT" | jq '.track.clips | length')"
expect_eq "split: halves" "75,125,175" "$(echo "$SPLIT" | jq -r '[.track.clips[1:4][].start] | join(",")')"
expect_eq "split: result matches get_timeline" "$(tool_json get_timeline '{}' | jq -c '.tracks[0]')" "$(echo "$SPLIT" | jq -c '.track')"
expect_eq "split: durations add up" 100 "$(echo "$SPLIT" | jq '.track.clips[1].duration + .track.clips[2].duration')"
expect_eq "split: total unchanged" 300 "$(tool_json get_timeline '{}' | jq '.duration')"

expect_eq "remove: unknown clip is an error" true "$(tool_is_error timeline_remove '{"track":0,"clip":9}')"
LIFT="$(tool_json timeline_remove '{"track":0,"clip":0,"ripple":false}')"
expect_eq "lift: leaves a blank" blank "$(echo "$LIFT" | jq -r '.track.clips[0].kind')"
expect_eq "lift: same number of items" 4 "$(echo "$LIFT" | jq '.track.clips | length')"
expect_eq "lift: later clips stay put" 75 "$(echo "$LIFT" | jq '.track.clips[1].start')"
expect_eq "remove: a blank is refused" true "$(tool_is_error timeline_remove '{"track":0,"clip":0}')"
RIP="$(tool_json timeline_remove '{"track":0,"clip":1}')"
expect_eq "ripple remove: gap closes" 3 "$(echo "$RIP" | jq '.track.clips | length')"
expect_eq "ripple remove: next clip moved up" 75 "$(echo "$RIP" | jq '.track.clips[1].start')"
expect_eq "ripple remove: total shrank" 250 "$(tool_json get_timeline '{}' | jq '.duration')"

# FOOTER (later tasks insert their blocks above this line)
if [ "$failures" -eq 0 ]; then echo "All checks passed."; else echo "${failures} check(s) failed."; exit 1; fi
