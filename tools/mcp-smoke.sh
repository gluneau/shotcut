#!/usr/bin/env bash
# Smoke test for the Shotcut MCP server.
# Usage: MCP_TOKEN=<token> [MCP_PORT=8765] tools/mcp-smoke.sh
# Needs: bash, curl, jq (and ffmpeg for the media checks). Shotcut must be running
# with Settings > MCP Server > Enable checked. The slow-request check takes ~12 s.
# WARNING: this opens and replaces whatever is loaded in the target Shotcut. Run it against a
# throwaway session (see docs/mcp-server.md, Testing). It exits with status 2 if the project has
# unsaved changes, unless MCP_SMOKE_FORCE=1 is set.
set -u

PORT="${MCP_PORT:-8765}"
TOKEN="${MCP_TOKEN:?set MCP_TOKEN to the token from Settings > MCP Server}"
URL="http://127.0.0.1:${PORT}/mcp"
AUTH=(-H "Authorization: Bearer ${TOKEN}")
JSON=(-H 'Content-Type: application/json')
failures=0

pass() { echo "PASS  $1"; }
fail() { echo "FAIL  $1 -- $2"; failures=$((failures + 1)); }
expect_eq() { if [ "$2" = "$3" ]; then pass "$1"; else fail "$1" "expected '$2', got '$3'"; fi; }

# Prints the HTTP status code of a curl call.
http_status() { curl -s --max-time 5 -o /dev/null -w '%{http_code}' "$@"; }

# Authenticated JSON-RPC POST; prints the response body.
rpc() { curl -s --max-time 10 -X POST "$URL" "${AUTH[@]}" "${JSON[@]}" -d "$1"; }

# call_tool NAME ARGS_JSON; prints the full JSON-RPC response.
call_tool() {
  rpc "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"$1\",\"arguments\":$2}}"
}
# tool_is_error NAME ARGS_JSON; prints true/false.
tool_is_error() { call_tool "$1" "$2" | jq -r '.result.isError'; }
# tool_json NAME ARGS_JSON; prints the tool's JSON payload (from the text content).
tool_json() { call_tool "$1" "$2" | jq -c '.result.content[0].text | fromjson'; }

# raw 'REQUEST TEXT'; sends it over a plain socket and prints the first response line.
raw() {
  exec 3<>"/dev/tcp/127.0.0.1/${PORT}" || return 1
  printf '%b' "$1" >&3
  timeout 14 head -n 1 <&3 | tr -d '\r'
  exec 3<&- 3>&-
}

if [ "$(tool_json get_state '{}' | jq -r '.modified')" = true ] && [ "${MCP_SMOKE_FORCE:-0}" != 1 ]; then
  echo "Refusing to run: the project in Shotcut has unsaved changes and this test replaces it."
  echo "Use a throwaway Shotcut session, or set MCP_SMOKE_FORCE=1 to run anyway."
  exit 2
fi

echo "== HTTP checks"
expect_eq "no token -> 401" 401 "$(http_status -X POST "$URL" "${JSON[@]}" -d '{}')"
expect_eq "wrong token -> 401" 401 "$(http_status -X POST "$URL" -H 'Authorization: Bearer nope' "${JSON[@]}" -d '{}')"
expect_eq "Origin header -> 403" 403 "$(http_status -X POST "$URL" "${AUTH[@]}" "${JSON[@]}" -H 'Origin: http://evil.example' -d '{}')"
expect_eq "foreign Host -> 403" 403 "$(http_status -X POST "$URL" "${AUTH[@]}" "${JSON[@]}" -H 'Host: evil.example' -d '{}')"
expect_eq "GET -> 405" 405 "$(http_status "$URL" "${AUTH[@]}")"
expect_eq "unknown path -> 404" 404 "$(http_status -X POST "http://127.0.0.1:${PORT}/other" "${AUTH[@]}" "${JSON[@]}" -d '{}')"
expect_eq "wrong content type -> 415" 415 "$(http_status -X POST "$URL" "${AUTH[@]}" -H 'Content-Type: text/plain' -d '{}')"
expect_eq "chunked body -> 411" 411 "$(http_status -X POST "$URL" "${AUTH[@]}" "${JSON[@]}" -H 'Transfer-Encoding: chunked' -d '{}')"
expect_eq "declared oversized body -> 413" 413 "$(http_status -X POST "$URL" "${AUTH[@]}" "${JSON[@]}" -H 'Content-Length: 1100000' -d '{}')"
expect_eq "oversized headers -> 431" 431 "$(http_status -X POST "$URL" "${AUTH[@]}" "${JSON[@]}" -H "X-Pad: $(head -c 9000 /dev/zero | tr '\0' a)" -d '{}')"
BASE="POST /mcp HTTP/1.1\r\nHost: 127.0.0.1:${PORT}\r\nAuthorization: Bearer ${TOKEN}\r\nContent-Type: application/json\r\n"
expect_eq "missing Content-Length -> 411" "HTTP/1.1 411 Length Required" "$(raw "${BASE}\r\n")"
expect_eq "non-numeric Content-Length -> 400" "HTTP/1.1 400 Bad Request" "$(raw "${BASE}Content-Length: abc\r\n\r\n")"
expect_eq "garbage request line -> 400" "HTTP/1.1 400 Bad Request" "$(raw 'NOT HTTP\r\n\r\n')"
expect_eq "HTTP/1.0 -> 505" "HTTP/1.1 505 HTTP Version Not Supported" "$(raw 'POST /mcp HTTP/1.0\r\nHost: 127.0.0.1:'"${PORT}"'\r\n\r\n')"
echo "(waiting ~12 s for the slow-request check)"
expect_eq "stalled request -> 408" "HTTP/1.1 408 Request Timeout" "$(raw "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1:${PORT}\r\n")"
# Hang up right after sending a full request, then make sure the server still answers.
( exec 4<>"/dev/tcp/127.0.0.1/${PORT}"; printf '%b' "${BASE}Content-Length: 40\r\n\r\n{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"ping\"}" >&4; exec 4<&- 4>&- )
expect_eq "server survives a client hang-up" "{}" "$(rpc '{"jsonrpc":"2.0","id":2,"method":"ping"}' | jq -c '.result')"

echo "== JSON-RPC checks"
INIT='{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"VERSION","capabilities":{},"clientInfo":{"name":"smoke","version":"0"}}}'
expect_eq "initialize echoes a supported version" 2025-03-26 "$(rpc "${INIT/VERSION/2025-03-26}" | jq -r '.result.protocolVersion')"
expect_eq "initialize replaces an unknown version" true "$(rpc "${INIT/VERSION/1999-01-01}" | jq -r '.result.protocolVersion | . != null and . != "1999-01-01"')"
expect_eq "initialize advertises tools" object "$(rpc "${INIT/VERSION/2025-06-18}" | jq -r '.result.capabilities.tools | type')"
expect_eq "initialized notification -> 202" 202 "$(http_status -X POST "$URL" "${AUTH[@]}" "${JSON[@]}" -d '{"jsonrpc":"2.0","method":"notifications/initialized"}')"
expect_eq "ping -> {}" "{}" "$(rpc '{"jsonrpc":"2.0","id":2,"method":"ping"}' | jq -c '.result')"
expect_eq "tools/list returns an array" array "$(rpc '{"jsonrpc":"2.0","id":3,"method":"tools/list"}' | jq -r '.result.tools | type')"
expect_eq "unknown method -> -32601" -32601 "$(rpc '{"jsonrpc":"2.0","id":4,"method":"nope"}' | jq -r '.error.code')"
expect_eq "batch -> -32600" -32600 "$(rpc '[{"jsonrpc":"2.0","id":5,"method":"ping"}]' | jq -r '.error.code')"
expect_eq "invalid JSON -> -32700" -32700 "$(rpc '{bad' | jq -r '.error.code')"
expect_eq "unknown tool -> -32602" -32602 "$(call_tool nope '{}' | jq -r '.error.code')"
expect_eq "non-object arguments -> -32602" -32602 "$(rpc '{"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"x","arguments":5}}' | jq -r '.error.code')"

echo "== project tools"
# Polls get_state (up to ~5 s) until the jq field equals the expected value; prints the last value.
wait_state() {
  local field="$1" want="$2" got="" i
  for i in $(seq 1 25); do
    got="$(tool_json get_state '{}' | jq -r "$field")"
    [ "$got" = "$want" ] && break
    sleep 0.2
  done
  echo "$got"
}
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
HAVE_CLIP=0
if command -v ffmpeg >/dev/null 2>&1 \
   && ffmpeg -loglevel error -f lavfi -i testsrc=duration=4:size=320x240:rate=25 -pix_fmt yuv420p "$TMP/clip.mp4"; then
  HAVE_CLIP=1
fi

expect_eq "tools/list has get_state" true "$(rpc '{"jsonrpc":"2.0","id":3,"method":"tools/list"}' | jq -r '[.result.tools[].name] | contains(["get_state","open_project","save_project"])')"
expect_eq "get_state.loaded is a boolean" boolean "$(tool_json get_state '{}' | jq -r '.loaded | type')"
expect_eq "get_state.modified is a boolean" boolean "$(tool_json get_state '{}' | jq -r '.modified | type')"

expect_eq "open_project: relative path is an error" true "$(tool_is_error open_project '{"path":"clip.mp4"}')"
expect_eq "open_project: missing file is an error" true "$(tool_is_error open_project "{\"path\":\"$TMP/missing.mp4\"}")"
expect_eq "open_project: directory is an error" true "$(tool_is_error open_project "{\"path\":\"$TMP\"}")"
expect_eq "open_project: no path is an error" true "$(tool_is_error open_project '{}')"
touch "$TMP/x.mp4"
expect_eq "open_project: resource path is an error" true "$(tool_is_error open_project '{"path":":/x.mlt"}')"
expect_eq "save_project: resource path is an error" true "$(tool_is_error save_project '{"path":":/x.mlt"}')"
expect_eq "open_project: non-boolean discard_changes is an error" true "$(tool_is_error open_project "{\"path\":\"$TMP/x.mp4\",\"discard_changes\":\"yes\"}")"

expect_eq "save_project: relative path is an error" true "$(tool_is_error save_project '{"path":"copy.mlt"}')"
expect_eq "save_project: non-.mlt path is an error" true "$(tool_is_error save_project "{\"path\":\"$TMP/copy.txt\"}")"
expect_eq "save_project: directory is an error" true "$(tool_is_error save_project "{\"path\":\"$TMP\"}")"
expect_eq "save_project: unwritable folder is an error" true "$(tool_is_error save_project '{"path":"/proc/copy.mlt"}')"
expect_eq "save_project: numeric path is an error" true "$(tool_is_error save_project '{"path":5}')"

if [ "$HAVE_CLIP" = 1 ]; then
  expect_eq "open_project: opens a media file" false "$(tool_is_error open_project "{\"path\":\"$TMP/clip.mp4\"}")"
  sleep 1.5
  expect_eq "get_state: loaded after open" true "$(tool_json get_state '{}' | jq -r '.loaded')"
  expect_eq "get_state: duration is 100 frames" 100 "$(tool_json get_state '{}' | jq -r '.duration')"
  expect_eq "save_project: writes a copy" false "$(tool_is_error save_project "{\"path\":\"$TMP/copy.mlt\"}")"
  expect_eq "save_project: the copy is a non-empty file" true "$([ -s "$TMP/copy.mlt" ] && echo true || echo false)"
  BEFORE="$(tool_json get_state '{}' | jq -c '[.project,.loaded]')"
  printf '<mlt' > "$TMP/broken.mlt"
  expect_eq "open_project: invalid .mlt is an error" true "$(tool_is_error open_project "{\"path\":\"$TMP/broken.mlt\"}")"
  expect_eq "open_project: invalid .mlt leaves state unchanged" "$BEFORE" "$(tool_json get_state '{}' | jq -c '[.project,.loaded]')"
  BEFORE="$(tool_json get_state '{}' | jq -c '[.project,.loaded,.modified]')"
  expect_eq "open_project: invalid .mlt with discard_changes is an error" true "$(tool_is_error open_project "{\"path\":\"$TMP/broken.mlt\",\"discard_changes\":true}")"
  expect_eq "open_project: invalid .mlt with discard_changes leaves state unchanged" "$BEFORE" "$(tool_json get_state '{}' | jq -c '[.project,.loaded,.modified]')"
  REALCOPY="$(realpath "$TMP/copy.mlt")"
  expect_eq "open_project: opens a saved .mlt project" false "$(tool_is_error open_project "{\"path\":\"$TMP/copy.mlt\"}")"
  expect_eq "get_state: project is the opened .mlt" "$REALCOPY" "$(wait_state .project "$REALCOPY")"
  expect_eq "open_project: opens the same .mlt again" false "$(tool_is_error open_project "{\"path\":\"$TMP/copy.mlt\"}")"
  expect_eq "get_state: project is still the .mlt" "$REALCOPY" "$(wait_state .project "$REALCOPY")"
  tool_is_error open_project "{\"path\":\"$TMP/clip.mp4\"}" >/dev/null
  sleep 1.5
  cp "$TMP/copy.mlt" "$TMP/ro.mlt"; chmod a-w "$TMP/ro.mlt"
  if [ -w "$TMP/ro.mlt" ]; then
    echo "SKIP  read-only save check (file still writable, running as root?)"
  else
    expect_eq "save_project: read-only existing file is an error" true "$(tool_is_error save_project "{\"path\":\"$TMP/ro.mlt\"}")"
  fi
else
  echo "SKIP  media checks (ffmpeg not available)"
fi

echo "== transport tools"
if [ "$HAVE_CLIP" = 1 ]; then
  tool_is_error open_project "{\"path\":\"$TMP/clip.mp4\",\"discard_changes\":true}" >/dev/null
  sleep 1.5
  tool_is_error play '{}' >/dev/null
  expect_eq "play before pause: playing" true "$(wait_state .playing true)"
  expect_eq "pause: ok" false "$(tool_is_error pause '{}')"
  expect_eq "pause: not playing" false "$(wait_state .playing false)"

  expect_eq "seek 50: ok" false "$(tool_is_error seek '{"position":50}')"
  expect_eq "seek 50: position is 50" 50 "$(wait_state .position 50)"

  expect_eq "seek: missing position is an error" true "$(tool_is_error seek '{}')"
  expect_eq "seek: string position is an error" true "$(tool_is_error seek '{"position":"abc"}')"
  expect_eq "seek: float position is an error" true "$(tool_is_error seek '{"position":1.5}')"
  expect_eq "seek: negative position is an error" true "$(tool_is_error seek '{"position":-1}')"
  expect_eq "pause: negative position is an error" true "$(tool_is_error pause '{"position":-5}')"
  expect_eq "play: zero speed is an error" true "$(tool_is_error play '{"speed":0}')"
  expect_eq "play: string speed is an error" true "$(tool_is_error play '{"speed":"fast"}')"

  tool_is_error seek '{"position":0}' >/dev/null
  wait_state .position 0 >/dev/null
  expect_eq "play: ok" false "$(tool_is_error play '{}')"
  expect_eq "play: playing" true "$(wait_state .playing true)"
  tool_is_error pause '{"position":10}' >/dev/null
  expect_eq "pause with position: stopped at 10" 10 "$(wait_state .position 10)"
  expect_eq "pause with position while paused: ok" false "$(tool_is_error pause '{"position":30}')"
  expect_eq "pause with position while paused: moved to 30" 30 "$(wait_state .position 30)"
  expect_eq "seek past the end is clamped to the last frame" 99 "$(tool_json seek '{"position":99999}' | jq -r '.position')"
else
  echo "SKIP  transport checks (no clip loaded)"
fi

echo "== action tools"
expect_eq "list_actions includes playerPlayPauseAction" true "$(tool_json list_actions '{}' | jq -r '[.actions[].key] | contains(["playerPlayPauseAction"])')"
expect_eq "list_actions excludes dialog-opening actions" false "$(tool_json list_actions '{}' | jq -r '[.actions[].key] | contains(["playerSetPositionAction"])')"
expect_eq "trigger_action: non-allowlisted key is an error" true "$(tool_is_error trigger_action '{"key":"playerSetPositionAction"}')"
expect_eq "trigger_action: unknown key is an error" true "$(tool_is_error trigger_action '{"key":"nope"}')"
expect_eq "trigger_action: missing key is an error" true "$(tool_is_error trigger_action '{}')"
expect_eq "trigger_action: non-boolean checked is an error" true "$(tool_is_error trigger_action '{"key":"playerLoopAction","checked":"yes"}')"
if [ "$HAVE_CLIP" = 1 ]; then
  tool_is_error open_project "{\"path\":\"$TMP/clip.mp4\",\"discard_changes\":true}" >/dev/null
  wait_state .loaded true >/dev/null
  tool_is_error pause '{"position":20}' >/dev/null
  wait_state .position 20 >/dev/null
  expect_eq "trigger_action: next frame is ok" false "$(tool_is_error trigger_action '{"key":"playerNextFrameAction"}')"
  expect_eq "trigger_action: moved one frame" 21 "$(wait_state .position 21)"
fi

# MANUAL CHECKS (need a modified project in the GUI; a modified project cannot be made through the tools)
#  1. Edit a loaded project (the title shows the modified marker), then call open_project with
#     discard_changes true on a broken .mlt. It must return an error, the project must stay
#     modified, and its autosave must keep working (the file under <appdata>/autosave still
#     exists and is updated).
#  2. Open a media file into a loaded timeline: no discard is needed and the modified flag is unchanged.


if [ "$failures" -eq 0 ]; then echo "All checks passed."; else echo "${failures} check(s) failed."; exit 1; fi
