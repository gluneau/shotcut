#!/usr/bin/env bash
# Perception and structure smoke test for the Shotcut MCP server (stage 3, part 3A).
# Usage: MCP_TOKEN=<token> [MCP_PORT=7468] [MCP_APPDATA=<dir>] tools/mcp-smoke-media.sh
# Needs: bash, curl, jq, ffmpeg, ffprobe. Run it against a FRESH, throwaway Shotcut (nothing
# loaded, empty playlist, no timeline): it aborts otherwise unless MCP_SMOKE_FORCE=1. See
# docs/mcp-server.md for the headless recipe.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=tools/mcp-smoke-lib.sh
. "$HERE/mcp-smoke-lib.sh"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fresh_guard

MEDIA="$("$HERE/mcp-fixtures.sh" "$TMP/media")" || { echo "ffmpeg could not create the fixtures"; exit 2; }

echo "== get_job"
expect_eq "tools/list has get_job" true "$(rpc '{"jsonrpc":"2.0","id":3,"method":"tools/list"}' | jq -r '[.result.tools[].name] | contains(["get_job"])')"
expect_eq "get_job: missing job_id is an error" true "$(tool_is_error get_job '{}')"
expect_eq "get_job: negative job_id is an error" true "$(tool_is_error get_job '{"job_id":-1}')"
expect_eq "get_job: string job_id is an error" true "$(tool_is_error get_job '{"job_id":"1"}')"
expect_eq "get_job: fractional job_id is an error" true "$(tool_is_error get_job '{"job_id":1.5}')"
expect_eq "get_job: unknown job_id is an error" true "$(tool_is_error get_job '{"job_id":999999}')"
expect_eq "get_job: unknown job_id message" "No such job." "$(tool_text get_job '{"job_id":999999}')"

# FOOTER (later tasks insert their blocks above this line)
if [ "$failures" -eq 0 ]; then echo "All checks passed."; else echo "${failures} check(s) failed."; exit 1; fi
