#!/usr/bin/env bash
# End-to-end smoke test for the Shotcut MCP server (stage 3): one use case of get_editing_guide
# per run, scripted from the guide's steps on the generated fixtures, exported and probed.
# Usage: MCP_TOKEN=<token> [MCP_PORT=7468] tools/mcp-smoke-e2e.sh CASE
# CASE: interview | short | tutorial | montage | interview-whisper. Each case needs its OWN
# fresh, throwaway Shotcut (nothing loaded, no timeline): it aborts otherwise unless
# MCP_SMOKE_FORCE=1 (project_set_format works only on an empty project).
# interview-whisper needs a real whisper-cli configured in that Shotcut (WHISPER_EXE must be set
# in the environment too; it exits 2, not a pass, without it) and whisper.cpp's samples/jfk.wav
# (JFK_WAV, default ~/GitHub/whisper.cpp/samples/jfk.wav).
# Needs: bash, curl, jq, ffmpeg, ffprobe, od, awk.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=tools/mcp-smoke-lib.sh
. "$HERE/mcp-smoke-lib.sh"

CASE="${1:-}"
case "$CASE" in
  interview|short|tutorial|montage|interview-whisper) ;;
  *) echo "usage: $0 interview|short|tutorial|montage|interview-whisper"; exit 2 ;;
esac

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fresh_guard

MEDIA="$("$HERE/mcp-fixtures.sh" "$TMP/media")" || { echo "ffmpeg could not create the fixtures"; exit 2; }

ok() { [ "$(echo "$1" | jq -r '.result.isError')" = false ]; }
# step NAME TOOL ARGS: calls TOOL, PASS when it succeeds; the payload goes to $R.
step() {
  local out
  out="$(call_tool "$2" "$3")"
  if ok "$out"; then
    pass "$1"
    R="$(echo "$out" | jq -c '.result.content[0].text | fromjson? // .result.content[0].text')"
  else
    fail "$1" "$(echo "$out" | jq -r '.result.content[0].text // .error.message // .')"
    R='{}'
  fi
}
# job_result JSON: the finished result of a tool that may have returned {job_id}.
job_result() {
  local id
  id="$(echo "$1" | jq -r '.job_id // empty')"
  if [ -n "$id" ]; then wait_job "$id" 180 | jq -c '.result // .'; else echo "$1"; fi
}
export_wait() {
  local job st=""
  job="$(tool_json export_video "$(jq -nc --arg p "$1" '{path:$p,preset:"x264-medium"}')" | jq '.job_id')"
  for _ in $(seq 1 300); do
    st="$(tool_json get_export_status "{\"job_id\":$job}" | jq -r '.state')"
    case "$st" in succeeded|failed|stopped|removed) break ;; esac
    sleep 1
  done
  echo "$st"
}
fdur() { ffprobe -v error -show_entries format=duration -of csv=p=0 "$1" 2>/dev/null; }
vsize() { ffprobe -v error -select_streams v:0 -show_entries stream=width,height -of csv=s=x:p=0 "$1" 2>/dev/null; }
# near A B TOL: true when |A - B| <= TOL.
near() { awk -v a="$1" -v b="$2" -v t="$3" 'BEGIN{d = a - b; if (d < 0) d = -d; print (a != "" && d <= t) ? "true" : "false"}'; }
# rgb FILE T X Y: "R G B" of the pixel at (X, Y) of FILE's frame at T seconds.
rgb() {
  ffmpeg -loglevel error -ss "$2" -i "$1" -frames:v 1 -vf "format=rgb24,crop=1:1:$3:$4" \
    -f rawvideo -pix_fmt rgb24 - | od -An -tu1 | xargs
}
# colour "R G B": red, green, blue, black, white or other.
colour() {
  echo "$1" | awk '{r=$1; g=$2; b=$3
    if (r > 150 && g < 100 && b < 100) print "red"; else if (g > 150 && r < 100 && b < 100) print "green"
    else if (b > 150 && r < 100 && g < 100) print "blue"; else if (r < 40 && g < 40 && b < 40) print "black"
    else if (r > 200 && g > 200 && b > 200) print "white"; else print "other"}'
}
# white_px FILE T X Y W H: the number of near-white pixels (R, G and B >= 200) in the rect.
white_px() {
  ffmpeg -loglevel error -ss "$2" -i "$1" -frames:v 1 -vf "format=rgb24,crop=$5:$6:$3:$4" \
    -f rawvideo -pix_fmt rgb24 - | od -An -v -tu1 -w3 \
    | awk '$1 >= 200 && $2 >= 200 && $3 >= 200 {n++} END {print n + 0}'
}
# clips_of TRACK: [start, duration] of every clip of TRACK.
clips_of() { tool_json get_timeline '{}' | jq -c "[.tracks[$1].clips[] | select(.kind==\"clip\") | [.start,.duration]]"; }
# pad_ranges FPS PAD CLIP_START CLIP_END RANGES: the silence timeline ranges, each shrunk by PAD
# frames at both ends (the guide's 0.15 s pads), except at the clip's own start and end, where a
# pad would leave a stub of a few frames; ranges shorter than 2 pads are dropped.
pad_ranges() {
  echo "$5" | jq -c --argjson p "$2" --argjson s "$3" --argjson e "$4" '
    [.[] | {start: (if .start <= $s then .start else .start + $p end),
            end: (if .end >= $e then .end else .end - $p end)} | select(.end > .start)]'
}
# caption_clips TRACK ARGS: captions_from_transcript on every clip of TRACK (ARGS are extra JSON
# members); a clip without a word is refused and skipped (the guide's "every clip the cuts left").
# Sets CAPS (captions in all) and CAPCALLS (successful calls).
caption_clips() {
  local n i out
  CAPS=0; CAPCALLS=0
  n="$(tool_json get_timeline '{}' | jq ".tracks[$1].clips | length")"
  for i in $(seq 0 $((n - 1))); do
    [ "$(tool_json get_timeline '{}' | jq -r ".tracks[$1].clips[$i].kind")" = clip ] || continue
    out="$(call_tool captions_from_transcript "{\"track\":$1,\"clip\":$i${2:+,$2}}")"
    if ok "$out"; then
      CAPS=$((CAPS + $(echo "$out" | jq '.result.content[0].text | fromjson | .captions')))
      CAPCALLS=$((CAPCALLS + 1))
    elif echo "$out" | jq -r '.result.content[0].text' | grep -q '^No word of the transcript'; then
      echo "INFO  captions: track $1 clip $i has no word (skipped)"
    else
      fail "captions_from_transcript track $1 clip $i" "$(echo "$out" | jq -r '.result.content[0].text // .')"
    fi
  done
}
# srt_words FILE: the number of words in all subtitle_feed texts of the saved project FILE.
srt_words() {
  awk '/<filter /{f=1; feed=0; buf=""} f && /name="mlt_service">subtitle_feed</{feed=1}
    f && /name="text">/{t=1} f && t {buf = buf $0 "\n"} t && /<\/property>/{t=0}
    /<\/filter>/{if (f && feed) printf "%s", buf; f=0}' "$1" \
    | sed 's/.*name="text">//; s#</property>.*##' \
    | grep -v -E '^[0-9]+$|--(>|&gt;)|^$' | wc -w
}
# transcript S E TEXT ...: a supplied transcript of segments (start, end, text) with evenly timed
# words.
transcript() {
  local args=()
  while [ $# -ge 3 ]; do args+=("$1|$2|$3"); shift 3; done
  printf '%s\n' "${args[@]}" | jq -Rnc '[inputs | split("|") | {s: (.[0] | tonumber),
      e: (.[1] | tonumber), t: .[2]}]
    | {segments: [.[] | .s as $s | .e as $e | (.t | split(" ")) as $w
      | (($e - $s) / ($w | length)) as $d
      | {start_s: $s, end_s: $e, text: .t, words: [range($w | length) as $i
          | {start_s: (($s + $d * $i) * 1000 | round / 1000),
             end_s: (($s + $d * ($i + 1)) * 1000 | round / 1000), text: $w[$i]}]}]}'
}

GUIDE_CASE="${CASE%-whisper}"
echo "== $CASE: get_editing_guide $GUIDE_CASE"
GUIDE="$(tool_json get_editing_guide "{\"use_case\":\"$GUIDE_CASE\"}")"
expect_eq "the guide has steps" true "$(echo "$GUIDE" | jq '.steps | length > 5')"
echo "INFO  guide steps: $(echo "$GUIDE" | jq -r '[.steps[].tool] | join(", ")')"

interview() { # SOURCE TRANSCRIPT_MODE (supplied|whisper)
  local SRC="$1" FPS SRCDUR R0 A SIL CE RANGES REMOVED EXPECT EL TT
  local OUT="$TMP/interview.mp4" SAVED="$TMP/interview.mlt"
  step "analyze_media" analyze_media "$(jq -nc --arg p "$SRC" '{path:$p}')"
  SRCDUR="$(echo "$R" | jq '.duration_s')"
  if [ "$2" = supplied ]; then
    # A JFK-style transcript over the fixture's three spoken stretches (0-2, 4-7, 8.5-10 s).
    TT="$(transcript 0.2 1.8 "And so, my fellow Americans," \
      4.3 6.8 "ask not what your country can do for you," \
      8.7 9.8 "ask what you can do for your country.")"
    step "transcribe: supply the transcript" transcribe "$(jq -nc --arg p "$SRC" --argjson t "$TT" '{path:$p,transcript:$t}')"
    expect_eq "transcribe: 22 words kept" 22 "$(echo "$R" | jq '[.segments[].words[]] | length')"
  else
    step "transcribe: Whisper on the file" transcribe "$(jq -nc --arg p "$SRC" '{path:$p,language:"en",wait_ms:20000}')"
    R="$(job_result "$R")"
    expect_eq "transcribe: Whisper found words" true "$(echo "$R" | jq '[.segments[].words[]?] | length > 15')"
  fi
  step "timeline_insert_range: the whole recording" timeline_insert_range "$(jq -nc --arg p "$SRC" --argjson o "$SRCDUR" '{path:$p,in_s:0,out_s:$o,track:0,position:0}')"
  FPS="$(tool_json get_timeline '{}' | jq '.fps')"
  # The sound on its own track (as from a separate recorder): the cuts must list both tracks.
  step "clip_detach_audio: the sound on its own track" clip_detach_audio '{"track":0,"clip":0}'
  A="$(echo "$R" | jq '.audio_track')"
  step "audio_levels: measure the voice" audio_levels "{\"track\":$A,\"clip\":0,\"wait_ms\":20000}"
  R0="$(job_result "$R")"
  echo "INFO  source loudness $(echo "$R0" | jq '.integrated_lufs') LUFS"
  step "clip_normalize: -16 LUFS before the cuts" clip_normalize "{\"track\":$A,\"clip\":0,\"target_lufs\":-16,\"wait_ms\":20000}"
  if [ -n "$(echo "$R" | jq -r '.job_id // empty')" ]; then
    expect_eq "clip_normalize: the background apply succeeded" succeeded "$(wait_job "$(echo "$R" | jq '.job_id')" 120 | jq -r '.state')"
  fi
  expect_eq "clip_normalize: the gain filter is on the audio clip" 1 "$(tool_json filter_list_applied "{\"track\":$A,\"clip\":0}" | jq '[.filters[] | select(.id=="gain")] | length')"
  # jfk.wav has a noise floor above the guide's -35 dB (no pause found there): -25 dB for it.
  local NOISE=-35; [ "$2" = whisper ] && NOISE=-25
  step "detect_silence: $NOISE dB, 0.6 s" detect_silence "{\"track\":$A,\"clip\":0,\"noise_db\":$NOISE,\"min_s\":0.6,\"wait_ms\":20000}"
  SIL="$(job_result "$R")"
  CE="$(tool_json get_timeline '{}' | jq ".tracks[$A].clips[0] | .start + .duration")"
  RANGES="$(pad_ranges "$FPS" "$(awk -v f="$FPS" 'BEGIN{printf "%d", f * 0.15 + 0.5}')" 0 "$CE" "$(echo "$SIL" | jq -c '[.ranges[].timeline_frames[]]')")"
  echo "INFO  cut ranges $RANGES"
  if [ "$2" = supplied ]; then
    expect_eq "two pauses found and padded by 4 frames (25 fps)" '[{"start":55,"end":97},{"start":180,"end":209}]' "$RANGES"
  fi
  step "save_project before the cuts" save_project "$(jq -nc --arg p "$SAVED" '{path:$p}')"
  REMOVED=0
  if [ "$(echo "$RANGES" | jq length)" -gt 0 ]; then
    step "timeline_remove_ranges: both tracks in one call" timeline_remove_ranges "{\"tracks\":[0,$A],\"ranges\":$RANGES}"
    REMOVED="$(echo "$R" | jq '.removed_frames["0"]')"
    expect_eq "the same frames left both tracks" "$REMOVED" "$(echo "$R" | jq ".removed_frames[\"$A\"]")"
    expect_eq "picture and sound clips stay aligned" "$(clips_of 0)" "$(clips_of "$A")"
  else
    echo "INFO  no pause of 0.6 s: nothing to cut"
  fi
  call_tool get_overview '{"columns":4,"rows":2}' | jq -e '.result.content[0].type == "image"' >/dev/null \
    && pass "get_overview: a contact sheet" || fail "get_overview" "no image"
  caption_clips 0
  expect_eq "captions on every clip with words" true "$([ "$CAPCALLS" -ge 1 ] && echo true || echo false)"
  [ "$2" = supplied ] && expect_eq "captions: 1 + 2 + 2 of at most 7 words" 5 "$CAPS"
  echo "INFO  $CAPS captions in $CAPCALLS calls"
  tool_json save_project "$(jq -nc --arg p "$SAVED" '{path:$p}')" >/dev/null
  if [ "$2" = supplied ]; then
    expect_eq "the saved SRTs hold the 22 words" 22 "$(srt_words "$SAVED")"
  else
    expect_eq "the saved SRTs hold more than 15 words" true "$([ "$(srt_words "$SAVED")" -gt 15 ] && echo true || echo false)"
  fi
  expect_eq "export_video x264-medium" succeeded "$(export_wait "$OUT")"
  EXPECT="$(awk -v d="$SRCDUR" -v r="$REMOVED" -v f="$FPS" 'BEGIN{printf "%.3f", d - r / f}')"
  echo "INFO  export $(fdur "$OUT") s, expected $EXPECT s (source $SRCDUR s minus $REMOVED frames)"
  expect_eq "export duration = source minus the cut pauses (within 0.3 s)" true "$(near "$(fdur "$OUT")" "$EXPECT" 0.3)"
  step "audio_levels on the export" audio_levels "$(jq -nc --arg p "$OUT" '{path:$p,wait_ms:20000}')"
  EL="$(job_result "$R")"
  echo "INFO  export loudness $(echo "$EL" | jq '.integrated_lufs') LUFS"
  expect_eq "export loudness within 1.5 LU of -16" true "$(echo "$EL" | jq '(.integrated_lufs + 16) | fabs <= 1.5')"
}

short() {
  local SC="$MEDIA/scenes.mp4" OUT="$TMP/short.mp4" SIL RANGES REMOVED EXPECT TT
  step "project_set_format 9:16 on the empty project" project_set_format '{"preset":"9:16"}'
  expect_eq "1080x1920 at 30 fps" '[1080,1920,30]' "$(echo "$R" | jq -c '[.width,.height,.fps_num]')"
  step "playlist_add the landscape source" playlist_add "$(jq -nc --arg p "$SC" '{path:$p}')"
  TT="$(transcript 1.1 1.9 "Wait for it." 4.2 6.9 "Red, then green, then blue.")"
  step "transcribe: supply the transcript" transcribe "$(jq -nc --arg p "$SC" --argjson t "$TT" '{path:$p,transcript:$t}')"
  step "timeline_insert_range: 1 to 8 s of the playlist item" timeline_insert_range '{"playlist_index":0,"in_s":1,"out_s":8,"track":0,"position":0}'
  expect_eq "the clip is 210 frames" 210 "$(echo "$R" | jq '.inserted.duration')"
  call_tool get_frame '{"position":30,"max_width":320}' | jq -e '.result.content[0].type == "image"' >/dev/null \
    && pass "get_frame: see the subject" || fail "get_frame" "no image"
  step "clip_reframe fill, focus_x 0.4" clip_reframe '{"track":0,"clip":0,"mode":"fill","focus_x":0.4}'
  step "clip_normalize -14 LUFS" clip_normalize '{"track":0,"clip":0,"target_lufs":-14,"wait_ms":20000}'
  if [ -n "$(echo "$R" | jq -r '.job_id // empty')" ]; then
    expect_eq "clip_normalize: the background apply succeeded" succeeded "$(wait_job "$(echo "$R" | jq '.job_id')" 120 | jq -r '.state')"
  fi
  step "detect_silence: -35 dB, 0.4 s" detect_silence '{"track":0,"clip":0,"noise_db":-35,"min_s":0.4,"wait_ms":20000}'
  SIL="$(job_result "$R")"
  RANGES="$(pad_ranges 30 3 0 210 "$(echo "$SIL" | jq -c '[.ranges[].timeline_frames[]]')")"
  expect_eq "dead air: 2.05-4.03 s padded by 3 frames, and the tail from 7.04 s to the end" '[{"start":34,"end":88},{"start":184,"end":210}]' "$RANGES"
  step "timeline_remove_ranges" timeline_remove_ranges "{\"tracks\":[0],\"ranges\":$RANGES}"
  REMOVED="$(echo "$R" | jq '.removed_frames["0"]')"
  caption_clips 0 '"max_words":4,"style":{"size":120,"position":"middle"}'
  expect_eq "captions: 1 on the first clip, 2 on the second" 3 "$CAPS"
  step "track_add: a Hook track on top" track_add '{"type":"video","name":"Hook"}'
  expect_eq "the Hook track is index 0" 0 "$(echo "$R" | jq '.added.index')"
  step "title_add: the hook for 2 s" title_add '{"text":"Wait for the last line","track":0,"position":0,"duration":60,"size":110,"valign":"top","bg_color":"#99000000"}'
  call_tool get_overview '{}' | jq -e '.result.content[0].type == "image"' >/dev/null \
    && pass "get_overview: a contact sheet" || fail "get_overview" "no image"
  expect_eq "export_video x264-medium" succeeded "$(export_wait "$OUT")"
  expect_eq "the short is 1080x1920" 1080x1920 "$(vsize "$OUT")"
  EXPECT="$(awk -v r="$REMOVED" 'BEGIN{printf "%.3f", (210 - r) / 30}')"
  echo "INFO  export $(fdur "$OUT") s, expected $EXPECT s"
  expect_eq "export duration = 7 s minus the dead air (within 0.3 s)" true "$(near "$(fdur "$OUT")" "$EXPECT" 0.3)"
  expect_eq "reframed: no black bars (bottom corners red at 0.5 s)" "red red" "$(colour "$(rgb "$OUT" 0.5 4 1915)") $(colour "$(rgb "$OUT" 0.5 1075 1915)")"
  expect_eq "reframed: the green shot fills the frame at 2.5 s" "green green" "$(colour "$(rgb "$OUT" 2.5 4 1915)") $(colour "$(rgb "$OUT" 2.5 1075 4)")"
  # The hook bar (110 px text, top) is there for 2 s; the 120 px captions sit in the middle.
  expect_eq "hook title: white text in the top band at 0.5 s" true "$([ "$(white_px "$OUT" 0.5 0 0 1080 200)" -gt 2000 ] && echo true || echo false)"
  expect_eq "hook title: gone at 2.5 s (no white in the top band)" 0 "$(white_px "$OUT" 2.5 0 0 1080 200)"
  expect_eq "captions: white text in the middle band at 2.5 s" true "$([ "$(white_px "$OUT" 2.5 0 760 1080 400)" -gt 1000 ] && echo true || echo false)"
  EL="$(job_result "$(tool_json audio_levels "$(jq -nc --arg p "$OUT" '{path:$p,wait_ms:20000}')")")"
  echo "INFO  export loudness $(echo "$EL" | jq '.integrated_lufs') LUFS"
  expect_eq "export loudness within 1.5 LU of -14" true "$(echo "$EL" | jq '(.integrated_lufs + 14) | fabs <= 1.5')"
}

tutorial() {
  local SCREEN="$MEDIA/scenes.mp4" FACE="$MEDIA/tone_av.mp4" OUT="$TMP/tutorial.mp4" SIL RANGES
  local REMOVED EXPECT TT CE
  step "project_set_format 16:9" project_set_format '{"preset":"16:9"}'
  step "timeline_insert_range: the screen recording" timeline_insert_range "$(jq -nc --arg p "$SCREEN" '{path:$p,in_s:0,out_s:9,track:0,position:0}')"
  step "track_add: a Face track on top" track_add '{"type":"video","name":"Face"}'
  expect_eq "the Face track is index 0" 0 "$(echo "$R" | jq '.added.index')"
  step "timeline_insert_range: the camera, mode overwrite" timeline_insert_range "$(jq -nc --arg p "$FACE" '{path:$p,in_s:0,out_s:9,track:0,position:0,mode:"overwrite"}')"
  expect_eq "both tracks last 270 frames" '[[[0,270]],[[0,270]]]' "$(jq -nc --argjson a "$(clips_of 0)" --argjson b "$(clips_of 1)" '[$a,$b]')"
  # The guide's corner (y 378) is under the lower third: its note says to move the face up.
  step "clip_set_transform: the face a quarter size, top-right (the guide's note)" clip_set_transform '{"track":0,"clip":0,"scale":0.25,"x":672,"y":-378}'
  TT="$(transcript 0.2 1.8 "Welcome to this tutorial." 4.3 6.8 "First we open the project panel.")"
  step "transcribe: supply the narration" transcribe "$(jq -nc --arg p "$FACE" --argjson t "$TT" '{path:$p,transcript:$t}')"
  step "clip_normalize the narration" clip_normalize '{"track":0,"clip":0,"target_lufs":-16,"wait_ms":20000}'
  if [ -n "$(echo "$R" | jq -r '.job_id // empty')" ]; then
    expect_eq "clip_normalize: the background apply succeeded" succeeded "$(wait_job "$(echo "$R" | jq '.job_id')" 120 | jq -r '.state')"
  fi
  step "clip_set_volume: the screen's own sound down" clip_set_volume '{"track":1,"clip":0,"gain_db":-30}'
  step "detect_silence: -35 dB, 0.8 s" detect_silence '{"track":0,"clip":0,"noise_db":-35,"min_s":0.8,"wait_ms":20000}'
  SIL="$(job_result "$R")"
  RANGES="$(pad_ranges 30 5 0 270 "$(echo "$SIL" | jq -c '[.ranges[].timeline_frames[]]')")"
  expect_eq "two pauses padded by 5 frames" 2 "$(echo "$RANGES" | jq length)"
  step "timeline_remove_ranges: both tracks in one call" timeline_remove_ranges "{\"tracks\":[0,1],\"ranges\":$RANGES}"
  REMOVED="$(echo "$R" | jq '.removed_frames["0"]')"
  expect_eq "face and screen stay in sync" "$(clips_of 0)" "$(clips_of 1)"
  step "track_add: a Titles track on top" track_add '{"type":"video","name":"Titles"}'
  step "lower_third_add: chapter 1" lower_third_add '{"line1":"Step 1","line2":"Install the tools","track":0,"position":0,"duration":45}'
  CE="$(tool_json get_timeline '{}' | jq '.tracks[1].clips[1].start')"
  step "lower_third_add: chapter 2" lower_third_add "{\"line1\":\"Step 2\",\"line2\":\"Open the panel\",\"track\":0,\"position\":$CE,\"duration\":45}"
  step "marker_add: chapter 1" marker_add '{"start":0,"text":"Step 1: Install the tools"}'
  step "marker_add: chapter 2" marker_add "{\"start\":$CE,\"text\":\"Step 2: Open the panel\"}"
  expect_eq "marker_list: the two chapters in order" "[[0,\"Step 1: Install the tools\"],[$CE,\"Step 2: Open the panel\"]]" "$(tool_json marker_list '{}' | jq -c '[.markers[] | [.start,.text]]')"
  caption_clips 1
  expect_eq "captions on the narration clips" true "$([ "$CAPS" -ge 2 ] && echo true || echo false)"
  expect_eq "export_video x264-medium" succeeded "$(export_wait "$OUT")"
  expect_eq "the tutorial is 1920x1080" 1920x1080 "$(vsize "$OUT")"
  EXPECT="$(awk -v r="$REMOVED" 'BEGIN{printf "%.3f", (270 - r) / 30}')"
  echo "INFO  export $(fdur "$OUT") s, expected $EXPECT s"
  expect_eq "export duration = 9 s minus the pauses (within 0.3 s)" true "$(near "$(fdur "$OUT")" "$EXPECT" 0.3)"
  # At 1.0 s: the screen (red) fills the frame, the face (testsrc) is a 360x270 picture centred at
  # (1632, 162), the lower third is the band 76%-90% (y 821-972).
  local C F
  C="$(rgb "$OUT" 1.0 960 540)"; F="$(rgb "$OUT" 1.0 1632 162)"
  echo "INFO  centre $C, face corner $F"
  expect_eq "the screen layer: the centre is red" red "$(colour "$C")"
  expect_eq "the face layer: the corner is not the screen's red" true "$([ "$(colour "$F")" != red ] && echo true || echo false)"
  expect_eq "outside the face the corner area is the screen (red)" red "$(colour "$(rgb "$OUT" 1.0 1400 162)")"
  expect_eq "the lower third: white text in its band at 1.0 s" true "$([ "$(white_px "$OUT" 1.0 0 821 960 151)" -gt 500 ] && echo true || echo false)"
}

montage() {
  local SC="$MEDIA/scenes.mp4" MUSIC="$MEDIA/audioonly.wav" OUT="$TMP/montage.mp4" SC0 DUR
  step "analyze_media" analyze_media "$(jq -nc --arg p "$SC" '{path:$p}')"
  step "detect_scenes: 0.3" detect_scenes "$(jq -nc --arg p "$SC" '{path:$p,threshold:0.3,wait_ms:20000}')"
  SC0="$(job_result "$R")"
  expect_eq "the cuts at 3 s and 6 s" '[3,6]' "$(echo "$SC0" | jq -c '[.scenes[].time_s]')"
  step "detect_silence on the music" detect_silence "$(jq -nc --arg p "$MUSIC" '{path:$p,noise_db:-30,min_s:0.2,wait_ms:20000}')"
  # Three 2 s shots from inside the scenes; the green one is the accent: its 1 s is inserted and
  # slowed to 0.5 BEFORE the next shot is placed.
  step "shot 1: red 0.5-2.5 s at 0" timeline_insert_range "$(jq -nc --arg p "$SC" '{path:$p,in_s:0.5,out_s:2.5,track:0,position:0}')"
  step "shot 2: green 3.5-4.5 s at 50" timeline_insert_range "$(jq -nc --arg p "$SC" '{path:$p,in_s:3.5,out_s:4.5,track:0,position:50}')"
  step "clip_set_speed 0.5 on the accent" clip_set_speed '{"track":0,"clip":1,"speed":0.5}'
  # Shotcut's speed formula gives 2 x 25 - 1 frames (out = round(24 x 2)), so the next shot goes
  # at the accent's real end, read from the result, not at 100.
  local ACC; ACC="$(echo "$R" | jq '.duration')"
  expect_eq "the accent now lasts 49 frames (25 x 2 - 1)" 49 "$ACC"
  step "shot 3: blue 6.5-8.5 s at the accent's end" timeline_insert_range "$(jq -nc --arg p "$SC" --argjson s $((50 + ACC)) '{path:$p,in_s:6.5,out_s:8.5,track:0,position:$s}')"
  expect_eq "three touching shots" '[[0,50],[50,49],[99,50]]' "$(clips_of 0)"
  local c out
  for c in 0 1 2; do
    out="$(call_tool clip_set_color "{\"track\":0,\"clip\":$c,\"saturation\":1.15,\"temperature_k\":6000,\"contrast\":0.05}")"
    if ok "$out"; then pass "clip_set_color: the guide's look on shot $c"
    elif echo "$out" | jq -r '.result.content[0].text' | grep -q 'frei0r'; then
      echo "INFO  no frei0r: contrast only ($(echo "$out" | jq -r '.result.content[0].text'))"
      step "clip_set_color: contrast only on shot $c" clip_set_color "{\"track\":0,\"clip\":$c,\"contrast\":0.05}"
    else fail "clip_set_color shot $c" "$(echo "$out" | jq -r '.result.content[0].text // .')"; fi
  done
  # From the first clip forwards (a clip with a transition after it takes none before it); each
  # transition is a new item, so shot 3 is item 3 for the second one, and each shortens the
  # timeline by its duration.
  step "transition into shot 2 (10 frames)" timeline_add_transition '{"track":0,"clip":1,"duration":10}'
  step "transition into shot 3 (item 3 now, 10 frames)" timeline_add_transition '{"track":0,"clip":3,"duration":10}'
  DUR="$(tool_json get_timeline '{}' | jq '.duration')"
  expect_eq "149 frames minus two 10-frame transitions" 129 "$DUR"
  step "track_add: a Music track" track_add '{"type":"audio","name":"Music"}'
  local MT; MT="$(echo "$R" | jq '.added.index')"
  step "timeline_insert_range: the music, mode overwrite" timeline_insert_range "$(jq -nc --arg p "$MUSIC" --argjson t "$MT" '{path:$p,in_s:0,out_s:5.16,track:$t,position:0,mode:"overwrite"}')"
  expect_eq "the music did not push the shots (shot 3 still ends at 129)" 129 "$(tool_json get_timeline '{}' | jq '.tracks[0].clips[-1] | .start + .duration')"
  out="$(call_tool track_set_ducking "{\"track\":$MT,\"threshold_db\":-12,\"reduction_db\":-12}")"
  if ok "$out"; then pass "track_set_ducking on the music"
  elif echo "$out" | jq -r '.result.content[0].text' | grep -q '7\.41'; then
    echo "INFO  ducking refused on this MLT, as the guide's note says: $(echo "$out" | jq -r '.result.content[0].text')"
  else fail "track_set_ducking" "$(echo "$out" | jq -r '.result.content[0].text // .')"; fi
  call_tool get_overview '{}' | jq -e '.result.content[0].type == "image"' >/dev/null \
    && pass "get_overview: a contact sheet" || fail "get_overview" "no image"
  expect_eq "export_video x264-medium" succeeded "$(export_wait "$OUT")"
  echo "INFO  export $(fdur "$OUT") s, expected 5.16 s (129 frames at 25 fps)"
  expect_eq "export duration 129 frames = 5.16 s (within 0.1 s)" true "$(near "$(fdur "$OUT")" 5.16 0.1)"
  expect_eq "export has 129 video frames" 129 "$(ffprobe -v error -count_frames -select_streams v:0 -show_entries stream=nb_read_frames -of csv=p=0 "$OUT" 2>/dev/null)"
  # Shot 1 to 40, dissolve 40-50, the accent alone 50-79, dissolve 79-89, shot 3 89-129.
  expect_eq "shot 1 is red (0.8 s)" red "$(colour "$(rgb "$OUT" 0.8 160 120)")"
  expect_eq "the accent is green (2.6 s)" green "$(colour "$(rgb "$OUT" 2.6 160 120)")"
  expect_eq "shot 3 is blue (4.4 s)" blue "$(colour "$(rgb "$OUT" 4.4 160 120)")"
  expect_eq "mid-dissolve into the accent is a mix (1.8 s)" other "$(colour "$(rgb "$OUT" 1.8 160 120)")"
  expect_eq "the export has the music" true "$(ffprobe -v error -select_streams a:0 -show_entries stream=codec_name -of csv=p=0 "$OUT" | grep -q aac && echo true || echo false)"
}

case "$CASE" in
  interview) interview "$MEDIA/tone_av.mp4" supplied ;;
  interview-whisper)
    if [ -z "${WHISPER_EXE:-}" ]; then echo "SKIP: WHISPER_EXE is not set"; exit 2; fi
    JFK_SRC="${JFK_WAV:-$HOME/GitHub/whisper.cpp/samples/jfk.wav}"
    [ -f "$JFK_SRC" ] || { echo "SKIP: $JFK_SRC is missing (set JFK_WAV)"; exit 2; }
    ffmpeg -loglevel error -y -f lavfi -i "testsrc=size=320x240:rate=25:duration=11" -i "$JFK_SRC" \
      -pix_fmt yuv420p -c:v libx264 -c:a aac -shortest "$MEDIA/jfk_av.mp4"
    interview "$MEDIA/jfk_av.mp4" whisper ;;
  short) short ;;
  tutorial) tutorial ;;
  montage) montage ;;
esac

# FOOTER: stage 3 is complete (58 tools); new use cases go above this line.
if [ "$failures" -eq 0 ]; then echo "All checks passed."; else echo "${failures} check(s) failed."; exit 1; fi
