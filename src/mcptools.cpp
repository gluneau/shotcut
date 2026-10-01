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
