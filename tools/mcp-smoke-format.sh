#!/usr/bin/env bash
# Format smoke test for the Shotcut MCP server (stage 3, part 3C): project_set_format, clip_reframe.
# Usage: MCP_TOKEN=<token> [MCP_PORT=7468] MCP_APPDATA=<scratch dir> tools/mcp-smoke-format.sh
# Run it against a FRESH, throwaway Shotcut (nothing loaded, no timeline); it aborts otherwise
# unless MCP_SMOKE_FORCE=1. It creates its media with ffmpeg in a temporary folder and exports
# two files. MCP_APPDATA (the instance's --appdata folder) is required: the script checks the
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
expect_eq "tools/list: 53 tools in all (through Task 18)" 53 "$(echo "$TOOLS" | jq 'length')"
expect_eq "tools/list has project_set_format" true "$(echo "$TOOLS" | jq '[.[].name] | index("project_set_format") != null')"
expect_eq "project_set_format schema refuses extra properties" false "$(echo "$TOOLS" | jq '.[] | select(.name=="project_set_format") | .inputSchema.additionalProperties')"
expect_eq "tools/list has clip_reframe" true "$(echo "$TOOLS" | jq '[.[].name] | index("clip_reframe") != null')"
expect_eq "clip_reframe schema refuses extra properties" false "$(echo "$TOOLS" | jq '.[] | select(.name=="clip_reframe") | .inputSchema.additionalProperties')"

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

if [ "$failures" -eq 0 ]; then echo "All checks passed."; else echo "${failures} check(s) failed."; exit 1; fi
