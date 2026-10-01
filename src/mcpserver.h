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
