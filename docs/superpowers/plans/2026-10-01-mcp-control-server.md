# Embedded MCP Control Server Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an opt-in MCP server inside Shotcut, over local HTTP, exposing stage 1 tools (project, transport, whitelisted actions).

**Architecture:** A hand-rolled HTTP/1.1 listener on `QTcpServer` (`McpServer`) authenticates each request, speaks MCP JSON-RPC on `POST /mcp`, and dispatches `tools/call` to a tool table (`McpTools`) whose handlers call existing `MAIN` / `MLT` / `Actions` APIs on the GUI thread. A Settings-menu submenu turns the server on and manages the token.

**Tech Stack:** C++17, Qt 6.10.3 (Core, Network, Widgets; already linked), MLT 7.36.1, CMake/Ninja, bash + curl + jq for the smoke test.

**Spec:** `docs/superpowers/specs/2026-10-01-mcp-control-server-design.md` (read it first; section 8 holds the resolved decisions).

## Global Constraints

- Server listens on `QHostAddress::LocalHost` (127.0.0.1) only; never all interfaces.
- Every request needs `Authorization: Bearer <token>`; token is 32 random bytes from `QRandomGenerator::system()`, hex-encoded, compared in constant time.
- A request with an `Origin` header is rejected (403); `Host` must be `127.0.0.1:<port>` or `localhost:<port>` (403 otherwise).
- Server is off by default; default port 8765; port configurable through the settings file only.
- No new Qt modules or CMake dependencies (`Qt6::Network` is already linked).
- HTTP limits: headers <= 8 KB, body <= 1 MB, 10 s read timeout, one request per connection (`Connection: close`), HTTP/1.1 only. Order of checks: size/format, Host, Origin, token, then JSON parse.
- One tool call in flight; a second `tools/call` gets JSON-RPC error `-32000` "busy".
- `open_project` / `save_project` accept absolute paths only; `save_project` with a `path` also requires a `.mlt` suffix.
- MCP protocol versions supported: `2025-06-18`, `2025-03-26`, `2024-11-05` (plus any newer one found in Task 2 Step 1).
- Edits made by tools must go through the undoable path; never call `MultitrackModel` mutators directly (stage 1 tools do not edit the timeline).
- Code conventions (`AGENTS.md`): `m_` member prefix, `camelCase` methods, `PascalCase` classes, header guards `#ifndef CLASSNAME_H`, new-style `connect()` with function pointers or lambdas only, 4-space indent, license header on new files, copyright year range in every modified file must end in 2026.
- License header for every new file (year 2026):

```cpp
/*
 * Copyright (c) 2026 Meltytech, LLC
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
```
- Build and run environment (used by every task, call it `ENV`):

```bash
export PKG_CONFIG_PATH=$HOME/mlt-install/lib/pkgconfig
export MLT_REPOSITORY=$HOME/mlt-install/lib/mlt-7
export MLT_DATA=$HOME/mlt-install/share/mlt-7
export MLT_PROFILES_PATH=$HOME/mlt-install/share/mlt-7/profiles
```

- Build command: `cmake --build build/cc-debug-linux` (from the repo root). Run command: `build/cc-debug-linux/src/shotcut`. There are no C++ unit tests in this repo and ClangFormat 14 is not installed, so match formatting by hand. The tests for this feature are `tools/mcp-smoke.sh` (grown task by task) plus the manual checks named in each task.
- Every commit message ends with a separate paragraph: `Claude-Session: https://claude.ai/code/session_01K71VLTscTALvS6w4U5nBux` (pass it as a second `-m`).

## Review Focus

Failure modes the spec implies but no happy-path test exercises, most likely first. Each has a named test in the task that owns the code.

1. A client hangs up before reading the reply (or while a slow call runs): the server must not write to a freed socket and must keep serving. (Task 2, `QPointer` guard + smoke "hang-up").
2. A slow or stalled client (partial headers) must get `408` after 10 s, not hold a connection forever. (Task 2, smoke "slow request").
3. Missing, chunked, or non-numeric `Content-Length` must get `411`/`400` rather than hang or be misparsed. (Task 2, smoke raw requests).
4. `seek`/`pause`/`play` with a string, float, negative, missing, or out-of-range value, or with nothing loaded, must return a tool error, never crash or seek wildly. (Task 4).
5. `open_project` / `save_project` with relative paths, directories, missing files, non-`.mlt` targets, or a non-boolean `discard_changes` must return a tool error and touch nothing. (Task 3).

---

## File Structure

| File | Action | Responsibility |
|------|--------|----------------|
| `src/settings.h`, `src/settings.cpp` | Modify | `mcpServerEnabled`, `mcpServerPort`, `mcpServerToken`, `regenerateMcpServerToken` |
| `src/mcptools.h`, `src/mcptools.cpp` | Create | `McpTool` struct, result helpers, the tool table |
| `src/mcpserver.h`, `src/mcpserver.cpp` | Create | HTTP listener, request checks, MCP JSON-RPC dispatch |
| `src/mainwindow.h`, `src/mainwindow.cpp` | Modify | Construct the server, "MCP Server" Settings submenu |
| `src/autosavefile.h`, `src/autosavefile.cpp` | Modify | Read-only `AutoSaveFile::exists(path)` (Task 3) |
| `src/CMakeLists.txt` | Modify | Register the four new source files |
| `tools/mcp-smoke.sh` | Create | curl/jq smoke test, grown per task |
| `docs/mcp-server.md` | Create | How to enable and connect (Task 6) |

---

### Task 1: Settings for the MCP server

**Files:**
- Modify: `src/settings.h` (declarations after `setTimelineScrollZoom`, around line 252)
- Modify: `src/settings.cpp` (definitions after `setTimelineScrollZoom`, around line 1207; add `#include <QRandomGenerator>`)

**Interfaces:**
- Consumes: nothing.
- Produces (used by Task 2):
  - `bool ShotcutSettings::mcpServerEnabled() const` / `void setMcpServerEnabled(bool)`
  - `int ShotcutSettings::mcpServerPort() const` (1024..65535, default 8765) / `void setMcpServerPort(int)`
  - `QString ShotcutSettings::mcpServerToken()` (creates and stores a token on first use)
  - `void ShotcutSettings::regenerateMcpServerToken()`

- [ ] **Step 1: Declare the settings in `src/settings.h`**

Find `void setTimelineScrollZoom(bool);` (around line 252) and add directly after it:

```cpp
    bool mcpServerEnabled() const;
    void setMcpServerEnabled(bool);
    int mcpServerPort() const;
    void setMcpServerPort(int);
    QString mcpServerToken();
    void regenerateMcpServerToken();
```

- [ ] **Step 2: Define them in `src/settings.cpp`**

Add `#include <QRandomGenerator>` to the Qt include block (alphabetical, after `<QMediaDevices>`). Find the end of `ShotcutSettings::setTimelineScrollZoom` (around line 1207) and add after it:

```cpp
bool ShotcutSettings::mcpServerEnabled() const
{
    return settings.value("mcpServer/enabled", false).toBool();
}

void ShotcutSettings::setMcpServerEnabled(bool b)
{
    settings.setValue("mcpServer/enabled", b);
}

int ShotcutSettings::mcpServerPort() const
{
    const int port = settings.value("mcpServer/port", 8765).toInt();
    return (port >= 1024 && port <= 65535) ? port : 8765;
}

void ShotcutSettings::setMcpServerPort(int port)
{
    settings.setValue("mcpServer/port", port);
}

QString ShotcutSettings::mcpServerToken()
{
    QString token = settings.value("mcpServer/token").toString();
    if (token.isEmpty()) {
        regenerateMcpServerToken();
        token = settings.value("mcpServer/token").toString();
    }
    return token;
}

void ShotcutSettings::regenerateMcpServerToken()
{
    quint32 words[8];
    QRandomGenerator::system()->fillRange(words);
    const QByteArray bytes(reinterpret_cast<const char *>(words), sizeof words);
    settings.setValue("mcpServer/token", QString::fromLatin1(bytes.toHex()));
}
```

- [ ] **Step 3: Confirm copyright years**

Run: `grep -m1 Copyright src/settings.h src/settings.cpp`
Expected: both ranges end in `2026` (they already do; change only if not).

- [ ] **Step 4: Build to verify it compiles**

Run: `ENV` exports, then `cmake --build build/cc-debug-linux 2>&1 | tail -5`
Expected: ends with `Linking CXX executable src/shotcut`, no errors.

- [ ] **Step 5: Commit**

```bash
git add src/settings.h src/settings.cpp
git commit -m "Add MCP server settings" -m "Claude-Session: https://claude.ai/code/session_01K71VLTscTALvS6w4U5nBux"
```

---

### Task 2: Server core, lifecycle wiring, and HTTP/JSON-RPC smoke tests

**Files:**
- Create: `src/mcptools.h`, `src/mcptools.cpp`, `src/mcpserver.h`, `src/mcpserver.cpp`, `tools/mcp-smoke.sh`
- Modify: `src/CMakeLists.txt` (insert the four files immediately before the `mltcontroller.cpp mltcontroller.h` line, around line 92, in the existing `foo.cpp foo.h` one-line style)
- Modify: `src/mainwindow.h`, `src/mainwindow.cpp`

**Interfaces:**
- Consumes: Task 1 settings.
- Produces (used by Tasks 3-5):
  - `struct McpTool { QString name; QString description; QJsonObject inputSchema; std::function<QJsonObject(const QJsonObject &)> handler; }`
  - `McpTools::all()` returns `QList<McpTool>`; `McpTools::textResult(const QString &text, bool isError = false)` and `McpTools::jsonResult(const QJsonObject &)` return the MCP `tools/call` result object.
  - `class McpServer : public QObject` with `bool start(quint16 port, const QString &token)`, `void stop()`, `void setToken(const QString &token)`, `bool isListening() const`, `QString errorString() const`.

- [ ] **Step 1: Verify the current MCP protocol versions**

Fetch the current MCP specification versions (WebFetch `https://modelcontextprotocol.io/specification` or search the docs for "protocol revision"). If a version newer than `2025-06-18` exists, put it first in `kSupportedVersions` in Step 5. If it cannot be reached, keep the three listed versions and say so in the commit message body.

- [ ] **Step 2: Write the smoke test first (it must fail: nothing is listening yet)**

Create `tools/mcp-smoke.sh` and `chmod +x` it:

```bash
#!/usr/bin/env bash
# Smoke test for the Shotcut MCP server.
# Usage: MCP_TOKEN=<token> [MCP_PORT=8765] tools/mcp-smoke.sh
# Needs: bash, curl, jq (and ffmpeg for the media checks). Shotcut must be running
# with Settings > MCP Server > Enable checked. The slow-request check takes ~12 s.
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

# --- tool checks are added below by later tasks ---

if [ "$failures" -eq 0 ]; then echo "All checks passed."; else echo "${failures} check(s) failed."; exit 1; fi
```

Note on the hang-up check: the JSON body `{"jsonrpc":"2.0","id":9,"method":"ping"}` is 40 bytes, which is why `Content-Length: 40` is used.

- [ ] **Step 3: Run it to verify it fails**

Run: `MCP_TOKEN=x tools/mcp-smoke.sh 2>&1 | tail -3`
Expected: failures (connection refused, status `000`), final line `N check(s) failed.`, exit code 1.

- [ ] **Step 4: Create `src/mcptools.h` and `src/mcptools.cpp` (empty tool table)**

`src/mcptools.h` (license header from Global Constraints first, then):

```cpp
#ifndef MCPTOOLS_H
#define MCPTOOLS_H

#include <QJsonObject>
#include <QList>
#include <QString>
#include <functional>

struct McpTool
{
    QString name;
    QString description;
    QJsonObject inputSchema;
    std::function<QJsonObject(const QJsonObject &arguments)> handler;
};

namespace McpTools {

// All tools the server offers. Handlers run on the GUI thread.
QList<McpTool> all();

// An MCP tools/call result containing one text item.
QJsonObject textResult(const QString &text, bool isError = false);

// An MCP tools/call result whose text item is the compact JSON of `object`.
QJsonObject jsonResult(const QJsonObject &object);

} // namespace McpTools

#endif // MCPTOOLS_H
```

`src/mcptools.cpp` (license header first, then):

```cpp
#include "mcptools.h"

#include <QJsonArray>
#include <QJsonDocument>

QJsonObject McpTools::textResult(const QString &text, bool isError)
{
    const QJsonObject item{{"type", "text"}, {"text", text}};
    return QJsonObject{{"content", QJsonArray{item}}, {"isError", isError}};
}

QJsonObject McpTools::jsonResult(const QJsonObject &object)
{
    return textResult(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
}

QList<McpTool> McpTools::all()
{
    return {};
}
```

- [ ] **Step 5: Create `src/mcpserver.h` and `src/mcpserver.cpp`**

`src/mcpserver.h` (full license header first, then):

```cpp
#ifndef MCPSERVER_H
#define MCPSERVER_H

#include "mcptools.h"

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QObject>
#include <QString>
#include <QTcpServer>

class QTcpSocket;

// Minimal MCP (Model Context Protocol) server over local HTTP.
// Single endpoint: POST /mcp with JSON-RPC 2.0. Loopback only, bearer-token auth.
class McpServer : public QObject
{
    Q_OBJECT

public:
    explicit McpServer(QObject *parent = nullptr);

    bool start(quint16 port, const QString &token);
    void stop();
    void setToken(const QString &token) { m_token = token; }
    bool isListening() const { return m_server.isListening(); }
    QString errorString() const { return m_server.errorString(); }

private:
    void onNewConnection();
    void processRequest(QTcpSocket *socket);
    void reply(QTcpSocket *socket,
               int status,
               const QByteArray &body,
               const QByteArray &extraHeaders = QByteArray());
    void replyError(QTcpSocket *socket,
                    int status,
                    const QString &message,
                    const QByteArray &extraHeaders = QByteArray());
    QByteArray handleJsonRpc(const QByteArray &body);
    QByteArray callTool(const QJsonValue &id, const QJsonObject &params);

    QTcpServer m_server;
    QHash<QTcpSocket *, QByteArray> m_buffers;
    QList<McpTool> m_tools;
    QString m_token;
    bool m_busy = false;
};

#endif // MCPSERVER_H
```

`src/mcpserver.cpp` (full license header first, then):

```cpp
#include "mcpserver.h"

#include "Logger.h"

#include <QCoreApplication>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QTcpSocket>
#include <QTimer>

namespace {

constexpr int kMaxHeaderBytes = 8 * 1024;
constexpr qint64 kMaxBodyBytes = 1024 * 1024;
constexpr int kReadTimeoutMs = 10000;

// Newest first. A client's requested version is echoed back if it is in this list.
const QList<QString> kSupportedVersions = {"2025-06-18", "2025-03-26", "2024-11-05"};

struct Request
{
    QByteArray method;
    QByteArray path;
    QHash<QByteArray, QByteArray> headers; // names lower-cased
};

bool parseHead(const QByteArray &head, Request &out)
{
    const QList<QByteArray> lines = head.split('\n');
    if (lines.isEmpty())
        return false;
    const QList<QByteArray> parts = lines.first().trimmed().split(' ');
    if (parts.size() != 3 || !parts.at(2).startsWith("HTTP/1."))
        return false;
    out.method = parts.at(0);
    out.path = parts.at(1);
    for (int i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines.at(i).trimmed();
        const int colon = line.indexOf(':');
        if (colon <= 0)
            return false;
        out.headers.insert(line.left(colon).trimmed().toLower(), line.mid(colon + 1).trimmed());
    }
    return true;
}

bool constantTimeEquals(const QByteArray &a, const QByteArray &b)
{
    if (a.size() != b.size())
        return false;
    unsigned char diff = 0;
    for (int i = 0; i < a.size(); ++i)
        diff |= static_cast<unsigned char>(a.at(i)) ^ static_cast<unsigned char>(b.at(i));
    return diff == 0;
}

QByteArray reasonPhrase(int status)
{
    switch (status) {
    case 200: return "OK";
    case 202: return "Accepted";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 411: return "Length Required";
    case 413: return "Payload Too Large";
    case 415: return "Unsupported Media Type";
    case 431: return "Request Header Fields Too Large";
    default: return "Error";
    }
}

QByteArray rpcResult(const QJsonValue &id, const QJsonObject &result)
{
    return QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}})
        .toJson(QJsonDocument::Compact);
}

QByteArray rpcError(const QJsonValue &id, int code, const QString &message)
{
    return QJsonDocument(QJsonObject{{"jsonrpc", "2.0"},
                                     {"id", id.isUndefined() ? QJsonValue(QJsonValue::Null) : id},
                                     {"error", QJsonObject{{"code", code}, {"message", message}}}})
        .toJson(QJsonDocument::Compact);
}

} // namespace

McpServer::McpServer(QObject *parent)
    : QObject(parent)
    , m_tools(McpTools::all())
{
    QObject::connect(&m_server, &QTcpServer::newConnection, this, &McpServer::onNewConnection);
}

bool McpServer::start(quint16 port, const QString &token)
{
    stop();
    m_token = token;
    if (!m_server.listen(QHostAddress::LocalHost, port)) {
        LOG_ERROR() << "MCP server cannot listen on port" << port << m_server.errorString();
        return false;
    }
    LOG_INFO() << "MCP server listening on 127.0.0.1:" << m_server.serverPort();
    return true;
}

void McpServer::stop()
{
    m_server.close();
    const auto sockets = m_server.findChildren<QTcpSocket *>();
    for (QTcpSocket *socket : sockets)
        socket->abort();
}

void McpServer::onNewConnection()
{
    while (QTcpSocket *socket = m_server.nextPendingConnection()) {
        m_buffers.insert(socket, QByteArray());

        auto *timer = new QTimer(socket);
        timer->setSingleShot(true);
        QObject::connect(timer, &QTimer::timeout, socket, [this, socket]() {
            // Only while the request is still incomplete; a running tool call has no buffer.
            if (m_buffers.contains(socket))
                replyError(socket, 408, "Request timed out");
        });
        timer->start(kReadTimeoutMs);

        QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket]() {
            if (!m_buffers.contains(socket))
                return;
            m_buffers[socket].append(socket->readAll());
            processRequest(socket);
        });
        QObject::connect(socket, &QTcpSocket::disconnected, socket, [this, socket]() {
            m_buffers.remove(socket);
            socket->deleteLater();
        });
    }
}

void McpServer::reply(QTcpSocket *socket,
                      int status,
                      const QByteArray &body,
                      const QByteArray &extraHeaders)
{
    m_buffers.remove(socket);
    QByteArray out = "HTTP/1.1 " + QByteArray::number(status) + " " + reasonPhrase(status) + "\r\n";
    out += "Content-Type: application/json\r\n";
    out += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    out += "Connection: close\r\n";
    out += extraHeaders;
    out += "\r\n";
    out += body;
    socket->write(out);
    socket->disconnectFromHost();
}

void McpServer::replyError(QTcpSocket *socket,
                           int status,
                           const QString &message,
                           const QByteArray &extraHeaders)
{
    reply(socket,
          status,
          QJsonDocument(QJsonObject{{"error", message}}).toJson(QJsonDocument::Compact),
          extraHeaders);
}

void McpServer::processRequest(QTcpSocket *socket)
{
    const QByteArray &buffer = m_buffers[socket];
    const int headEnd = buffer.indexOf("\r\n\r\n");
    if (headEnd < 0) {
        if (buffer.size() > kMaxHeaderBytes)
            replyError(socket, 431, "Headers too large");
        return;
    }
    if (headEnd > kMaxHeaderBytes) {
        replyError(socket, 431, "Headers too large");
        return;
    }

    Request request;
    if (!parseHead(buffer.left(headEnd), request)) {
        replyError(socket, 400, "Malformed request");
        return;
    }

    // Order: Host, Origin, token, then routing, then body.
    const QByteArray host = request.headers.value("host").toLower();
    const QByteArray port = QByteArray::number(m_server.serverPort());
    if (host != "127.0.0.1:" + port && host != "localhost:" + port) {
        replyError(socket, 403, "Host not allowed");
        return;
    }
    if (request.headers.contains("origin")) {
        replyError(socket, 403, "Origin not allowed");
        return;
    }
    if (m_token.isEmpty()
        || !constantTimeEquals(request.headers.value("authorization"), "Bearer " + m_token.toUtf8())) {
        replyError(socket, 401, "Unauthorized", "WWW-Authenticate: Bearer\r\n");
        return;
    }
    if (request.path != "/mcp") {
        replyError(socket, 404, "Not found");
        return;
    }
    if (request.method != "POST") {
        replyError(socket, 405, "Use POST", "Allow: POST\r\n");
        return;
    }
    if (!request.headers.value("content-type").toLower().startsWith("application/json")) {
        replyError(socket, 415, "Content-Type must be application/json");
        return;
    }
    if (request.headers.contains("transfer-encoding")
        || !request.headers.contains("content-length")) {
        replyError(socket, 411, "Content-Length is required; chunked bodies are not supported");
        return;
    }
    bool ok = false;
    const qint64 length = request.headers.value("content-length").toLongLong(&ok);
    if (!ok || length < 0) {
        replyError(socket, 400, "Invalid Content-Length");
        return;
    }
    if (length > kMaxBodyBytes) {
        replyError(socket, 413, "Body too large");
        return;
    }
    if (buffer.size() < headEnd + 4 + length)
        return; // wait for the rest of the body

    const QByteArray body = buffer.mid(headEnd + 4, length);
    m_buffers.remove(socket); // the request is complete; stop the read timeout from firing

    QPointer<QTcpSocket> guard(socket);
    const QByteArray response = handleJsonRpc(body);
    if (!guard)
        return; // the client went away while the call was running
    if (response.isEmpty())
        reply(socket, 202, QByteArray());
    else
        reply(socket, 200, response);
}

QByteArray McpServer::handleJsonRpc(const QByteArray &body)
{
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError)
        return rpcError(QJsonValue(QJsonValue::Null), -32700, "Parse error");
    if (!doc.isObject())
        return rpcError(QJsonValue(QJsonValue::Null),
                        -32600,
                        "Invalid Request (batches are not supported)");

    const QJsonObject request = doc.object();
    const QJsonValue id = request.value("id");
    const QString method = request.value("method").toString();
    if (request.value("jsonrpc").toString() != "2.0" || method.isEmpty())
        return rpcError(id, -32600, "Invalid Request");
    if (!request.contains("id"))
        return QByteArray(); // a notification gets no reply

    const QJsonObject params = request.value("params").toObject();
    if (method == "ping")
        return rpcResult(id, QJsonObject());
    if (method == "initialize") {
        const QString requested = params.value("protocolVersion").toString();
        const QString version = kSupportedVersions.contains(requested) ? requested
                                                                       : kSupportedVersions.first();
        return rpcResult(id,
                         QJsonObject{{"protocolVersion", version},
                                     {"capabilities", QJsonObject{{"tools", QJsonObject()}}},
                                     {"serverInfo",
                                      QJsonObject{{"name", "shotcut"},
                                                  {"version", QCoreApplication::applicationVersion()}}}});
    }
    if (method == "tools/list") {
        QJsonArray tools;
        for (const McpTool &tool : m_tools) {
            tools.append(QJsonObject{{"name", tool.name},
                                     {"description", tool.description},
                                     {"inputSchema", tool.inputSchema}});
        }
        return rpcResult(id, QJsonObject{{"tools", tools}});
    }
    if (method == "tools/call")
        return callTool(id, params);
    return rpcError(id, -32601, "Method not found");
}

QByteArray McpServer::callTool(const QJsonValue &id, const QJsonObject &params)
{
    const QString name = params.value("name").toString();
    std::function<QJsonObject(const QJsonObject &)> handler;
    for (const McpTool &tool : m_tools) {
        if (tool.name == name)
            handler = tool.handler;
    }
    if (!handler)
        return rpcError(id, -32602, "Unknown tool: " + name);

    const QJsonValue arguments = params.value("arguments");
    if (!arguments.isUndefined() && !arguments.isObject())
        return rpcError(id, -32602, "arguments must be an object");
    if (m_busy)
        return rpcError(id, -32000, "Server is busy with another tool call");

    m_busy = true;
    const QJsonObject result = handler(arguments.toObject());
    m_busy = false;
    return rpcResult(id, result);
}
```

In `processRequest`, `const QByteArray &buffer = m_buffers[socket];` is only used before `m_buffers.remove(socket)`; do not touch `buffer` after any `reply`/`replyError`/`remove` call.

- [ ] **Step 6: Register the files in `src/CMakeLists.txt`**

Insert immediately before the `mltcontroller.cpp mltcontroller.h` line:

```
  mcpserver.cpp mcpserver.h
  mcptools.cpp mcptools.h
```

- [ ] **Step 7: Wire the server into `MainWindow`**

Run `grep -n setupSettingsMenu src/mainwindow.h` to find the declaration. In `src/mainwindow.h`: add `class McpServer;` to the forward declarations (next to `class QUndoStack;`), add `void setupMcpServer();` directly after the `void setupSettingsMenu();` declaration, and add the member `McpServer *m_mcpServer = nullptr;` directly after `QUndoStack *m_undoStack;`.

In `src/mainwindow.cpp`: add `#include "mcpserver.h"` to the project includes (alphabetical), and add the call `setupMcpServer();` on the line directly after `setupSettingsMenu();` (around line 227). Insert this function directly before `void MainWindow::setupSettingsMenu()`:

```cpp
void MainWindow::setupMcpServer()
{
    m_mcpServer = new McpServer(this);

    QMenu *menu = ui->menuSettings->addMenu(tr("MCP Server"));
    QAction *enable = menu->addAction(tr("Enable"));
    enable->setObjectName("mcpServerEnableAction");
    enable->setCheckable(true);
    QAction *copyToken = menu->addAction(tr("Copy Token to Clipboard"));
    copyToken->setObjectName("mcpServerCopyTokenAction");
    QAction *regenerate = menu->addAction(tr("Regenerate Token"));
    regenerate->setObjectName("mcpServerRegenerateTokenAction");

    connect(enable, &QAction::toggled, this, [this, enable](bool on) {
        Settings.setMcpServerEnabled(on);
        if (!on) {
            m_mcpServer->stop();
            showStatusMessage(tr("MCP server stopped"));
        } else if (m_mcpServer->start(Settings.mcpServerPort(), Settings.mcpServerToken())) {
            showStatusMessage(tr("MCP server listening on 127.0.0.1:%1").arg(Settings.mcpServerPort()));
        } else {
            showStatusMessage(tr("MCP server failed to start: %1").arg(m_mcpServer->errorString()));
            enable->setChecked(false); // re-enters this lambda with on == false
        }
    });
    connect(copyToken, &QAction::triggered, this, [this]() {
        QGuiApplication::clipboard()->setText(Settings.mcpServerToken());
        showStatusMessage(tr("MCP token copied to the clipboard"));
    });
    connect(regenerate, &QAction::triggered, this, [this]() {
        Settings.regenerateMcpServerToken();
        m_mcpServer->setToken(Settings.mcpServerToken());
        showStatusMessage(tr("MCP token regenerated"));
    });

    enable->setChecked(Settings.mcpServerEnabled()); // starts the server when enabled
}
```

If `QGuiApplication` is not found, add `#include <QGuiApplication>`. Confirm `grep -m1 Copyright src/mainwindow.h src/mainwindow.cpp` ranges end in 2026; update if not.

- [ ] **Step 8: Build**

Run: `ENV` exports, then `cmake --build build/cc-debug-linux 2>&1 | grep -E "error|warning: unused|Linking" | head`
Expected: `Linking CXX executable src/shotcut`, no errors. Fix any compile errors before continuing (re-run until clean).

- [ ] **Step 9: Start Shotcut, enable the server, run the smoke test**

Run Shotcut in a separate terminal (`ENV` exports, then `build/cc-debug-linux/src/shotcut`). In the app: Settings > MCP Server > Enable, then Copy Token to Clipboard. Then:

Run: `MCP_TOKEN=<pasted token> tools/mcp-smoke.sh`
Expected: every line `PASS`, final line `All checks passed.` (takes ~15 s). Fix any `FAIL` before continuing. In the Shotcut log (terminal) there should be `MCP server listening on 127.0.0.1: 8765`.

- [ ] **Step 10: Manual checks**

1. Restart Shotcut: the Enable item is still checked and the server is up again (setting persists).
2. Uncheck Enable: `curl -s -o /dev/null -w '%{http_code}\n' http://127.0.0.1:8765/mcp` prints `000`.
3. Port in use: `python3 -m http.server 8765 --bind 127.0.0.1` in another terminal, then check Enable. Expected: status bar says "MCP server failed to start", the item unchecks itself, Shotcut does not crash. Stop the python server afterwards.
4. Regenerate Token: the old token now gets `401`, the new one works without restarting.

- [ ] **Step 11: Commit**

```bash
git add src/mcpserver.h src/mcpserver.cpp src/mcptools.h src/mcptools.cpp src/CMakeLists.txt src/mainwindow.h src/mainwindow.cpp tools/mcp-smoke.sh
git commit -m "Add embedded MCP server core with Settings menu" -m "Claude-Session: https://claude.ai/code/session_01K71VLTscTALvS6w4U5nBux"
```

---

### Task 3: Project tools (`get_state`, `open_project`, `save_project`)

**Files:**
- Modify: `src/autosavefile.h`, `src/autosavefile.cpp` (add `static bool exists(const QString &filename)`)
- Modify: `src/mcptools.cpp`
- Modify: `tools/mcp-smoke.sh`

**Interfaces:**
- Consumes: `McpTool`, `McpTools::textResult`, `McpTools::jsonResult` (Task 2); `MAIN` (`fileName()`, `isWindowModified()`, `setWindowModified(bool)`, `open(path, props, play, skipConvert)`, `on_actionSave_triggered()`, `saveXML(path)`), `MLT` (`producer()`).
- Produces: tools `get_state`, `open_project`, `save_project`; helper `isLoaded()` used by Task 4.

**Why `AutoSaveFile::exists`:** `~AutoSaveFile()` deletes its file, so calling `AutoSaveFile::getFile()` just to test for a stale auto-save would destroy the user's recovery file. Add a read-only check instead.

- [ ] **Step 1: Write the failing smoke checks**

In `tools/mcp-smoke.sh`, insert before the `# --- tool checks are added below ---` marker line (keep the marker):

```bash
echo "== project tools"
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
expect_eq "open_project: non-boolean discard_changes is an error" true "$(tool_is_error open_project "{\"path\":\"$TMP\",\"discard_changes\":\"yes\"}")"

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
else
  echo "SKIP  media checks (ffmpeg not available)"
fi
```

- [ ] **Step 2: Run it to verify it fails**

Run: `MCP_TOKEN=<token> tools/mcp-smoke.sh 2>&1 | sed -n '/project tools/,$p'`
Expected: `FAIL` for the `tools/list has get_state` check and most of the others (`Unknown tool`, so `isError` is `null`).

- [ ] **Step 3: Add `AutoSaveFile::exists`**

In `src/autosavefile.h`, add directly after `static AutoSaveFile *getFile(const QString &filename);`:

```cpp
    static bool exists(const QString &filename);
```

In `src/autosavefile.cpp`, add directly after the `AutoSaveFile::getFile` definition:

```cpp
bool AutoSaveFile::exists(const QString &filename)
{
    return QFileInfo(path(), hashName(filename) + extension).exists();
}
```

Confirm `grep -m1 Copyright src/autosavefile.h src/autosavefile.cpp` ends in 2026; update if not.

- [ ] **Step 4: Implement the tools in `src/mcptools.cpp`**

Replace the include block and `all()` with:

```cpp
#include "mcptools.h"

#include "autosavefile.h"
#include "mainwindow.h"
#include "mltcontroller.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>

namespace {

QJsonObject schema(const char *json)
{
    return QJsonDocument::fromJson(json).object();
}

bool isLoaded()
{
    return MLT.producer() && MLT.producer()->is_valid();
}

// True if `path` can be written (the file, or its folder when it does not exist yet).
// Mirrors Util::warnIfNotWritable() so its warning dialog never appears.
bool isWritableTarget(const QString &path)
{
    QFileInfo info(path);
    if (!info.isDir())
        info = QFileInfo(info.dir().path());
    return info.isWritable();
}

QJsonObject getState(const QJsonObject &)
{
    const bool loaded = isLoaded();
    QJsonObject state;
    state["project"] = MAIN.fileName().isEmpty() ? QJsonValue(QJsonValue::Null)
                                                 : QJsonValue(MAIN.fileName());
    state["modified"] = MAIN.isWindowModified();
    state["loaded"] = loaded;
    state["playing"] = loaded && !MLT.isPaused();
    state["position"] = loaded ? MLT.producer()->position() : 0;
    state["duration"] = loaded ? MLT.producer()->get_playtime() : 0;
    state["fps"] = MLT.profile().fps();
    return McpTools::jsonResult(state);
}

QJsonObject openProject(const QJsonObject &arguments)
{
    const QString path = arguments.value("path").toString();
    if (path.isEmpty() || !QDir::isAbsolutePath(path))
        return McpTools::textResult("path must be an absolute file path.", true);
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile())
        return McpTools::textResult("No such file: " + path, true);

    const QJsonValue discard = arguments.value("discard_changes");
    if (!discard.isUndefined() && !discard.isBool())
        return McpTools::textResult("discard_changes must be a boolean.", true);

    const QString suffix = info.suffix().toLower();
    if ((suffix == "mlt" || suffix == "xml") && AutoSaveFile::exists(path)) {
        return McpTools::textResult("An auto-save file exists for this project. Open the project "
                                    "once in the Shotcut window to choose whether to recover it.",
                                    true);
    }
    if (MAIN.isWindowModified()) {
        if (!discard.toBool(false)) {
            return McpTools::textResult("The current project has unsaved changes. Save it first "
                                        "or pass discard_changes: true.",
                                        true);
        }
        MAIN.setWindowModified(false); // so open() does not ask what to do with the changes
    }

    // open() reports some failures only through the status bar, so clients should call
    // get_state afterwards to confirm what is loaded.
    if (!MAIN.open(path, nullptr, false))
        return McpTools::textResult("Shotcut could not open " + path, true);
    return McpTools::jsonResult(QJsonObject{{"requested", path}, {"project", MAIN.fileName()}});
}

QJsonObject saveProject(const QJsonObject &arguments)
{
    const QJsonValue pathValue = arguments.value("path");
    if (pathValue.isUndefined() || pathValue.isNull()) {
        const QString current = MAIN.fileName();
        if (current.isEmpty()) {
            return McpTools::textResult("This project has no file name yet. Pass a path to save "
                                        "a copy.",
                                        true);
        }
        if (!isWritableTarget(current))
            return McpTools::textResult("Cannot write to " + current, true);
        if (!MAIN.on_actionSave_triggered())
            return McpTools::textResult("Saving failed.", true);
        return McpTools::jsonResult(QJsonObject{{"saved", current}});
    }

    const QString path = pathValue.toString();
    if (path.isEmpty() || !QDir::isAbsolutePath(path)
        || !path.endsWith(".mlt", Qt::CaseInsensitive)) {
        return McpTools::textResult("path must be an absolute path ending in .mlt.", true);
    }
    if (QFileInfo(path).isDir())
        return McpTools::textResult("path is a directory: " + path, true);
    if (!isWritableTarget(path))
        return McpTools::textResult("Cannot write to " + path, true);
    if (!MAIN.saveXML(path))
        return McpTools::textResult("Saving failed.", true);
    return McpTools::jsonResult(QJsonObject{{"savedCopy", path}});
}

} // namespace
```

Keep the `textResult` and `jsonResult` definitions from Task 2 (they go below the anonymous namespace, unchanged), and replace `McpTools::all()` with:

```cpp
QList<McpTool> McpTools::all()
{
    return {
        {"get_state",
         "Report the current project: file path (or null), modified flag, whether media is loaded, "
         "whether it is playing, playhead position and duration in frames, and frame rate.",
         schema(R"({"type":"object","properties":{}})"),
         getState},
        {"open_project",
         "Open a project (.mlt) or media file by absolute path. Fails if the current project has "
         "unsaved changes unless discard_changes is true. Call get_state afterwards to confirm.",
         schema(R"({"type":"object","properties":{
             "path":{"type":"string","description":"Absolute path to the file."},
             "discard_changes":{"type":"boolean","description":"Discard unsaved changes in the current project."}
           },"required":["path"]})"),
         openProject},
        {"save_project",
         "Save the project. Without path: saves to the current file. With path (absolute, ending "
         "in .mlt): writes a copy and leaves the current file unchanged.",
         schema(R"({"type":"object","properties":{
             "path":{"type":"string","description":"Absolute .mlt path for a copy."}
           }})"),
         saveProject},
    };
}
```

- [ ] **Step 5: Build, restart Shotcut, run the smoke test**

Run: `ENV` exports, `cmake --build build/cc-debug-linux 2>&1 | grep -E "error|Linking"`. Restart Shotcut, re-copy the token if you regenerated it, then `MCP_TOKEN=<token> tools/mcp-smoke.sh`.
Expected: all `PASS`, final `All checks passed.`

- [ ] **Step 6: Manual checks (need the GUI)**

1. In Shotcut, make an edit so the title bar shows the modified marker, then call `open_project` on a real file without `discard_changes`: it returns an error and Shotcut shows no dialog. With `"discard_changes":true` it opens.
2. Save a project from the GUI to `~/x.mlt`, then call `save_project` with `{}`: returns `{"saved":"/home/.../x.mlt"}`.
3. With an untitled project, `save_project` `{}` returns the "no file name yet" error and no dialog.

- [ ] **Step 7: Commit**

```bash
git add src/autosavefile.h src/autosavefile.cpp src/mcptools.cpp tools/mcp-smoke.sh
git commit -m "Add MCP project tools: get_state, open_project, save_project" -m "Claude-Session: https://claude.ai/code/session_01K71VLTscTALvS6w4U5nBux"
```

---

### Task 4: Transport tools (`play`, `pause`, `seek`)

**Files:**
- Modify: `src/mcptools.cpp`
- Modify: `tools/mcp-smoke.sh`

**Interfaces:**
- Consumes: `isLoaded()`, `McpTools::textResult/jsonResult` (Tasks 2-3); `MLT.play(double)`, `MLT.pause(int)`, `MLT.seek(int)`, `MAIN.isMultitrackValid()`, `MAIN.seekTimeline(int, bool)`.
- Produces: tools `play`, `pause`, `seek`.

- [ ] **Step 1: Write the failing smoke checks**

Insert before the `# --- tool checks are added below ---` marker (keep the marker). These need the clip opened in Task 3's checks, so they run only when `HAVE_CLIP=1`:

```bash
echo "== transport tools"
if [ "$HAVE_CLIP" = 1 ]; then
  expect_eq "pause: ok" false "$(tool_is_error pause '{}')"
  sleep 0.5
  expect_eq "pause: not playing" false "$(tool_json get_state '{}' | jq -r '.playing')"

  expect_eq "seek 50: ok" false "$(tool_is_error seek '{"position":50}')"
  sleep 0.5
  expect_eq "seek 50: position is 50" 50 "$(tool_json get_state '{}' | jq -r '.position')"
  expect_eq "seek past the end is clamped to the last frame" 99 "$(tool_json seek '{"position":99999}' | jq -r '.position')"

  expect_eq "seek: missing position is an error" true "$(tool_is_error seek '{}')"
  expect_eq "seek: string position is an error" true "$(tool_is_error seek '{"position":"abc"}')"
  expect_eq "seek: float position is an error" true "$(tool_is_error seek '{"position":1.5}')"
  expect_eq "seek: negative position is an error" true "$(tool_is_error seek '{"position":-1}')"
  expect_eq "pause: negative position is an error" true "$(tool_is_error pause '{"position":-5}')"
  expect_eq "play: zero speed is an error" true "$(tool_is_error play '{"speed":0}')"
  expect_eq "play: string speed is an error" true "$(tool_is_error play '{"speed":"fast"}')"

  expect_eq "play: ok" false "$(tool_is_error play '{}')"
  sleep 0.5
  expect_eq "play: playing" true "$(tool_json get_state '{}' | jq -r '.playing')"
  tool_is_error pause '{"position":10}' >/dev/null
  sleep 0.5
  expect_eq "pause with position: stopped at 10" 10 "$(tool_json get_state '{}' | jq -r '.position')"
else
  echo "SKIP  transport checks (no clip loaded)"
fi
```

- [ ] **Step 2: Run it to verify it fails**

Run: `MCP_TOKEN=<token> tools/mcp-smoke.sh 2>&1 | sed -n '/transport tools/,$p'`
Expected: `FAIL` on the tool checks (`Unknown tool`, `isError` is `null`).

- [ ] **Step 3: Implement the tools**

In `src/mcptools.cpp`, add `#include <cmath>` and `#include <limits>` to the includes. Inside the anonymous namespace (after `saveProject`) add:

```cpp
enum class FrameArg { Missing, Ok, Invalid };

// Reads a non-negative integer frame number from `arguments[key]`.
FrameArg readFrame(const QJsonObject &arguments, const char *key, int &value)
{
    const QJsonValue v = arguments.value(key);
    if (v.isUndefined())
        return FrameArg::Missing;
    const double d = v.toDouble();
    if (!v.isDouble() || d != std::floor(d) || d < 0
        || d > std::numeric_limits<int>::max()) {
        return FrameArg::Invalid;
    }
    value = static_cast<int>(d);
    return FrameArg::Ok;
}

int lastFrame()
{
    return qMax(0, MLT.producer()->get_playtime() - 1);
}

QJsonObject playTool(const QJsonObject &arguments)
{
    double speed = 1.0;
    const QJsonValue v = arguments.value("speed");
    if (!v.isUndefined()) {
        if (!v.isDouble() || v.toDouble() == 0.0) {
            return McpTools::textResult("speed must be a non-zero number (use pause to stop).",
                                        true);
        }
        speed = v.toDouble();
    }
    if (!isLoaded())
        return McpTools::textResult("Nothing is loaded.", true);
    MLT.play(speed);
    return McpTools::textResult("Playing.");
}

QJsonObject pauseTool(const QJsonObject &arguments)
{
    int position = -1; // -1 means pause without seeking
    const FrameArg result = readFrame(arguments, "position", position);
    if (result == FrameArg::Invalid)
        return McpTools::textResult("position must be a non-negative integer frame number.", true);
    if (!isLoaded())
        return McpTools::textResult("Nothing is loaded.", true);
    if (result == FrameArg::Ok)
        position = qMin(position, lastFrame());
    MLT.pause(position);
    return McpTools::textResult("Paused.");
}

QJsonObject seekTool(const QJsonObject &arguments)
{
    int position = 0;
    if (readFrame(arguments, "position", position) != FrameArg::Ok)
        return McpTools::textResult("position must be a non-negative integer frame number.", true);
    if (!isLoaded())
        return McpTools::textResult("Nothing is loaded.", true);
    position = qMin(position, lastFrame());
    if (MAIN.isMultitrackValid())
        MAIN.seekTimeline(position);
    else
        MLT.seek(position);
    return McpTools::jsonResult(QJsonObject{{"position", position}});
}
```

Append these entries inside the braces of `McpTools::all()`, after `saveProject`:

```cpp
        {"play",
         "Start playback. Optional speed (non-zero number, default 1.0; negative plays backwards).",
         schema(R"({"type":"object","properties":{
             "speed":{"type":"number","description":"Playback speed, default 1.0."}
           }})"),
         playTool},
        {"pause",
         "Pause playback. Optional position (frame number) to move the playhead to.",
         schema(R"({"type":"object","properties":{
             "position":{"type":"integer","minimum":0,"description":"Frame to move to."}
           }})"),
         pauseTool},
        {"seek",
         "Move the playhead to a frame number (clamped to the last frame).",
         schema(R"({"type":"object","properties":{
             "position":{"type":"integer","minimum":0,"description":"Frame number."}
           },"required":["position"]})"),
         seekTool},
```

- [ ] **Step 4: Build, restart Shotcut, run the smoke test**

Run: `cmake --build build/cc-debug-linux 2>&1 | grep -E "error|Linking"`, restart Shotcut, then `MCP_TOKEN=<token> tools/mcp-smoke.sh`.
Expected: all `PASS`. If `seek 50: position is 50` is flaky, raise the `sleep` to 1 s and note it in the commit body.

- [ ] **Step 5: Manual check**

Open a project with a timeline (several clips) in the GUI and call `seek` with a mid-timeline frame: the timeline playhead and the player both move to it (this exercises the `isMultitrackValid()` branch the smoke test cannot reach).

- [ ] **Step 6: Commit**

```bash
git add src/mcptools.cpp tools/mcp-smoke.sh
git commit -m "Add MCP transport tools: play, pause, seek" -m "Claude-Session: https://claude.ai/code/session_01K71VLTscTALvS6w4U5nBux"
```

---

### Task 5: Action tools (`list_actions`, `trigger_action`)

**Files:**
- Modify: `src/mcptools.cpp`
- Modify: `tools/mcp-smoke.sh`

**Interfaces:**
- Consumes: `Actions` (`operator[](key)`), `McpTools` helpers.
- Produces: tools `list_actions`, `trigger_action`; the allowlist from spec section 8.

- [ ] **Step 1: Write the failing smoke checks**

Insert before the `# --- tool checks are added below ---` marker (keep the marker):

```bash
echo "== action tools"
expect_eq "list_actions includes playerPlayPauseAction" true "$(tool_json list_actions '{}' | jq -r '[.actions[].key] | contains(["playerPlayPauseAction"])')"
expect_eq "list_actions excludes dialog-opening actions" false "$(tool_json list_actions '{}' | jq -r '[.actions[].key] | contains(["playerSetPositionAction"])')"
expect_eq "trigger_action: non-allowlisted key is an error" true "$(tool_is_error trigger_action '{"key":"playerSetPositionAction"}')"
expect_eq "trigger_action: unknown key is an error" true "$(tool_is_error trigger_action '{"key":"nope"}')"
expect_eq "trigger_action: missing key is an error" true "$(tool_is_error trigger_action '{}')"
expect_eq "trigger_action: non-boolean checked is an error" true "$(tool_is_error trigger_action '{"key":"playerLoopAction","checked":"yes"}')"
if [ "$HAVE_CLIP" = 1 ]; then
  tool_is_error pause '{"position":20}' >/dev/null
  sleep 0.5
  expect_eq "trigger_action: next frame is ok" false "$(tool_is_error trigger_action '{"key":"playerNextFrameAction"}')"
  sleep 0.5
  expect_eq "trigger_action: moved one frame" 21 "$(tool_json get_state '{}' | jq -r '.position')"
fi
```

- [ ] **Step 2: Run it to verify it fails**

Run: `MCP_TOKEN=<token> tools/mcp-smoke.sh 2>&1 | sed -n '/action tools/,$p'`
Expected: `FAIL` on the action checks.

- [ ] **Step 3: Implement the tools**

In `src/mcptools.cpp` add `#include "actions.h"` (alphabetical, before `autosavefile.h`) and `#include <QAction>`. Inside the anonymous namespace add:

```cpp
// Player actions whose slots open no dialog. Anything else is refused.
const QStringList &allowedActions()
{
    static const QStringList keys = {
        "playerPlayPauseAction",
        "playerPauseAction",
        "playerRewindAction",
        "playerFastForwardAction",
        "playerSeekStartAction",
        "playerSeekEndAction",
        "playerNextFrameAction",
        "playerPreviousFrameAction",
        "playerForwardOneSecondAction",
        "playerBackwardOneSecondAction",
        "playerForwardFiveSecondsAction",
        "playerBackwardFiveSecondsAction",
        "playerForwardTenSecondsAction",
        "playerBackwardTenSecondsAction",
        "playerSkipNextAction",
        "playerSkipPreviousAction",
        "playerLoopAction",
    };
    return keys;
}

QJsonObject listActions(const QJsonObject &)
{
    QJsonArray list;
    for (const QString &key : allowedActions()) {
        QAction *action = Actions[key];
        if (!action)
            continue;
        list.append(QJsonObject{{"key", key},
                                {"text", action->iconText()},
                                {"enabled", action->isEnabled()},
                                {"checkable", action->isCheckable()},
                                {"checked", action->isChecked()}});
    }
    return McpTools::jsonResult(QJsonObject{{"actions", list}});
}

QJsonObject triggerAction(const QJsonObject &arguments)
{
    const QString key = arguments.value("key").toString();
    if (!allowedActions().contains(key)) {
        return McpTools::textResult("Action not available: '" + key
                                        + "'. Use list_actions for the available keys.",
                                    true);
    }
    const QJsonValue checked = arguments.value("checked");
    if (!checked.isUndefined() && !checked.isBool())
        return McpTools::textResult("checked must be a boolean.", true);
    QAction *action = Actions[key];
    if (!action)
        return McpTools::textResult("Action is not registered: " + key, true);
    if (!action->isEnabled())
        return McpTools::textResult("Action is currently disabled: " + key, true);

    // Triggering a checkable action toggles it, so only trigger when the state must change.
    if (action->isCheckable() && checked.isBool()) {
        if (action->isChecked() != checked.toBool())
            action->trigger();
    } else {
        action->trigger();
    }
    return McpTools::jsonResult(QJsonObject{{"key", key}, {"checked", action->isChecked()}});
}
```

Append these entries inside the braces of `McpTools::all()`:

```cpp
        {"list_actions",
         "List the player actions that trigger_action can run (key, text, enabled, checkable, checked).",
         schema(R"({"type":"object","properties":{}})"),
         listActions},
        {"trigger_action",
         "Run one of the player actions listed by list_actions, e.g. playerNextFrameAction. "
         "For checkable actions (such as playerLoopAction) pass checked to set the state.",
         schema(R"({"type":"object","properties":{
             "key":{"type":"string","description":"Action key from list_actions."},
             "checked":{"type":"boolean","description":"Desired state for a checkable action."}
           },"required":["key"]})"),
         triggerAction},
```

- [ ] **Step 4: Build, restart Shotcut, run the smoke test**

Run: `cmake --build build/cc-debug-linux 2>&1 | grep -E "error|Linking"`, restart Shotcut, then `MCP_TOKEN=<token> tools/mcp-smoke.sh`.
Expected: all `PASS`. If `list_actions` is missing a key, run `tool_json list_actions '{}'` and check which `Actions[...]` keys are not registered in this build (a key that does not exist is skipped silently by design). Remove any permanently missing key from the allowlist and from spec section 8 in the same commit.

- [ ] **Step 5: Confirm the allowlist opens no dialogs (manual)**

With a clip loaded, call `trigger_action` once for every key returned by `list_actions` (a quick loop with `curl`). Expected: no dialog appears for any of them. Remove any key that opens a dialog, and update spec section 8.

- [ ] **Step 6: Commit**

```bash
git add src/mcptools.cpp tools/mcp-smoke.sh
git commit -m "Add MCP action tools: list_actions, trigger_action" -m "Claude-Session: https://claude.ai/code/session_01K71VLTscTALvS6w4U5nBux"
```

---

### Task 6: Docs and end-to-end verification

**Files:**
- Create: `docs/mcp-server.md`

**Interfaces:**
- Consumes: everything above. Produces: user-facing setup notes.

- [ ] **Step 1: Write `docs/mcp-server.md`**

```markdown
# MCP server

Shotcut can run a local [Model Context Protocol](https://modelcontextprotocol.io) server so an
MCP client (for example Claude Code) can control it. It is off by default.

## Enable

1. Settings > MCP Server > Enable. The server listens on `127.0.0.1:8765` only.
2. Settings > MCP Server > Copy Token to Clipboard.
3. Connect a client, for example:

       claude mcp add --transport http shotcut http://127.0.0.1:8765/mcp \
         --header "Authorization: Bearer <token>"

The port can be changed with `mcpServer/port` (1024-65535) in Shotcut's settings file.
"Regenerate Token" invalidates the old token immediately.

## Tools

| Tool | What it does |
|------|--------------|
| `get_state` | Project path, modified flag, loaded/playing, position, duration, fps |
| `open_project` | Open a project or media file (absolute path). Refuses if there are unsaved changes unless `discard_changes` is true |
| `save_project` | Save to the current file, or write a copy to an absolute `.mlt` path |
| `play`, `pause`, `seek` | Transport control (frame numbers) |
| `list_actions`, `trigger_action` | A fixed allowlist of player actions |

## Security

Loopback only; every request needs the bearer token; requests carrying an `Origin` header or a
foreign `Host` header are rejected, which blocks web pages and DNS rebinding. The token is stored
unencrypted in the settings file. Rare project-repair or compatibility dialogs inside `open()` can
still appear for unusual project files and block the request until answered.

## Testing

`MCP_TOKEN=<token> tools/mcp-smoke.sh` (needs curl and jq; ffmpeg for the media checks).
```

- [ ] **Step 2: Run the full smoke test from a clean start**

Quit Shotcut, start it fresh (`ENV` exports, `build/cc-debug-linux/src/shotcut`), enable the server, copy the token, run `MCP_TOKEN=<token> tools/mcp-smoke.sh`.
Expected: every check `PASS`, `All checks passed.`

- [ ] **Step 3: Verify with a real MCP client**

Run: `claude mcp add --transport http shotcut-dev http://127.0.0.1:8765/mcp --header "Authorization: Bearer <token>"`, then in a fresh Claude Code session ask it to open the ffmpeg test clip, seek to frame 50, and report the state.
Expected: the client lists the eight tools, the clip opens in Shotcut, the playhead moves to 50, `get_state` agrees. Afterwards remove the dev registration: `claude mcp remove shotcut-dev`.

- [ ] **Step 4: Regression checks**

Run: `node src/qml/export-edl/test-node.js && node src/qml/export-chapters/test-node.js`
Expected: both pass (unchanged code; confirms nothing else broke).
Run: `git status --short`
Expected: clean except `docs/mcp-server.md`.

- [ ] **Step 5: Commit**

```bash
git add docs/mcp-server.md
git commit -m "Document the MCP server" -m "Claude-Session: https://claude.ai/code/session_01K71VLTscTALvS6w4U5nBux"
```

---

## Self-Review Notes

- **Spec coverage:** Section 2 files -> Tasks 1-3; section 3 security -> Task 2 (checks + smoke); section 4 HTTP/MCP -> Task 2; section 5 tools -> Tasks 3-5; section 6 lifecycle -> Task 2 Step 10 manual checks; section 7 verification -> Tasks 2-6; section 8 decisions -> Tasks 3-5. The spec's `tools/mcp-smoke.sh` is Task 2.
- **Known limits carried from the spec:** `open()` can still show repair/compat dialogs for unusual project files; the media and transport checks skip when `ffmpeg` is absent; `seek` returns the clamped target frame, not a confirmed position (callers use `get_state`).
- **Type consistency:** `McpTool` fields, `McpTools::textResult/jsonResult`, `isLoaded()`, `readFrame()` and `FrameArg` are defined before use; `McpServer::start(quint16, const QString &)` matches the call in `setupMcpServer`, where `Settings.mcpServerPort()` returns `int` and converts implicitly.
