#!/usr/bin/env bash
# Checks that timeline_remove_ranges controls ripple itself when the USER's Shotcut settings
# "ripple all tracks" and "ripple markers" are both on, and puts them back.
# Usage: MCP_TOKEN=<token> [MCP_PORT=7468] tools/mcp-smoke-ripple.sh
# Run it against a FRESH, throwaway Shotcut whose settings have Timeline > Ripple All Tracks and
# Ripple Markers turned on (shotcut.ini: [timeline] rippleAllTracks=true, rippleMarkers=true);
# with other settings it reports that, skips its checks and exits 2 (not a pass).
# Needs: bash, curl, jq, ffmpeg.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=tools/mcp-smoke-lib.sh
. "$HERE/mcp-smoke-lib.sh"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fresh_guard

MEDIA="$("$HERE/mcp-fixtures.sh" "$TMP/media")" || { echo "ffmpeg could not create the fixtures"; exit 2; }

# V1 and A1, both tone_av.mp4 0-249, and markers at 20 (before), 75 (inside [50,100)) and 150.
cat > "$TMP/two.mlt" <<XML
<?xml version="1.0" encoding="utf-8"?>
<mlt LC_NUMERIC="C" producer="main_bin">
  <profile description="320x240 25 fps" width="320" height="240" progressive="1" sample_aspect_num="1" sample_aspect_den="1" display_aspect_num="4" display_aspect_den="3" frame_rate_num="25" frame_rate_den="1" colorspace="601"/>
  <playlist id="main_bin"><property name="xml_retain">1</property></playlist>
  <producer id="black" in="0" out="249"><property name="length">250</property><property name="resource">0</property><property name="mlt_service">color</property><property name="set.test_audio">0</property></producer>
  <playlist id="background"><entry producer="black" in="0" out="249"/></playlist>
  <chain id="chain0" out="249"><property name="length">250</property><property name="resource">$MEDIA/tone_av.mp4</property><property name="mlt_service">avformat-novalidate</property></chain>
  <chain id="chain1" out="249"><property name="length">250</property><property name="resource">$MEDIA/tone_av.mp4</property><property name="mlt_service">avformat-novalidate</property></chain>
  <playlist id="playlist0"><property name="shotcut:video">1</property><property name="shotcut:name">V1</property><entry producer="chain0" in="0" out="249"/></playlist>
  <playlist id="playlist1"><property name="shotcut:audio">1</property><property name="shotcut:name">A1</property><entry producer="chain1" in="0" out="249"/></playlist>
  <tractor id="tractor0" in="0" out="249">
    <property name="shotcut">1</property>
    <properties name="shotcut:markers">
      <properties name="0"><property name="text">before</property><property name="start">00:00:00.800</property><property name="end">00:00:00.800</property><property name="color">#008000</property></properties>
      <properties name="1"><property name="text">inside</property><property name="start">00:00:03.000</property><property name="end">00:00:03.000</property><property name="color">#008000</property></properties>
      <properties name="2"><property name="text">after</property><property name="start">00:00:06.000</property><property name="end">00:00:06.000</property><property name="color">#008000</property></properties>
    </properties>
    <track producer="background"/>
    <track producer="playlist0"/>
    <track producer="playlist1" hide="video"/>
    <transition id="transition0"><property name="a_track">0</property><property name="b_track">1</property><property name="mlt_service">mix</property><property name="always_active">1</property><property name="sum">1</property></transition>
    <transition id="transition1"><property name="a_track">0</property><property name="b_track">2</property><property name="mlt_service">mix</property><property name="always_active">1</property><property name="sum">1</property></transition>
  </tractor>
</mlt>
XML
open_fixture_project "$TMP/two.mlt" >/dev/null
sleep 1
SETTINGS='{"all_tracks":true,"markers":true}'
settings() { tool_json get_timeline '{}' | jq -c '{all_tracks: .settings.ripple_all_tracks, markers: .settings.ripple_markers}'; }
if [ "$(settings)" != "$SETTINGS" ]; then
  echo "SKIP  the ripple settings are not both on in this Shotcut; nothing checked."
  exit 2
fi
# Markers as "text@start" from a saved copy of the project.
markers() {
  tool_json save_project "{\"path\":\"$TMP/saved.mlt\"}" >/dev/null
  tr -d '\n' < "$TMP/saved.mlt" | sed 's/> *</></g' | grep -o 'name="text">[^<]*</property><property name="start">[^<]*' | sed 's/name="text">//; s#</property><property name="start">#@#' | tr '\n' ' '
}
clipf() { jq -c "[.clips[] | select(.kind==\"clip\") | [.start, .duration, .in, .out]]"; }
TL0="$(tool_json get_timeline '{}')"
M0="$(markers)"
expect_eq "fixture markers" "before@00:00:00.800 inside@00:00:03.000 after@00:00:06.000 " "$M0"

echo "== timeline_remove_ranges with the user's ripple settings on"
R="$(tool_json timeline_remove_ranges '{"tracks":[0],"ranges":[{"start":50,"end":100}]}')"
expect_eq "unlisted track: the result reports the effective flags" '{"all_tracks":false,"markers":true}' "$(echo "$R" | jq -c '.ripple')"
expect_eq "unlisted track: the listed track rippled" "[[0,50,0,49],[50,150,100,249]]" "$(echo "$R" | jq -c '.tracks[0]' | clipf)"
expect_eq "unlisted track: A1 untouched despite ripple-all-tracks" "$(echo "$TL0" | jq -c '.tracks[1]')" "$(tool_json get_timeline '{}' | jq -c '.tracks[1]')"
expect_eq "unlisted track: the user's settings are back on" "$SETTINGS" "$(settings)"
tool_json undo '{}' >/dev/null
expect_eq "unlisted track: undo restores" "$TL0" "$(tool_json get_timeline '{}')"

R="$(tool_json timeline_remove_ranges '{"tracks":[0,1],"ranges":[{"start":50,"end":100}]}')"
expect_eq "two tracks: markers flag reported" true "$(echo "$R" | jq -r '.ripple.markers')"
expect_eq "two tracks: both tracks cut the same" true "$(echo "$R" | jq '[.tracks[] | [.clips[] | [.start,.duration,.in]]] | .[0] == .[1]')"
expect_eq "two tracks: the marker inside is deleted, the later one shifted ONCE (6.0 s -> 4.0 s)" "before@00:00:00.800 after@00:00:04.000 " "$(markers)"
expect_eq "two tracks: the user's settings are back on" "$SETTINGS" "$(settings)"
TL1="$(tool_json get_timeline '{}')"
tool_json undo '{}' >/dev/null
expect_eq "two tracks: undo restores the clips" "$TL0" "$(tool_json get_timeline '{}')"
expect_eq "two tracks: undo restores the markers" "$M0" "$(markers)"
tool_json redo '{}' >/dev/null
expect_eq "two tracks: redo re-applies the clips" "$TL1" "$(tool_json get_timeline '{}')"
expect_eq "two tracks: redo re-applies the markers" "before@00:00:00.800 after@00:00:04.000 " "$(markers)"

expect_eq "an error leaves the settings on" true "$(tool_is_error timeline_remove_ranges '{"tracks":[0,0],"ranges":[{"start":0,"end":10}]}')"
expect_eq "after an error the settings are on" "$SETTINGS" "$(settings)"

if [ "$failures" -eq 0 ]; then echo "All checks passed."; else echo "${failures} check(s) failed."; exit 1; fi
