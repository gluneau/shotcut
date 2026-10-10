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
expect_eq "tools/list: 56 tools in all (through Task 20)" 56 "$(echo "$TOOLS" | jq 'length')"
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
roundtrip "remove + ripple remove" "tool_json filter_remove '{\"track\":0,\"clip\":0,\"index\":0}'" "tool_json timeline_remove '{\"track\":0,\"clip\":0,\"ripple\":true}'"
undo_text >/dev/null; undo_text >/dev/null
expect_eq "remove + ripple remove: the gain is back at index 0 with its properties" "$FA0" "$(fa 0 0)"
expect_eq "remove + ripple remove: back to the start" "$S0" "$(state)"
roundtrip "set + split" "tool_json filter_set '{\"track\":0,\"clip\":0,\"index\":0,\"params\":{\"level\":2}}'" "tool_json timeline_split '{\"track\":0,\"position\":50}'"
expect_eq "set + split: after redo x2 both halves have gain 2" "2 2" "$(echo "$(fa 0 0 | jq '.filters[0].params.level') $(fa 0 1 | jq '.filters[0].params.level')")"
undo_text >/dev/null; undo_text >/dev/null
expect_eq "set + split: the value is restored" "$FA0" "$(fa 0 0)"
expect_eq "set + split: back to the start" "$S0" "$(state)"

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

echo "== audio recipes: clip_set_volume"
for a in 'clip_set_volume {"track":0,"clip":0,"gain_db":1,"extra":1}' \
  'clip_normalize {"track":0,"clip":0,"target":-16}' 'track_set_ducking {"track":1,"threshold":-10}' \
  'clip_detach_audio {"track":0,"clip":0,"force":true}'; do
  expect_eq "${a%% *}: an unknown argument is refused by name" true "$(tool_text "${a%% *}" "${a#* }" | grep -q '^Unknown argument' && echo true)"
done
expect_eq "tools/list has the four audio recipes" true "$(rpc '{"jsonrpc":"2.0","id":3,"method":"tools/list"}' | jq '[.result.tools[].name] | contains(["clip_set_volume","clip_normalize","track_set_ducking","clip_detach_audio"])')"
# V1: 0 tone_av [0,100) with a 10-frame fade in, 1 noaudio [100,150); A1: audio-only clip [0,50).
open_fixture_project "$EMPTY" >/dev/null
sleep 1
ins "$MEDIA/tone_av.mp4" 0 4 0 0
ins "$MEDIA/noaudio.mp4" 0 2 0 100
AT="$(tool_json track_add '{"type":"audio"}' | jq '.added.index')"
ins "$MEDIA/audioonly.wav" 0 2 "$AT" 0
tool_json timeline_fade '{"track":0,"clip":0,"fade_in":10}' >/dev/null
vstate() { echo "$(tool_json get_timeline '{}' | jq -c 'del(.position)')|$(fa 0 0)|$(fa 0 1)|$(fa "$AT" 0)"; }
vol() { tool_json clip_set_volume "{\"track\":$1,\"clip\":$2,\"gain_db\":$3}"; }
FADES="$(fa 0 0 | jq -c '[.filters[] | select(.id != "gain")]')"
expect_eq "volume fixture: the fade added fade filters" true "$(echo "$FADES" | jq '[.[].id] | index("fadeInVolume") != null')"
V0="$(vstate)"
R="$(vol 0 0 6)"
expect_eq "volume +6: result gain_db, changed, track" "6 true 0" "$(echo "$R" | jq -r '"\(.gain_db) \(.changed) \(.track.index)"')"
expect_eq "volume +6: one gain filter with level 6" '[6]' "$(fa 0 0 | jq -c '[.filters[] | select(.id=="gain") | .params.level]')"
expect_eq "volume +6: the result index is the gain filter's" "$(echo "$R" | jq '.index')" "$(fa 0 0 | jq '[.filters[] | select(.id=="gain")][0].index')"
expect_eq "volume +6: the fade filters are untouched" "$FADES" "$(fa 0 0 | jq -c '[.filters[] | select(.id != "gain")]')"
V1="$(vstate)"
vol 0 0 3 >/dev/null
expect_eq "volume 3: the same filter changes (still one)" '[3]' "$(fa 0 0 | jq -c '[.filters[] | select(.id=="gain") | .params.level]')"
expect_eq "volume 3: the fade filters are untouched" "$FADES" "$(fa 0 0 | jq -c '[.filters[] | select(.id != "gain")]')"
V2="$(vstate)"
R="$(vol 0 0 3)"
expect_eq "volume 3 again: changed false" false "$(echo "$R" | jq '.changed')"
expect_eq "volume 3 again: no undo step (the next undo is still the 3 dB step)" "Set clip volume to 3 dB" "$(undo_text)"
expect_eq "volume 3: undo restores +6" "$V1" "$(vstate)"
expect_eq "volume +6: undo text" "Set clip volume to 6 dB" "$(undo_text)"
expect_eq "volume +6: undo restores the start" "$V0" "$(vstate)"
redo_text >/dev/null; expect_eq "volume +6: redo" "$V1" "$(vstate)"
redo_text >/dev/null; expect_eq "volume 3: redo" "$V2" "$(vstate)"
R="$(vol 0 0 0)"
expect_eq "volume 0: removes the gain filter" '[]' "$(fa 0 0 | jq -c '[.filters[] | select(.id=="gain")]')"
expect_eq "volume 0: no index in the result" false "$(echo "$R" | jq 'has("index")')"
expect_eq "volume 0: the fade filters are untouched" "$FADES" "$(fa 0 0 | jq -c '[.filters[] | select(.id != "gain")]')"
expect_eq "volume 0: same timeline as the start" "$V0" "$(vstate)"
R="$(vol 0 0 0)"
expect_eq "volume 0 again: changed false" false "$(echo "$R" | jq '.changed')"
expect_eq "volume 0: one undo step with its own text" "Reset clip volume to 0 dB" "$(undo_text)"
expect_eq "volume 0: undo restores level 3" "$V2" "$(vstate)"
redo_text >/dev/null; expect_eq "volume 0: redo removes it again" "$V0" "$(vstate)"
vol "$AT" 0 -4.5 >/dev/null
expect_eq "volume on an audio-only clip on the audio track" '[-4.5]' "$(fa "$AT" 0 | jq -c '[.filters[] | select(.id=="gain") | .params.level]')"
undo_text >/dev/null
V0="$(vstate)"
expect_eq "volume: a clip without audio is an error" true "$(ferr clip_set_volume '{"track":0,"clip":1,"gain_db":3}')"
expect_eq "volume: no audio message" true "$(tool_text clip_set_volume '{"track":0,"clip":1,"gain_db":3}' | grep -qi 'no audio' && echo true)"
for g in 25 -71 '"6"' null; do
  expect_eq "volume: gain_db $g is an error" true "$(ferr clip_set_volume "{\"track\":0,\"clip\":0,\"gain_db\":$g}")"
done
expect_eq "volume: missing gain_db is an error" true "$(ferr clip_set_volume '{"track":0,"clip":0}')"
expect_eq "volume: a blank is an error" true "$(ferr clip_set_volume "{\"track\":$AT,\"clip\":1,\"gain_db\":3}")"
expect_eq "volume: missing clip is an error" true "$(ferr clip_set_volume '{"track":0,"clip":9,"gain_db":3}')"
tool_json track_set '{"track":0,"lock":true}' >/dev/null
expect_eq "volume: a locked track is an error" true "$(ferr clip_set_volume '{"track":0,"clip":0,"gain_db":3}')"
tool_json track_set '{"track":0,"lock":false}' >/dev/null
undo_text >/dev/null; undo_text >/dev/null
expect_eq "volume: errors changed nothing" "$V0" "$(vstate)"

echo "== audio recipes: clip_normalize"
ffmpeg -loglevel error -y -f lavfi -i "testsrc=size=320x240:rate=25:duration=4" -f lavfi -i "anullsrc=r=16000:cl=mono" -t 4 -pix_fmt yuv420p -c:v libx264 -c:a aac -shortest "$TMP/silent_av.mp4"
ins "$TMP/silent_av.mp4" 0 2 0 150
expect_eq "normalize fixture: the silent clip is V1 item 2" '["clip",150,50]' "$(tool_json get_timeline '{}' | jq -c '.tracks[0].clips[2] | [.kind,.start,.duration]')"
V0="$(vstate)|$(fa 0 2)"
expect_eq "normalize: a silent source is an error" true "$(ferr clip_normalize '{"track":0,"clip":2,"wait_ms":20000}')"
expect_eq "normalize: silent message" true "$(tool_text clip_normalize '{"track":0,"clip":2,"wait_ms":20000}' | grep -qi 'silent' && echo true)"
expect_eq "normalize: a clip without audio is an error" true "$(ferr clip_normalize '{"track":0,"clip":1,"wait_ms":20000}')"
for t in -5 -31 '"x"'; do
  expect_eq "normalize: target_lufs $t is an error" true "$(ferr clip_normalize "{\"track\":0,\"clip\":0,\"target_lufs\":$t}")"
done
expect_eq "normalize: wait_ms 20001 is an error" true "$(ferr clip_normalize '{"track":0,"clip":0,"wait_ms":20001}')"
expect_eq "normalize: errors changed nothing" "$V0" "$(vstate)|$(fa 0 2)"
N="$(tool_json clip_normalize '{"track":0,"clip":0,"wait_ms":20000}')"
expect_eq "normalize: measured_lufs is a finite number" true "$(echo "$N" | jq '(.measured_lufs | type) == "number" and .measured_lufs > -70 and .measured_lufs < 0')"
expect_eq "normalize: target_lufs defaults to -16" -16 "$(echo "$N" | jq '.target_lufs')"
expect_eq "normalize: applied_gain_db = target - measured (within 0.1)" true "$(echo "$N" | jq '((.target_lufs - .measured_lufs) - .applied_gain_db) | fabs < 0.1')"
expect_eq "normalize: the gain filter has the applied gain" "$(echo "$N" | jq -c '[.applied_gain_db]')" "$(fa 0 0 | jq -c '[.filters[] | select(.id=="gain") | .params.level]')"
expect_eq "normalize: the fade filters are untouched" "$FADES" "$(fa 0 0 | jq -c '[.filters[] | select(.id != "gain")]')"
expect_eq "normalize: one undo step named after the target" "Normalize to -16 LUFS" "$(undo_text)"
expect_eq "normalize: undo restores" "$V0" "$(vstate)|$(fa 0 2)"
N2="$(tool_json clip_normalize '{"track":0,"clip":0,"target_lufs":-20}')"
expect_eq "normalize again (cached measurement, wait_ms 0): finished at once" true "$(echo "$N2" | jq 'has("applied_gain_db") and (has("job_id") | not)')"
expect_eq "normalize -20: 4 dB less gain than -16" true "$(jq -n --argjson a "$N" --argjson b "$N2" '($a.applied_gain_db - $b.applied_gain_db - 4) | fabs < 0.02')"
expect_eq "normalize -20: undo text" "Normalize to -20 LUFS" "$(undo_text)"

echo "== clip_normalize: loudness of an export"
open_fixture_project "$EMPTY" >/dev/null
sleep 1
ins "$MEDIA/tone_av.mp4" 0 10 0 0
N="$(tool_json clip_normalize '{"track":0,"clip":0,"wait_ms":20000}')"
expect_eq "export normalize: applied" true "$(echo "$N" | jq 'has("applied_gain_db")')"
OUTN="$TMP/normalized.mp4"
EJ="$(tool_json export_video "$(jq -nc --arg p "$OUTN" '{path:$p,preset:"x264-medium"}')" | jq -r '.job_id')"
EST=""
for _ in $(seq 1 300); do
  EST="$(tool_json get_export_status "{\"job_id\":$EJ}" | jq -r '.state')"
  case "$EST" in succeeded|failed|stopped|removed) break ;; esac
  sleep 1
done
expect_eq "export normalize: the export succeeded" succeeded "$EST"
EL="$(tool_json audio_levels "$(jq -nc --arg p "$OUTN" '{path:$p,wait_ms:20000}')")"
echo "INFO  normalize: measured $(echo "$N" | jq .measured_lufs) LUFS, gain $(echo "$N" | jq .applied_gain_db) dB, export $(echo "$EL" | jq .integrated_lufs) LUFS"
expect_eq "export normalize: the export's integrated loudness is within 1.5 LU of -16" true "$(echo "$EL" | jq '(.integrated_lufs + 16) | fabs <= 1.5')"

echo "== clip_normalize: edits while measuring"
# 8 kHz 8-bit audio measures at about 350 s of audio per second (job cap test): 2400 s ~ 7 s.
# A measurement is cached per clip place and trim, so every check first moves the clip to a
# place not measured yet.
ffmpeg -v error -nostdin -y -f lavfi -i "sine=frequency=440:duration=2400:sample_rate=8000" -c:a pcm_u8 "$TMP/long_norm.wav"
AT2="$(tool_json track_add '{"type":"audio"}' | jq '.added.index')"
ins "$TMP/long_norm.wav" 0 2400 "$AT2" 0
# lclip TRACK [NAME]: index of the clip named NAME (default long_norm.wav) on TRACK.
lclip() { tool_json get_timeline '{}' | jq "[.tracks[$1].clips[] | .name == \"${2:-long_norm.wav}\"] | index(true)"; }
lstart() { tool_json get_timeline '{}' | jq ".tracks[$1].clips[$(lclip "$1")].start"; }
gains() { fa "$1" "$2" | jq -c '[.filters[] | select(.id=="gain") | .params.level]'; }
tlstate() { echo "$(tool_json get_timeline '{}' | jq -c 'del(.position)')|$(fa "$1" "$(lclip "$1")")"; }
# norm_start TRACK CLIP [TARGET]: clip_normalize with wait_ms 0; sets JID (empty when it finished
# at once) and MJ (its measure job).
norm_start() {
  J="$(tool_json clip_normalize "{\"track\":$1,\"clip\":$2,\"target_lufs\":${3:--16}}")"
  JID="$(echo "$J" | jq -r '.job_id // empty')"; MJ="$(echo "$J" | jq -r '.measure_job_id // empty')"
}
measuring() { [ -n "$JID" ] && [ "$(tool_json get_job "{\"job_id\":$MJ}" | jq -r '.state')" = running ]; }
lmove() { tool_json timeline_move "{\"track\":$1,\"clip\":$(lclip "$1"),\"to_start\":$(( $(lstart "$1") + 100 ))}" >/dev/null; }

# Trimming the clip during the measurement: nothing is applied.
NS0="$(tlstate "$AT2")"
norm_start "$AT2" "$(lclip "$AT2")"
tool_json timeline_trim "{\"track\":$AT2,\"clip\":$(lclip "$AT2"),\"edge\":\"in\",\"delta\":10}" >/dev/null
if measuring; then
  expect_eq "trimmed: wait_ms 0 returned a running job" running "$(echo "$J" | jq -r '.state')"
  F="$(wait_job "$JID" 120)"
  expect_eq "trimmed: the job failed" failed "$(echo "$F" | jq -r '.state')"
  expect_eq "trimmed: the job's error" "The clip changed while measuring; nothing was changed." "$(echo "$F" | jq -r '.error')"
  expect_eq "trimmed: no gain filter was added" '[]' "$(gains "$AT2" "$(lclip "$AT2")")"
else
  echo "SKIP: trimmed while measuring: the measurement was not running at the trim ($J)"
fi
undo_text >/dev/null
expect_eq "trimmed: undo of the trim gives the start" "$NS0" "$(tlstate "$AT2")"

# Moving the clip during the measurement: it is the same clip (Shotcut's clip id), so it applies.
lmove "$AT2"
norm_start "$AT2" "$(lclip "$AT2")" -18
lmove "$AT2"
if measuring; then
  NBM="$(tlstate "$AT2")"
  F="$(wait_job "$JID" 120)"
  expect_eq "moved: the job succeeded" succeeded "$(echo "$F" | jq -r '.state')"
  expect_eq "moved: the job result has the applied gain" true "$(echo "$F" | jq '.result | (.target_lufs == -18) and ((.target_lufs - .measured_lufs - .applied_gain_db) | fabs < 0.1)')"
  expect_eq "moved: the result names the moved clip" "$AT2 $(lclip "$AT2")" "$(echo "$F" | jq -r '"\(.result.track.index) \(.result.clip)"')"
  expect_eq "moved: the gain filter is on the moved clip" "$(echo "$F" | jq -c '[.result.applied_gain_db]')" "$(gains "$AT2" "$(lclip "$AT2")")"
  expect_eq "moved: it is an ordinary undo step" "Normalize to -18 LUFS" "$(undo_text)"
  expect_eq "moved: undo restores" "$NBM" "$(tlstate "$AT2")"
else
  echo "SKIP: moved while measuring: the measurement was not running at the move ($J)"
fi

# Splitting an EARLIER clip during the measurement shifts the clip's index: it still applies.
ins "$MEDIA/audioonly.wav" 0 2 "$AT2" 0
norm_start "$AT2" "$(lclip "$AT2")"
BEFORE_IDX="$(lclip "$AT2")"
tool_json timeline_split "{\"track\":$AT2,\"position\":25}" >/dev/null
if measuring; then
  expect_eq "split earlier: the clip's index moved up by one" "$(( BEFORE_IDX + 1 ))" "$(lclip "$AT2")"
  F="$(wait_job "$JID" 120)"
  expect_eq "split earlier: the job succeeded" succeeded "$(echo "$F" | jq -r '.state')"
  expect_eq "split earlier: applied to the right clip" "$(echo "$F" | jq -c '[.result.applied_gain_db]')" "$(gains "$AT2" "$(lclip "$AT2")")"
  expect_eq "split earlier: the split halves have no gain" '[[],[]]' "$(echo "[$(gains "$AT2" 0),$(gains "$AT2" 1)]" | jq -c .)"
  expect_eq "split earlier: undo text" "Normalize to -16 LUFS" "$(undo_text)"
else
  echo "SKIP: split earlier while measuring: the measurement was not running at the split ($J)"
fi

# A new video track during the measurement shifts every row: it still applies.
lmove "$AT2"
norm_start "$AT2" "$(lclip "$AT2")"
tool_json track_add '{"type":"video"}' >/dev/null
if measuring; then
  NT=$(( AT2 + 1 ))
  F="$(wait_job "$JID" 120)"
  expect_eq "row shift: the job succeeded" succeeded "$(echo "$F" | jq -r '.state')"
  expect_eq "row shift: the result names the shifted track" "$NT" "$(echo "$F" | jq '.result.track.index')"
  expect_eq "row shift: applied to the right clip" "$(echo "$F" | jq -c '[.result.applied_gain_db]')" "$(gains "$NT" "$(lclip "$NT")")"
  expect_eq "row shift: undo text" "Normalize to -16 LUFS" "$(undo_text)"
else
  echo "SKIP: track added while measuring: the measurement was not running at the add ($J)"
  undo_text >/dev/null
fi
undo_text >/dev/null # the track_add
expect_eq "row shift: back to the original rows" audio "$(tool_json get_timeline '{}' | jq -r ".tracks[$AT2].type")"

echo "== clip_normalize: a background apply waits for the running tool call"
# A (2400 s) is started with wait_ms 0, then B (3600 s) with wait_ms 20000. A's measurement ends
# inside B's call; its gain must be applied only after B's call returned, so A's undo step is the
# newest (without the gate it would land inside B's wait, before B's own step).
ffmpeg -v error -nostdin -y -f lavfi -i "sine=frequency=440:duration=3600:sample_rate=8000" -c:a pcm_u8 "$TMP/long_b.wav"
AT3="$(tool_json track_add '{"type":"audio"}' | jq '.added.index')"
ins "$TMP/long_b.wav" 0 3600 "$AT3" 0
lmove "$AT2"
norm_start "$AT2" "$(lclip "$AT2")" -18
JA="$JID"
if ! measuring; then
  echo "SKIP: idle gate: A's measurement was not running when B started ($J)"
else
  B="$(tool_json clip_normalize "{\"track\":$AT3,\"clip\":0,\"target_lufs\":-20,\"wait_ms\":20000}")"
  AMS="$(tool_json get_job "{\"job_id\":$MJ}" | jq -r '.state')"
  if ! echo "$B" | jq -e 'has("applied_gain_db")' >/dev/null; then
    echo "SKIP: idle gate: B did not finish within its wait ($B)"
    wait_job "$(echo "$B" | jq -r '.job_id')" 120 >/dev/null
    wait_job "$JA" 120 >/dev/null
  elif [ "$AMS" != succeeded ]; then
    echo "SKIP: idle gate: A's measurement had not ended when B's call returned ($AMS)"
    wait_job "$JA" 120 >/dev/null
  else
    FA="$(wait_job "$JA" 120)"
    expect_eq "idle gate: A's job succeeded" succeeded "$(echo "$FA" | jq -r '.state')"
    expect_eq "idle gate: A's step is the newest (applied after B's call)" "Normalize to -18 LUFS" "$(undo_text)"
    expect_eq "idle gate: B's step is before it" "Normalize to -20 LUFS" "$(undo_text)"
  fi
fi
rm -f "$TMP/long_b.wav"

echo "== track_set_ducking"
DS0="$(tool_json get_timeline '{}' | jq -c 'del(.position)')"
DUCK="$(call_tool track_set_ducking "{\"track\":$AT2}")"
if echo "$DUCK" | jq -r '.result.content[0].text' | grep -q 'needs MLT 7.41'; then
  expect_eq "ducking: this MLT has no ducking: error" true "$(echo "$DUCK" | jq -r '.result.isError')"
  echo "INFO  ducking: $(echo "$DUCK" | jq -r '.result.content[0].text')"
  expect_eq "ducking: the version error changed nothing" "$DS0" "$(tool_json get_timeline '{}' | jq -c 'del(.position)')"
else
  expect_eq "ducking: succeeds" false "$(echo "$DUCK" | jq -r '.result.isError')"
  expect_eq "ducking: duck_threshold reads back -12" -12 "$(echo "$DUCK" | jq -r '.result.content[0].text | fromjson | .ducking.duck_threshold')"
  expect_eq "ducking: one undo step" "Set track $AT2 audio ducking" "$(undo_text)"
fi
expect_eq "ducking: the top track is an error" true "$(ferr track_set_ducking '{"track":0}')"
expect_eq "ducking: top track message" true "$(tool_text track_set_ducking '{"track":0}' | grep -q 'top track' && echo true)"
expect_eq "ducking: a missing track is an error" true "$(ferr track_set_ducking '{"track":99}')"
for a in '"threshold_db":1' '"reduction_db":-71' '"fade_in_ms":5001' '"fade_out_ms":-1' '"enabled":"yes"'; do
  expect_eq "ducking: {$a} is an error" true "$(ferr track_set_ducking "{\"track\":$AT2,$a}")"
done

echo "== clip_normalize: the project is replaced while measuring"
lmove "$AT2"
J="$(tool_json clip_normalize "{\"track\":$AT2,\"clip\":$(lclip "$AT2")}")"
JID="$(echo "$J" | jq -r '.job_id // empty')"
open_fixture_project "$EMPTY" >/dev/null
sleep 1
if [ -z "$JID" ] || [ "$(tool_json get_job "{\"job_id\":$(echo "$J" | jq '.measure_job_id')}" | jq -r '.state')" != running ]; then
  echo "SKIP: project replaced while measuring: the measurement was not running ($J)"
else
  F="$(wait_job "$JID" 120)"
  expect_eq "replaced: the job failed" failed "$(echo "$F" | jq -r '.state')"
  expect_eq "replaced: the job's error" "The project was closed or replaced while measuring; nothing was changed." "$(echo "$F" | jq -r '.error')"
  expect_eq "replaced: the new project is not modified" false "$(tool_json get_state '{}' | jq '.modified')"
fi
rm -f "$TMP/long_norm.wav"

echo "== clip_detach_audio"
open_fixture_project "$EMPTY" >/dev/null
sleep 1
ins "$MEDIA/tone_av.mp4" 0 4 0 0
ins "$MEDIA/noaudio.mp4" 0 2 0 100
tool_json clip_set_volume '{"track":0,"clip":0,"gain_db":5}' >/dev/null
DT0="$(tool_json get_timeline '{}' | jq -c 'del(.position)')|$(fa 0 0)"
expect_eq "detach: noaudio.mp4 is an error" true "$(ferr clip_detach_audio '{"track":0,"clip":1}')"
expect_eq "detach: no audio message" true "$(tool_text clip_detach_audio '{"track":0,"clip":1}' | grep -qi 'no audio' && echo true)"
expect_eq "detach: a missing clip is an error" true "$(ferr clip_detach_audio '{"track":0,"clip":5}')"
tool_json track_set '{"track":0,"lock":true}' >/dev/null
expect_eq "detach: a locked track is an error" true "$(ferr clip_detach_audio '{"track":0,"clip":0}')"
tool_json track_set '{"track":0,"lock":false}' >/dev/null
undo_text >/dev/null; undo_text >/dev/null
expect_eq "detach: errors changed nothing" "$DT0" "$(tool_json get_timeline '{}' | jq -c 'del(.position)')|$(fa 0 0)"
R="$(tool_json clip_detach_audio '{"track":0,"clip":0}')"
expect_eq "detach: an audio track was added (2 tracks), audio_track 1" "2 1 audio" "$(echo "$R" | jq -r '"\(.tracks | length) \(.audio_track) \(.tracks[1].type)"')"
expect_eq "detach: the audio clip covers the same range" '["clip",0,100,0,99,true]' "$(echo "$R" | jq -c '.tracks[1].clips[0] | [.kind,.start,.duration,.in,.out,.has_audio]')"
expect_eq "detach: the video clip stays, without audio" '["clip",0,100,0,99,false]' "$(echo "$R" | jq -c '.tracks[0].clips[0] | [.kind,.start,.duration,.in,.out,.has_audio]')"
expect_eq "detach: the result tracks are get_timeline's" "$(echo "$R" | jq -c '.tracks')" "$(tool_json get_timeline '{}' | jq -c '.tracks')"
expect_eq "detach: the gain (audio filter) went with the audio" '[[],[5]]' "$(echo "[$(fa 0 0 | jq -c '[.filters[] | select(.id=="gain") | .params.level]'),$(fa 1 0 | jq -c '[.filters[] | select(.id=="gain") | .params.level]')]" | jq -c .)"
DT1="$(tool_json get_timeline '{}' | jq -c 'del(.position)')"
expect_eq "detach: one undo step" "Detach audio" "$(undo_text)"
expect_eq "detach: undo restores the timeline and the filters" "$DT0" "$(tool_json get_timeline '{}' | jq -c 'del(.position)')|$(fa 0 0)"
redo_text >/dev/null
expect_eq "detach: redo" "$DT1" "$(tool_json get_timeline '{}' | jq -c 'del(.position)')"
expect_eq "detach: the audio-only clip is refused (audio track)" true "$(ferr clip_detach_audio '{"track":1,"clip":0}')"
expect_eq "detach: the detached video clip has no audio left" true "$(ferr clip_detach_audio '{"track":0,"clip":0}')"
undo_text >/dev/null
# An audio track that is blank over the range is reused (a new, empty track is not: Shotcut
# keeps a one-frame blank on it); a locked one is refused.
tool_json track_add '{"type":"audio"}' >/dev/null
ins "$MEDIA/audioonly.wav" 0 2 1 200
tool_json track_set '{"track":1,"lock":true}' >/dev/null
expect_eq "detach: a locked target audio track is an error" true "$(ferr clip_detach_audio '{"track":0,"clip":0}')"
tool_json track_set '{"track":1,"lock":false}' >/dev/null
R="$(tool_json clip_detach_audio '{"track":0,"clip":0}')"
expect_eq "detach: the existing empty audio track is used (no new track)" "2 1" "$(echo "$R" | jq -r '"\(.tracks | length) \(.audio_track)"')"
expect_eq "detach: the audio clip is on it" '["clip",0,100]' "$(echo "$R" | jq -c '.tracks[1].clips[0] | [.kind,.start,.duration]')"

echo "== picture recipes"
TOOLS="$(rpc '{"jsonrpc":"2.0","id":3,"method":"tools/list"}' | jq -c '[.result.tools[].name]')"
expect_eq "tools/list has the three picture recipes" true "$(echo "$TOOLS" | jq 'contains(["clip_set_color","clip_set_transform","clip_set_blur"])')"
open_fixture_project "$EMPTY" >/dev/null
sleep 1
# V1: 0 scenes.mp4 0-1 s (red) [0,25), 1 audioonly.wav [25,75); an audio track: 0 audioonly.wav.
ins "$MEDIA/scenes.mp4" 0 1 0 0
ins "$MEDIA/audioonly.wav" 0 2 0 25
PAT="$(tool_json track_add '{"type":"audio"}' | jq '.added.index')"
ins "$MEDIA/audioonly.wav" 0 2 "$PAT" 0
expect_eq "picture fixture: V1 items" '[["clip",0,25],["clip",25,50]]' "$(tool_json get_timeline '{}' | jq -c '[.tracks[0].clips[] | [.kind,.start,.duration]]')"
pstate() { echo "$(tool_json get_timeline '{}' | jq -c 'del(.position)')|$(fa 0 0)|$(fa 0 1)|$(fa "$PAT" 0)"; }
AV="$(tool_json filter_list_available '{}')"
has_filter() { echo "$AV" | jq --arg id "$1" '[.filters[].id] | index($id) != null'; }

echo "== clip_set_color"
P0="$(pstate)"
R="$(tool_json clip_set_color '{"track":0,"clip":0,"brightness":0.5}')"
expect_eq "color: brightness 0.5 is level 1.5" "$(jn '[{"id":"brightness","params":{"level":1.5}}]')" "$(fparams 0 0)"
expect_eq "color: result names the filter and its index" "$(jn '[{"id":"brightness","index":0}]')" "$(echo "$R" | jq -cS '.filters')"
expect_eq "color: result track is track 0" 0 "$(echo "$R" | jq '.track.index')"
P1="$(pstate)"
tool_json clip_set_color '{"track":0,"clip":0,"brightness":-0.25}' >/dev/null
expect_eq "color: a second call updates the brightness filter (no second filter)" "$(jn '[{"id":"brightness","params":{"level":0.75}}]')" "$(fparams 0 0)"
expect_eq "color: the second call is one undo step" "Set clip color: brightness=-0.25" "$(undo_text)"
expect_eq "color: undo goes back to brightness 0.5" "$P1" "$(pstate)"
expect_eq "color: the first call is one undo step" "Set clip color: brightness=0.5" "$(undo_text)"
expect_eq "color: undo removes the added filter" "$P0" "$(pstate)"
redo_text >/dev/null
expect_eq "color: redo adds it again" "$P1" "$(pstate)"
R="$(tool_json clip_set_color '{"track":0,"clip":0,"brightness":0.2,"contrast":0.5}')"
expect_eq "color: contrast 0.5 is gain 1.5 and gamma 0.5 (Shotcut's Contrast at 75 %)" "$(jn '[{"id":"brightness","params":{"level":1.2}},{"id":"contrast","params":{"gamma_r":0.5,"gamma_g":0.5,"gamma_b":0.5,"gain_r":1.5,"gain_g":1.5,"gain_b":1.5}}]')" "$(fparams 0 0)"
expect_eq "color: result lists both filters" "$(jn '[{"id":"brightness","index":0},{"id":"contrast","index":1}]')" "$(echo "$R" | jq -cS '.filters')"
P2="$(pstate)"
tool_json clip_set_color '{"track":0,"clip":0,"contrast":-1}' >/dev/null
expect_eq "color: contrast -1 is gain 0 and gamma 2, brightness kept" "$(jn '[{"id":"brightness","params":{"level":1.2}},{"id":"contrast","params":{"gamma_r":2,"gamma_g":2,"gamma_b":2,"gain_r":0,"gain_g":0,"gain_b":0}}]')" "$(fparams 0 0)"
undo_text >/dev/null
expect_eq "color: two values in one call are one undo step" "Set clip color: brightness=0.2, contrast=0.5" "$(undo_text)"
expect_eq "color: that undo restores brightness 0.5 without contrast" "$P1" "$(pstate)"
redo_text >/dev/null
expect_eq "color: redo of the two-value call" "$P2" "$(pstate)"
undo_text >/dev/null
tool_json save_project "{\"path\":\"$TMP/pic_color.mlt\"}" >/dev/null
expect_eq "color: saved brightness filter has rgb_only 1" 1 "$(grep -c '<property name="rgb_only">1</property>' "$TMP/pic_color.mlt")"
if [ "$(has_filter saturation)" = true ]; then
  tool_json clip_set_color '{"track":0,"clip":0,"saturation":2}' >/dev/null
  expect_eq "color: saturation 2 listed" 2 "$(fa 0 0 | jq '.filters[] | select(.id=="saturation") | .params.saturation')"
  tool_json save_project "{\"path\":\"$TMP/pic_sat.mlt\"}" >/dev/null
  expect_eq "color: saturation 2 is frei0r property \"0\" 0.25" 1 "$(grep -c '<property name="0">0.25</property>' "$TMP/pic_sat.mlt")"
  tool_json clip_set_color '{"track":0,"clip":0,"saturation":1.5}' >/dev/null
  expect_eq "color: saturation updates, not stacks" '[1.5]' "$(fa 0 0 | jq -c '[.filters[] | select(.id=="saturation") | .params.saturation]')"
  undo_text >/dev/null; undo_text >/dev/null
else
  expect_eq "color: saturation unavailable here is an error" true "$(ferr clip_set_color '{"track":0,"clip":0,"saturation":2}')"
  expect_eq "color: the error says why" true "$(tool_text clip_set_color '{"track":0,"clip":0,"saturation":2}' | grep -q 'frei0r.saturat0r' && echo true)"
  echo "SKIP: saturation value checks: this MLT has no frei0r.saturat0r"
fi
if [ "$(has_filter white_balance)" = true ]; then
  tool_json clip_set_color '{"track":0,"clip":0,"temperature_k":7500}' >/dev/null
  expect_eq "color: temperature 7500 listed" 7500 "$(fa 0 0 | jq '.filters[] | select(.id=="white_balance") | .params.temperature_k')"
  tool_json save_project "{\"path\":\"$TMP/pic_wb.mlt\"}" >/dev/null
  expect_eq "color: temperature 7500 K is frei0r property \"1\" 0.5" 1 "$(grep -c '<property name="1">0.5</property>' "$TMP/pic_wb.mlt")"
  undo_text >/dev/null
else
  expect_eq "color: temperature unavailable here is an error" true "$(ferr clip_set_color '{"track":0,"clip":0,"temperature_k":7500}')"
  expect_eq "color: the error says why" true "$(tool_text clip_set_color '{"track":0,"clip":0,"temperature_k":7500}' | grep -q 'frei0r.colgate' && echo true)"
  echo "SKIP: temperature value checks: this MLT has no frei0r.colgate"
fi
expect_eq "color: back to brightness 0.5 only" "$P1" "$(pstate)"
for a in '{"track":0,"clip":0}' '{"track":0,"clip":0,"brightness":1.5}' '{"track":0,"clip":0,"brightness":-1.1}' \
  '{"track":0,"clip":0,"contrast":1.01}' '{"track":0,"clip":0,"saturation":3.5}' '{"track":0,"clip":0,"saturation":-0.1}' \
  '{"track":0,"clip":0,"temperature_k":1999}' '{"track":0,"clip":0,"temperature_k":15001}' '{"track":0,"clip":0,"brightness":"0.5"}' \
  '{"track":0,"clip":0,"brightness":0.1,"hue":3}' '{"track":0,"clip":1,"brightness":0.1}' "{\"track\":$PAT,\"clip\":0,\"brightness\":0.1}" \
  '{"track":0,"clip":9,"brightness":0.1}' '{"track":0,"brightness":0.1}'; do
  expect_eq "color: error for $a" true "$(ferr clip_set_color "$a")"
done
expect_eq "color: an unknown key is named" true "$(tool_text clip_set_color '{"track":0,"clip":0,"brightness":0.1,"hue":3}' | grep -q 'hue' && echo true)"
expect_eq "color: an audio-only clip on a video track is refused as having no picture" true "$(tool_text clip_set_color '{"track":0,"clip":1,"brightness":0.1}' | grep -qi 'no picture' && echo true)"
expect_eq "color: brightness plus an unavailable or invalid value changes nothing" true "$(ferr clip_set_color '{"track":0,"clip":0,"brightness":0.9,"contrast":5}')"
tool_json track_set '{"track":0,"lock":true}' >/dev/null
expect_eq "color: a locked track is an error" true "$(ferr clip_set_color '{"track":0,"clip":0,"brightness":0.1}')"
tool_json track_set '{"track":0,"lock":false}' >/dev/null
undo_text >/dev/null; undo_text >/dev/null
expect_eq "color: errors changed nothing" "$P1" "$(pstate)"
undo_text >/dev/null

echo "== clip_set_transform"
T0="$(pstate)"
R="$(tool_json clip_set_transform '{"track":0,"clip":0,"scale":0.5,"x":40,"y":-30,"rotation":15}')"
expect_eq "transform: rect from the fit rect, scaled about its centre and offset; rotation" "$(jn '[{"id":"size_position","params":{"rect":{"x":120,"y":30,"w":160,"h":120},"rotation":15}}]')" "$(fparams 0 0)"
expect_eq "transform: result names the filter" "$(jn '[{"id":"size_position","index":0}]')" "$(echo "$R" | jq -cS '.filters')"
expect_eq "transform: one undo step" "Set clip transform: scale=0.5, x=40, y=-30, rotation=15" "$(undo_text)"
expect_eq "transform: undo removes it" "$T0" "$(pstate)"
redo_text >/dev/null
T1="$(pstate)"
tool_json save_project "{\"path\":\"$TMP/pic_tr.mlt\"}" >/dev/null
expect_eq "transform: Fit mode (transition.fill 0)" 1 "$(grep -c '<property name="transition.fill">0</property>' "$TMP/pic_tr.mlt")"
expect_eq "transform: Fit mode (transition.distort 0)" 1 "$(grep -c '<property name="transition.distort">0</property>' "$TMP/pic_tr.mlt")"
expect_eq "transform: transparent background" 1 "$(grep -c '<property name="background">color:#00000000</property>' "$TMP/pic_tr.mlt")"
tool_json clip_set_transform '{"track":0,"clip":0,"scale":0.25}' >/dev/null
expect_eq "transform: a second call updates (x, y back to 0; rotation kept)" "$(jn '[{"id":"size_position","params":{"rect":{"x":120,"y":90,"w":80,"h":60},"rotation":15}}]')" "$(fparams 0 0)"
tool_json clip_set_transform '{"track":0,"clip":0,"rotation":-90}' >/dev/null
expect_eq "transform: rotation alone keeps the rect" "$(jn '[{"id":"size_position","params":{"rect":{"x":120,"y":90,"w":80,"h":60},"rotation":-90}}]')" "$(fparams 0 0)"
undo_text >/dev/null; undo_text >/dev/null
expect_eq "transform: two undos go back to the first call" "$T1" "$(pstate)"
undo_text >/dev/null
R="$(tool_json clip_set_transform '{"track":0,"clip":0,"scale":2,"crop":{"left":10,"top":20}}')"
expect_eq "transform: crop is added before the size filter, both in one call" '["crop","size_position"]' "$(fa 0 0 | jq -c '[.filters[].id]')"
expect_eq "transform: crop values" "$(jn '{"left":10,"right":0,"top":20,"bottom":0}')" "$(fa 0 0 | jq -cS '.filters[0].params')"
expect_eq "transform: scale 2 rect" "$(jn '{"x":-160,"y":-120,"w":640,"h":480}')" "$(fa 0 0 | jq -cS '.filters[1].params.rect')"
T2="$(pstate)"
tool_json clip_set_transform '{"track":0,"clip":0,"crop":{"right":5}}' >/dev/null
expect_eq "transform: a crop call updates the crop (other sides kept)" "$(jn '{"left":10,"right":5,"top":20,"bottom":0}')" "$(fa 0 0 | jq -cS '.filters[0].params')"
expect_eq "transform: still two filters" 2 "$(fa 0 0 | jq '.filters | length')"
undo_text >/dev/null
expect_eq "transform: crop plus scale was one undo step" "Set clip transform: scale=2, crop left=10 top=20" "$(undo_text)"
expect_eq "transform: undo removed both" "$T0" "$(pstate)"
for a in '{"track":0,"clip":0}' '{"track":0,"clip":0,"scale":0.04}' '{"track":0,"clip":0,"scale":8.1}' '{"track":0,"clip":0,"rotation":361}' \
  '{"track":0,"clip":0,"x":"1"}' '{"track":0,"clip":0,"crop":{"left":-1}}' '{"track":0,"clip":0,"crop":{"left":1.5}}' \
  '{"track":0,"clip":0,"crop":{"middle":3}}' '{"track":0,"clip":0,"crop":{}}' '{"track":0,"clip":0,"crop":5}' \
  '{"track":0,"clip":0,"scale":1,"fill":true}' '{"track":0,"clip":1,"scale":0.5}' "{\"track\":$PAT,\"clip\":0,\"scale\":0.5}" \
  '{"track":0,"clip":0,"crop":{"left":200,"right":200}}' '{"track":0,"clip":0,"scale":0.5,"crop":{"top":120,"bottom":120}}'; do
  expect_eq "transform: error for $a" true "$(ferr clip_set_transform "$a")"
done
expect_eq "transform: errors changed nothing" "$T0" "$(pstate)"
A='{"track":0,"clip":0,"scale":0.05,"x":100000,"crop":{"left":10}}'
expect_eq "transform: x moving the picture too far is an error (with a crop)" true "$(ferr clip_set_transform "$A")"
expect_eq "transform: that error is about x and y, not a rect argument" true "$(tool_text clip_set_transform "$A" | grep -q 'x and y' && ! tool_text clip_set_transform "$A" | grep -q 'rect' && echo true)"
expect_eq "transform: that error changed nothing (no crop left behind)" "$T0" "$(pstate)"
# An existing Size/Position filter keeps its Fill mode when only rotation is given.
add '{"track":0,"clip":0,"filter":"size_position"}' >/dev/null
tool_json clip_set_transform '{"track":0,"clip":0,"rotation":30}' >/dev/null
expect_eq "transform: rotation alone on an existing size filter sets rotation" 30 "$(fa 0 0 | jq '.filters[0].params.rotation')"
tool_json save_project "{\"path\":\"$TMP/pic_fill.mlt\"}" >/dev/null
expect_eq "transform: rotation alone keeps an existing Fill mode (transition.fill 1)" "1 0" "$(echo "$(grep -c '<property name="transition.fill">1</property>' "$TMP/pic_fill.mlt") $(grep -c '<property name="transition.fill">0</property>' "$TMP/pic_fill.mlt")")"
tool_json clip_set_transform '{"track":0,"clip":0,"scale":0.5}' >/dev/null
tool_json save_project "{\"path\":\"$TMP/pic_fit.mlt\"}" >/dev/null
expect_eq "transform: scale on an existing size filter sets Fit mode (transition.fill 0)" "0 1" "$(echo "$(grep -c '<property name="transition.fill">1</property>' "$TMP/pic_fit.mlt") $(grep -c '<property name="transition.fill">0</property>' "$TMP/pic_fit.mlt")")"
undo_text >/dev/null; undo_text >/dev/null; undo_text >/dev/null
expect_eq "transform: three undos go back" "$T0" "$(pstate)"

echo "== clip_set_blur"
B0="$(pstate)"
R="$(tool_json clip_set_blur '{"track":0,"clip":0,"mode":"box","radius":7}')"
expect_eq "blur box: radius 7 both ways" "$(jn '[{"id":"box_blur","params":{"hradius":7,"vradius":7}}]')" "$(fparams 0 0)"
expect_eq "blur box: result" "$(jn '[{"id":"box_blur","index":0}]')" "$(echo "$R" | jq -cS '.filters')"
expect_eq "blur box: one undo step" "Set clip blur: box radius=7" "$(undo_text)"
expect_eq "blur box: undo" "$B0" "$(pstate)"
redo_text >/dev/null
tool_json clip_set_blur '{"track":0,"clip":0,"mode":"box"}' >/dev/null
expect_eq "blur box: a second call replaces (default radius 5)" "$(jn '[{"id":"box_blur","params":{"hradius":5,"vradius":5}}]')" "$(fparams 0 0)"
tool_json clip_set_blur '{"track":0,"clip":0,"mode":"pad"}' >/dev/null
expect_eq "blur pad: fit rect and blur 4 after the box blur" "$(jn '[{"id":"box_blur","params":{"hradius":5,"vradius":5}},{"id":"blur_pad","params":{"blur":4,"rect":{"x":0,"y":0,"w":320,"h":240}}}]')" "$(fparams 0 0)"
tool_json clip_set_blur '{"track":0,"clip":0,"mode":"pad"}' >/dev/null
expect_eq "blur pad: a second call does not stack" 2 "$(fa 0 0 | jq '.filters | length')"
expect_eq "blur pad: one undo step" "Set clip blur: pad" "$(undo_text)"
undo_text >/dev/null; undo_text >/dev/null; undo_text >/dev/null
expect_eq "blur: four undos go back to the start" "$B0" "$(pstate)"
for a in '{"track":0,"clip":0}' '{"track":0,"clip":0,"mode":"gaussian"}' '{"track":0,"clip":0,"mode":"box","radius":101}' \
  '{"track":0,"clip":0,"mode":"box","radius":2.5}' '{"track":0,"clip":0,"mode":"pad","radius":5}' '{"track":0,"clip":0,"mode":"box","amount":3}' \
  '{"track":0,"clip":1,"mode":"box"}' "{\"track\":$PAT,\"clip\":0,\"mode\":\"pad\"}"; do
  expect_eq "blur: error for $a" true "$(ferr clip_set_blur "$a")"
done
expect_eq "blur: errors changed nothing" "$B0" "$(pstate)"

echo "== picture recipes: keyframes and rollback after a successful step"
# clip 0 is scenes.mp4 with a Brightness whose level has keyframes and a Size/Position whose rect
# has keyframes.
cat > "$TMP/kf.mlt" <<XML
<?xml version="1.0" encoding="utf-8"?>
<mlt LC_NUMERIC="C" producer="main_bin">
  <profile description="320x240 25 fps" width="320" height="240" progressive="1" sample_aspect_num="1" sample_aspect_den="1" display_aspect_num="4" display_aspect_den="3" frame_rate_num="25" frame_rate_den="1" colorspace="601"/>
  <playlist id="main_bin"><property name="xml_retain">1</property></playlist>
  <producer id="black" in="0" out="24"><property name="length">25</property><property name="resource">0</property><property name="mlt_service">color</property><property name="set.test_audio">0</property></producer>
  <playlist id="background"><entry producer="black" in="0" out="24"/></playlist>
  <chain id="chain0" out="224"><property name="length">225</property><property name="resource">$MEDIA/scenes.mp4</property><property name="mlt_service">avformat-novalidate</property>
    <filter id="b0"><property name="mlt_service">brightness</property><property name="shotcut:filter">brightness</property><property name="level">0=1;24=1.5</property></filter>
    <filter id="a0"><property name="mlt_service">affine</property><property name="shotcut:filter">affineSizePosition</property><property name="transition.rect">0=0 0 320 240 1;24=80 60 160 120 1</property><property name="transition.fill">1</property></filter>
  </chain>
  <playlist id="playlist0"><property name="shotcut:video">1</property><property name="shotcut:name">V1</property><entry producer="chain0" in="0" out="24"/></playlist>
  <tractor id="tractor0" in="0" out="24">
    <property name="shotcut">1</property>
    <track producer="background"/>
    <track producer="playlist0"/>
    <transition id="transition0"><property name="a_track">0</property><property name="b_track">1</property><property name="mlt_service">mix</property><property name="always_active">1</property><property name="sum">1</property></transition>
  </tractor>
</mlt>
XML
open_fixture_project "$TMP/kf.mlt" >/dev/null
sleep 1
kstate() { echo "$(tool_json get_timeline '{}' | jq -c 'del(.position)')|$(fa 0 0)"; }
K0="$(kstate)"
expect_eq "keyframes: clip_set_color on a keyframed brightness is an error" true "$(ferr clip_set_color '{"track":0,"clip":0,"brightness":0.2}')"
expect_eq "keyframes: the message says keyframes" true "$(tool_text clip_set_color '{"track":0,"clip":0,"brightness":0.2}' | grep -q 'keyframes' && echo true)"
expect_eq "keyframes: nothing changed" "$K0" "$(kstate)"
A='{"track":0,"clip":0,"scale":0.5,"crop":{"left":10}}'
expect_eq "rollback: crop (added) then scale on a keyframed rect is an error" true "$(ferr clip_set_transform "$A")"
expect_eq "rollback: the crop added by the first step is gone" "$K0" "$(kstate)"
expect_eq "rollback: nothing is left to undo" true "$(ferr undo '{}')"

echo "== picture recipes: exported frames"
open_fixture_project "$EMPTY" >/dev/null
sleep 1
ins "$MEDIA/scenes.mp4" 0 1 0 0
# export_frame NAME: exports the 1 s timeline and writes the frame at 0.5 s as raw RGB (320x240).
export_frame() {
  local out="$TMP/pic_$1.mp4" ej st=""
  ej="$(tool_json export_video "$(jq -nc --arg p "$out" '{path:$p,preset:"x264-medium"}')" | jq -r '.job_id')"
  for _ in $(seq 1 120); do
    st="$(tool_json get_export_status "{\"job_id\":$ej}" | jq -r '.state')"
    case "$st" in succeeded|failed|stopped|removed) break ;; esac
    sleep 1
  done
  expect_eq "export $1: succeeded" succeeded "$st"
  ffmpeg -v error -nostdin -y -ss 0.5 -i "$out" -frames:v 1 -f rawvideo -pix_fmt rgb24 "$TMP/pic_$1.rgb"
}
yavg() { ffmpeg -v error -nostdin -ss 0.5 -i "$TMP/pic_$1.mp4" -frames:v 1 -vf signalstats,metadata=print:file=- -f null - | sed -n 's/.*YAVG=//p' | head -1; }
rgbmeans() { od -An -v -tu1 -w3 "$TMP/pic_$1.rgb" | awk '{r+=$1;g+=$2;b+=$3;n++} END {printf "%.1f %.1f %.1f\n", r/n, g/n, b/n}'; }
pixel() { od -An -v -tu1 -j $(( ($3 * 320 + $2) * 3 )) -N3 "$TMP/pic_$1.rgb" | xargs; }
export_frame orig
Y0="$(yavg orig)"
expect_eq "frame orig: the centre is red" true "$(pixel orig 160 120 | awk '{print ($1 > 180 && $2 < 60 && $3 < 60) ? "true" : "false"}')"
tool_json clip_set_color '{"track":0,"clip":0,"brightness":-0.5}' >/dev/null
export_frame dark
Y1="$(yavg dark)"
echo "INFO  picture: YAVG original $Y0, brightness -0.5 $Y1"
expect_eq "frame brightness -0.5: darker than the original (YAVG)" true "$(awk -v a="$Y1" -v b="$Y0" 'BEGIN {print (a != "" && b != "" && a + 0 < b + 0) ? "true" : "false"}')"
undo_text >/dev/null
if [ "$(has_filter saturation)" = true ]; then
  tool_json clip_set_color '{"track":0,"clip":0,"saturation":0}' >/dev/null
  export_frame grey
  echo "INFO  picture: saturation 0 RGB means $(rgbmeans grey)"
  expect_eq "frame saturation 0: R, G and B means within 4 (grey)" true "$(rgbmeans grey | awk '{m=$1; M=$1; for (i=2;i<=3;i++) {if ($i<m) m=$i; if ($i>M) M=$i}; print (M - m <= 4) ? "true" : "false"}')"
  undo_text >/dev/null
else
  echo "SKIP: saturation 0 frame check: this MLT has no frei0r.saturat0r"
fi
tool_json clip_set_transform '{"track":0,"clip":0,"scale":0.5}' >/dev/null
export_frame half
echo "INFO  picture: scale 0.5 corners $(pixel half 2 2) / $(pixel half 317 237), centre $(pixel half 160 120)"
for c in "2 2" "317 2" "2 237" "317 237"; do
  expect_eq "frame scale 0.5: corner $c is black" true "$(pixel half $c | awk '{print ($1 < 30 && $2 < 30 && $3 < 30) ? "true" : "false"}')"
done
expect_eq "frame scale 0.5: the centre is red" true "$(pixel half 160 120 | awk '{print ($1 > 180 && $2 < 60 && $3 < 60) ? "true" : "false"}')"

echo "== clip_set_speed"
TOOLS="$(rpc '{"jsonrpc":"2.0","id":3,"method":"tools/list"}' | jq -c '[.result.tools[].name]')"
expect_eq "tools/list has clip_set_speed" true "$(echo "$TOOLS" | jq 'contains(["clip_set_speed"])')"
ffmpeg -loglevel error -y -f lavfi -i "color=c=blue:size=320x240" -frames:v 1 "$TMP/still.png"
# V1: 0 tone_av.mp4 0-249 with a gain of 6 dB [0,250), 1 blank [250,400), 2 tone_av.mp4 50-99
# [400,450) (last on its track). V2: 0 a still image [0,50), 1 a colour generator [50,100),
# 2 tone_av.mp4 with a fade-in [100,150), 3 tone_av.mp4 with a keyframed gain [150,200),
# 4 tone_av.mp4 [200,250), 5 tone_av.mp4 with a fade-in whose level is plain (shotcut:animIn only)
# [250,300), 6 a .mlt project clip (xml producer) [300,350). A1: 0 audioonly.wav 0-49. (A proxy clip cannot be faked here: Shotcut
# drops shotcut:proxy when it opens a project with proxy editing off.)
cat > "$TMP/speedsub.mlt" <<XML
<?xml version="1.0" encoding="utf-8"?>
<mlt LC_NUMERIC="C" producer="sub">
  <profile description="320x240 25 fps" width="320" height="240" progressive="1" sample_aspect_num="1" sample_aspect_den="1" display_aspect_num="4" display_aspect_den="3" frame_rate_num="25" frame_rate_den="1" colorspace="601"/>
  <producer id="sub" in="0" out="49"><property name="length">50</property><property name="resource">#00ff00</property><property name="mlt_service">color</property></producer>
</mlt>
XML
cat > "$TMP/speed.mlt" <<XML
<?xml version="1.0" encoding="utf-8"?>
<mlt LC_NUMERIC="C" producer="main_bin">
  <profile description="320x240 25 fps" width="320" height="240" progressive="1" sample_aspect_num="1" sample_aspect_den="1" display_aspect_num="4" display_aspect_den="3" frame_rate_num="25" frame_rate_den="1" colorspace="601"/>
  <playlist id="main_bin"><property name="xml_retain">1</property></playlist>
  <producer id="black" in="0" out="449"><property name="length">450</property><property name="resource">0</property><property name="mlt_service">color</property><property name="set.test_audio">0</property></producer>
  <playlist id="background"><entry producer="black" in="0" out="449"/></playlist>
  <chain id="chain0" out="249"><property name="length">250</property><property name="resource">$MEDIA/tone_av.mp4</property><property name="mlt_service">avformat-novalidate</property>
    <filter id="g0"><property name="mlt_service">volume</property><property name="shotcut:filter">audioGain</property><property name="level">6</property></filter>
  </chain>
  <chain id="chain1" out="249"><property name="length">250</property><property name="resource">$MEDIA/tone_av.mp4</property><property name="mlt_service">avformat-novalidate</property></chain>
  <producer id="img" in="0" out="49"><property name="length">50</property><property name="resource">$TMP/still.png</property><property name="mlt_service">qimage</property></producer>
  <producer id="col" in="0" out="49"><property name="length">50</property><property name="resource">#ff0000</property><property name="mlt_service">color</property><property name="shotcut:caption">red</property></producer>
  <chain id="chain2" out="249"><property name="length">250</property><property name="resource">$MEDIA/tone_av.mp4</property><property name="mlt_service">avformat-novalidate</property>
    <filter id="f2"><property name="mlt_service">brightness</property><property name="shotcut:filter">fadeInBrightness</property><property name="alpha">1</property><property name="level">0=0;24=1</property><property name="shotcut:animIn">00:00:01.000</property></filter>
  </chain>
  <chain id="chain3" out="249"><property name="length">250</property><property name="resource">$MEDIA/tone_av.mp4</property><property name="mlt_service">avformat-novalidate</property>
    <filter id="g3"><property name="mlt_service">volume</property><property name="shotcut:filter">audioGain</property><property name="level">0=0;49=6</property></filter>
  </chain>
  <chain id="chain4" out="249"><property name="length">250</property><property name="resource">$MEDIA/tone_av.mp4</property><property name="mlt_service">avformat-novalidate</property></chain>
  <chain id="chain6" out="249"><property name="length">250</property><property name="resource">$MEDIA/tone_av.mp4</property><property name="mlt_service">avformat-novalidate</property>
    <filter id="f6"><property name="mlt_service">brightness</property><property name="shotcut:filter">fadeInBrightness</property><property name="alpha">1</property><property name="level">1</property><property name="shotcut:animIn">00:00:01.000</property></filter>
  </chain>
  <producer id="xmlp" in="0" out="49"><property name="length">50</property><property name="resource">$TMP/speedsub.mlt</property><property name="mlt_service">xml</property></producer>
  <chain id="chain5" out="249"><property name="length">250</property><property name="resource">$MEDIA/audioonly.wav</property><property name="mlt_service">avformat-novalidate</property></chain>
  <playlist id="playlist0"><property name="shotcut:video">1</property><property name="shotcut:name">V1</property><entry producer="chain0" in="0" out="249"/><blank length="150"/><entry producer="chain1" in="50" out="99"/></playlist>
  <playlist id="playlist1"><property name="shotcut:video">1</property><property name="shotcut:name">V2</property><entry producer="img" in="0" out="49"/><entry producer="col" in="0" out="49"/><entry producer="chain2" in="0" out="49"/><entry producer="chain3" in="0" out="49"/><entry producer="chain4" in="0" out="49"/><entry producer="chain6" in="0" out="49"/><entry producer="xmlp" in="0" out="49"/></playlist>
  <playlist id="playlist2"><property name="shotcut:audio">1</property><property name="shotcut:name">A1</property><entry producer="chain5" in="0" out="49"/></playlist>
  <tractor id="tractor0" in="0" out="449">
    <property name="shotcut">1</property>
    <track producer="background"/>
    <track producer="playlist0"/>
    <track producer="playlist1"/>
    <track producer="playlist2" hide="video"/>
    <transition id="transition0"><property name="a_track">0</property><property name="b_track">1</property><property name="mlt_service">mix</property><property name="always_active">1</property><property name="sum">1</property></transition>
    <transition id="transition1"><property name="a_track">0</property><property name="b_track">2</property><property name="mlt_service">mix</property><property name="always_active">1</property><property name="sum">1</property></transition>
    <transition id="transition2"><property name="a_track">1</property><property name="b_track">2</property><property name="mlt_service">frei0r.cairoblend</property><property name="always_active">1</property></transition>
    <transition id="transition3"><property name="a_track">0</property><property name="b_track">3</property><property name="mlt_service">mix</property><property name="always_active">1</property><property name="sum">1</property></transition>
  </tractor>
</mlt>
XML
open_fixture_project "$TMP/speed.mlt" >/dev/null
sleep 1
TL="$(tool_json get_timeline '{}')"
V1="$(echo "$TL" | jq '.tracks[] | select(.name=="V1") | .index')"
V2="$(echo "$TL" | jq '.tracks[] | select(.name=="V2") | .index')"
A1="$(echo "$TL" | jq '.tracks[] | select(.name=="A1") | .index')"
items() { tool_json get_timeline '{}' | jq -c "[.tracks[$1].clips[] | [.kind,.start,.duration]]"; }
inout() { tool_json get_timeline '{}' | jq -c ".tracks[$1].clips[$2] | [.in,.out]"; }
expect_eq "speed fixture: V1 items" '[["clip",0,250],["blank",250,150],["clip",400,50]]' "$(items "$V1")"
expect_eq "speed fixture: V2 items" '[["clip",0,50],["clip",50,50],["clip",100,50],["clip",150,50],["clip",200,50],["clip",250,50],["clip",300,50]]' "$(items "$V2")"
sstate() {
  local t c s
  s="$(tool_json get_timeline '{}' | jq -c 'del(.position)')"
  for t in "$V1 0" "$V1 2" "$V2 2" "$V2 3" "$A1 0"; do s="$s|$(fa $t)"; done
  echo "$s"
}
# Prints true when two saved projects are identical apart from shotcut:caption lines, else their
# first differences (stderr). Shotcut's undo of the speed change selects the clip, and the
# Properties panel then writes the clip's computed caption (AvformatProducerWidget, through
# Util::updateCaption) on its own timer: a save right after the undo may or may not have it.
# Every other byte must match.
same_file_exact() {
  if cmp -s <(grep -v 'name="shotcut:caption"' "$1") <(grep -v 'name="shotcut:caption"' "$2"); then echo true
  else diff <(grep -v 'name="shotcut:caption"' "$1") <(grep -v 'name="shotcut:caption"' "$2") | head -20 >&2; fi
}
# As same_file_exact, but meta.media.* lines are also left out: MLT caches probe results there
# when it opens or decodes a file, so an untouched clip that Shotcut's undo rebuilds from XML can
# gain some (seen: a neighbour's meta.media.top_field_first and meta.media.progressive) depending
# on when the preview reads it.
same_file() {
  local x='name="(meta\.media\.|shotcut:caption")'
  if cmp -s <(grep -Ev "$x" "$1") <(grep -Ev "$x" "$2"); then echo true
  else diff <(grep -Ev "$x" "$1") <(grep -Ev "$x" "$2") | head -20 >&2; fi
}
speed() { tool_json clip_set_speed "$(jq -nc --argjson t "$1" --argjson c "$2" --argjson s "$3" '{track:$t,clip:$c,speed:$s}')"; }
S0="$(sstate)"
tool_json save_project "{\"path\":\"$TMP/speed0.mlt\"}" >/dev/null
R="$(speed "$V1" 0 2)"
expect_eq "speed 2: the 250-frame clip lasts 125 frames, in 0 out 124" '[0,125,0,124]' "$(echo "$R" | jq -c ".track.clips[0] | [.start,.duration,.in,.out]")"
expect_eq "speed 2: the clip keeps its start; the gap grows; the next clip does not move" '[["clip",0,125],["blank",125,275],["clip",400,50]]' "$(items "$V1")"
expect_eq "speed 2: result speed and frames" '{"duration":125,"speed":2}' "$(echo "$R" | jq -cS '{speed, duration}')"
expect_eq "speed 2: the clip's gain filter is kept" "$(jn '[{"id":"gain","params":{"level":6}}]')" "$(fparams "$V1" 0)"
expect_eq "speed 2: the clip's name shows the speed" true "$(tool_json get_timeline '{}' | jq ".tracks[$V1].clips[0].name | test(\"2x\")")"
tool_json save_project "{\"path\":\"$TMP/speed2.mlt\"}" >/dev/null
expect_eq "speed 2: saved as a timewarp producer with warp_speed 2" 1 "$(grep -c '<property name="warp_speed">2</property>' "$TMP/speed2.mlt")"
expect_eq "speed 2: keep_pitch defaults to true (warp_pitch 1)" 1 "$(grep -c '<property name="warp_pitch">1</property>' "$TMP/speed2.mlt")"
S1="$(sstate)"
expect_eq "speed 2: one undo step" "Set clip speed: 2x" "$(undo_text)"
expect_eq "speed 2: undo restores the timeline exactly" "$S0" "$(sstate)"
tool_json save_project "{\"path\":\"$TMP/speed0u.mlt\"}" >/dev/null
expect_eq "speed 2: the project saved after undo is identical (but for the panel-written caption)" true "$(same_file_exact "$TMP/speed0.mlt" "$TMP/speed0u.mlt")"
redo_text >/dev/null
expect_eq "speed 2: redo applies it again" "$S1" "$(sstate)"
R="$(speed "$V1" 0 2)"
expect_eq "speed 2 again: changed false" false "$(echo "$R" | jq '.changed')"
expect_eq "speed 2 again: nothing changed" "$S1" "$(sstate)"
expect_eq "speed 2 again: no undo step (the next undo is the first 2x)" "Set clip speed: 2x" "$(undo_text)"
expect_eq "speed 2 again: that undo restores the start" "$S0" "$(sstate)"
redo_text >/dev/null
expect_eq "speed 0.5 after 2: lengthening into the next clip is an error" true "$(ferr clip_set_speed "{\"track\":$V1,\"clip\":0,\"speed\":0.5}")"
expect_eq "speed 0.5 after 2: the error names the free frames (275)" true "$(tool_text clip_set_speed "{\"track\":$V1,\"clip\":0,\"speed\":0.5}" | grep -q '275' && echo true)"
expect_eq "speed 0.5 after 2: nothing changed" "$S1" "$(sstate)"
# A blank's in and out (offsets in MLT's blank producer) are not saved in a project and are not
# kept by Shotcut's undo, which rebuilds a blank from its length: compare blanks by start and
# duration here, and the saved projects byte for byte.
sstate_nb() { local s; s="$(sstate)"; echo "$(jq -c '(.tracks[].clips[] | select(.kind=="blank")) |= del(.in,.out)' <<<"${s%%|*}")|${s#*|}"; }
S1NB="$(sstate_nb)"
tool_json save_project "{\"path\":\"$TMP/speed1.mlt\"}" >/dev/null
R="$(speed "$V1" 0 1)"
# Shotcut scales out, not the length: out 124 x 2 = 248, so 249 frames (250 +-1).
expect_eq "speed 1 after 2: start 0, in 0, 250 frames +-1" true "$(echo "$R" | jq ".track.clips[0] | .start == 0 and .in == 0 and (.duration - 250 | fabs) <= 1")"
expect_eq "speed 1 after 2: the next clip did not move" '["clip",400,50]' "$(items "$V1" | jq -c '.[-1]')"
expect_eq "speed 1 after 2: one undo step" "Set clip speed: 1x" "$(undo_text)"
expect_eq "speed 1 after 2: undo (blanks by start and duration)" "$S1NB" "$(sstate_nb)"
tool_json save_project "{\"path\":\"$TMP/speed1u.mlt\"}" >/dev/null
expect_eq "speed 1 after 2: the project saved after undo equals the one saved before" true "$(same_file "$TMP/speed1.mlt" "$TMP/speed1u.mlt")"
undo_text >/dev/null
expect_eq "speed: back to the start" "$S0" "$(sstate)"
# The trimmed clip (in 50, out 99), last on its track.
R="$(speed "$V1" 2 2)"
expect_eq "speed 2 on a trimmed clip: start 400, in 25, duration 25 +-1" true "$(echo "$R" | jq ".track.clips[2] | .start == 400 and .in == 25 and (.duration - 25 | fabs) <= 1")"
undo_text >/dev/null
R="$(speed "$V1" 2 0.5)"
expect_eq "speed 0.5 on the last clip: start 400, in 100, duration 100 +-1" true "$(echo "$R" | jq ".track.clips[2] | .start == 400 and .in == 100 and (.duration - 100 | fabs) <= 1")"
expect_eq "speed 0.5: one undo step" "Set clip speed: 0.5x" "$(undo_text)"
expect_eq "speed 0.5: undo" "$S0" "$(sstate)"
R="$(tool_json clip_set_speed "{\"track\":$V1,\"clip\":2,\"speed\":1.5,\"keep_pitch\":false}")"
tool_json save_project "{\"path\":\"$TMP/speedp.mlt\"}" >/dev/null
expect_eq "speed keep_pitch false: warp_pitch 0" "1 0" "$(echo "$(grep -c '<property name="warp_pitch">0</property>' "$TMP/speedp.mlt") $(grep -c '<property name="warp_pitch">1</property>' "$TMP/speedp.mlt")")"
undo_text >/dev/null
R="$(speed "$A1" 0 2)"
expect_eq "speed 2 on an audio clip: start 0, 25 frames +-1" true "$(echo "$R" | jq ".track.clips[0] | .start == 0 and (.duration - 25 | fabs) <= 1")"
undo_text >/dev/null
expect_eq "speed: back to the start again" "$S0" "$(sstate)"
for a in "{\"track\":$V1,\"clip\":1,\"speed\":2}" "{\"track\":$V2,\"clip\":0,\"speed\":2}" "{\"track\":$V2,\"clip\":1,\"speed\":2}" \
  "{\"track\":$V2,\"clip\":2,\"speed\":2}" "{\"track\":$V2,\"clip\":3,\"speed\":2}" \
  "{\"track\":$V1,\"clip\":0,\"speed\":0}" "{\"track\":$V1,\"clip\":0,\"speed\":11}" "{\"track\":$V1,\"clip\":0,\"speed\":-2}" \
  "{\"track\":$V1,\"clip\":0,\"speed\":0.09}" "{\"track\":$V1,\"clip\":0,\"speed\":\"2\"}" "{\"track\":$V1,\"clip\":0}" \
  "{\"track\":$V1,\"clip\":0,\"speed\":2,\"keep_pitch\":\"yes\"}" "{\"track\":$V1,\"clip\":0,\"speed\":2,\"reverse\":true}" \
  "{\"track\":$V1,\"clip\":9,\"speed\":2}" "{\"track\":$V1,\"speed\":2}"; do
  expect_eq "speed: error for $a" true "$(ferr clip_set_speed "$a")"
done
expect_eq "speed: a fade is refused with 'Remove fades and keyframes first'" true "$(tool_text clip_set_speed "{\"track\":$V2,\"clip\":2,\"speed\":2}" | grep -q 'Remove fades and keyframes first' && echo true)"
expect_eq "speed: keyframes are refused with 'Remove fades and keyframes first'" true "$(tool_text clip_set_speed "{\"track\":$V2,\"clip\":3,\"speed\":2}" | grep -q 'Remove fades and keyframes first' && echo true)"
expect_eq "speed: a fade with a plain level (shotcut:animIn only) is refused" true "$(tool_text clip_set_speed "{\"track\":$V2,\"clip\":5,\"speed\":2}" | grep -q 'Remove fades and keyframes first' && echo true)"
expect_eq "speed: a .mlt project clip is refused (media file clips only)" true "$(tool_text clip_set_speed "{\"track\":$V2,\"clip\":6,\"speed\":2}" | grep -q 'media file clips only' && echo true)"
tool_json track_set "{\"track\":$V1,\"lock\":true}" >/dev/null
expect_eq "speed: a locked track is an error" true "$(ferr clip_set_speed "{\"track\":$V1,\"clip\":0,\"speed\":2}")"
tool_json track_set "{\"track\":$V1,\"lock\":false}" >/dev/null
undo_text >/dev/null; undo_text >/dev/null
expect_eq "speed: errors changed nothing" "$S0" "$(sstate)"

echo "== clip_set_speed: transitions and export"
open_fixture_project "$EMPTY" >/dev/null
sleep 1
ins "$MEDIA/tone_av.mp4" 0 4 0 0
ins "$MEDIA/tone_av.mp4" 4 8 0 100
tool_json timeline_add_transition '{"track":0,"clip":1,"duration":10}' >/dev/null
expect_eq "speed transition fixture: clip, transition, clip" '["clip","transition","clip"]' "$(tool_json get_timeline '{}' | jq -c '[.tracks[0].clips[].kind]')"
for c in 0 1 2; do
  expect_eq "speed: item $c next to or in a transition is an error" true "$(ferr clip_set_speed "{\"track\":0,\"clip\":$c,\"speed\":2}")"
done
open_fixture_project "$EMPTY" >/dev/null
sleep 1
ins "$MEDIA/tone_av.mp4" 0 10 0 0
speed 0 0 2 >/dev/null
EJ="$(tool_json export_video "$(jq -nc --arg p "$TMP/speed.mp4" '{path:$p,preset:"x264-medium"}')" | jq -r '.job_id')"
ST=""
for _ in $(seq 1 120); do
  ST="$(tool_json get_export_status "{\"job_id\":$EJ}" | jq -r '.state')"
  case "$ST" in succeeded|failed|stopped|removed) break ;; esac
  sleep 1
done
expect_eq "speed export: succeeded" succeeded "$ST"
DUR="$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$TMP/speed.mp4")"
echo "INFO  speed 2 export duration $DUR s (source 10 s)"
expect_eq "speed export: 10 s at speed 2 lasts 5 s (+-0.2)" true "$(awk -v d="$DUR" 'BEGIN {print (d != "" && d >= 4.8 && d <= 5.2) ? "true" : "false"}')"
expect_eq "speed export: has an audio stream" audio "$(ffprobe -v error -select_streams a -show_entries stream=codec_type -of csv=p=0 "$TMP/speed.mp4" | head -1)"

echo "== clip_set_speed and clip_detach_audio: preview guard"
# Both run inside EditPreviewGuard: afterwards the preview must show the edited timeline (a fresh
# frame), and a call made while playing ends paused. scenes.mp4 is red 0-74, green 75-149, blue
# 150-224 (with audio).
gf() { call_tool get_frame "{\"position\":$1}" | jq -r '.result.content[0].data' | base64 -d > "$TMP/gf.jpg"; }
rgb() {
  local hex r g b
  hex="$(ffmpeg -v error -i "$TMP/gf.jpg" -vf "crop=100:100:110:70,scale=1:1" -f rawvideo -pix_fmt rgb24 - | xxd -p)"
  r=$((16#${hex:0:2})); g=$((16#${hex:2:2})); b=$((16#${hex:4:2}))
  if [ "$r" -gt 150 ] && [ "$g" -lt 90 ] && [ "$b" -lt 90 ]; then echo R
  elif [ "$g" -gt 150 ] && [ "$r" -lt 90 ] && [ "$b" -lt 90 ]; then echo G
  elif [ "$b" -gt 150 ] && [ "$r" -lt 90 ] && [ "$g" -lt 90 ]; then echo B
  else echo "?($hex)"; fi
}
playing() { tool_json get_state '{}' | jq -r '.playing'; }
open_fixture_project "$EMPTY" >/dev/null
sleep 1
ins "$MEDIA/scenes.mp4" 0 9 0 0
gf 100; expect_eq "guard fixture: frame 100 is green" G "$(rgb)"
speed 0 0 2 >/dev/null
gf 100; expect_eq "speed 2: frame 100 is now source frame 200, blue (fresh frame)" B "$(rgb)"
undo_text >/dev/null
gf 100; expect_eq "speed undone: frame 100 is green again" G "$(rgb)"
tool_json play '{}' >/dev/null
sleep 0.5
speed 0 0 2 >/dev/null
expect_eq "speed while playing: playback is paused afterwards" false "$(playing)"
gf 100; expect_eq "speed while playing: frame 100 is blue" B "$(rgb)"
undo_text >/dev/null
tool_json play '{}' >/dev/null
sleep 0.5
expect_eq "detach while playing: succeeds" false "$(ferr clip_detach_audio '{"track":0,"clip":0}')"
expect_eq "detach while playing: playback is paused afterwards" false "$(playing)"
gf 100; expect_eq "detach: frame 100 is still green (fresh frame)" G "$(rgb)"
undo_text >/dev/null
gf 10; expect_eq "detach undone: frame 10 is red" R "$(rgb)"

# FOOTER (later tasks insert their blocks above this line)
if [ "$failures" -eq 0 ]; then echo "All checks passed."; else echo "${failures} check(s) failed."; exit 1; fi
