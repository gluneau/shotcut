#!/usr/bin/env bash
# Shared helpers for the stage 3 MCP smoke scripts. Source it; do not run it.
# Env: MCP_TOKEN (required), MCP_PORT (default 7468), MCP_APPDATA (the --appdata dir of the
# Shotcut under test), MCP_SMOKE_FORCE=1 (skip the fresh-session guard).
# Needs: bash, curl, jq.

PORT="${MCP_PORT:-7468}"
TOKEN="${MCP_TOKEN:?set MCP_TOKEN to the token from Settings > MCP Server}"
URL="http://127.0.0.1:${PORT}/mcp"
AUTH=(-H "Authorization: Bearer ${TOKEN}")
JSON=(-H 'Content-Type: application/json')
failures=0

pass() { echo "PASS  $1"; }
fail() { echo "FAIL  $1 -- $2"; failures=$((failures + 1)); }
expect_eq() { if [ "$2" = "$3" ]; then pass "$1"; else fail "$1" "expected '$2', got '$3'"; fi; }

rpc() { curl -s --max-time 60 -X POST "$URL" "${AUTH[@]}" "${JSON[@]}" -d "$1"; }
call_tool() {
  rpc "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"$1\",\"arguments\":$2}}"
}
tool_is_error() { call_tool "$1" "$2" | jq -r '.result.isError'; }
# Payload of a tool that returns JSON text.
tool_json() { call_tool "$1" "$2" | jq -c '.result.content[0].text | fromjson'; }
# Text of a tool call (the error message of a failing call).
tool_text() { call_tool "$1" "$2" | jq -r '.result.content[0].text'; }

# wait_state TOOL ARGS JQ_FILTER WANT: poll up to 10 s until the filter equals WANT; prints the last value.
wait_state() {
  local value=""
  for _ in $(seq 1 50); do
    value="$(tool_json "$1" "$2" | jq -r "$3")"
    [ "$value" = "$4" ] && break
    sleep 0.2
  done
  echo "$value"
}

# wait_job ID [SECONDS]: poll get_job until state is not "running" (default 60 s); prints the final JSON.
wait_job() {
  local out="" i
  for i in $(seq 1 $(( ${2:-60} * 5 ))); do
    out="$(tool_json get_job "{\"job_id\":$1}")"
    [ "$(echo "$out" | jq -r '.state')" != running ] && break
    sleep 0.2
  done
  echo "$out"
}

# fresh_guard: exits 2 unless the target is a fresh session (fails closed).
fresh_guard() {
  local state
  state="$(tool_json get_state '{}' 2>/dev/null)" || state=""
  if [ "${MCP_SMOKE_FORCE:-0}" != 1 ]; then
    if ! echo "$state" | jq -e '.loaded == false and .modified == false' >/dev/null 2>&1; then
      echo "Refusing to run: the target Shotcut is not a fresh session (set MCP_SMOKE_FORCE=1 to override)."
      exit 2
    fi
  fi
}
