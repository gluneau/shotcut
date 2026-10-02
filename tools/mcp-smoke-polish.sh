#!/usr/bin/env bash
# Look-and-sound smoke test for the Shotcut MCP server (stage 3, part 3B): the generic filter
# tools on timeline clips.
# Usage: MCP_TOKEN=<token> [MCP_PORT=7468] tools/mcp-smoke-polish.sh
# Run it against a FRESH, throwaway Shotcut (nothing loaded, no timeline); it aborts otherwise
# unless MCP_SMOKE_FORCE=1. It creates its media with ffmpeg in a temporary folder.
# Needs: bash, curl, jq, ffmpeg.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=tools/mcp-smoke-lib.sh
. "$HERE/mcp-smoke-lib.sh"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fresh_guard

MEDIA="$("$HERE/mcp-fixtures.sh" "$TMP/media")" || { echo "ffmpeg could not create the fixtures"; exit 2; }

echo "== filter tools"
TOOLS="$(rpc '{"jsonrpc":"2.0","id":3,"method":"tools/list"}' | jq -c '[.result.tools[].name]')"
expect_eq "tools/list has the five filter tools" true "$(echo "$TOOLS" | jq 'contains(["filter_list_available","filter_list_applied","filter_add","filter_set","filter_remove"])')"
expect_eq "filter tools need a timeline" true "$(tool_is_error filter_list_applied '{"track":0,"clip":0}')"

# The timeline, built with timeline_insert_range from an empty project (320x240, 25 fps):
# V1: 0 tone_av [0,100), 1 tone_av [100,200), 2 noaudio [200,250), 3 blank [250,300), 4 tone_av
# [300,350); an audio track AT: 0 audioonly.wav [0,50), 1 blank, 2 tone_av [100,150).
EMPTY="$TMP/empty.mlt"
cat > "$EMPTY" <<'XML'
<?xml version="1.0" encoding="utf-8"?>
<mlt LC_NUMERIC="C" producer="playlist0">
  <profile description="320x240 25 fps" width="320" height="240" progressive="1" sample_aspect_num="1" sample_aspect_den="1" display_aspect_num="4" display_aspect_den="3" frame_rate_num="25" frame_rate_den="1" colorspace="601"/>
  <playlist id="playlist0"/>
</mlt>
XML
open_fixture_project "$EMPTY" >/dev/null
sleep 1
ins() { tool_json timeline_insert_range "$(jq -nc --arg p "$1" --argjson i "$2" --argjson o "$3" --argjson t "$4" --argjson pos "$5" '{path:$p,in_s:$i,out_s:$o,track:$t,position:$pos}')" >/dev/null; }
ins "$MEDIA/tone_av.mp4" 0 4 0 0
ins "$MEDIA/tone_av.mp4" 4 8 0 100
ins "$MEDIA/noaudio.mp4" 0 2 0 200
ins "$MEDIA/tone_av.mp4" 0 2 0 300
AT="$(tool_json track_add '{"type":"audio"}' | jq '.added.index')"
ins "$MEDIA/audioonly.wav" 0 2 "$AT" 0
ins "$MEDIA/tone_av.mp4" 0 2 "$AT" 100
expect_eq "fixture: V1 items" '[["clip",0,100],["clip",100,100],["clip",200,50],["blank",250,50],["clip",300,50]]' "$(tool_json get_timeline '{}' | jq -c '[.tracks[0].clips[] | [.kind,.start,.duration]]')"
expect_eq "fixture: audio track items" '[["clip",0,50],["blank",50,50],["clip",100,50]]' "$(tool_json get_timeline '{}' | jq -c "[.tracks[$AT].clips[] | [.kind,.start,.duration]]")"

# JSON with sorted keys (Shotcut writes object keys in alphabetical order).
jn() { jq -cS . <<<"$1"; }
fa() { tool_json filter_list_applied "{\"track\":$1,\"clip\":$2}" | jq -cS .; }
fparams() { fa "$1" "$2" | jq -cS "[.filters[] | {id, params}]"; }
# The whole observable state: the timeline and the filters of every clip.
state() {
  echo "$(tool_json get_timeline '{}' | jq -c 'del(.position)')|$(fa 0 0)|$(fa 0 1)|$(fa 0 2)|$(fa 0 4)|$(fa "$AT" 0)|$(fa "$AT" 2)"
}
undo_text() { tool_json undo '{}' | jq -r '.text'; }
redo_text() { tool_json redo '{}' | jq -r '.text'; }
add() { tool_json filter_add "$1"; }
ferr() { tool_is_error "$1" "$2"; }

echo "== filter_list_available"
AV="$(tool_json filter_list_available '{}')"
for id in gain brightness box_blur size_position crop normalize_one_pass color_grading blur_pad; do
  expect_eq "available: $id is listed with params" true "$(echo "$AV" | jq --arg id "$id" '[.filters[] | select(.id == $id and (.params | length) > 0)] | length == 1')"
done
expect_eq "available: saturation and white_balance are listed or reported unavailable" 2 "$(echo "$AV" | jq '[(.filters[].id), (.unavailable[]?.id)] | map(select(. == "saturation" or . == "white_balance")) | length')"
expect_eq "available: gain is the volume service, audio, level -70..24 default 0 dB" "$(jn '{"service":"volume","kind":"audio","params":[{"name":"level","type":"number","min":-70,"max":24,"default":0,"unit":"dB"}]}')" "$(echo "$AV" | jq -cS '.filters[] | select(.id=="gain") | {service, kind, params}')"
expect_eq "available: crop is clip only and single" "$(jn '{"service":"crop","kind":"video","allow_multiple":false,"clip_only":true}')" "$(echo "$AV" | jq -cS '.filters[] | select(.id=="crop") | {service, kind, allow_multiple, clip_only}')"
expect_eq "available: crop params" '["left","right","top","bottom"]' "$(echo "$AV" | jq -c '[.filters[] | select(.id=="crop") | .params[].name]')"
expect_eq "available: size_position has a rect param" rect "$(echo "$AV" | jq -r '.filters[] | select(.id=="size_position") | .params[] | select(.name=="rect") | .type')"
expect_eq "available: box_blur params are integers 0..100 default 5" '[["hradius","int",0,100,5],["vradius","int",0,100,5]]' "$(echo "$AV" | jq -c '[.filters[] | select(.id=="box_blur") | .params[] | [.name,.type,.min,.max,.default]]')"
expect_eq "available: no GPU (movit) service is listed" 0 "$(echo "$AV" | jq '[.filters[] | select(.service | startswith("movit."))] | length')"
expect_eq "available: every entry has id, service, label, kind, allow_multiple, clip_only" true "$(echo "$AV" | jq '[.filters[] | has("id") and has("service") and has("label") and has("kind") and has("allow_multiple") and has("clip_only")] | all')"

echo "== filter_add / filter_list_applied"
expect_eq "applied: a clip without filters lists none" "$(jn '{"filters":[]}')" "$(fa 0 0)"
S0="$(state)"
R="$(add '{"track":0,"clip":0,"filter":"gain","params":{"level":6}}')"
expect_eq "add gain: result index 0 and the track" "0 0" "$(echo "$R" | jq -r '"\(.index) \(.track.index)"')"
expect_eq "add gain: listed with level 6" "$(jn '{"filters":[{"index":0,"id":"gain","service":"volume","disabled":false,"params":{"level":6}}]}')" "$(fa 0 0)"
S1="$(state)"
expect_eq "add gain: one undo step with its own text" "Add Gain / Volume filter" "$(undo_text)"
expect_eq "add gain: undo restores the state" "$S0" "$(state)"
expect_eq "add gain: redo text" "Add Gain / Volume filter" "$(redo_text)"
expect_eq "add gain: redo re-applies the state" "$S1" "$(state)"

echo "== filter_set"
R="$(tool_json filter_set '{"track":0,"clip":0,"index":0,"params":{"level":3}}')"
expect_eq "set level 3: result index and track" "0 0" "$(echo "$R" | jq -r '"\(.index) \(.track.index)"')"
expect_eq "set level 3: listed" "$(jn '[{"id":"gain","params":{"level":3}}]')" "$(fparams 0 0)"
tool_json filter_set '{"track":0,"clip":0,"index":0,"params":{"level":4}}' >/dev/null
expect_eq "set level 4: listed" "$(jn '[{"id":"gain","params":{"level":4}}]')" "$(fparams 0 0)"
S2="$(state)"
expect_eq "set: the second set is its own undo step" "Set Gain / Volume: level=4" "$(undo_text)"
expect_eq "set: one undo goes back to level 3" "$(jn '[{"id":"gain","params":{"level":3}}]')" "$(fparams 0 0)"
expect_eq "set: the first set is its own undo step" "Set Gain / Volume: level=3" "$(undo_text)"
expect_eq "set: a second undo goes back to level 6" "$(jn '[{"id":"gain","params":{"level":6}}]')" "$(fparams 0 0)"
expect_eq "set: back to the state after the add" "$S1" "$(state)"
redo_text >/dev/null; redo_text >/dev/null
expect_eq "set: two redos give level 4 again" "$S2" "$(state)"
tool_json filter_set '{"track":0,"clip":0,"index":0,"params":{"level":4}}' >/dev/null
tool_json filter_set '{"track":0,"clip":0,"index":0,"params":{"level":4}}' >/dev/null
undo_text >/dev/null
expect_eq "set: the same value twice is two undo steps (one undo keeps the first)" "$(jn '[{"id":"gain","params":{"level":4}}]')" "$(fparams 0 0)"
undo_text >/dev/null
expect_eq "set: the same value twice: second undo is back to the earlier state" "$S2" "$(state)"

echo "== defaults"
add '{"track":0,"clip":1,"filter":"box_blur"}' >/dev/null
expect_eq "box_blur without params takes the defaults" "$(jn '[{"id":"box_blur","params":{"hradius":5,"vradius":5}}]')" "$(fparams 0 1)"
add '{"track":0,"clip":1,"filter":"size_position"}' >/dev/null
expect_eq "size_position: default rect is the fit rect, rotation 0" "$(jn '{"rect":{"x":0,"y":0,"w":320,"h":240},"rotation":0}')" "$(fa 0 1 | jq -cS '.filters[1].params')"
tool_json filter_set '{"track":0,"clip":1,"index":1,"params":{"rect":{"x":10,"y":20,"w":160,"h":120},"rotation":15}}' >/dev/null
expect_eq "size_position: rect and rotation set" "$(jn '{"rect":{"x":10,"y":20,"w":160,"h":120},"rotation":15}')" "$(fa 0 1 | jq -cS '.filters[1].params')"
expect_eq "size_position: the set's undo text names both params" "Set Size, Position & Rotate: rect=10 20 160 120, rotation=15" "$(undo_text)"
add '{"track":0,"clip":1,"filter":"blur_pad"}' >/dev/null
expect_eq "blur_pad: default rect and blur 4" "$(jn '{"blur":4,"rect":{"x":0,"y":0,"w":320,"h":240}}')" "$(fa 0 1 | jq -cS '.filters[2].params')"
add '{"track":0,"clip":1,"filter":"normalize_one_pass"}' >/dev/null
expect_eq "normalize_one_pass: defaults" "$(jn '{"target_loudness":-23,"window":10,"max_gain":15,"min_gain":-15,"max_rate":3}')" "$(fa 0 1 | jq -cS '.filters[3].params')"
add '{"track":0,"clip":1,"filter":"color_grading","params":{"gain_r":1.5}}' >/dev/null
expect_eq "color_grading: defaults plus the given value" "$(jn '{"lift_r":0,"lift_g":0,"lift_b":0,"gamma_r":1,"gamma_g":1,"gamma_b":1,"gain_r":1.5,"gain_g":1,"gain_b":1}')" "$(fa 0 1 | jq -cS '.filters[4].params')"
expect_eq "five filters listed in the order added" '["box_blur","size_position","blur_pad","normalize_one_pass","color_grading"]' "$(fa 0 1 | jq -c '[.filters[].id]')"
expect_eq "indexes are 0..4" '[0,1,2,3,4]' "$(fa 0 1 | jq -c '[.filters[].index]')"
for _ in 1 2 3 4 5; do undo_text >/dev/null; done
expect_eq "five undos remove the five filters" "$(jn '{"filters":[]}')" "$(fa 0 1)"

echo "== filter_remove"
add '{"track":0,"clip":0,"filter":"brightness","params":{"level":0.5}}' >/dev/null
expect_eq "brightness added after gain" "$(jn '[{"id":"gain","params":{"level":4}},{"id":"brightness","params":{"level":0.5}}]')" "$(fparams 0 0)"
R1="$(state)"
R="$(tool_json filter_remove '{"track":0,"clip":0,"index":0}')"
expect_eq "remove gain: result index and track" "0 0" "$(echo "$R" | jq -r '"\(.index) \(.track.index)"')"
expect_eq "remove gain: only brightness is left, now index 0" "$(jn '{"filters":[{"index":0,"id":"brightness","service":"brightness","disabled":false,"params":{"level":0.5}}]}')" "$(fa 0 0)"
R2="$(state)"
expect_eq "remove: one undo step with its own text" "Remove Gain / Volume filter" "$(undo_text)"
expect_eq "remove: undo restores the filter at its index" "$R1" "$(state)"
redo_text >/dev/null
expect_eq "remove: redo removes it again" "$R2" "$(state)"
undo_text >/dev/null
expect_eq "remove: undo again" "$R1" "$(state)"

echo "== save and reopen"
tool_json save_project "{\"path\":\"$TMP/saved.mlt\"}" >/dev/null
expect_eq "saved project has shotcut:filter audioGain" 1 "$(grep -c '<property name="shotcut:filter">audioGain</property>' "$TMP/saved.mlt")"
expect_eq "saved project has no _shotcut:newFilter" 0 "$(grep -c '_shotcut:newFilter' "$TMP/saved.mlt")"
expect_eq "saved project has the gain level 4" 1 "$(grep -c '<property name="level">4</property>' "$TMP/saved.mlt")"
expect_eq "saved project: brightness has rgb_only 1" 1 "$(grep -c '<property name="rgb_only">1</property>' "$TMP/saved.mlt")"
BEFORE="$(fa 0 0)"
open_fixture_project "$TMP/saved.mlt" >/dev/null
sleep 1
expect_eq "reopened copy: the same filters with the same params" "$BEFORE" "$(fa 0 0)"
S0="$(state)"

echo "== errors leave everything unchanged"
expect_eq "brightness on the audio-only clip is an error" true "$(ferr filter_add "{\"track\":$AT,\"clip\":0,\"filter\":\"brightness\"}")"
expect_eq "picture filter on an audio-only clip: message" true "$(tool_text filter_add "{\"track\":$AT,\"clip\":0,\"filter\":\"brightness\"}" | grep -qi 'audio' && echo true)"
expect_eq "brightness on a clip on an audio track is an error" true "$(ferr filter_add "{\"track\":$AT,\"clip\":2,\"filter\":\"brightness\"}")"
expect_eq "gain on the clip without audio is an error" true "$(ferr filter_add '{"track":0,"clip":2,"filter":"gain"}')"
expect_eq "audio filter on a clip without audio: message" true "$(tool_text filter_add '{"track":0,"clip":2,"filter":"gain"}' | grep -qi 'no audio' && echo true)"
expect_eq "gain on the audio-only clip works (then undone)" false "$(ferr filter_add "{\"track\":$AT,\"clip\":0,\"filter\":\"gain\"}")"
undo_text >/dev/null
expect_eq "unknown filter id is an error" true "$(ferr filter_add '{"track":0,"clip":0,"filter":"sharpen"}')"
expect_eq "a movit service name is an error" true "$(ferr filter_add '{"track":0,"clip":0,"filter":"movit.blur"}')"
expect_eq "missing filter is an error" true "$(ferr filter_add '{"track":0,"clip":0}')"
expect_eq "unknown param is an error" true "$(ferr filter_add '{"track":0,"clip":0,"filter":"gain","params":{"gain":1}}')"
expect_eq "unknown param: the message names the known ones" true "$(tool_text filter_add '{"track":0,"clip":0,"filter":"gain","params":{"gain":1}}' | grep -q 'level' && echo true)"
expect_eq "level 99 is an error" true "$(ferr filter_add '{"track":0,"clip":0,"filter":"gain","params":{"level":99}}')"
expect_eq "a string for a number is an error" true "$(ferr filter_add '{"track":0,"clip":0,"filter":"gain","params":{"level":"6"}}')"
expect_eq "a fraction for an integer is an error" true "$(ferr filter_add '{"track":0,"clip":1,"filter":"box_blur","params":{"hradius":2.5}}')"
expect_eq "params that are not an object is an error" true "$(ferr filter_add '{"track":0,"clip":0,"filter":"gain","params":[6]}')"
expect_eq "a rect without h is an error" true "$(ferr filter_add '{"track":0,"clip":0,"filter":"size_position","params":{"rect":{"x":0,"y":0,"w":10}}}')"
expect_eq "a rect with zero width is an error" true "$(ferr filter_add '{"track":0,"clip":0,"filter":"size_position","params":{"rect":{"x":0,"y":0,"w":0,"h":10}}}')"
expect_eq "crop wider than the frame is an error" true "$(ferr filter_add '{"track":0,"clip":0,"filter":"crop","params":{"left":400}}')"
expect_eq "blank target is an error" true "$(ferr filter_add '{"track":0,"clip":3,"filter":"brightness"}')"
expect_eq "missing clip is an error" true "$(ferr filter_add '{"track":0,"clip":99,"filter":"brightness"}')"
expect_eq "missing track is an error" true "$(ferr filter_add '{"track":99,"clip":0,"filter":"brightness"}')"
expect_eq "negative track is an error" true "$(ferr filter_add '{"track":-1,"clip":0,"filter":"brightness"}')"
expect_eq "negative clip is an error" true "$(ferr filter_add '{"track":0,"clip":-1,"filter":"brightness"}')"
expect_eq "list on a blank is an error" true "$(ferr filter_list_applied '{"track":0,"clip":3}')"
for idx in -1 1.5 '"0"' '"+1"' 2 99999999999; do
  expect_eq "filter_set index $idx is an error" true "$(ferr filter_set "{\"track\":0,\"clip\":0,\"index\":$idx,\"params\":{\"level\":1}}")"
  expect_eq "filter_remove index $idx is an error" true "$(ferr filter_remove "{\"track\":0,\"clip\":0,\"index\":$idx}")"
done
expect_eq "filter_set with empty params is an error" true "$(ferr filter_set '{"track":0,"clip":0,"index":0,"params":{}}')"
expect_eq "filter_set an unknown param is an error" true "$(ferr filter_set '{"track":0,"clip":0,"index":0,"params":{"level2":1}}')"
expect_eq "filter_set out of range is an error" true "$(ferr filter_set '{"track":0,"clip":0,"index":0,"params":{"level":-71}}')"
expect_eq "filter_set a param of another filter is an error" true "$(ferr filter_set '{"track":0,"clip":0,"index":1,"params":{"hradius":1}}')"
expect_eq "filter_remove on a clip without filters is an error" true "$(ferr filter_remove '{"track":0,"clip":4,"index":0}')"
expect_eq "errors so far changed nothing" "$S0" "$(state)"

add '{"track":0,"clip":0,"filter":"crop","params":{"left":10}}' >/dev/null
S0="$(state)"
expect_eq "a second crop is an error (allow_multiple false)" true "$(ferr filter_add '{"track":0,"clip":0,"filter":"crop"}')"
expect_eq "duplicate: message" true "$(tool_text filter_add '{"track":0,"clip":0,"filter":"crop"}' | grep -qi 'already' && echo true)"
expect_eq "a second brightness is fine (allow_multiple)" false "$(ferr filter_add '{"track":0,"clip":0,"filter":"brightness"}')"
undo_text >/dev/null
expect_eq "duplicate refusal changed nothing" "$S0" "$(state)"
undo_text >/dev/null

# Transition between V1 clips 0 and 1 (it becomes item 1).
S0="$(state)"
tool_json timeline_add_transition '{"track":0,"clip":1,"duration":10}' >/dev/null
expect_eq "fixture: item 1 is a transition" transition "$(tool_json get_timeline '{}' | jq -r '.tracks[0].clips[1].kind')"
ST="$(state)"
expect_eq "transition target: add is an error" true "$(ferr filter_add '{"track":0,"clip":1,"filter":"brightness"}')"
expect_eq "transition target: list is an error" true "$(ferr filter_list_applied '{"track":0,"clip":1}')"
expect_eq "transition target: errors changed nothing" "$ST" "$(state)"
undo_text >/dev/null
expect_eq "transition undone" "$S0" "$(state)"

tool_json track_set '{"track":0,"lock":true}' >/dev/null
SL="$(state)"
expect_eq "locked track: add is an error" true "$(ferr filter_add '{"track":0,"clip":0,"filter":"brightness"}')"
expect_eq "locked track: set is an error" true "$(ferr filter_set '{"track":0,"clip":0,"index":0,"params":{"level":1}}')"
expect_eq "locked track: remove is an error" true "$(ferr filter_remove '{"track":0,"clip":0,"index":0}')"
expect_eq "locked track: message" true "$(tool_text filter_add '{"track":0,"clip":0,"filter":"brightness"}' | grep -qi 'locked' && echo true)"
expect_eq "locked track: list still works" false "$(ferr filter_list_applied '{"track":0,"clip":0}')"
expect_eq "locked track: errors changed nothing" "$SL" "$(state)"
tool_json track_set '{"track":0,"lock":false}' >/dev/null

echo "== four filters: undo all, redo all"
F0="$(state)"
add '{"track":0,"clip":4,"filter":"gain","params":{"level":-3}}' >/dev/null; F1="$(state)"
add '{"track":0,"clip":4,"filter":"brightness","params":{"level":1.2}}' >/dev/null; F2="$(state)"
add '{"track":0,"clip":4,"filter":"box_blur","params":{"hradius":2,"vradius":3}}' >/dev/null; F3="$(state)"
add '{"track":0,"clip":4,"filter":"crop","params":{"top":8,"bottom":8}}' >/dev/null; F4="$(state)"
expect_eq "four filters listed in order" "$(jn '[{"id":"gain","params":{"level":-3}},{"id":"brightness","params":{"level":1.2}},{"id":"box_blur","params":{"hradius":2,"vradius":3}},{"id":"crop","params":{"left":0,"right":0,"top":8,"bottom":8}}]')" "$(fparams 0 4)"
expect_eq "undo 4" "Add Crop: Source filter" "$(undo_text)"; expect_eq "after undo 4" "$F3" "$(state)"
expect_eq "undo 3" "Add Blur: Box filter" "$(undo_text)"; expect_eq "after undo 3" "$F2" "$(state)"
expect_eq "undo 2" "Add Brightness filter" "$(undo_text)"; expect_eq "after undo 2" "$F1" "$(state)"
expect_eq "undo 1" "Add Gain / Volume filter" "$(undo_text)"; expect_eq "after undo 1" "$F0" "$(state)"
redo_text >/dev/null; expect_eq "after redo 1" "$F1" "$(state)"
redo_text >/dev/null; expect_eq "after redo 2" "$F2" "$(state)"
redo_text >/dev/null; expect_eq "after redo 3" "$F3" "$(state)"
redo_text >/dev/null; expect_eq "after redo 4" "$F4" "$(state)"
tool_json filter_set '{"track":0,"clip":4,"index":3,"params":{"left":4,"right":6}}' >/dev/null
expect_eq "crop set" "$(jn '{"left":4,"right":6,"top":8,"bottom":8}')" "$(fa 0 4 | jq -cS '.filters[3].params')"
expect_eq "crop set: undo text" "Set Crop: Source: left=4, right=6" "$(undo_text)"
expect_eq "crop set: undo restores" "$F4" "$(state)"

echo "== filter edits interleaved with structural edits (Shotcut rebuilds the clip on undo)"
# V1 now: 0 tone_av [0,100) with gain 4 and brightness, 1 tone_av [100,200) without filters, ...
expect_eq "interleave fixture: clip 1 has no filters" '{"filters":[]}' "$(fa 0 1)"
# roundtrip NAME FILTER_OP STRUCT_OP: both ops, then undo x2 (start), redo x2 (after both), undo x2.
roundtrip() {
  local s0 s1 s2
  s0="$(state)"; eval "$2" >/dev/null; s1="$(state)"; eval "$3" >/dev/null; s2="$(state)"
  if [ "$s0" != "$s1" ] && [ "$s1" != "$s2" ]; then pass "$1: both edits changed something"
  else fail "$1: both edits changed something" "an edit did nothing"; fi
  undo_text >/dev/null; expect_eq "$1: undo of the structural edit" "$s1" "$(state)"
  undo_text >/dev/null; expect_eq "$1: undo of the filter edit gives the start" "$s0" "$(state)"
  redo_text >/dev/null; expect_eq "$1: redo of the filter edit" "$s1" "$(state)"
  redo_text >/dev/null; expect_eq "$1: redo of the structural edit" "$s2" "$(state)"
}
# The filters of the V1 clip that starts at frame $1.
fa_at() {
  local c; c="$(tool_json get_timeline '{}' | jq "[.tracks[0].clips[] | .start == $1 and .kind == \"clip\"] | index(true)")"
  fa 0 "$c"
}
GAIN6='{"track":0,"clip":1,"filter":"gain","params":{"level":6}}'
ONEGAIN="$(jn '[{"id":"gain","params":{"level":6}}]')"
S0="$(state)"
roundtrip "add + transition" "add '$GAIN6'" "tool_json timeline_add_transition '{\"track\":0,\"clip\":1,\"duration\":10}'"
expect_eq "add + transition: after redo x2 exactly one gain, level 6" "$ONEGAIN" "$(fa 0 2 | jq -cS '[.filters[] | {id, params}]')"
undo_text >/dev/null; undo_text >/dev/null
expect_eq "add + transition: back to the start" "$S0" "$(state)"
roundtrip "add + move" "add '$GAIN6'" "tool_json timeline_move '{\"track\":0,\"clip\":1,\"to_start\":400}'"
expect_eq "add + move: after redo x2 exactly one gain on the moved clip" "$ONEGAIN" "$(fa_at 400 | jq -cS '[.filters[] | {id, params}]')"
undo_text >/dev/null; undo_text >/dev/null
expect_eq "add + move: back to the start" "$S0" "$(state)"
FA0="$(fa 0 0)"
roundtrip "remove + move" "tool_json filter_remove '{\"track\":0,\"clip\":0,\"index\":0}'" "tool_json timeline_move '{\"track\":0,\"clip\":0,\"to_start\":400}'"
expect_eq "remove + move: after redo x2 only brightness on the moved clip" '["brightness"]' "$(fa_at 400 | jq -c '[.filters[].id]')"
undo_text >/dev/null; undo_text >/dev/null
expect_eq "remove + move: the gain is back at index 0 with its properties" "$FA0" "$(fa 0 0)"
expect_eq "remove + move: back to the start" "$S0" "$(state)"
roundtrip "set + move" "tool_json filter_set '{\"track\":0,\"clip\":0,\"index\":0,\"params\":{\"level\":2}}'" "tool_json timeline_move '{\"track\":0,\"clip\":0,\"to_start\":400}'"
expect_eq "set + move: after redo x2 the moved clip's gain is 2" 2 "$(fa_at 400 | jq '.filters[0].params.level')"
undo_text >/dev/null; undo_text >/dev/null
expect_eq "set + move: the value is restored" "$FA0" "$(fa 0 0)"
expect_eq "set + move: back to the start" "$S0" "$(state)"
roundtrip "add + split" "add '$GAIN6'" "tool_json timeline_split '{\"track\":0,\"position\":150}'"
undo_text >/dev/null; undo_text >/dev/null
expect_eq "add + split: no leftover filter" '{"filters":[]}' "$(fa 0 1)"
expect_eq "add + split: back to the start" "$S0" "$(state)"
roundtrip "add + trim" "add '$GAIN6'" "tool_json timeline_trim '{\"track\":0,\"clip\":1,\"edge\":\"in\",\"delta\":10}'"
undo_text >/dev/null; undo_text >/dev/null
expect_eq "add + trim: back to the start" "$S0" "$(state)"

echo "== fit rect on a clip whose shape differs from the video mode; keyframes"
ffmpeg -loglevel error -y -f lavfi -i "testsrc=size=480x240:rate=25:duration=2" -pix_fmt yuv420p -c:v libx264 "$TMP/wide.mp4"
# Video mode 320x240 with 4:3 pixels (16:9 display); clip 0 is 480x240 square pixels (2:1);
# clip 1 is tone_av.mp4 with a gain whose level has keyframes.
cat > "$TMP/wide.mlt" <<XML
<?xml version="1.0" encoding="utf-8"?>
<mlt LC_NUMERIC="C" producer="main_bin">
  <profile description="320x240 4:3 pixels" width="320" height="240" progressive="1" sample_aspect_num="4" sample_aspect_den="3" display_aspect_num="16" display_aspect_den="9" frame_rate_num="25" frame_rate_den="1" colorspace="601"/>
  <playlist id="main_bin"><property name="xml_retain">1</property></playlist>
  <producer id="black" in="0" out="99"><property name="length">100</property><property name="resource">0</property><property name="mlt_service">color</property><property name="set.test_audio">0</property></producer>
  <playlist id="background"><entry producer="black" in="0" out="99"/></playlist>
  <chain id="chain0" out="49"><property name="length">50</property><property name="resource">$TMP/wide.mp4</property><property name="mlt_service">avformat-novalidate</property></chain>
  <chain id="chain1" out="249"><property name="length">250</property><property name="resource">$MEDIA/tone_av.mp4</property><property name="mlt_service">avformat-novalidate</property>
    <filter id="g0"><property name="mlt_service">volume</property><property name="shotcut:filter">audioGain</property><property name="level">0=0;50=6</property></filter>
  </chain>
  <playlist id="playlist0"><property name="shotcut:video">1</property><property name="shotcut:name">V1</property><entry producer="chain0" in="0" out="49"/><entry producer="chain1" in="0" out="49"/></playlist>
  <tractor id="tractor0" in="0" out="99">
    <property name="shotcut">1</property>
    <track producer="background"/>
    <track producer="playlist0"/>
    <transition id="transition0"><property name="a_track">0</property><property name="b_track">1</property><property name="mlt_service">mix</property><property name="always_active">1</property><property name="sum">1</property></transition>
  </tractor>
</mlt>
XML
open_fixture_project "$TMP/wide.mlt" >/dev/null
sleep 1
add '{"track":0,"clip":0,"filter":"size_position"}' >/dev/null
# SizePositionUI.qml:318-328: clip DAR 2 > 16:9, so w = 320, h = w * SAR / DAR = 213.333 (not
# rounded), x = 0, y = round((240 - h) / 2) = 13.
expect_eq "fit rect (size_position): the video mode's SAR is used, size not rounded" "$(jn '{"x":0,"y":13,"w":320,"h":213.333}')" "$(fa 0 0 | jq -cS '.filters[0].params.rect')"
add '{"track":0,"clip":0,"filter":"blur_pad"}' >/dev/null
# pillar_echo/ui.qml:29-37: w = 320, h = round(w / DAR) = 160, y = 40 (no SAR).
expect_eq "fit rect (blur_pad): no SAR, size rounded" "$(jn '{"x":0,"y":40,"w":320,"h":160}')" "$(fa 0 0 | jq -cS '.filters[1].params.rect')"
expect_eq "keyframes: listed as the raw string" "0=0;50=6" "$(fa 0 1 | jq -r '.filters[0].params.level')"
SK="$(state_k() { echo "$(tool_json get_timeline '{}' | jq -c 'del(.position)')|$(fa 0 0)|$(fa 0 1)"; }; state_k)"
expect_eq "keyframes: filter_set on a keyframed param is an error" true "$(ferr filter_set '{"track":0,"clip":1,"index":0,"params":{"level":3}}')"
expect_eq "keyframes: message" true "$(tool_text filter_set '{"track":0,"clip":1,"index":0,"params":{"level":3}}' | grep -q 'has keyframes' && echo true)"
expect_eq "keyframes: nothing changed" "$SK" "$(echo "$(tool_json get_timeline '{}' | jq -c 'del(.position)')|$(fa 0 0)|$(fa 0 1)")"

echo "== filters Shotcut has but the allowlist does not"
cat > "$TMP/other.mlt" <<XML
<?xml version="1.0" encoding="utf-8"?>
<mlt LC_NUMERIC="C" producer="main_bin">
  <profile description="320x240 25 fps" width="320" height="240" progressive="1" sample_aspect_num="1" sample_aspect_den="1" display_aspect_num="4" display_aspect_den="3" frame_rate_num="25" frame_rate_den="1" colorspace="601"/>
  <playlist id="main_bin"><property name="xml_retain">1</property></playlist>
  <producer id="black" in="0" out="99"><property name="length">100</property><property name="resource">0</property><property name="mlt_service">color</property><property name="set.test_audio">0</property></producer>
  <playlist id="background"><entry producer="black" in="0" out="99"/></playlist>
  <chain id="chain0" out="249"><property name="length">250</property><property name="resource">$MEDIA/tone_av.mp4</property><property name="mlt_service">avformat-novalidate</property>
    <filter id="h0"><property name="mlt_service">audiolevel</property><property name="shotcut:hidden">1</property></filter>
    <filter id="f0"><property name="mlt_service">sepia</property><property name="u">75</property></filter>
    <filter id="f1"><property name="mlt_service">volume</property><property name="shotcut:filter">fadeInVolume</property><property name="level">0</property></filter>
    <filter id="f2"><property name="mlt_service">volume</property><property name="shotcut:filter">audioGain</property><property name="level">-6</property><property name="disable">1</property></filter>
    <filter id="h1"><property name="mlt_service">audiolevel</property><property name="shotcut:hidden">1</property><property name="shotcut:hiddenPosition">post</property></filter>
  </chain>
  <playlist id="playlist0"><property name="shotcut:video">1</property><property name="shotcut:name">V1</property><entry producer="chain0" in="0" out="99"/></playlist>
  <tractor id="tractor0" in="0" out="99">
    <property name="shotcut">1</property>
    <track producer="background"/>
    <track producer="playlist0"/>
    <transition id="transition0"><property name="a_track">0</property><property name="b_track">1</property><property name="mlt_service">mix</property><property name="always_active">1</property><property name="sum">1</property></transition>
  </tractor>
</mlt>
XML
open_fixture_project "$TMP/other.mlt" >/dev/null
sleep 1
O="$(fa 0 0)"
expect_eq "other filters: listed with their id and service, params empty" '[["sepia","sepia",{}],["fadeInVolume","volume",{}],["gain","volume",{"level":-6}]]' "$(echo "$O" | jq -c '[.filters[] | [.id,.service,.params]]')"
expect_eq "other filters: disabled is reported" '[false,false,true]' "$(echo "$O" | jq -c '[.filters[].disabled]')"
expect_eq "other filters: filter_set on a filter outside the allowlist is an error" true "$(ferr filter_set '{"track":0,"clip":0,"index":0,"params":{"u":10}}')"
expect_eq "other filters: a fade is not taken for a gain" true "$(ferr filter_set '{"track":0,"clip":0,"index":1,"params":{"level":1}}')"
OS="$(tool_json get_timeline '{}')|$O"
expect_eq "other filters: remove works on any user filter" false "$(ferr filter_remove '{"track":0,"clip":0,"index":0}')"
expect_eq "other filters: remove text names the service" "Remove sepia filter" "$(undo_text)"
expect_eq "other filters: undo restores" "$OS" "$(tool_json get_timeline '{}')|$(fa 0 0)"
# MLT order of the clip's filters (hidden ones included) in a saved copy.
order() {
  tool_json save_project "{\"path\":\"$TMP/order.mlt\"}" >/dev/null
  grep -o 'name="mlt_service">\(audiolevel\|sepia\|volume\|brightness\)<' "$TMP/order.mlt" | sed 's/.*">//; s/<$//' | tr '\n' ' '
}
expect_eq "hidden filters: undo of a remove puts the filter back between them" "audiolevel sepia volume volume audiolevel " "$(order)"
add '{"track":0,"clip":0,"filter":"brightness"}' >/dev/null
expect_eq "hidden filters: a new filter goes after the last user filter, before the post hidden one" "audiolevel sepia volume volume brightness audiolevel " "$(order)"
expect_eq "hidden filters: listed as the fourth user filter" '["sepia","fadeInVolume","gain","brightness"]' "$(fa 0 0 | jq -c '[.filters[].id]')"
undo_text >/dev/null
expect_eq "hidden filters: undo removes only the new filter" "audiolevel sepia volume volume audiolevel " "$(order)"

# FOOTER (later tasks insert their blocks above this line)
if [ "$failures" -eq 0 ]; then echo "All checks passed."; else echo "${failures} check(s) failed."; exit 1; fi
