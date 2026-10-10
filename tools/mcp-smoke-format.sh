#!/usr/bin/env bash
# Format smoke test for the Shotcut MCP server (stage 3, part 3C): project_set_format.
# Usage: MCP_TOKEN=<token> [MCP_PORT=7468] [MCP_APPDATA=<scratch dir>] tools/mcp-smoke-format.sh
# Run it against a FRESH, throwaway Shotcut (nothing loaded, no timeline); it aborts otherwise
# unless MCP_SMOKE_FORCE=1. It creates its media with ffmpeg in a temporary folder and exports
# one file. With MCP_APPDATA it also checks the saved Video Mode setting is left alone.
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
expect_eq "tools/list: 52 tools in all (through Task 17)" 52 "$(echo "$TOOLS" | jq 'length')"
expect_eq "tools/list has project_set_format" true "$(echo "$TOOLS" | jq '[.[].name] | index("project_set_format") != null')"
expect_eq "project_set_format schema refuses extra properties" false "$(echo "$TOOLS" | jq '.[] | select(.name=="project_set_format") | .inputSchema.additionalProperties')"

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
