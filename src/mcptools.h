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

// Writes every subtitle track's SRT into its subtitle_feed filter now. Shotcut writes it 500 ms
// after an edit, undo or redo, so call this before the project is saved or exported.
void commitSubtitleFeeds();

// McpServer marks the span of every tools/call handler with these.
void setToolCallRunning(bool running);
bool toolCallRunning();

// Runs fn later on the GUI thread, in call order, at a moment when no tool call is executing and
// no modal dialog or popup is open (checked every 100 ms). For edits that a background job makes
// when it finishes (clip_normalize): a tool call may be waiting in a nested event loop when the job
// ends, and an edit there would interleave with that tool's undo macro.
void runWhenIdle(std::function<void()> fn);

// Tool groups, registered by all(). Each lives in its own mcptools_*.cpp.
QList<McpTool> frameTools();
QList<McpTool> timelineTools();
QList<McpTool> exportTools();
QList<McpTool> analysisTools();
QList<McpTool> transcribeTools();
QList<McpTool> overviewTools();
QList<McpTool> structureTools();
QList<McpTool> filterTools();
QList<McpTool> audioTools();
QList<McpTool> pictureTools();
QList<McpTool> formatTools();

} // namespace McpTools

#endif // MCPTOOLS_H
