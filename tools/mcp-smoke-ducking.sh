#!/usr/bin/env bash
# Checks track_set_ducking end to end on MLT 7.41 or newer: exports a tone that starts halfway
# through above a steady music track and measures how far the music dips.
# Usage: MCP_TOKEN=<token> [MCP_PORT=7468] tools/mcp-smoke-ducking.sh
# Run it against a FRESH, throwaway Shotcut. On an older MLT it reports that and exits 2 (not a pass).
# Needs: bash, curl, jq, ffmpeg.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=tools/mcp-smoke-lib.sh
. "$HERE/mcp-smoke-lib.sh"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fresh_guard

# The voice is silent for 4 s and then a loud 1 kHz tone; the music is a steady, quieter 200 Hz.
ffmpeg -loglevel error -y -f lavfi -i "testsrc=size=320x240:rate=25:duration=8" \
  -f lavfi -i "aevalsrc='0.5*sin(2*PI*1000*t)*gt(t,4)':s=48000:d=8" \
  -pix_fmt yuv420p -c:v libx264 -c:a aac -b:a 192k -shortest "$TMP/voice.mp4" || exit 2
ffmpeg -loglevel error -y -f lavfi -i "sine=frequency=200:sample_rate=48000:duration=8" \
  -af "volume=-14dB" "$TMP/music.wav" || exit 2

export_it() {
  local job st=""
  job="$(tool_json export_video "$(jq -nc --arg p "$1" '{path:$p,preset:"x264-medium"}')" | jq '.job_id')"
  for _ in $(seq 1 300); do
    st="$(tool_json get_export_status "{\"job_id\":$job}" | jq -r '.state')"
    case "$st" in succeeded|failed|stopped|removed) break ;; esac
    sleep 1
  done
  echo "$st"
}
# music_db FILE START LENGTH: mean level of the music alone (the 1 kHz voice filtered out).
music_db() {
  ffmpeg -hide_banner -ss "$2" -t "$3" -i "$1" -vn \
    -af "lowpass=f=350,lowpass=f=350,lowpass=f=350,lowpass=f=350,volumedetect" -f null - 2>&1 \
    | sed -n 's/.*mean_volume: \(-\{0,1\}[0-9.]*\) dB.*/\1/p'
}
# dip FILE: how many dB the music falls once the voice has started.
dip() { awk -v a="$(music_db "$1" 1 2.5)" -v b="$(music_db "$1" 5.5 2)" 'BEGIN{printf "%.1f", a - b}'; }
between() { awk -v v="$1" -v lo="$2" -v hi="$3" 'BEGIN{print (v >= lo && v <= hi) ? "true" : "false"}'; }

tool_json timeline_insert_range "$(jq -nc --arg p "$TMP/voice.mp4" '{path:$p,in_s:0,out_s:8,track:0,position:0}')" >/dev/null
MT="$(tool_json track_add '{"type":"audio","name":"Music"}' | jq '.added.index')"
tool_json timeline_insert_range "$(jq -nc --arg p "$TMP/music.wav" --argjson t "$MT" '{path:$p,in_s:0,out_s:8,track:$t,position:0,mode:"overwrite"}')" >/dev/null

ARGS="{\"track\":$MT,\"threshold_db\":-30,\"reduction_db\":-12,\"fade_in_ms\":100,\"fade_out_ms\":100}"
OUT="$(call_tool track_set_ducking "$ARGS")"
if echo "$OUT" | jq -r '.result.content[0].text' | grep -q 'needs MLT 7.41'; then
  echo "SKIP  $(echo "$OUT" | jq -r '.result.content[0].text')"
  exit 2
fi
expect_eq "the call succeeds" false "$(echo "$OUT" | jq -r '.result.isError')"
expect_eq "duck_threshold is read back" -30 "$(echo "$OUT" | jq -r '.result.content[0].text | fromjson | .ducking.duck_threshold')"

expect_eq "export with ducking" succeeded "$(export_it "$TMP/on.mp4")"
D="$(dip "$TMP/on.mp4")"
echo "INFO  ducking on: the music dips $D dB"
expect_eq "ducking on: the music dips about 12 dB" true "$(between "$D" 9 15)"

tool_json undo '{}' >/dev/null
expect_eq "export after undo" succeeded "$(export_it "$TMP/undone.mp4")"
D="$(dip "$TMP/undone.mp4")"
echo "INFO  after undo: the music dips $D dB"
expect_eq "after undo: the music stays level" true "$(between "$D" -1 1)"

tool_json redo '{}' >/dev/null
expect_eq "export after redo" succeeded "$(export_it "$TMP/redone.mp4")"
D="$(dip "$TMP/redone.mp4")"
echo "INFO  after redo: the music dips $D dB"
expect_eq "after redo: the music dips about 12 dB" true "$(between "$D" 9 15)"

tool_json track_set_ducking "{\"track\":$MT,\"enabled\":false}" >/dev/null
expect_eq "export with ducking disabled" succeeded "$(export_it "$TMP/off.mp4")"
D="$(dip "$TMP/off.mp4")"
echo "INFO  disabled: the music dips $D dB"
expect_eq "disabled: the music stays level" true "$(between "$D" -1 1)"

if [ "$failures" -eq 0 ]; then echo "All checks passed."; else echo "${failures} check(s) failed."; exit 1; fi
