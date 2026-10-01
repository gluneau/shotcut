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
