#!/usr/bin/env bash
# A stand-in for whisper-cli used by tools/mcp-smoke-media.sh to test transcribe's parser without
# a real model: it ignores the audio and writes a canned transcript to <--output-file>.json.
# Configure it as the Whisper program of a throwaway Shotcut (any existing file as the model).
# The canned transcript has Unicode punctuation tokens (an ellipsis and an em dash) that must not
# stretch a word's time, and a segment after the end of any short file (it is clamped to nothing).
out=""
while [ $# -gt 0 ]; do
  case "$1" in
    --output-file|-of) out="$2"; shift ;;
  esac
  shift
done
[ -n "$out" ] || { echo "error: no --output-file" >&2; exit 1; }
cat > "$out.json" <<'JSON'
{
  "result": {"language": "en"},
  "transcription": [
    {
      "offsets": {"from": 0, "to": 4000},
      "text": " Hello… world—",
      "tokens": [
        {"text": "[_BEG_]", "offsets": {"from": 0, "to": 0}},
        {"text": " Hello", "offsets": {"from": 0, "to": 500}},
        {"text": "…", "offsets": {"from": 500, "to": 3000}},
        {"text": " world", "offsets": {"from": 3000, "to": 3500}},
        {"text": "—", "offsets": {"from": 3500, "to": 4000}}
      ]
    },
    {
      "offsets": {"from": 86000000, "to": 86001000},
      "text": " Beyond the end",
      "tokens": [
        {"text": " Beyond", "offsets": {"from": 86000000, "to": 86000500}},
        {"text": " the", "offsets": {"from": 86000500, "to": 86000700}},
        {"text": " end", "offsets": {"from": 86000700, "to": 86001000}}
      ]
    }
  ]
}
JSON
echo "fake whisper: wrote $out.json" >&2
