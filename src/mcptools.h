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

#ifndef MCPTOOLS_H
#define MCPTOOLS_H

#include <QByteArray>
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

// An MCP tools/call result with an image item (base64 in the JSON) and an optional text item.
QJsonObject imageResult(const QByteArray &bytes,
                        const QString &mimeType,
                        const QString &text = QString());

// True while an export started by export_video is queued or running.
bool exportBusy();

// Tool groups, registered by all(). Each lives in its own mcptools_*.cpp.
QList<McpTool> frameTools();
QList<McpTool> timelineTools();
QList<McpTool> exportTools();
QList<McpTool> analysisTools();
QList<McpTool> structureTools();

} // namespace McpTools

#endif // MCPTOOLS_H
