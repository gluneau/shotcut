/*
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

#include "mcpserver.h"

#include "Logger.h"

#include <QApplication>
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
const QList<QString> kSupportedVersions = {"2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"};

struct Request
{
    QByteArray method;
    QByteArray path;
    QByteArray version;
    QHash<QByteArray, QByteArray> headers; // names lower-cased
};

bool parseHead(const QByteArray &head, Request &out)
{
    const QList<QByteArray> lines = head.split('\n');
    if (lines.isEmpty())
        return false;
    const QList<QByteArray> parts = lines.first().trimmed().split(' ');
    if (parts.size() != 3 || !parts.at(2).startsWith("HTTP/"))
        return false;
    out.method = parts.at(0);
    out.path = parts.at(1);
    out.version = parts.at(2);
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
    case 505: return "HTTP Version Not Supported";
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

    if (request.version != "HTTP/1.1") {
        replyError(socket, 505, "Only HTTP/1.1 is supported");
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
        if (tool.name == name) {
            handler = tool.handler;
            break;
        }
    }
    if (!handler)
        return rpcError(id, -32602, "Unknown tool: " + name);

    const QJsonValue arguments = params.value("arguments");
    if (!arguments.isUndefined() && !arguments.isObject())
        return rpcError(id, -32602, "arguments must be an object");
    if (m_busy)
        return rpcError(id, -32000, "Server is busy with another tool call");
    // A tool call runs inside whatever event loop is current; refuse while the user has a modal
    // dialog open (the only modal a tool creates is LongUiTask, which exists only under m_busy).
    if (QApplication::activeModalWidget())
        return rpcError(id, -32000, "A dialog is open in Shotcut; close it and try again.");

    m_busy = true;
    const QJsonObject result = handler(arguments.toObject());
    m_busy = false;
    return rpcResult(id, result);
}
