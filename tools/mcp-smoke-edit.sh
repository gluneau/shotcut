#!/usr/bin/env bash
# Edit / see / export smoke test for the Shotcut MCP server (stage 2).
# Usage: MCP_TOKEN=<token> [MCP_PORT=7468] [MCP_APPDATA=<dir>] tools/mcp-smoke-edit.sh
# MCP_APPDATA is the --appdata directory of the Shotcut under test. When set, the script writes a
# deliberately slow custom preset there (presets/encode/mcp-slow) and uses it to test, without
# racing the render, that a second export is refused while one runs; it also proves a custom
# preset is applied (1280x720 output). When unset, those two checks run against the fast
# x264-medium render and may flake (a NOTE is printed).
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

echo "== timeline: image add and undo"
TL_BEFORE="$(tool_json get_timeline '{}' | jq -c '[.duration, ([.tracks[0].clips[] | select(.kind=="clip")] | length)]')"
ffmpeg -loglevel error -y -f lavfi -i testsrc=size=320x240:rate=1 -frames:v 1 "$TMP/img.png"
expect_eq "playlist_add: still image is not an error" false "$(tool_is_error playlist_add "{\"path\":\"$TMP/img.png\"}")"
expect_eq "get_playlist: four items after the image" 4 "$(tool_json get_playlist '{}' | jq '.items | length')"
expect_eq "get_playlist: image has a positive duration" true "$(tool_json get_playlist '{}' | jq '.items[3].duration > 0')"
expect_eq "image add: timeline unaffected" "$TL_BEFORE" "$(tool_json get_timeline '{}' | jq -c '[.duration, ([.tracks[0].clips[] | select(.kind=="clip")] | length)]')"
expect_eq "undo (image add): not an error" false "$(tool_is_error undo '{}')"
expect_eq "undo (image add): playlist is back to three items" 3 "$(tool_json get_playlist '{}' | jq '.items | length')"
expect_eq "undo (image add): paths in order" "$CLIP_A,$CLIP_B,$CLIP_C" "$(tool_json get_playlist '{}' | jq -r '[.items[].path] | join(",")')"
expect_eq "undo (image add): timeline unaffected" "$TL_BEFORE" "$(tool_json get_timeline '{}' | jq -c '[.duration, ([.tracks[0].clips[] | select(.kind=="clip")] | length)]')"

echo "== timeline: trim, move, undo, redo"
expect_eq "trim: both frame and delta is an error" true "$(tool_is_error timeline_trim '{"track":0,"clip":2,"edge":"in","frame":5,"delta":5}')"
expect_eq "trim: neither frame nor delta is an error" true "$(tool_is_error timeline_trim '{"track":0,"clip":2,"edge":"in"}')"
expect_eq "trim: bad edge is an error" true "$(tool_is_error timeline_trim '{"track":0,"clip":2,"edge":"middle","delta":1}')"
expect_eq "trim: string delta is an error" true "$(tool_is_error timeline_trim '{"track":0,"clip":2,"edge":"in","delta":"x"}')"
expect_eq "trim: blank item is refused" true "$(tool_is_error timeline_trim '{"track":0,"clip":0,"edge":"out","delta":1}')"
expect_eq "trim: out past the source is refused" true "$(tool_is_error timeline_trim '{"track":0,"clip":2,"edge":"out","delta":-9999}')"
TRIM="$(tool_json timeline_trim '{"track":0,"clip":2,"edge":"out","frame":225}')"
expect_eq "trim out: clip is 25 frames shorter" 100 "$(echo "$TRIM" | jq '.track.clips[2].duration')"
expect_eq "trim out: total shrank" 225 "$(tool_json get_timeline '{}' | jq '.duration')"
TRIM="$(tool_json timeline_trim '{"track":0,"clip":2,"edge":"in","delta":10,"ripple":true}')"
expect_eq "trim in (ripple): clip is 10 frames shorter" 90 "$(echo "$TRIM" | jq '.track.clips[2].duration')"

expect_eq "move: onto a neighbour is refused" true "$(tool_is_error timeline_move '{"track":0,"clip":2,"to_start":20}')"
expect_eq "move: to a negative start is an error" true "$(tool_is_error timeline_move '{"track":0,"clip":1,"to_start":-1}')"
expect_eq "move: blank item is refused" true "$(tool_is_error timeline_move '{"track":0,"clip":0,"to_start":5}')"
expect_eq "move: unknown to_track is an error" true "$(tool_is_error timeline_move '{"track":0,"clip":1,"to_start":0,"to_track":9}')"
MOVED="$(tool_json timeline_move '{"track":0,"clip":1,"to_start":0}')"
expect_eq "move: clip now starts at 0" 0 "$(echo "$MOVED" | jq '[.track.clips[] | select(.kind=="clip")][0].start')"
expect_eq "move: result matches get_timeline" "$(tool_json get_timeline '{}' | jq -c '.tracks[0]')" "$(echo "$MOVED" | jq -c '.track')"

# History: newest first.
UNDO="$(tool_json undo '{}')"
expect_eq "undo: names the command" true "$(echo "$UNDO" | jq '.text | length > 0')"
expect_eq "undo: the move is reverted" 75 "$(tool_json get_timeline '{}' | jq '[.tracks[0].clips[] | select(.kind=="clip")][0].start')"
expect_eq "redo: the move is applied again" 0 "$(tool_json redo '{}' >/dev/null; tool_json get_timeline '{}' | jq '[.tracks[0].clips[] | select(.kind=="clip")][0].start')"
expect_eq "undo again" 75 "$(tool_json undo '{}' >/dev/null; tool_json get_timeline '{}' | jq '[.tracks[0].clips[] | select(.kind=="clip")][0].start')"
expect_eq "redo: nothing newer than the redone step" true "$(tool_json redo '{}' >/dev/null; tool_is_error redo '{}')"
tool_json undo '{}' >/dev/null

echo "== timeline: lift edge cases"
expect_eq "state before lifts: two plain clips" 2 "$(tool_json get_timeline '{}' | jq '[.tracks[0].clips[] | select(.kind=="clip")] | length')"
expect_eq "state before lifts: starts" "75,125" "$(tool_json get_timeline '{}' | jq -r '[.tracks[0].clips[] | select(.kind=="clip") | .start] | join(",")')"
RAW="$(call_tool timeline_remove '{"track":0,"clip":1,"ripple":false}')"
expect_eq "lift next to the blank: not an error" false "$(echo "$RAW" | jq '.result.isError')"
LIFT="$(echo "$RAW" | jq -c '.result.content[0].text | fromjson')"
expect_eq "lift next to the blank: one clip left" 1 "$(echo "$LIFT" | jq '[.track.clips[] | select(.kind=="clip")] | length')"
tool_json undo '{}' >/dev/null
expect_eq "undo of the lift: two clips" 2 "$(tool_json get_timeline '{}' | jq '[.tracks[0].clips[] | select(.kind=="clip")] | length')"
expect_eq "undo of the lift: same starts" "75,125" "$(tool_json get_timeline '{}' | jq -r '[.tracks[0].clips[] | select(.kind=="clip") | .start] | join(",")')"
RAW="$(call_tool timeline_remove '{"track":0,"clip":2,"ripple":false}')"
expect_eq "lift the last clip: not an error" false "$(echo "$RAW" | jq '.result.isError')"
LIFT="$(echo "$RAW" | jq -c '.result.content[0].text | fromjson')"
expect_eq "lift the last clip: one clip left" 1 "$(echo "$LIFT" | jq '[.track.clips[] | select(.kind=="clip")] | length')"
tool_json undo '{}' >/dev/null
expect_eq "undo of the last-clip lift: two clips" 2 "$(tool_json get_timeline '{}' | jq '[.tracks[0].clips[] | select(.kind=="clip")] | length')"
expect_eq "final: total duration" 215 "$(tool_json get_timeline '{}' | jq '.duration')"

echo "== timeline: trim extension and clamp"
PSTARTS='[.tracks[0].clips[] | select(.kind=="clip") | .start] | join(",")'
EXT="$(tool_json timeline_trim '{"track":0,"clip":1,"edge":"in","frame":65}')"
expect_eq "trim in extension: not clamped" "65,60" "$(echo "$EXT" | jq -r '[.track.clips[] | select(.kind=="clip")][0] | "\(.start),\(.duration)"')"
tool_json undo '{}' >/dev/null
expect_eq "undo of the extension: start restored" "75,50" "$(tool_json get_timeline '{}' | jq -r '[.tracks[0].clips[] | select(.kind=="clip")][0] | "\(.start),\(.duration)"')"
expect_eq "trim in: extension past the gap is an error" true "$(tool_is_error timeline_trim '{"track":0,"clip":1,"edge":"in","delta":-9999}')"
expect_eq "trim: frame beyond 1e9 is an error" true "$(tool_is_error timeline_trim '{"track":0,"clip":1,"edge":"in","frame":2000000000}')"
expect_eq "move: to_start beyond 1e9 is an error" true "$(tool_is_error timeline_move '{"track":0,"clip":1,"to_start":2000000000}')"
expect_eq "after the clamp error: starts unchanged" "75,125" "$(tool_json get_timeline '{}' | jq -r "$PSTARTS")"

echo "== timeline: transition and fade"
TL="$(tool_json get_timeline '{}')"
# Use the last two plain clips on track 0 as the pair (previous clip, incoming clip).
N="$(echo "$TL" | jq '.tracks[0].clips | length')"
PAIR_OK="$(echo "$TL" | jq '[.tracks[0].clips[-2].kind, .tracks[0].clips[-1].kind] == ["clip","clip"]
        and (.tracks[0].clips[-2].start + .tracks[0].clips[-2].duration) == .tracks[0].clips[-1].start')"
expect_eq "transition fixture: last two items are butted clips" true "$PAIR_OK"
IN=$((N - 1))
DUR_BEFORE="$(echo "$TL" | jq '.duration')"
expect_eq "transition: first item cannot be the incoming clip" true "$(tool_is_error timeline_add_transition '{"track":0,"clip":0,"duration":10}')"
expect_eq "transition: zero duration is an error" true "$(tool_is_error timeline_add_transition "{\"track\":0,\"clip\":$IN,\"duration\":0}")"
expect_eq "transition: longer than a clip is an error" true "$(tool_is_error timeline_add_transition "{\"track\":0,\"clip\":$IN,\"duration\":9999}")"
expect_eq "transition: string duration is an error" true "$(tool_is_error timeline_add_transition "{\"track\":0,\"clip\":$IN,\"duration\":\"x\"}")"
TRANS="$(tool_json timeline_add_transition "{\"track\":0,\"clip\":$IN,\"duration\":20}")"
expect_eq "transition: a transition item exists" true "$(echo "$TRANS" | jq '[.track.clips[] | select(.kind=="transition")] | length == 1')"
expect_eq "transition: index points at it" transition "$(echo "$TRANS" | jq -r '.track.clips[.transition_index].kind')"
expect_eq "transition: timeline got 20 frames shorter" "$((DUR_BEFORE - 20))" "$(tool_json get_timeline '{}' | jq '.duration')"
expect_eq "transition: result matches get_timeline" "$(tool_json get_timeline '{}' | jq -c '.tracks[0]')" "$(echo "$TRANS" | jq -c '.track')"
expect_eq "transition: a second one on the same pair is refused" true "$(tool_is_error timeline_add_transition "{\"track\":0,\"clip\":$((IN + 1)),\"duration\":10}")"
expect_eq "transition is undoable" false "$(tool_is_error undo '{}')"
expect_eq "undo removed it" "$DUR_BEFORE" "$(tool_json get_timeline '{}' | jq '.duration')"

echo "== timeline: mid-track transition (ripple) and refusals"
PSTARTS_M='[.tracks[0].clips[] | select(.kind=="clip") | .start]'
expect_eq "mid-track: append A, B, C" false "$(tool_is_error timeline_add_playlist '{}')"
TL="$(tool_json get_timeline '{}')"
expect_eq "mid-track: six items [blank, B2, C, A, B, C']" "blank,clip,clip,clip,clip,clip" "$(echo "$TL" | jq -r '[.tracks[0].clips[].kind] | join(",")')"
M_DUR="$(echo "$TL" | jq '.duration')"
M_STARTS="$(echo "$TL" | jq -c "$PSTARTS_M")"
M_A_START="$(echo "$TL" | jq '.tracks[0].clips[3].start')"
M1="$(tool_json timeline_add_transition '{"track":0,"clip":2,"duration":10}')"
expect_eq "mid-track: transition item right after B2" transition "$(echo "$M1" | jq -r '.track.clips[2].kind')"
expect_eq "mid-track: total shortened by exactly 10" "$((M_DUR - 10))" "$(tool_json get_timeline '{}' | jq '.duration')"
expect_eq "mid-track: the clip after the incoming one moved left by exactly 10" "$((M_A_START - 10))" "$(echo "$M1" | jq '[.track.clips[] | select(.kind=="clip")][2].start')"
expect_eq "mid-track: no blank opened after the incoming clip" "blank,clip,transition,clip,clip,clip,clip" "$(echo "$M1" | jq -r '[.track.clips[].kind] | join(",")')"
# Incoming B (item 5 now): between A and B.
M2="$(tool_json timeline_add_transition '{"track":0,"clip":5,"duration":10}')"
expect_eq "mid-track: second transition created" transition "$(echo "$M2" | jq -r '.track.clips[5].kind')"
TRACK_BEFORE="$(tool_json get_timeline '{}' | jq -c '.tracks[0]')"
# Incoming A (item 4) now has a transition on its right.
expect_eq "refusal: incoming clip that already has a transition after it" true "$(tool_is_error timeline_add_transition '{"track":0,"clip":4,"duration":10}')"
expect_eq "refusal: track unchanged" "$TRACK_BEFORE" "$(tool_json get_timeline '{}' | jq -c '.tracks[0]')"
tool_json undo '{}' >/dev/null
tool_json undo '{}' >/dev/null
expect_eq "mid-track undo: starts restored" "$M_STARTS" "$(tool_json get_timeline '{}' | jq -c "$PSTARTS_M")"
expect_eq "mid-track undo: total restored" "$M_DUR" "$(tool_json get_timeline '{}' | jq '.duration')"
tool_json undo '{}' >/dev/null
expect_eq "mid-track undo of the append: total 215, two plain clips" "215,2" "$(tool_json get_timeline '{}' | jq -r '"\(.duration),\([.tracks[0].clips[] | select(.kind=="clip")] | length)"')"


expect_eq "fade: nothing requested is an error" true "$(tool_is_error timeline_fade '{"track":0,"clip":1}')"
expect_eq "fade: negative is an error" true "$(tool_is_error timeline_fade '{"track":0,"clip":1,"fade_in":-1}')"
expect_eq "fade: longer than the clip is an error" true "$(tool_is_error timeline_fade '{"track":0,"clip":1,"fade_in":99999}')"
expect_eq "fade: blank item is an error" true "$(tool_is_error timeline_fade '{"track":0,"clip":0,"fade_in":5}')"
FADE="$(tool_json timeline_fade '{"track":0,"clip":1,"fade_in":10,"fade_out":5}')"
expect_eq "fade: values reported" "10,5" "$(echo "$FADE" | jq -r '.track.clips[1] | "\(.fade_in),\(.fade_out)"')"
expect_eq "fade: get_timeline agrees" "10,5" "$(tool_json get_timeline '{}' | jq -r '.tracks[0].clips[1] | "\(.fade_in),\(.fade_out)"')"
FADE="$(tool_json timeline_fade '{"track":0,"clip":1,"fade_in":0}')"
expect_eq "fade: 0 removes" "0,5" "$(echo "$FADE" | jq -r '.track.clips[1] | "\(.fade_in),\(.fade_out)"')"
tool_json timeline_fade '{"track":0,"clip":1,"fade_in":7,"fade_out":3}' >/dev/null
expect_eq "fade: both values in one call are one undo step" "0,5" "$(tool_json undo '{}' >/dev/null; tool_json get_timeline '{}' | jq -r '.tracks[0].clips[1] | "\(.fade_in),\(.fade_out)"')"
echo "== get_frame: a frame taken after an edit at a position requested before"
FP=$(( $(tool_json get_timeline '{}' | jq '.tracks[0].clips[1].start') + 2 ))
for rep in 1 2 3 4 5; do
  frame_to_file "{\"position\":$FP}" "$TMP/e0.jpg"
  tool_json timeline_fade '{"track":0,"clip":1,"fade_in":30}' >/dev/null
  frame_to_file "{\"position\":$FP}" "$TMP/e1.jpg"
  tool_json undo '{}' >/dev/null
  frame_to_file "{\"position\":$FP}" "$TMP/e2.jpg"
  expect_eq "edit then same position ($rep): faded frame differs" true "$([ "$(md5sum < "$TMP/e0.jpg")" != "$(md5sum < "$TMP/e1.jpg")" ] && echo true || echo false)"
  expect_eq "edit then same position ($rep): undo restores the original frame" true "$([ "$(md5sum < "$TMP/e0.jpg")" = "$(md5sum < "$TMP/e2.jpg")" ] && echo true || echo false)"
done
tool_json timeline_fade '{"track":0,"clip":1,"fade_in":10,"fade_out":5}' >/dev/null
tool_json timeline_fade '{"track":0,"clip":1,"fade_in":0}' >/dev/null
# Put the transition back so the export below renders it.
TRANS="$(tool_json timeline_add_transition "{\"track\":0,\"clip\":$IN,\"duration\":20}")"
TI="$(echo "$TRANS" | jq '.transition_index')"
expect_eq "transition again for the export" transition "$(echo "$TRANS" | jq -r '.track.clips[.transition_index].kind')"

echo "== timeline: clips joined to a transition"
TL="$(tool_json get_timeline '{}')"
TSTART="$(echo "$TL" | jq ".tracks[0].clips[$TI].start")"
PSTARTS_T='[.tracks[0].clips[] | select(.kind=="clip") | .start] | join(",")'
STARTS_BEFORE="$(echo "$TL" | jq -r "$PSTARTS_T")"
expect_eq "joined: move of the incoming clip is refused" true "$(tool_is_error timeline_move "{\"track\":0,\"clip\":$((TI + 1)),\"to_start\":5000}")"
expect_eq "joined: move of the previous clip is refused" true "$(tool_is_error timeline_move "{\"track\":0,\"clip\":$((TI - 1)),\"to_start\":5000}")"
expect_eq "joined: plain clip starts unchanged" "$STARTS_BEFORE" "$(tool_json get_timeline '{}' | jq -r "$PSTARTS_T")"
expect_eq "joined: transition still present" transition "$(tool_json get_timeline '{}' | jq -r ".tracks[0].clips[$TI].kind")"
expect_eq "joined: fade on a transition item is an error" true "$(tool_is_error timeline_fade "{\"track\":0,\"clip\":$TI,\"fade_in\":5}")"
PLAIN_N="$(tool_json get_timeline '{}' | jq '[.tracks[0].clips[] | select(.kind=="clip")] | length')"
RAW="$(call_tool timeline_remove "{\"track\":0,\"clip\":$((TI + 1)),\"ripple\":false}")"
if [ "$(echo "$RAW" | jq -r '.result.isError')" = true ]; then
  pass "remove next to a transition: refused with a message"
else
  expect_eq "remove next to a transition: exactly one plain clip fewer" "$((PLAIN_N - 1))" "$(tool_json get_timeline '{}' | jq '[.tracks[0].clips[] | select(.kind=="clip")] | length')"
  tool_json undo '{}' >/dev/null
fi
expect_eq "remove next to a transition: state restored" "$STARTS_BEFORE|$PLAIN_N" "$(tool_json get_timeline '{}' | jq -r "($PSTARTS_T) + \"|\" + (([.tracks[0].clips[] | select(.kind==\"clip\")] | length | tostring))")"

frame_to_file "{\"position\":$((TSTART - 3))}" "$TMP/tr_before.jpg"
frame_to_file "{\"position\":$((TSTART + 5))}" "$TMP/tr_mid.jpg"
frame_to_file "{\"position\":$((TSTART + 23))}" "$TMP/tr_after.jpg"
expect_eq "transition frame: JPEG signature" ffd8ff "$(head -c 3 "$TMP/tr_mid.jpg" | xxd -p)"
expect_eq "transition frame: differs from the frame before" true "$([ "$(md5sum < "$TMP/tr_mid.jpg")" != "$(md5sum < "$TMP/tr_before.jpg")" ] && echo true || echo false)"
expect_eq "transition frame: differs from the frame after" true "$([ "$(md5sum < "$TMP/tr_mid.jpg")" != "$(md5sum < "$TMP/tr_after.jpg")" ] && echo true || echo false)"

echo "== get_frame: strict repeatability on the timeline with the transition"
# Positions: blank (10), plain clip (90), inside the transition (TSTART+10), plain clip C (150).
FAIL_BEFORE_STRESS=$failures
declare -A FIRST
for round in 1 2 3 4 5 6 7 8; do
  if [ $((round % 2)) -eq 0 ]; then ORDER="150 $((TSTART + 10)) 90 10"; else ORDER="10 90 $((TSTART + 10)) 150"; fi
  for pos in $ORDER; do
    frame_to_file "{\"position\":$pos}" "$TMP/st.jpg"
    SUM="$(md5sum < "$TMP/st.jpg" | cut -c1-32)"
    if [ -z "${FIRST[$pos]:-}" ]; then FIRST[$pos]="$SUM"; fi
    if [ "${FIRST[$pos]}" != "$SUM" ]; then fail "get_frame stress: position $pos round $round" "image differs from round 1 (stale frame)"; fi
  done
done
[ "$failures" -eq "$FAIL_BEFORE_STRESS" ] && pass "get_frame stress: 8 rounds x 4 positions byte-identical"
DISTINCT="$(printf '%s\n' "${FIRST[@]}" | sort -u | wc -l)"
expect_eq "get_frame stress: four positions give four different images" 4 "$DISTINCT"
frame_to_file "{\"position\":90}" "$TMP/same1.jpg"; frame_to_file "{\"position\":90}" "$TMP/same2.jpg"
expect_eq "get_frame: the same position twice in a row is identical" true "$([ "$(md5sum < "$TMP/same1.jpg")" = "$(md5sum < "$TMP/same2.jpg")" ] && echo true || echo false)"

echo "== export"
OUT="$TMP/out.mp4"
TLDUR="$(tool_json get_timeline '{}' | jq '.duration')"

expect_eq "export: relative path is an error" true "$(tool_is_error export_video '{"path":"out.mp4"}')"
expect_eq "export: wrong extension is an error" true "$(tool_is_error export_video "{\"path\":\"$TMP/out.mkv\"}")"
expect_eq "export: unknown preset is an error" true "$(tool_is_error export_video "{\"path\":\"$OUT\",\"preset\":\"no-such-preset\"}")"
expect_eq "export: unwritable folder is an error" true "$(tool_is_error export_video '{"path":"/proc/out.mp4"}')"
expect_eq "export: missing folder is an error" true "$(tool_is_error export_video "{\"path\":\"$TMP/nodir/out.mp4\"}")"
echo existing > "$TMP/exists.mp4"
expect_eq "export: existing file without overwrite is an error" true "$(tool_is_error export_video "{\"path\":\"$TMP/exists.mp4\"}")"
expect_eq "export: bad overwrite type is an error" true "$(tool_is_error export_video "{\"path\":\"$OUT\",\"overwrite\":\"yes\"}")"
expect_eq "export: nothing started yet" idle "$(tool_json get_export_status '{}' | jq -r '.state')"
expect_eq "export: unknown job_id is an error" true "$(tool_is_error get_export_status '{"job_id":999}')"

for BADPRESET in '/etc/passwd' '../x' '..\x' '.hidden' 'a/../../etc/passwd'; do
  ARGS="$(jq -nc --arg p "$OUT" --arg preset "$BADPRESET" '{path: $p, preset: $preset}')"
  expect_eq "export: preset '$BADPRESET' is refused" true "$(tool_is_error export_video "$ARGS")"
done
expect_eq "export: refused presets started no job" idle "$(tool_json get_export_status '{}' | jq -r '.state')"

echo "== export: never overwrite media used by the project"
# A is only in the playlist, B is on the timeline; a symlink and a "/./" path must not get around it.
ln -s "$CLIP_B" "$TMP/link.mp4"
SUM_A="$(md5sum < "$CLIP_A")"; SUM_B="$(md5sum < "$CLIP_B")"
expect_eq "export: overwrite of a timeline clip is refused" true "$(tool_is_error export_video "{\"path\":\"$CLIP_B\",\"overwrite\":true}")"
expect_eq "export: overwrite of a playlist-only clip is refused" true "$(tool_is_error export_video "{\"path\":\"$CLIP_A\",\"overwrite\":true}")"
expect_eq "export: a /./ path to a clip is refused" true "$(tool_is_error export_video "{\"path\":\"$TMP/./b.mp4\",\"overwrite\":true}")"
expect_eq "export: a symlink to a clip is refused" true "$(tool_is_error export_video "{\"path\":\"$TMP/link.mp4\",\"overwrite\":true}")"
expect_eq "export: clip B is unchanged" "$SUM_B" "$(md5sum < "$CLIP_B")"
expect_eq "export: clip A is unchanged" "$SUM_A" "$(md5sum < "$CLIP_A")"
expect_eq "export: refused overwrites started no job" idle "$(tool_json get_export_status '{}' | jq -r '.state')"

wait_export() { # job id -> final state (up to 300 s)
  local st=""
  for _ in $(seq 1 300); do
    st="$(tool_json get_export_status "{\"job_id\":$1}" | jq -r '.state')"
    case "$st" in succeeded|failed|stopped|removed) break ;; esac
    sleep 1
  done
  echo "$st"
}

if [ -n "${MCP_APPDATA:-}" ]; then
  echo "== export: slow custom preset (deterministic while-running checks)"
  mkdir -p "$MCP_APPDATA/presets/encode"
  printf '%s\n' f=mp4 vcodec=libx264 preset=slower crf=18 width=1280 height=720 acodec=aac ab=128k \
    meta.preset.extension=mp4 > "$MCP_APPDATA/presets/encode/mcp-slow"
  SLOW="$TMP/slow.mp4"
  SSTART="$(tool_json export_video "{\"path\":\"$SLOW\",\"preset\":\"mcp-slow\"}")"
  SJOB="$(echo "$SSTART" | jq '.job_id')"
  expect_eq "export: custom preset by file name is accepted" "$SLOW" "$(echo "$SSTART" | jq -r '.output')"
  expect_eq "export: slow job is not finished right after it starts" true "$(tool_json get_export_status "{\"job_id\":$SJOB}" | jq '.state | IN("pending","running")')"
  expect_eq "export: a second export while one runs is an error" true "$(tool_is_error export_video "{\"path\":\"$TMP/second.mp4\"}")"
  expect_eq "export: a repeat with overwrite while running is an error (no dialog)" true "$(tool_is_error export_video "{\"path\":\"$SLOW\",\"overwrite\":true}")"
  expect_eq "export: slow job still running after the refusals" true "$(tool_json get_export_status "{\"job_id\":$SJOB}" | jq '.state | IN("pending","running")')"
  SSTATE="$(wait_export "$SJOB")"
  expect_eq "export: slow job finished successfully" succeeded "$SSTATE"
  echo "NOTE: slow export took $(tool_json get_export_status "{\"job_id\":$SJOB}" | jq '.elapsed_ms') ms"
  expect_eq "export: custom preset applied (1280x720)" 1280x720 "$(ffprobe -v error -select_streams v:0 -show_entries stream=width,height -of csv=s=x:p=0 "$SLOW")"
  expect_eq "export: custom preset applied (h264)" h264 "$(ffprobe -v error -select_streams v:0 -show_entries stream=codec_name -of csv=p=0 "$SLOW")"
else
  echo "NOTE: MCP_APPDATA is not set; the two 'while one runs' checks race the fast render and may flake."
fi

START="$(tool_json export_video "{\"path\":\"$OUT\"}")"
JOB="$(echo "$START" | jq '.job_id')"
expect_eq "export: returns a job id and the output path" "$OUT" "$(echo "$START" | jq -r '.output')"
if [ -z "${MCP_APPDATA:-}" ]; then
  expect_eq "export: a second export while one runs is an error" true "$(tool_is_error export_video "{\"path\":\"$TMP/second.mp4\"}")"
  expect_eq "export: a repeat with overwrite while running is an error (no dialog)" true "$(tool_is_error export_video "{\"path\":\"$OUT\",\"overwrite\":true}")"
fi
STATE="$(wait_export "$JOB")"
expect_eq "export: finished successfully" succeeded "$STATE"
S="$(tool_json get_export_status "{\"job_id\":$JOB}")"
expect_eq "export: percent is 100" 100 "$(echo "$S" | jq '.percent')"
expect_eq "export: output exists" true "$(echo "$S" | jq '.output_exists')"
expect_eq "export: output is not empty" true "$(echo "$S" | jq '.output_bytes > 0')"
expect_eq "export: exit code 0" 0 "$(echo "$S" | jq '.exit_code')"
expect_eq "export: latest job is the default" "$JOB" "$(tool_json get_export_status '{}' | jq '.job_id')"
expect_eq "ffprobe: video codec h264" h264 "$(ffprobe -v error -select_streams v:0 -show_entries stream=codec_name -of csv=p=0 "$OUT")"
expect_eq "ffprobe: audio codec aac" aac "$(ffprobe -v error -select_streams a:0 -show_entries stream=codec_name -of csv=p=0 "$OUT")"
DUR="$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$OUT")"
expect_eq "ffprobe: duration matches the timeline (+-0.5 s)" true "$(awk -v d="$DUR" -v f="$TLDUR" 'BEGIN { e = f / 25.0; print (d > e - 0.5 && d < e + 0.5) ? "true" : "false" }')"
expect_eq "export: existing output without overwrite is now refused" true "$(tool_is_error export_video "{\"path\":\"$OUT\"}")"

# FOOTER (later tasks insert their blocks above this line)
if [ "$failures" -eq 0 ]; then echo "All checks passed."; else echo "${failures} check(s) failed."; exit 1; fi
