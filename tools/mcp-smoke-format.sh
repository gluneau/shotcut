#!/usr/bin/env bash
# Format smoke test for the Shotcut MCP server (stage 3, part 3C): project_set_format, clip_reframe,
# title_add, lower_third_add, captions_from_transcript.
# Usage: MCP_TOKEN=<token> [MCP_PORT=7468] MCP_APPDATA=<scratch dir> tools/mcp-smoke-format.sh
# Run it against a FRESH, throwaway Shotcut (nothing loaded, no timeline); it aborts otherwise
# unless MCP_SMOKE_FORCE=1. It creates its media with ffmpeg in a temporary folder and exports
# three files. MCP_APPDATA (the instance's --appdata folder) is required: the script checks the
# saved Video Mode setting in its shotcut.ini is left alone (a FAIL when it is not set).
# Needs: bash, curl, jq, ffmpeg, ffprobe.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=tools/mcp-smoke-lib.sh
. "$HERE/mcp-smoke-lib.sh"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fresh_guard

MEDIA="$("$HERE/mcp-fixtures.sh" "$TMP/media")" || { echo "ffmpeg could not create the fixtures"; exit 2; }

TOOLS="$(rpc '{"jsonrpc":"2.0","id":3,"method":"tools/list"}' | jq -c '[.result.tools[]]')"
expect_eq "tools/list: 56 tools in all (through Task 20)" 56 "$(echo "$TOOLS" | jq 'length')"
expect_eq "tools/list has project_set_format" true "$(echo "$TOOLS" | jq '[.[].name] | index("project_set_format") != null')"
expect_eq "project_set_format schema refuses extra properties" false "$(echo "$TOOLS" | jq '.[] | select(.name=="project_set_format") | .inputSchema.additionalProperties')"
expect_eq "tools/list has clip_reframe" true "$(echo "$TOOLS" | jq '[.[].name] | index("clip_reframe") != null')"
expect_eq "clip_reframe schema refuses extra properties" false "$(echo "$TOOLS" | jq '.[] | select(.name=="clip_reframe") | .inputSchema.additionalProperties')"
for t in title_add lower_third_add; do
  expect_eq "tools/list has $t" true "$(echo "$TOOLS" | jq --arg t "$t" '[.[].name] | index($t) != null')"
  expect_eq "$t schema refuses extra properties" false "$(echo "$TOOLS" | jq --arg t "$t" '.[] | select(.name==$t) | .inputSchema.additionalProperties')"
done

prof() { tool_json get_state '{}' | jq -c '.profile'; }
fmt() { tool_json project_set_format "$1" | jq -cS .; }
ferr() { tool_is_error project_set_format "$1"; }

echo "== project_set_format: argument errors (nothing changes)"
P0="$(prof)"
expect_eq "get_state has profile {width,height,fps_num,fps_den}" true "$(echo "$P0" | jq 'keys == ["fps_den","fps_num","height","width"]')"
expect_eq "no arguments is an error" true "$(ferr '{}')"
expect_eq "fps alone is an error" true "$(ferr '{"fps":30}')"
expect_eq "odd width is an error" true "$(ferr '{"width":721,"height":1280}')"
expect_eq "odd height is an error" true "$(ferr '{"width":720,"height":1281}')"
expect_eq "width without height is an error" true "$(ferr '{"width":720}')"
expect_eq "height without width is an error" true "$(ferr '{"height":1280}')"
expect_eq "preset and width is an error" true "$(ferr '{"preset":"9:16","width":720,"height":1280}')"
expect_eq "width 62 is an error" true "$(ferr '{"width":62,"height":1280}')"
expect_eq "height 7682 is an error" true "$(ferr '{"width":720,"height":7682}')"
expect_eq "fractional width is an error" true "$(ferr '{"width":720.5,"height":1280}')"
expect_eq "string width is an error" true "$(ferr '{"width":"720","height":1280}')"
expect_eq "fps 31 is an error" true "$(ferr '{"preset":"9:16","fps":31}')"
expect_eq "fps as a string is an error" true "$(ferr '{"preset":"9:16","fps":"30"}')"
expect_eq "preset 21:9 is an error" true "$(ferr '{"preset":"21:9"}')"
expect_eq "preset as a number is an error" true "$(ferr '{"preset":1}')"
expect_eq "an unknown argument is an error" true "$(ferr '{"preset":"9:16","fsp":30}')"
expect_eq "the errors changed nothing" "$P0" "$(prof)"

echo "== project_set_format on the empty session"
expect_eq "16:9 is 1920x1080 30 fps" '{"dar":"16:9","fps_den":1,"fps_num":30,"height":1080,"sar":"1:1","width":1920}' "$(fmt '{"preset":"16:9"}')"
expect_eq "1:1 is 1080x1080" '{"dar":"1:1","fps_den":1,"fps_num":30,"height":1080,"sar":"1:1","width":1080}' "$(fmt '{"preset":"1:1"}')"
expect_eq "4:5 is 1080x1350" '{"dar":"4:5","fps_den":1,"fps_num":30,"height":1350,"sar":"1:1","width":1080}' "$(fmt '{"preset":"4:5"}')"
expect_eq "custom 720x1280 at 25 fps" '{"dar":"9:16","fps_den":1,"fps_num":25,"height":1280,"sar":"1:1","width":720}' "$(fmt '{"width":720,"height":1280,"fps":25}')"
expect_eq "get_state.profile agrees (720x1280 25)" '{"fps_den":1,"fps_num":25,"height":1280,"width":720}' "$(prof | jq -cS .)"
expect_eq "get_state.fps is 25" 25 "$(tool_json get_state '{}' | jq '.fps')"
expect_eq "fps 23.976 is 24000/1001" '[24000,1001]' "$(fmt '{"preset":"16:9","fps":23.976}' | jq -c '[.fps_num,.fps_den]')"
expect_eq "fps 29.97 is 30000/1001" '[30000,1001]' "$(fmt '{"preset":"16:9","fps":29.97}' | jq -c '[.fps_num,.fps_den]')"
expect_eq "fps 59.94 is 60000/1001" '[60000,1001]' "$(fmt '{"preset":"16:9","fps":59.94}' | jq -c '[.fps_num,.fps_den]')"
expect_eq "fps 60 is 60/1" '[60,1]' "$(fmt '{"preset":"16:9","fps":60}' | jq -c '[.fps_num,.fps_den]')"
expect_eq "9:16 is 1080x1920 30 fps (default fps)" '{"dar":"9:16","fps_den":1,"fps_num":30,"height":1920,"sar":"1:1","width":1080}' "$(fmt '{"preset":"9:16"}')"
P916='{"fps_den":1,"fps_num":30,"height":1920,"width":1080}'
expect_eq "get_state.profile agrees (1080x1920 30)" "$P916" "$(prof | jq -cS .)"
expect_eq "nothing is loaded after project_set_format" false "$(tool_json get_state '{}' | jq '.loaded')"

echo "== the format survives adding media"
expect_eq "playlist_add scenes.mp4" false "$(tool_is_error playlist_add "{\"path\":\"$MEDIA/scenes.mp4\"}")"
expect_eq "a clip is loaded" true "$(wait_state get_state '{}' '.loaded' true)"
expect_eq "the 320x240 25 fps clip did not change the profile" "$P916" "$(prof | jq -cS .)"
expect_eq "timeline_add_playlist" false "$(tool_is_error timeline_add_playlist '{}')"
expect_eq "get_timeline.fps is 30" 30 "$(tool_json get_timeline '{}' | jq '.fps')"

echo "== refused once something is loaded"
expect_eq "project_set_format with a clip loaded is an error" true "$(ferr '{"preset":"16:9"}')"
expect_eq "the refusal message" "The project must be empty: start a new project first." "$(tool_text project_set_format '{"preset":"16:9"}')"
expect_eq "the refusal changed nothing" "$P916" "$(prof | jq -cS .)"

echo "== export is 1080x1920 at 30 fps"
OUT="$TMP/vertical.mp4"
JOB="$(tool_json export_video "{\"path\":\"$OUT\"}" | jq '.job_id')"
st=""
for _ in $(seq 1 300); do
  st="$(tool_json get_export_status "{\"job_id\":$JOB}" | jq -r '.state')"
  case "$st" in succeeded|failed|stopped|removed) break ;; esac
  sleep 1
done
expect_eq "export succeeded" succeeded "$st"
expect_eq "exported size is 1080x1920" 1080x1920 "$(ffprobe -v error -select_streams v:0 -show_entries stream=width,height -of csv=s=x:p=0 "$OUT" 2>/dev/null)"
expect_eq "exported frame rate is 30/1" 30/1 "$(ffprobe -v error -select_streams v:0 -show_entries stream=r_frame_rate -of csv=p=0 "$OUT" 2>/dev/null)"

# pixel X Y FILE: "R G B" of the frame at 0.5 s of FILE at (X, Y).
pixel() { ffmpeg -loglevel error -ss 0.5 -i "$3" -frames:v 1 -vf "format=rgb24,crop=1:1:$1:$2" -f rawvideo -pix_fmt rgb24 - | od -An -tu1 | xargs; }
# corners FILE: "red" or "black" or "other" for each corner of the 1080x1920 frame (4 px inset).
corners() {
  local out="" xy r g b
  for xy in "4 4" "1075 4" "4 1915" "1075 1915"; do
    read -r r g b <<<"$(pixel $xy "$1")"
    if [ "${r:-0}" -gt 150 ] && [ "${g:-255}" -lt 100 ] && [ "${b:-255}" -lt 100 ]; then out="$out red"
    elif [ "${r:-255}" -lt 30 ] && [ "${g:-255}" -lt 30 ] && [ "${b:-255}" -lt 30 ]; then out="$out black"
    else out="$out other($r,$g,$b)"; fi
  done
  echo "${out# }"
}
expect_eq "control: without a reframe the 4:3 clip leaves black bars (corners)" "black black black black" "$(corners "$OUT")"

echo "== clip_reframe: argument errors (nothing changes)"
fl() { tool_json filter_list_applied '{"track":0,"clip":0}' | jq -c '[.filters[] | {id, rect: .params.rect}]'; }
rf() { tool_json clip_reframe "$1" | jq -c .; }
rerr() { tool_is_error clip_reframe "$1"; }
expect_eq "the timeline clip has no filters yet" '[]' "$(fl)"
kf51="$(jq -nc '[range(0;51) | {frame: ., focus_x: 0.5}]')"
expect_eq "focus_x 1.5 is an error" true "$(rerr '{"track":0,"clip":0,"mode":"fill","focus_x":1.5}')"
expect_eq "focus_x -0.1 is an error" true "$(rerr '{"track":0,"clip":0,"mode":"fill","focus_x":-0.1}')"
expect_eq "mode missing is an error" true "$(rerr '{"track":0,"clip":0}')"
expect_eq "mode crop is an error" true "$(rerr '{"track":0,"clip":0,"mode":"crop"}')"
expect_eq "an unknown argument is an error" true "$(rerr '{"track":0,"clip":0,"mode":"fill","focus":0.5}')"
expect_eq "keyframes out of order is an error" true "$(rerr '{"track":0,"clip":0,"mode":"fill","keyframes":[{"frame":30,"focus_x":0},{"frame":10,"focus_x":1}]}')"
expect_eq "keyframes on the same frame is an error" true "$(rerr '{"track":0,"clip":0,"mode":"fill","keyframes":[{"frame":10,"focus_x":0},{"frame":10,"focus_x":1}]}')"
expect_eq "a negative keyframe frame is an error" true "$(rerr '{"track":0,"clip":0,"mode":"fill","keyframes":[{"frame":-1,"focus_x":0}]}')"
expect_eq "a keyframe after the clip's end is an error" true "$(rerr '{"track":0,"clip":0,"mode":"fill","keyframes":[{"frame":270,"focus_x":0}]}')"
expect_eq "a keyframe focus_x 1.5 is an error" true "$(rerr '{"track":0,"clip":0,"mode":"fill","keyframes":[{"frame":0,"focus_x":1.5}]}')"
expect_eq "a keyframe with an unknown key is an error" true "$(rerr '{"track":0,"clip":0,"mode":"fill","keyframes":[{"frame":0,"focus_x":0,"y":1}]}')"
expect_eq "empty keyframes is an error" true "$(rerr '{"track":0,"clip":0,"mode":"fill","keyframes":[]}')"
expect_eq "51 keyframes is an error" true "$(rerr "{\"track\":0,\"clip\":0,\"mode\":\"fill\",\"keyframes\":$kf51}")"
expect_eq "keyframes with pad is an error" true "$(rerr '{"track":0,"clip":0,"mode":"pad","keyframes":[{"frame":0,"focus_x":0}]}')"
expect_eq "focus_x with pad is an error" true "$(rerr '{"track":0,"clip":0,"mode":"pad","focus_x":0.2}')"
expect_eq "focus_x with keyframes is an error" true "$(rerr '{"track":0,"clip":0,"mode":"fill","focus_x":0.2,"keyframes":[{"frame":0,"focus_x":0}]}')"
expect_eq "interpolation bouncy is an error" true "$(rerr '{"track":0,"clip":0,"mode":"fill","interpolation":"bouncy","keyframes":[{"frame":0,"focus_x":0}]}')"
expect_eq "interpolation without keyframes is an error" true "$(rerr '{"track":0,"clip":0,"mode":"fill","interpolation":"smooth"}')"
expect_eq "a blank or missing clip is an error" true "$(rerr '{"track":0,"clip":5,"mode":"fill"}')"
expect_eq "the errors changed nothing" '[]' "$(fl)"

echo "== clip_reframe fill (320x240 4:3 clip in 1080x1920)"
rf '{"track":0,"clip":0,"mode":"fill"}' >/dev/null
expect_eq "fill: one size_position filter, rect height 1920 (y 0), width 1920*4/3 = 2560, centred" '[{"id":"size_position","rect":{"h":1920,"w":2560,"x":-740,"y":0}}]' "$(fl | jq -cS .)"
SAVED="$TMP/reframe.mlt"
# reframe_prop NAME: the property NAME of the filters marked shotcut:reframe=1 in $SAVED.
reframe_prop() {
  awk -v want="$1" '/<filter /{f=1; mark=0; val=""} f && /name="shotcut:reframe">1</{mark=1}
    f && index($0, "name=\"" want "\">"){v=$0; sub(/.*">/,"",v); sub(/<\/property>.*/,"",v); val=v}
    /<\/filter>/{ if (f && mark) print val; f=0 }' "$SAVED"
}
tool_json save_project "{\"path\":\"$SAVED\"}" >/dev/null
expect_eq "fill: transition.fill 1" 1 "$(reframe_prop transition.fill)"
expect_eq "fill: transition.distort 0" 0 "$(reframe_prop transition.distort)"
expect_eq "fill: transition.rect" "-740 0 2560 1920 1" "$(reframe_prop transition.rect)"
expect_eq "fill: shotcut:filter affineSizePosition" affineSizePosition "$(reframe_prop shotcut:filter)"
expect_eq "fill: undo text" "Reframe clip (fill)" "$(tool_json undo '{}' | jq -r '.text')"
expect_eq "fill: one undo removes it" '[]' "$(fl)"
tool_json redo '{}' >/dev/null
expect_eq "fill: redo puts it back" '[{"id":"size_position","rect":{"h":1920,"w":2560,"x":-740,"y":0}}]' "$(fl | jq -cS .)"
rf '{"track":0,"clip":0,"mode":"fill","focus_x":0}' >/dev/null
expect_eq "focus_x 0: x 0 (the left edge), still one filter" '[{"id":"size_position","rect":{"h":1920,"w":2560,"x":0,"y":0}}]' "$(fl | jq -cS .)"
rf '{"track":0,"clip":0,"mode":"fill","focus_x":1}' >/dev/null
expect_eq "focus_x 1: x 1080 - 2560 = -1480" '[{"id":"size_position","rect":{"h":1920,"w":2560,"x":-1480,"y":0}}]' "$(fl | jq -cS .)"
expect_eq "focus_x 1: undo goes back to focus_x 0" '[{"id":"size_position","rect":{"h":1920,"w":2560,"x":0,"y":0}}]' "$(tool_json undo '{}' >/dev/null; fl | jq -cS .)"
tool_json redo '{}' >/dev/null
rf '{"track":0,"clip":0,"mode":"fill","focus_x":0.25}' >/dev/null
expect_eq "focus_x 0.25: x 540 - 640 = -100" '{"h":1920,"w":2560,"x":-100,"y":0}' "$(fl | jq -cS '.[0].rect')"

echo "== clip_reframe keyframes"
rf '{"track":0,"clip":0,"mode":"fill","keyframes":[{"frame":0,"focus_x":0},{"frame":60,"focus_x":1}]}' >/dev/null
expect_eq "linear keyframes: the animation string" '"0=0 0 2560 1920 1;60=-1480 0 2560 1920 1"' "$(fl | jq -c '.[0].rect')"
rf '{"track":0,"clip":0,"mode":"fill","interpolation":"smooth","keyframes":[{"frame":0,"focus_x":0},{"frame":30,"focus_x":0.5},{"frame":90,"focus_x":1}]}' >/dev/null
expect_eq "smooth keyframes: ~ on every keyframe" '"0~=0 0 2560 1920 1;30~=-740 0 2560 1920 1;90~=-1480 0 2560 1920 1"' "$(fl | jq -c '.[0].rect')"
expect_eq "keyframes: still one filter" 1 "$(fl | jq 'length')"
tool_json save_project "{\"path\":\"$SAVED\"}" >/dev/null
# MLT parsed the string: it saves the keyframes as times (frames 30 and 90 at 30 fps), types kept.
expect_eq "keyframes: MLT read the animation back (saved as times)" "00:00:00.000~=0 0 2560 1920 1;00:00:01.000~=-740 0 2560 1920 1;00:00:03.000~=-1480 0 2560 1920 1" "$(reframe_prop transition.rect)"
rf '{"track":0,"clip":0,"mode":"fill","keyframes":[{"frame":12,"focus_x":0.5}]}' >/dev/null
expect_eq "one keyframe" '"12=-740 0 2560 1920 1"' "$(fl | jq -c '.[0].rect')"
rf '{"track":0,"clip":0,"mode":"fill"}' >/dev/null
expect_eq "fill without keyframes replaces the keyframes" '{"h":1920,"w":2560,"x":-740,"y":0}' "$(fl | jq -cS '.[0].rect')"

echo "== clip_reframe replaces only its own filter"
tool_json filter_set '{"track":0,"clip":0,"index":0,"params":{"rect":{"x":-700,"y":0,"w":2560,"h":1920}}}' >/dev/null
expect_eq "filter_set tweaked the reframe" -700 "$(fl | jq '.[0].rect.x')"
tool_json save_project "{\"path\":\"$SAVED\"}" >/dev/null
expect_eq "filter_set keeps the shotcut:reframe mark" "-700 0 2560 1920 1" "$(reframe_prop transition.rect)"
rf '{"track":0,"clip":0,"mode":"fill"}' >/dev/null
expect_eq "clip_reframe replaced the tweaked reframe" '[{"id":"size_position","rect":{"h":1920,"w":2560,"x":-740,"y":0}}]' "$(fl | jq -cS .)"
echo "== the mark survives remove and undo"
tool_json filter_remove '{"track":0,"clip":0,"index":0}' >/dev/null
expect_eq "the reframe is removed" '[]' "$(fl)"
tool_json undo '{}' >/dev/null
expect_eq "undo restores it" 1 "$(fl | jq 'length')"
rf '{"track":0,"clip":0,"mode":"fill","focus_x":0.5}' >/dev/null
expect_eq "clip_reframe after the undo: exactly one filter (the restored one was replaced)" '[{"id":"size_position","rect":{"h":1920,"w":2560,"x":-740,"y":0}}]' "$(fl | jq -cS .)"

echo "== refused on a clip with its own Size/Position or Blur: Pad"
tool_json filter_add '{"track":0,"clip":0,"filter":"size_position","params":{"rect":{"x":10,"y":20,"w":300,"h":400}}}' >/dev/null
FU="$(fl | jq -cS .)"
expect_eq "the user's size_position is the second filter" '{"h":400,"w":300,"x":10,"y":20}' "$(echo "$FU" | jq -cS '.[1].rect')"
expect_eq "fill with the user's size_position is an error" true "$(rerr '{"track":0,"clip":0,"mode":"fill","focus_x":0}')"
expect_eq "pad with the user's size_position is an error" true "$(rerr '{"track":0,"clip":0,"mode":"pad"}')"
expect_eq "the refusal names the fix" true "$(tool_text clip_reframe '{"track":0,"clip":0,"mode":"fill"}' | grep -q 'Remove or adjust the clip.s own Size, Position & Rotate / Blur: Pad first' && echo true || echo false)"
expect_eq "the refusals changed nothing" "$FU" "$(fl | jq -cS .)"
tool_json filter_remove '{"track":0,"clip":0,"index":1}' >/dev/null
expect_eq "clip_set_blur pad (a user Blur: Pad)" false "$(tool_is_error clip_set_blur '{"track":0,"clip":0,"mode":"pad"}')"
FU="$(fl | jq -cS .)"
expect_eq "fill with the user's Blur: Pad is an error" true "$(rerr '{"track":0,"clip":0,"mode":"fill"}')"
expect_eq "pad with the user's Blur: Pad is an error" true "$(rerr '{"track":0,"clip":0,"mode":"pad"}')"
expect_eq "those refusals changed nothing" "$FU" "$(fl | jq -cS .)"
tool_json undo '{}' >/dev/null
expect_eq "undo removed the user's Blur: Pad" '["size_position"]' "$(fl | jq -c '[.[].id]')"
rf '{"track":0,"clip":0,"mode":"fill","focus_x":0}' >/dev/null
expect_eq "without the user's filters the reframe is set again" '[{"id":"size_position","rect":{"h":1920,"w":2560,"x":0,"y":0}}]' "$(fl | jq -cS .)"

echo "== clip_reframe pad"
rf '{"track":0,"clip":0,"mode":"pad"}' >/dev/null
expect_eq "pad: blur_pad replaces the fill reframe" '["blur_pad"]' "$(fl | jq -c '[.[].id]')"
expect_eq "pad: undo text" "Reframe clip (pad)" "$(tool_json undo '{}' | jq -r '.text')"
expect_eq "pad: one undo brings the fill back" '[{"id":"size_position","rect":{"h":1920,"w":2560,"x":0,"y":0}}]' "$(fl | jq -cS .)"
tool_json redo '{}' >/dev/null
rf '{"track":0,"clip":0,"mode":"pad"}' >/dev/null
expect_eq "pad again: still one blur_pad" '["blur_pad"]' "$(fl | jq -c '[.[].id]')"
rf '{"track":0,"clip":0,"mode":"fill"}' >/dev/null
expect_eq "fill after pad: the pad reframe is replaced" '[{"id":"size_position","rect":{"h":1920,"w":2560,"x":-740,"y":0}}]' "$(fl | jq -cS .)"

echo "== clip_reframe on a clip that already matches"
ffmpeg -loglevel error -y -f lavfi -i "color=c=white:size=180x320:rate=30:duration=2" -pix_fmt yuv420p -c:v libx264 "$TMP/tall.mp4"
ffmpeg -loglevel error -y -f lavfi -i "color=c=white:size=180x318:rate=30:duration=2" -pix_fmt yuv420p -c:v libx264 "$TMP/almost.mp4"
ffmpeg -loglevel error -y -f lavfi -i "color=c=white:size=184x320:rate=30:duration=2" -pix_fmt yuv420p -c:v libx264 "$TMP/wider.mp4"
ins1() { tool_is_error timeline_insert_range "$(jq -nc --arg p "$1" '{path:$p,in_s:0,out_s:1,track:0,position:270}')"; }
expect_eq "insert a 9:16 clip" false "$(ins1 "$TMP/tall.mp4")"
expect_eq "a 9:16 clip is an error" "The clip already matches the project format." "$(tool_text clip_reframe '{"track":0,"clip":1,"mode":"fill"}')"
expect_eq "a 9:16 clip is an error with pad too" true "$(rerr '{"track":0,"clip":1,"mode":"pad"}')"
expect_eq "the error added nothing" 0 "$(tool_json filter_list_applied '{"track":0,"clip":1}' | jq '.filters | length')"
tool_json undo '{}' >/dev/null
expect_eq "insert a 180x318 clip (0.6% off)" false "$(ins1 "$TMP/almost.mp4")"
expect_eq "within 1% is an error" true "$(rerr '{"track":0,"clip":1,"mode":"fill"}')"
tool_json undo '{}' >/dev/null
expect_eq "insert a 184x320 clip (2.2% off)" false "$(ins1 "$TMP/wider.mp4")"
expect_eq "2.2% off is reframed" '{"h":1920,"w":1104,"x":-12,"y":0}' "$(rf '{"track":0,"clip":1,"mode":"fill"}' >/dev/null; tool_json filter_list_applied '{"track":0,"clip":1}' | jq -cS '.filters[0].params.rect')"
tool_json undo '{}' >/dev/null
tool_json undo '{}' >/dev/null
expect_eq "the timeline has the one clip again" 1 "$(tool_json get_timeline '{}' | jq '[.tracks[0].clips[] | select(.kind=="clip")] | length')"

echo "== keyframes on a clip with a non-zero in point"
# reframe_attr NAME: the attribute NAME of the <filter> elements marked shotcut:reframe=1 in $SAVED.
reframe_attr() {
  awk -v want="$1" '/<filter /{f=1; mark=0; a=""; if (match($0, want "=\"[^\"]*\"")) a=substr($0, RSTART + length(want) + 2, RLENGTH - length(want) - 3)}
    f && /name="shotcut:reframe">1</{mark=1} /<\/filter>/{ if (f && mark) print a; f=0 }' "$SAVED"
}
tool_json filter_remove '{"track":0,"clip":0,"index":0}' >/dev/null
expect_eq "trim the clip's in edge by 15 frames (ripple)" false "$(tool_is_error timeline_trim '{"track":0,"clip":0,"edge":"in","delta":15,"ripple":true}')"
expect_eq "the clip's in point is 15" 15 "$(tool_json get_timeline '{}' | jq '.tracks[0].clips[0].in')"
rf '{"track":0,"clip":0,"mode":"fill","keyframes":[{"frame":0,"focus_x":0},{"frame":30,"focus_x":1}]}' >/dev/null
expect_eq "keyframes stay clip-relative (0 is the clip's first frame)" '"0=0 0 2560 1920 1;30=-1480 0 2560 1920 1"' "$(fl | jq -c '.[0].rect')"
tool_json save_project "{\"path\":\"$SAVED\"}" >/dev/null
expect_eq "the reframe filter's in is the clip's in (15 frames = 0.5 s at 30 fps)" "00:00:00.500" "$(reframe_attr in)"
# MLT positions a filter's keyframes from the filter's in, so frame 0 is the clip's first frame.
expect_eq "the saved keyframes are clip-relative (from that in)" "0=0 0 2560 1920 1;30=-1480 0 2560 1920 1" "$(reframe_prop transition.rect)"
rf '{"track":0,"clip":0,"mode":"fill"}' >/dev/null
expect_eq "fill again for the export (one filter)" '[{"id":"size_position","rect":{"h":1920,"w":2560,"x":-740,"y":0}}]' "$(fl | jq -cS .)"

echo "== export a 1-second 9:16 project with fill: no black bars"
expect_eq "trim the clip to 30 frames" false "$(tool_is_error timeline_trim '{"track":0,"clip":0,"edge":"out","frame":30}')"
expect_eq "the timeline lasts 30 frames" 30 "$(tool_json get_timeline '{}' | jq '.duration')"
OUT2="$TMP/reframed.mp4"
JOB="$(tool_json export_video "{\"path\":\"$OUT2\"}" | jq '.job_id')"
st=""
for _ in $(seq 1 300); do
  st="$(tool_json get_export_status "{\"job_id\":$JOB}" | jq -r '.state')"
  case "$st" in succeeded|failed|stopped|removed) break ;; esac
  sleep 1
done
expect_eq "reframed export succeeded" succeeded "$st"
expect_eq "reframed export is 1080x1920" 1080x1920 "$(ffprobe -v error -select_streams v:0 -show_entries stream=width,height -of csv=s=x:p=0 "$OUT2" 2>/dev/null)"
expect_eq "the four corners are red (no black bars)" "red red red red" "$(corners "$OUT2")"

echo "== title_add and lower_third_add: argument errors (nothing changes)"
trk() { tool_json get_timeline '{}' | jq -c ".tracks[$1]"; }
terr() { tool_is_error title_add "$1"; }
lerr() { tool_is_error lower_third_add "$1"; }
expect_eq "track_add a video track for the titles (on top, index 0)" 0 "$(tool_json track_add '{"type":"video"}' | jq '.added.index')"
expect_eq "the background clip is now on track 1" 30 "$(trk 1 | jq '.clips[0].duration')"
T0="$(trk 0)"
TL0="$(tool_json get_timeline '{}' | jq -c '.tracks')"
long501="$(jq -nr '[range(0;501) | "a"] | join("")')"
expect_eq "empty text is an error" true "$(terr '{"text":"","track":0,"position":0,"duration":30}')"
expect_eq "blank text is an error" true "$(terr '{"text":"  \n ","track":0,"position":0,"duration":30}')"
expect_eq "text of 501 characters is an error" true "$(terr "{\"text\":\"$long501\",\"track\":0,\"position\":0,\"duration\":30}")"
expect_eq "text as a number is an error" true "$(terr '{"text":5,"track":0,"position":0,"duration":30}')"
expect_eq "text missing is an error" true "$(terr '{"track":0,"position":0,"duration":30}')"
expect_eq "size 5 is an error" true "$(terr '{"text":"Hi","track":0,"position":0,"duration":30,"size":5}')"
expect_eq "size 401 is an error" true "$(terr '{"text":"Hi","track":0,"position":0,"duration":30,"size":401}')"
expect_eq "color #fff is an error" true "$(terr '{"text":"Hi","track":0,"position":0,"duration":30,"color":"#fff"}')"
expect_eq "color white is an error" true "$(terr '{"text":"Hi","track":0,"position":0,"duration":30,"color":"white"}')"
expect_eq "bg_color #gg0000 is an error" true "$(terr '{"text":"Hi","track":0,"position":0,"duration":30,"bg_color":"#gg0000"}')"
expect_eq "outline 21 is an error" true "$(terr '{"text":"Hi","track":0,"position":0,"duration":30,"outline":21}')"
expect_eq "align centre is an error" true "$(terr '{"text":"Hi","track":0,"position":0,"duration":30,"align":"centre"}')"
expect_eq "valign center is an error" true "$(terr '{"text":"Hi","track":0,"position":0,"duration":30,"valign":"center"}')"
expect_eq "an empty font is an error" true "$(terr '{"text":"Hi","track":0,"position":0,"duration":30,"font":""}')"
expect_eq "duration 0 is an error" true "$(terr '{"text":"Hi","track":0,"position":0,"duration":0}')"
expect_eq "duration 100001 is an error" true "$(terr '{"text":"Hi","track":0,"position":0,"duration":100001}')"
expect_eq "position -1 is an error" true "$(terr '{"text":"Hi","track":0,"position":-1,"duration":30}')"
expect_eq "mode ripple is an error" true "$(terr '{"text":"Hi","track":0,"position":0,"duration":30,"mode":"ripple"}')"
expect_eq "an unknown argument is an error" true "$(terr '{"text":"Hi","track":0,"position":0,"duration":30,"colour":"#ffffff"}')"
expect_eq "a missing track is an error" true "$(terr '{"text":"Hi","track":9,"position":0,"duration":30}')"
long257="$(jq -nr '[range(0;257) | "a"] | join("")')"
expect_eq "text of 257 characters is an error (the Filters panel keeps 256)" true "$(terr "{\"text\":\"$long257\",\"track\":0,\"position\":0,\"duration\":30}")"
# 129 # signs are 258 characters once escaped (\#).
hash129="$(jq -nr '[range(0;129) | "#"] | join("")')"
expect_eq "129 # signs are an error (each counts twice)" true "$(terr "{\"text\":\"$hash129\",\"track\":0,\"position\":0,\"duration\":30}")"
# 171 three-byte characters: 513 bytes, more than MLT's text filter renders (511).
long_e="$(jq -nr '[range(0;171) | "€"] | join("")')"
expect_eq "text over 511 UTF-8 bytes is an error" true "$(terr "{\"text\":\"$long_e\",\"track\":0,\"position\":0,\"duration\":30}")"
AUD="$(tool_json track_add '{"type":"audio"}' | jq '.added.index')"
expect_eq "title_add on an audio track is an error" true "$(terr "{\"text\":\"Hi\",\"track\":$AUD,\"position\":0,\"duration\":30}")"
expect_eq "lower_third_add on an audio track is an error" true "$(lerr "{\"line1\":\"Hi\",\"track\":$AUD,\"position\":0,\"duration\":30}")"
tool_json undo '{}' >/dev/null
tool_json track_set '{"track":0,"lock":true}' >/dev/null
expect_eq "title_add on a locked track is an error" true "$(terr '{"text":"Hi","track":0,"position":0,"duration":30}')"
expect_eq "lower_third_add on a locked track is an error" true "$(lerr '{"line1":"Hi","track":0,"position":0,"duration":30}')"
tool_json track_set '{"track":0,"lock":false}' >/dev/null
expect_eq "empty line1 is an error" true "$(lerr '{"line1":"","track":0,"position":0,"duration":30}')"
expect_eq "line1 with a newline is an error" true "$(lerr '{"line1":"a\nb","track":0,"position":0,"duration":30}')"
expect_eq "blank line2 is an error" true "$(lerr '{"line1":"a","line2":" ","track":0,"position":0,"duration":30}')"
expect_eq "bar_color red is an error" true "$(lerr '{"line1":"a","track":0,"position":0,"duration":30,"bar_color":"red"}')"
expect_eq "lower_third_add duration 0 is an error" true "$(lerr '{"line1":"a","track":0,"position":0,"duration":0}')"
expect_eq "lower_third_add size is an unknown argument" true "$(lerr '{"line1":"a","track":0,"position":0,"duration":30,"size":40}')"
expect_eq "lower_third_add mode ripple is an error" true "$(lerr '{"line1":"a","track":0,"position":0,"duration":30,"mode":"ripple"}')"
expect_eq "line1 with a carriage return is an error" true "$(lerr '{"line1":"a\rb","track":0,"position":0,"duration":30}')"
expect_eq "line2 with a carriage return is an error" true "$(lerr '{"line1":"a","line2":"b\r","track":0,"position":0,"duration":30}')"
expect_eq "title text too long: the error names text" true "$(tool_text title_add "{\"text\":\"$hash129\",\"track\":0,\"position\":0,\"duration\":30}" | grep -q 'text may have at most 256' && echo true || echo false)"
hash150="$(jq -nr '[range(0;150) | "#"] | join("")')"
expect_eq "lower third too long: the error names line1 and line2" true "$(tool_text lower_third_add "{\"line1\":\"$hash150\",\"track\":0,\"position\":0,\"duration\":30}" | grep -q 'line1 and line2 together may have at most 256' && echo true || echo false)"
expect_eq "the errors changed nothing" "$TL0" "$(tool_json get_timeline '{}' | jq -c '.tracks')"

echo "== title_add over the background clip"
SAVED="$TMP/titles.mlt"
# dyn_prop NAME [N]: property NAME of the Nth (default first) dynamictext filter in $SAVED, with its
# newlines; empty when the filter or the property is missing.
dyn_prop() {
  awk -v want="$1" -v nth="${2:-1}" '
    /<filter /{f=1; dyn=0; val=""; cap=0}
    f && cap {line=$0; if (sub(/<\/property>.*/, "", line)) cap=0; val=val "\n" line; next}
    f && /name="mlt_service">dynamictext</{dyn=1}
    f && index($0, "name=\"" want "\">") {line=$0; sub(/.*name="[^"]*">/, "", line)
      if (sub(/<\/property>.*/, "", line)) val=line; else {val=line; cap=1}}
    /<\/filter>/{ if (f && dyn && ++n == nth) {printf "%s", val; exit} f=0 }' "$SAVED"
}
R="$(tool_json title_add '{"text":"Hello\nWorld","track":0,"position":0,"duration":30}')"
expect_eq "title_add result clip" '{"duration":30,"start":0}' "$(echo "$R" | jq -cS '.clip')"
expect_eq "the result's track is get_timeline's track 0" "$(trk 0)" "$(echo "$R" | jq -c '.track')"
expect_eq "the title clip: start 0, 30 frames, named after the first line" '{"duration":30,"kind":"clip","name":"Hello","start":0}' "$(trk 0 | jq -cS '.clips[0] | {kind,start,duration,name}')"
expect_eq "the background clip is unchanged" "$(echo "$TL0" | jq -c '.[1]')" "$(trk 1)"
tool_json save_project "{\"path\":\"$SAVED\"}" >/dev/null
expect_eq "saved: the exact argument (two lines)" "Hello"$'\n'"World" "$(dyn_prop argument)"
expect_eq "saved: shotcut:filter dynamicText" dynamicText "$(dyn_prop shotcut:filter)"
# near SIZE VALUE: "true" when VALUE is within 1 of SIZE (the Filters panel may round the size
# through its point size when it shows the filter, as for Shotcut's own text clips).
near() { awk -v a="$1" -v b="$2" 'BEGIN{d=a-b; print (b != "" && d <= 1 && d >= -1) ? "true" : "false"}'; }
expect_eq "saved: size = 1920 / 12 (within 1)" true "$(near 160 "$(dyn_prop size)")"
expect_eq "saved: a point size is stored with it" true "$(dyn_prop shotcut:pointSize | grep -qE '^[1-9][0-9]*$' && echo true || echo false)"
expect_eq "saved: fgcolour" "#ffffffff" "$(dyn_prop fgcolour)"
expect_eq "saved: bgcolour" "#00000000" "$(dyn_prop bgcolour)"
expect_eq "saved: olcolour" "#aa000000" "$(dyn_prop olcolour)"
expect_eq "saved: outline 3" 3 "$(dyn_prop outline)"
expect_eq "saved: weight 700" 700 "$(dyn_prop weight)"
expect_eq "saved: halign center, valign middle" "center middle" "$(dyn_prop halign) $(dyn_prop valign)"
expect_eq "saved: geometry is the whole frame" "0%/0%:100%x100%" "$(dyn_prop geometry)"
# Like Open Other > Text on Linux: no family set, so the filter's own default (Sans) is saved.
expect_eq "saved: family is the filter's default" Sans "$(dyn_prop family)"
# 1 as Open Other > Text: with 0 the Filters panel replaces size by height / lines when shown.
expect_eq "saved: shotcut:usePointSize 1 (the size is kept)" 1 "$(dyn_prop shotcut:usePointSize)"
expect_eq "title_add: undo text" "Add title" "$(tool_json undo '{}' | jq -r '.text')"
expect_eq "one undo restores track 0 byte for byte" "$T0" "$(trk 0)"
tool_json redo '{}' >/dev/null
expect_eq "redo puts the title back" '{"duration":30,"name":"Hello","start":0}' "$(trk 0 | jq -cS '.clips[0] | {start,duration,name}')"

echo "== export with the title: white text in the centre"
export_wait() {
  local job st=""
  job="$(tool_json export_video "{\"path\":\"$1\"}" | jq '.job_id')"
  for _ in $(seq 1 300); do
    st="$(tool_json get_export_status "{\"job_id\":$job}" | jq -r '.state')"
    case "$st" in succeeded|failed|stopped|removed) break ;; esac
    sleep 1
  done
  echo "$st"
}
# ymax FILE: the highest luma (0-255) in the centre band (y 760-1160) of FILE's frame at 0.5 s.
ymax() {
  ffmpeg -loglevel error -ss 0.5 -i "$1" -frames:v 1 \
    -vf "crop=1080:400:0:760,signalstats,metadata=print:key=lavfi.signalstats.YMAX:file=-" \
    -f null - 2>/dev/null | sed -n 's/.*YMAX=//p' | head -1
}
OUT3="$TMP/titled.mp4"
expect_eq "titled export succeeded" succeeded "$(export_wait "$OUT3")"
expect_eq "the title's centre band has white pixels (YMAX >= 235)" true "$(awk -v y="$(ymax "$OUT3")" 'BEGIN{print (y != "" && y >= 235) ? "true" : "false"}')"
expect_eq "control: the same frame without the title has none" true "$(awk -v y="$(ymax "$OUT2")" 'BEGIN{print (y != "" && y < 235) ? "true" : "false"}')"

echo "== title_add options"
tool_json title_add '{"text":"Take #1","track":0,"position":0,"duration":10,"mode":"insert","font":"DejaVu Sans","size":200,"color":"#FF0000","bg_color":"#80000000","outline":0,"align":"left","valign":"top"}' >/dev/null
expect_eq "insert: the new title is first, the old one moved to 10" '[[0,10,"Take #1"],[10,30,"Hello"]]' "$(trk 0 | jq -c '[.clips[] | [.start,.duration,.name]]')"
tool_json save_project "{\"path\":\"$SAVED\"}" >/dev/null
expect_eq "options: # is escaped so it shows as text" 'Take \#1' "$(dyn_prop argument)"
expect_eq "options: family, colours, outline, alignment" 'DejaVu Sans|#ffff0000|#80000000|0|left|top' "$(dyn_prop family)|$(dyn_prop fgcolour)|$(dyn_prop bgcolour)|$(dyn_prop outline)|$(dyn_prop halign)|$(dyn_prop valign)"
expect_eq "options: size 200 (within 1)" true "$(near 200 "$(dyn_prop size)")"
tool_json undo '{}' >/dev/null
expect_eq "undo removes the inserted title" '[[0,30,"Hello"]]' "$(trk 0 | jq -c '[.clips[] | select(.kind=="clip") | [.start,.duration,.name]]')"
long256="$(jq -nr '[range(0;256) | "a"] | join("")')"
expect_eq "text of 256 characters is accepted" false "$(terr "{\"text\":\"$long256\",\"track\":0,\"position\":0,\"duration\":30}")"
tool_json undo '{}' >/dev/null
expect_eq "undo again: back to the one title" '[[0,30,"Hello"]]' "$(trk 0 | jq -c '[.clips[] | select(.kind=="clip") | [.start,.duration,.name]]')"
tool_json title_add '{"text":"Top\r\nMiddle\rBottom","track":0,"position":0,"duration":10}' >/dev/null
expect_eq "CR LF and CR: the title is named after its first line" Top "$(trk 0 | jq -r '.clips[0].name')"
tool_json save_project "{\"path\":\"$SAVED\"}" >/dev/null
found=false
for n in 1 2 3; do [ "$(dyn_prop argument $n)" = "Top"$'\n'"Middle"$'\n'"Bottom" ] && found=true; done
expect_eq "CR LF and CR are saved as newlines" true "$found"
expect_eq "no carriage return in the saved project" 0 "$(grep -c $'\r\|&#13;\|&#xd;' "$SAVED")"
tool_json undo '{}' >/dev/null
expect_eq "undo: back to the one title" '[[0,30,"Hello"]]' "$(trk 0 | jq -c '[.clips[] | select(.kind=="clip") | [.start,.duration,.name]]')"

echo "== lower_third_add"
T1="$(trk 0)"
R="$(tool_json lower_third_add '{"line1":"Jane Doe","line2":"Director","track":0,"position":30,"duration":45}')"
expect_eq "lower_third_add result clip" '{"duration":45,"start":30}' "$(echo "$R" | jq -cS '.clip')"
expect_eq "the lower third is named after line1" '[30,45,"Jane Doe"]' "$(trk 0 | jq -c '.clips[-1] | [.start,.duration,.name]')"
tool_json save_project "{\"path\":\"$SAVED\"}" >/dev/null
expect_eq "saved: argument is line1 newline line2" "Jane Doe"$'\n'"Director" "$(dyn_prop argument 2)"
expect_eq "saved: lower-third geometry" "0%/76%:100%x14%" "$(dyn_prop geometry 2)"
expect_eq "saved: halign left, valign bottom" "left bottom" "$(dyn_prop halign 2) $(dyn_prop valign 2)"
expect_eq "saved: size = 1920 / 20 (within 1)" true "$(near 96 "$(dyn_prop size 2)")"
expect_eq "saved: bgcolour is the default bar colour" "#99000000" "$(dyn_prop bgcolour 2)"
expect_eq "saved: fgcolour white" "#ffffffff" "$(dyn_prop fgcolour 2)"
expect_eq "lower_third_add: undo text" "Add lower third" "$(tool_json undo '{}' | jq -r '.text')"
expect_eq "one undo restores track 0 byte for byte" "$T1" "$(trk 0)"
tool_json lower_third_add '{"line1":"Solo","track":0,"position":30,"duration":20,"color":"#00ff00","bar_color":"#cc102030","font":"DejaVu Sans"}' >/dev/null
tool_json save_project "{\"path\":\"$SAVED\"}" >/dev/null
expect_eq "one line: argument is line1 alone" Solo "$(dyn_prop argument 2)"
expect_eq "one line: colour, bar colour, font" '#ff00ff00|#cc102030|DejaVu Sans' "$(dyn_prop fgcolour 2)|$(dyn_prop bgcolour 2)|$(dyn_prop family 2)"
tool_json undo '{}' >/dev/null
expect_eq "undo: track 0 is back to the one title" "$T1" "$(trk 0)"

echo "== the saved Video Mode setting is untouched"
if [ -n "${MCP_APPDATA:-}" ]; then
  INI="$MCP_APPDATA/shotcut.ini"
  # [player] profile= is Settings.playerProfile(); empty or absent is the default (Automatic).
  expect_eq "shotcut.ini has no saved [player] profile" "" "$(awk '/^\[/{s=($0=="[player]")} s && /^profile=/{sub(/^profile=/,""); print}' "$INI")"
else
  fail "shotcut.ini has no saved [player] profile" "MCP_APPDATA is not set, so shotcut.ini cannot be checked"
fi

echo "== refused on a project with only a timeline (nothing in the playlist)"
# Last: opening a project cannot be undone, and every earlier check needs the empty session.
TL="$TMP/timeline-only.mlt"
cat > "$TL" <<XML
<?xml version="1.0" encoding="utf-8"?>
<mlt LC_NUMERIC="C" producer="main_bin">
  <profile description="640x360 25 fps" width="640" height="360" progressive="1" sample_aspect_num="1" sample_aspect_den="1" display_aspect_num="16" display_aspect_den="9" frame_rate_num="25" frame_rate_den="1" colorspace="709"/>
  <playlist id="main_bin">
    <property name="xml_retain">1</property>
  </playlist>
  <producer id="black" in="0" out="49">
    <property name="length">50</property>
    <property name="resource">0</property>
    <property name="mlt_service">color</property>
  </producer>
  <playlist id="background">
    <entry producer="black" in="0" out="49"/>
  </playlist>
  <chain id="chain0" out="49">
    <property name="resource">$MEDIA/scenes.mp4</property>
    <property name="mlt_service">avformat-novalidate</property>
  </chain>
  <playlist id="playlist0">
    <property name="shotcut:video">1</property>
    <property name="shotcut:name">V1</property>
    <entry producer="chain0" in="0" out="49"/>
  </playlist>
  <tractor id="tractor0" in="0" out="49">
    <property name="shotcut">1</property>
    <track producer="background"/>
    <track producer="playlist0"/>
  </tractor>
</mlt>
XML
open_fixture_project "$TL" >/dev/null
sleep 1
expect_eq "the opened project has a timeline" 1 "$(tool_json get_timeline '{}' | jq '[.tracks[] | select(.type=="video")] | length')"
expect_eq "the opened project's playlist is empty" 0 "$(tool_json get_playlist '{}' | jq '.items | length')"
PTL="$(prof)"
expect_eq "the opened project is 640x360 25 fps" '{"fps_den":1,"fps_num":25,"height":360,"width":640}' "$(echo "$PTL" | jq -cS .)"
expect_eq "project_set_format on a timeline-only project is an error" true "$(ferr '{"preset":"9:16"}')"
expect_eq "the refusal changed nothing (timeline only)" "$PTL" "$(prof)"

echo "== captions_from_transcript"
# On the timeline-only project opened above: 640x360, 25 fps (one frame = 40 ms), V1 = scenes.mp4
# for 50 frames.
CTOOL='.[] | select(.name=="captions_from_transcript")'
expect_eq "tools/list has captions_from_transcript" true "$(echo "$TOOLS" | jq '[.[].name] | index("captions_from_transcript") != null')"
expect_eq "captions_from_transcript schema refuses extra properties" false "$(echo "$TOOLS" | jq "$CTOOL | .inputSchema.additionalProperties")"
expect_eq "captions_from_transcript style refuses extra properties" false "$(echo "$TOOLS" | jq "$CTOOL | .inputSchema.properties.style.additionalProperties")"
cap() { tool_json captions_from_transcript "$1"; }
cerr() { tool_is_error captions_from_transcript "$1"; }
SAVED="$TMP/captions.mlt"
save() { tool_json save_project "{\"path\":\"$SAVED\"}" >/dev/null; }
# svc_prop SERVICE NAME [N]: property NAME of the Nth (default first) filter of SERVICE in $SAVED,
# newlines kept, XML entities decoded; empty when missing.
svc_prop() {
  awk -v svc="$1" -v want="$2" -v nth="${3:-1}" '
    /<filter /{f=1; hit=0; val=""; cap=0}
    f && cap {line=$0; if (sub(/<\/property>.*/, "", line)) cap=0; val=val "\n" line; next}
    f && index($0, "name=\"mlt_service\">" svc "<") {hit=1}
    f && index($0, "name=\"" want "\">") {line=$0; sub(/.*name="[^"]*">/, "", line)
      if (sub(/<\/property>.*/, "", line)) val=line; else {val=line; cap=1}}
    /<\/filter>/{ if (f && hit && ++n == nth) {gsub(/&gt;/, ">", val); gsub(/&lt;/, "<", val)
      gsub(/&quot;/, "\"", val); gsub(/&amp;/, "\\&", val); printf "%s", val; exit} f=0 }' "$SAVED"
}
nsvc() { grep -c "name=\"mlt_service\">$1<" "$SAVED"; }

TONE="$MEDIA/tone_av.mp4"
# 12 words: "one".."seven" from 0.4 s in 0.4 s steps (segment 0.4-3.2 s), then "eight".."twelve"
# from 4.0 s (segment 4.0-6.0 s); every time is a whole frame at 25 fps.
TT="$(jq -nc '["one","two","three","four","five","six","seven"] as $a
  | ["eight","nine","ten","eleven","twelve"] as $b
  | def t(x): (x * 100 | round) / 100;
  {segments:[
    {start_s:0.4, end_s:3.2, text:($a | join(" ")),
     words:[range(7) as $i | {start_s:t(0.4 + 0.4 * $i), end_s:t(0.8 + 0.4 * $i), text:$a[$i]}]},
    {start_s:4.0, end_s:6.0, text:($b | join(" ")),
     words:[range(5) as $i | {start_s:t(4.0 + 0.4 * $i), end_s:t(4.4 + 0.4 * $i), text:$b[$i]}]}]}')"
expect_eq "supply the 12-word transcript for tone_av.mp4" word "$(tool_json transcribe "$(jq -nc --arg p "$TONE" --argjson t "$TT" '{path:$p,transcript:$t}')" | jq -r '.granularity')"
expect_eq "track_add a video track for the captioned clip (index 0)" 0 "$(tool_json track_add '{"type":"video"}' | jq '.added.index')"
expect_eq "insert tone_av.mp4 (10 s) at frame 50 of track 0" false "$(tool_is_error timeline_insert_range "$(jq -nc --arg p "$TONE" '{path:$p,in_s:0,out_s:10,track:0,position:50}')")"
expect_eq "the clip is item 1 of track 0, frames 50-299" '[1,50,250]' "$(tool_json get_timeline '{}' | jq -c '.tracks[0].clips[1] | [.index,.start,.duration]')"
# Hidden, so the export shows only the captions over black after the 2 s of scenes.mp4.
tool_json track_set '{"track":0,"hide":true}' >/dev/null

echo "== captions_from_transcript: refusals (nothing changes)"
save
expect_eq "no subtitle track before" 0 "$(nsvc subtitle_feed)"
expect_eq "a clip without a transcript is an error" true "$(cerr '{"track":1,"clip":0}')"
expect_eq "the error says to run transcribe" true "$(tool_text captions_from_transcript '{"track":1,"clip":0}' | grep -q 'transcribe' && echo true || echo false)"
expect_eq "a blank is an error" true "$(cerr '{"track":0,"clip":0}')"
expect_eq "a missing clip is an error" true "$(cerr '{"track":0,"clip":9}')"
expect_eq "a missing track is an error" true "$(cerr '{"track":7,"clip":0}')"
expect_eq "clip missing is an error" true "$(cerr '{"track":0}')"
expect_eq "max_words 0 is an error" true "$(cerr '{"track":0,"clip":1,"max_words":0}')"
expect_eq "max_words 21 is an error" true "$(cerr '{"track":0,"clip":1,"max_words":21}')"
expect_eq "max_words 2.5 is an error" true "$(cerr '{"track":0,"clip":1,"max_words":2.5}')"
expect_eq "max_chars 5 is an error" true "$(cerr '{"track":0,"clip":1,"max_chars":5}')"
expect_eq "max_chars 101 is an error" true "$(cerr '{"track":0,"clip":1,"max_chars":101}')"
expect_eq "min_duration_ms 199 is an error" true "$(cerr '{"track":0,"clip":1,"min_duration_ms":199}')"
expect_eq "min_duration_ms 5001 is an error" true "$(cerr '{"track":0,"clip":1,"min_duration_ms":5001}')"
expect_eq "language en (not ISO 639-2) is an error" true "$(cerr '{"track":0,"clip":1,"language":"en"}')"
expect_eq "language zzz (unknown) is an error" true "$(cerr '{"track":0,"clip":1,"language":"zzz"}')"
expect_eq "burn_in as a string is an error" true "$(cerr '{"track":0,"clip":1,"burn_in":"yes"}')"
expect_eq "style.color red is an error" true "$(cerr '{"track":0,"clip":1,"style":{"color":"red"}}')"
expect_eq "style.outline_color #12345 is an error" true "$(cerr '{"track":0,"clip":1,"style":{"outline_color":"#12345"}}')"
expect_eq "style.size 7 is an error" true "$(cerr '{"track":0,"clip":1,"style":{"size":7}}')"
expect_eq "style.position left is an error" true "$(cerr '{"track":0,"clip":1,"style":{"position":"left"}}')"
expect_eq "style.valign center is an error" true "$(cerr '{"track":0,"clip":1,"style":{"valign":"center"}}')"
expect_eq "an unknown style key is an error" true "$(cerr '{"track":0,"clip":1,"style":{"font":"Sans"}}')"
expect_eq "style that is not an object is an error" true "$(cerr '{"track":0,"clip":1,"style":"big"}')"
expect_eq "style with burn_in false is an error" true "$(cerr '{"track":0,"clip":1,"burn_in":false,"style":{"size":30}}')"
expect_eq "an unknown argument is an error" true "$(cerr '{"track":0,"clip":1,"maxwords":5}')"
expect_eq "the bad style error names style.color" true "$(tool_text captions_from_transcript '{"track":0,"clip":1,"style":{"color":"red"}}' | grep -q 'style.color' && echo true || echo false)"
expect_eq "the unknown style key error reads style: Unknown argument 'font'" true "$(tool_text captions_from_transcript '{"track":0,"clip":1,"style":{"font":"Sans"}}' | grep -q "^style: Unknown argument 'font'; " && echo true || echo false)"
expect_eq "language with a trailing newline is an error" true "$(cerr '{"track":0,"clip":1,"language":"eng\n"}')"
# A path with %2 in it is quoted as it is in the errors (no QString::arg substitution into it).
PCT="$MEDIA/pct%2x.mp4"
cp "$TONE" "$PCT"
expect_eq "insert pct%2x.mp4 at frame 1200 of track 0" false "$(tool_is_error timeline_insert_range "$(jq -nc --arg p "$PCT" '{path:$p,in_s:0,out_s:2,track:0,position:1200}')")"
PCTCLIP="$(tool_json get_timeline '{}' | jq '.tracks[0].clips | length - 1')"
expect_eq "the no-transcript error quotes the path with %2 unchanged" true "$(tool_text captions_from_transcript "{\"track\":0,\"clip\":$PCTCLIP}" | grep -qF "There is no transcript of $PCT: run transcribe on track 0 clip $PCTCLIP" && echo true || echo false)"
tool_json undo '{}' >/dev/null
save
expect_eq "the refusals added no subtitle track" 0 "$(nsvc subtitle_feed)"
expect_eq "the refusals added no subtitle filter" 0 "$(nsvc subtitle)"

echo "== captions_from_transcript: max_words 5"
R="$(cap '{"track":0,"clip":1,"max_words":5}')"
expect_eq "result: track name, count, burn_in" '{"burn_in":true,"captions":3,"track_name":"Captions 1"}' "$(echo "$R" | jq -cS '{track_name,captions,burn_in}')"
expect_eq "result: first caption (2400-4400 ms)" '{"end_ms":4400,"start_ms":2400,"text":"one two three four five"}' "$(echo "$R" | jq -cS '.first')"
expect_eq "result: last caption (6000-8000 ms)" '{"end_ms":8000,"start_ms":6000,"text":"eight nine ten eleven twelve"}' "$(echo "$R" | jq -cS '.last')"
LANG3="$(echo "$R" | jq -r '.language')"
expect_eq "result: language is an ISO 639-2 code (the system's)" true "$(echo "$LANG3" | grep -qE '^[a-z]{3}$' && echo true || echo false)"
save
SRT="$(printf '1\n00:00:02,400 --> 00:00:04,400\none two three four five\n\n2\n00:00:04,400 --> 00:00:05,200\nsix seven\n\n3\n00:00:06,000 --> 00:00:08,000\neight nine ten eleven twelve\n')"
expect_eq "saved: subtitle_feed text is the exact SRT" "$SRT" "$(svc_prop subtitle_feed text)"
expect_eq "saved: subtitle_feed feed and lang" "Captions 1|$LANG3" "$(svc_prop subtitle_feed feed)|$(svc_prop subtitle_feed lang)"
expect_eq "saved: the subtitle_feed is hidden" 1 "$(svc_prop subtitle_feed shotcut:hidden)"
expect_eq "saved: one subtitle burn-in filter, feed Captions 1" "1|Captions 1" "$(nsvc subtitle)|$(svc_prop subtitle feed)"
expect_eq "saved: burn-in shotcut:filter subtitles" subtitles "$(svc_prop subtitle shotcut:filter)"
expect_eq "saved: burn-in colours and outline" '#ffffffff|#00000000|#aa000000|3' "$(svc_prop subtitle fgcolour)|$(svc_prop subtitle bgcolour)|$(svc_prop subtitle olcolour)|$(svc_prop subtitle outline)"
expect_eq "saved: burn-in weight, style, usePointSize, typewriter" '700|normal|1|0' "$(svc_prop subtitle weight)|$(svc_prop subtitle style)|$(svc_prop subtitle shotcut:usePointSize)|$(svc_prop subtitle typewriter)"
expect_eq "saved: burn-in size 360 / 20 = 18" 18 "$(svc_prop subtitle size)"
expect_eq "saved: burn-in geometry, valign, halign (bottom)" '20%/75%:60%x20%|bottom|center' "$(svc_prop subtitle geometry)|$(svc_prop subtitle valign)|$(svc_prop subtitle halign)"

echo "== captions_from_transcript: burned into the export"
OUT4="$TMP/captioned.mp4"
expect_eq "captioned export succeeded" succeeded "$(export_wait "$OUT4")"
# ybottom FILE T: the highest luma in the bottom quarter of FILE's frame at T seconds.
ybottom() {
  ffmpeg -loglevel error -ss "$2" -i "$1" -frames:v 1 \
    -vf "crop=iw:ih/4:0:ih*3/4,signalstats,metadata=print:key=lavfi.signalstats.YMAX:file=-" \
    -f null - 2>/dev/null | sed -n 's/.*YMAX=//p' | head -1
}
# The default size is 360 / 20 = 18 px: after x264 its thin white strokes peak at about YMAX 190
# (measured 170-194), so the threshold is 150; the black frame between captions is 16.
expect_eq "3.4 s (inside the first caption): bright text in the bottom quarter (YMAX >= 150)" true "$(awk -v y="$(ybottom "$OUT4" 3.4)" 'BEGIN{print (y != "" && y >= 150) ? "true" : "false"}')"
expect_eq "7.0 s (inside the last caption): bright text too" true "$(awk -v y="$(ybottom "$OUT4" 7.0)" 'BEGIN{print (y != "" && y >= 150) ? "true" : "false"}')"
expect_eq "5.6 s (between the captions): the bottom quarter stays dark (YMAX < 50)" true "$(awk -v y="$(ybottom "$OUT4" 5.6)" 'BEGIN{print (y != "" && y < 50) ? "true" : "false"}')"

echo "== captions_from_transcript: one undo removes the track and the filter"
expect_eq "undo text" "Captions from transcript" "$(tool_json undo '{}' | jq -r '.text')"
save
expect_eq "after undo: no subtitle track" 0 "$(nsvc subtitle_feed)"
expect_eq "after undo: no subtitle filter" 0 "$(nsvc subtitle)"
tool_json redo '{}' >/dev/null
# No sleep: Shotcut writes the SRT into the feed 500 ms after the redo; save_project writes it first.
save
expect_eq "redo then an immediate save_project copy: the track (with its SRT) and the filter" "1|1|$SRT" "$(nsvc subtitle_feed)|$(nsvc subtitle)|$(svc_prop subtitle_feed text)"
tool_json undo '{}' >/dev/null
tool_json redo '{}' >/dev/null
CUR="$(tool_json get_state '{}' | jq -r '.project')"
tool_json save_project '{}' >/dev/null
expect_eq "redo then an immediate save_project to the project file: the SRT is in it" "$SRT" "$(SAVED="$CUR" svc_prop subtitle_feed text)"
tool_json undo '{}' >/dev/null
tool_json redo '{}' >/dev/null
OUT5="$TMP/captioned-redo.mp4"
expect_eq "redo then an immediate export_video succeeded" succeeded "$(export_wait "$OUT5")"
expect_eq "that export has the caption at 3.4 s (YMAX >= 150)" true "$(awk -v y="$(ybottom "$OUT5" 3.4)" 'BEGIN{print (y != "" && y >= 150) ? "true" : "false"}')"
tool_json undo '{}' >/dev/null

echo "== captions_from_transcript: burn_in false, a second track"
R="$(cap '{"track":0,"clip":1,"max_words":5,"burn_in":false}')"
expect_eq "burn_in false: result" '{"burn_in":false,"captions":3,"track_name":"Captions 1"}' "$(echo "$R" | jq -cS '{track_name,captions,burn_in}')"
save
expect_eq "burn_in false: a subtitle track and no subtitle filter" "1|0" "$(nsvc subtitle_feed)|$(nsvc subtitle)"
R="$(cap '{"track":0,"clip":1}')"
expect_eq "a second call makes Captions 2 (defaults: 7 words, 42 characters)" '{"captions":2,"track_name":"Captions 2"}' "$(echo "$R" | jq -cS '{track_name,captions}')"
expect_eq "defaults: the segment end breaks the captions" '["one two three four five six seven","eight nine ten eleven twelve"]' "$(echo "$R" | jq -c '[.first.text,.last.text]')"
save
expect_eq "two subtitle tracks, one burn-in for Captions 2" "2|1|Captions 2" "$(nsvc subtitle_feed)|$(nsvc subtitle)|$(svc_prop subtitle feed)"
tool_json undo '{}' >/dev/null
tool_json undo '{}' >/dev/null
save
expect_eq "two undos remove both" "0|0" "$(nsvc subtitle_feed)|$(nsvc subtitle)"

echo "== captions_from_transcript: max_chars, style, language"
R="$(cap '{"track":0,"clip":1,"max_words":20,"max_chars":10,"burn_in":false}')"
expect_eq "max_chars 10: 7 captions" 7 "$(echo "$R" | jq '.captions')"
save
expect_eq "max_chars 10: the caption texts" '["one two","three four","five six","seven","eight nine","ten eleven","twelve"]' "$(svc_prop subtitle_feed text | awk 'NF && !/^[0-9]+$/ && !/-->/' | jq -Rsc 'split("\n") | map(select(length > 0))')"
tool_json undo '{}' >/dev/null
R="$(cap '{"track":0,"clip":1,"language":"fra","style":{"size":30,"color":"#ffff00","outline_color":"#000000","bg_color":"#80000000","position":"top"}}')"
expect_eq "language fra" fra "$(echo "$R" | jq -r '.language')"
save
expect_eq "saved: lang fra" fra "$(svc_prop subtitle_feed lang)"
expect_eq "style: size, colours" '30|#ffffff00|#ff000000|#80000000' "$(svc_prop subtitle size)|$(svc_prop subtitle fgcolour)|$(svc_prop subtitle olcolour)|$(svc_prop subtitle bgcolour)"
expect_eq "style: position top (valign top by default)" '20%/5%:60%x20%|top' "$(svc_prop subtitle geometry)|$(svc_prop subtitle valign)"
tool_json undo '{}' >/dev/null
R="$(cap '{"track":0,"clip":1,"style":{"position":"middle","valign":"bottom"}}')"
save
expect_eq "style: position middle, valign bottom" '20%/40%:60%x20%|bottom' "$(svc_prop subtitle geometry)|$(svc_prop subtitle valign)"
tool_json undo '{}' >/dev/null

echo "== captions_from_transcript: a trimmed clip"
expect_eq "insert tone_av.mp4 from 3 s at frame 400 of track 0" false "$(tool_is_error timeline_insert_range "$(jq -nc --arg p "$TONE" '{path:$p,in_s:3,out_s:10,track:0,position:400}')")"
expect_eq "the trimmed clip is item 3, in 75" '[3,400,75]' "$(tool_json get_timeline '{}' | jq -c '.tracks[0].clips[3] | [.index,.start,.in]')"
R="$(cap '{"track":0,"clip":3,"max_words":5}')"
# "seven" starts at 2.8 s, before the clip's in point; "eight" (4.0 s = source frame 100) is at
# timeline frame 400 + 100 - 75 = 425 = 17000 ms.
expect_eq "trimmed: one caption, the words inside the clip, shifted to its place" '{"captions":1,"first":{"end_ms":19000,"start_ms":17000,"text":"eight nine ten eleven twelve"}}' "$(echo "$R" | jq -cS '{captions,first}')"
tool_json undo '{}' >/dev/null
expect_eq "speed 2 on the trimmed clip" false "$(tool_is_error clip_set_speed '{"track":0,"clip":3,"speed":2}')"
expect_eq "a speed-changed clip is an error" true "$(cerr '{"track":0,"clip":3}')"
tool_json undo '{}' >/dev/null
tool_json undo '{}' >/dev/null
expect_eq "the trimmed clip is gone again" 2 "$(tool_json get_timeline '{}' | jq '.tracks[0].clips | length')"

echo "== captions_from_transcript: an audio-only clip, a gap and the minimum duration"
AO="$MEDIA/audioonly.wav"
AT='{"segments":[{"start_s":0,"end_s":0.2,"text":"Hi","words":[{"start_s":0,"end_s":0.2,"text":"Hi"}]},{"start_s":0.4,"end_s":2.6,"text":"there friend again","words":[{"start_s":0.4,"end_s":0.8,"text":"there"},{"start_s":0.8,"end_s":1.2,"text":"friend"},{"start_s":2.4,"end_s":2.6,"text":"again"}]}]}'
expect_eq "supply a transcript for audioonly.wav" word "$(tool_json transcribe "$(jq -nc --arg p "$AO" --argjson t "$AT" '{path:$p,transcript:$t}')" | jq -r '.granularity')"
ATR="$(tool_json track_add '{"type":"audio"}' | jq '.added.index')"
expect_eq "insert audioonly.wav at frame 0 of the audio track" false "$(tool_is_error timeline_insert_range "$(jq -nc --arg p "$AO" --argjson t "$ATR" '{path:$p,in_s:0,out_s:10,track:$t,position:0}')")"
R="$(cap "{\"track\":$ATR,\"clip\":0,\"burn_in\":false}")"
expect_eq "audio-only: 3 captions" 3 "$(echo "$R" | jq '.captions')"
save
SRT2="$(printf '1\n00:00:00,000 --> 00:00:00,400\nHi\n\n2\n00:00:00,400 --> 00:00:01,200\nthere friend\n\n3\n00:00:02,400 --> 00:00:03,100\nagain\n')"
expect_eq "audio-only: Hi stretched only up to the next caption, a 1.2 s gap breaks, again stretched to 700 ms" "$SRT2" "$(svc_prop subtitle_feed text)"
tool_json undo '{}' >/dev/null
R="$(cap "{\"track\":$ATR,\"clip\":0,\"burn_in\":false,\"min_duration_ms\":1000}")"
expect_eq "min_duration_ms 1000: again lasts 1000 ms" '{"end_ms":3400,"start_ms":2400,"text":"again"}' "$(echo "$R" | jq -cS '.last')"
tool_json undo '{}' >/dev/null

tool_json track_set "{\"track\":$ATR,\"lock\":true}" >/dev/null
expect_eq "a clip on a locked track can be captioned (the clip is not changed)" false "$(cerr "{\"track\":$ATR,\"clip\":0,\"burn_in\":false}")"
tool_json undo '{}' >/dev/null
tool_json track_set "{\"track\":$ATR,\"lock\":false}" >/dev/null

echo "== captions_from_transcript: a Whisper result must cover the clip"
SN="$MEDIA/space name.mp4"
expect_eq "insert all of 'space name.mp4' at frame 600 of track 0" false "$(tool_is_error timeline_insert_range "$(jq -nc --arg p "$SN" '{path:$p,in_s:0,out_s:9,track:0,position:600}')")"
expect_eq "insert its 4-9 s at frame 900 of track 0" false "$(tool_is_error timeline_insert_range "$(jq -nc --arg p "$SN" '{path:$p,in_s:4,out_s:9,track:0,position:900}')")"
expect_eq "the two clips are items 3 and 5" '[[600,0],[900,100]]' "$(tool_json get_timeline '{}' | jq -c '[.tracks[0].clips[3,5] | [.start,.in]]')"
W="$(tool_json transcribe '{"track":0,"clip":5,"wait_ms":20000}')"
if echo "$W" | jq -e '.path' >/dev/null 2>&1 || echo "$W" | jq -e '.job_id' >/dev/null 2>&1; then
  job="$(echo "$W" | jq -r '.job_id // empty')"
  if [ -n "$job" ]; then W="$(wait_job "$job" 120 | jq -c '.result')"; fi
  expect_eq "Whisper ran on the trimmed clip only (in_s 4, out_s 9)" '[4,9]' "$(echo "$W" | jq -c '[.in_s,.out_s]')"
  expect_eq "the full clip is refused: the kept transcript covers only 4-9 s" true "$(tool_text captions_from_transcript '{"track":0,"clip":3,"burn_in":false}' | grep -q 'covers 4 to 9 s' && echo true || echo false)"
  R="$(cap '{"track":0,"clip":5,"burn_in":false}')"
  if [ "$(echo "$W" | jq -r '[.segments[].words[]?.text] | join(" ")')" = "Hello… world—" ]; then
    # tools/mcp-fake-whisper.sh: Hello… 4.0-4.5 s and world— 7.0-7.5 s of the source (frames
    # 100 and 175 from the clip's in point 100 at frame 900); the 2.5 s pause breaks.
    expect_eq "fake Whisper: two captions, language eng from en" '{"captions":2,"first":{"end_ms":36700,"start_ms":36000,"text":"Hello…"},"language":"eng","last":{"end_ms":39700,"start_ms":39000,"text":"world—"}}' "$(echo "$R" | jq -cS '{captions,first,last,language}')"
  else
    expect_eq "real Whisper: the covered clip is not refused for coverage" false "$(echo "$R" | grep -q 'covers' && echo true || echo false)"
  fi
  tool_json undo '{}' >/dev/null 2>&1
else
  echo "SKIP (no whisper): captions coverage checks need a configured Whisper (WHISPER_EXE and WHISPER_MODEL for the harness, or tools/mcp-fake-whisper.sh)"
fi

echo "== the default title size is at most 400 on a tall video mode"
# Last again: another project is opened. 720x4920: height / 12 = 410.
TALL="$TMP/tall.mlt"
cat > "$TALL" <<XML
<?xml version="1.0" encoding="utf-8"?>
<mlt LC_NUMERIC="C" producer="main_bin">
  <profile description="720x4920 25 fps" width="720" height="4920" progressive="1" sample_aspect_num="1" sample_aspect_den="1" display_aspect_num="6" display_aspect_den="41" frame_rate_num="25" frame_rate_den="1" colorspace="709"/>
  <playlist id="main_bin">
    <property name="xml_retain">1</property>
  </playlist>
  <producer id="black" in="0" out="49">
    <property name="length">50</property>
    <property name="resource">0</property>
    <property name="mlt_service">color</property>
  </producer>
  <playlist id="background">
    <entry producer="black" in="0" out="49"/>
  </playlist>
  <chain id="chain0" out="49">
    <property name="resource">$MEDIA/scenes.mp4</property>
    <property name="mlt_service">avformat-novalidate</property>
  </chain>
  <playlist id="playlist0">
    <property name="shotcut:video">1</property>
    <property name="shotcut:name">V1</property>
    <entry producer="chain0" in="0" out="49"/>
  </playlist>
  <tractor id="tractor0" in="0" out="49">
    <property name="shotcut">1</property>
    <track producer="background"/>
    <track producer="playlist0"/>
  </tractor>
</mlt>
XML
open_fixture_project "$TALL" >/dev/null
sleep 1
expect_eq "the tall project is 720x4920" '{"fps_den":1,"fps_num":25,"height":4920,"width":720}' "$(prof | jq -cS .)"
expect_eq "title_add with the default size" false "$(terr '{"text":"Tall","track":0,"position":0,"duration":10}')"
SAVED="$TMP/tall-saved.mlt"
tool_json save_project "{\"path\":\"$SAVED\"}" >/dev/null
expect_eq "saved: size 400, not 4920 / 12 = 410 (within 1)" true "$(near 400 "$(dyn_prop size)")"

if [ "$failures" -eq 0 ]; then echo "All checks passed."; else echo "${failures} check(s) failed."; exit 1; fi
