#!/usr/bin/env bash
# Creates the stage 3 media fixtures in DIR (created if missing) with ffmpeg lavfi sources.
# Usage: tools/mcp-fixtures.sh DIR
# silence.wav   16 kHz mono 10 s; 440 Hz at -20 dBFS 0-2, 4-7, 8.5-10 s; silent 2-4 and 7-8.5 s
# scenes.mp4    320x240 25 fps 9 s; red 0-3 s, green 3-6 s, blue 6-9 s; silence.wav audio
# tone_av.mp4   10 s video plus silence.wav audio
# noaudio.mp4   4 s video only
# audioonly.wav the tone only (same as silence.wav)
# "space name.mp4" copy of scenes.mp4
set -eu
DIR="${1:?usage: mcp-fixtures.sh DIR}"
mkdir -p "$DIR"
DIR="$(cd "$DIR" && pwd)"
ff() { ffmpeg -loglevel error -y "$@"; }

# lavfi sine peaks at 1/8 (-18 dBFS); -2 dB gives a -20 dBFS peak
ff -f lavfi -i "sine=frequency=440:sample_rate=16000:duration=10" -ac 1 \
  -af "volume=enable='between(t,2,4)+between(t,7,8.5)':volume=0,volume=-2dB" \
  -c:a pcm_s16le "$DIR/silence.wav"
cp "$DIR/silence.wav" "$DIR/audioonly.wav"

# green is 0x00ff00: CSS green has almost the luma of red, and the scene filter only sees luma
ff -f lavfi -i "color=c=red:size=320x240:rate=25:duration=3" \
   -f lavfi -i "color=c=0x00ff00:size=320x240:rate=25:duration=3" \
   -f lavfi -i "color=c=blue:size=320x240:rate=25:duration=3" -i "$DIR/silence.wav" \
  -filter_complex "[0:v][1:v][2:v]concat=n=3:v=1:a=0,format=yuv420p[v]" -map "[v]" -map 3:a \
  -c:v libx264 -c:a aac -ar 16000 -shortest "$DIR/scenes.mp4"

ff -f lavfi -i "testsrc=size=320x240:rate=25:duration=10" -i "$DIR/silence.wav" \
  -pix_fmt yuv420p -c:v libx264 -c:a aac -shortest "$DIR/tone_av.mp4"

ff -f lavfi -i "smptebars=size=320x240:rate=25:duration=4" -pix_fmt yuv420p -c:v libx264 -an \
  "$DIR/noaudio.mp4"

cp "$DIR/scenes.mp4" "$DIR/space name.mp4"
echo "$DIR"
