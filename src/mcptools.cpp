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

#include "mcptools.h"

#include "actions.h"
#include "autosavefile.h"
#include "mainwindow.h"
#include "mcptoolsutil.h"
#include "mltcontroller.h"

#include <QAction>
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTimer>

#include <deque>

using namespace McpTools;

namespace {

// True if the requested file is what Shotcut now has open: the project file for .mlt/.xml,
// otherwise the producer's resource.
bool isCurrentFile(const QString &path, bool isProjectFile)
{
    const QString wanted = QFileInfo(path).canonicalFilePath();
    const QString current = isProjectFile ? MAIN.fileName() : MLT.resource();
    return !wanted.isEmpty() && QFileInfo(current).canonicalFilePath() == wanted;
}

// True if `path` can be written (the file, or its folder when it does not exist yet).
// Mirrors Util::warnIfNotWritable() so its warning dialog never appears.
bool isWritableTarget(const QString &path)
{
    QFileInfo info(path);
    if (info.isFile() && !info.isWritable())
        return false;
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
    state["profile"] = QJsonObject{{"width", MLT.profile().width()},
                                   {"height", MLT.profile().height()},
                                   {"fps_num", MLT.profile().frame_rate_num()},
                                   {"fps_den", MLT.profile().frame_rate_den()}};
    return McpTools::jsonResult(state);
}

QJsonObject openProject(const QJsonObject &arguments)
{
    const QString path = arguments.value("path").toString();
    if (path.isEmpty() || isResourcePath(path) || !QDir::isAbsolutePath(path))
        return McpTools::textResult("path must be an absolute file path.", true);
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile())
        return McpTools::textResult("No such file: " + path, true);

    const QJsonValue discard = arguments.value("discard_changes");
    if (!discard.isUndefined() && !discard.isBool())
        return McpTools::textResult("discard_changes must be a boolean.", true);

    // Same case-sensitive test as MainWindow::open().
    const bool isProjectFile = path.endsWith(".mlt") || path.endsWith(".xml");
    if (isProjectFile && AutoSaveFile::exists(path)) {
        return McpTools::textResult("An auto-save file exists for this project. Open the project "
                                    "once in the Shotcut window to choose whether to recover it.",
                                    true);
    }
    // open() only asks what to do with unsaved changes when it opens a project file or when no
    // project is loaded. Otherwise the project stays open and its changes must not be touched.
    const bool wouldPrompt = isProjectFile || (!MAIN.playlist() && !MAIN.multitrack());
    const bool hadChanges = wouldPrompt && MAIN.isWindowModified();
    QSharedPointer<AutoSaveFile> oldAutosave;
    if (hadChanges) {
        if (!discard.toBool(false)) {
            return McpTools::textResult("The current project has unsaved changes. Save it first "
                                        "or pass discard_changes: true.",
                                        true);
        }
        // so open() does not ask; keep the handle so a failed open can put it back
        oldAutosave = MAIN.discardModified();
    }

    // open() reports some failures only through the status bar, so clients should call
    // get_state afterwards to confirm what is loaded.
    if (!MAIN.open(path, nullptr, false) || !isCurrentFile(path, isProjectFile)) {
        // open() can return without opening anything; the old project is then still loaded.
        if (hadChanges)
            MAIN.restoreDiscardedModified(oldAutosave);
        return McpTools::textResult("Shotcut did not open " + path
                                        + "; see the status bar or call get_state.",
                                    true);
    }
    return McpTools::jsonResult(QJsonObject{{"requested", path},
                                                  {"project",
                                                   MAIN.fileName().isEmpty()
                                                       ? QJsonValue(QJsonValue::Null)
                                                       : QJsonValue(MAIN.fileName())}});
}

QJsonObject saveProject(const QJsonObject &arguments)
{
    McpTools::commitSubtitleFeeds();
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
        // on_actionSave_triggered() returns true even when the write fails (it shows its own
        // error dialog), so check the file on disk. Timestamps may be coarse; allow 2 s.
        const QDateTime started = QDateTime::currentDateTime().addSecs(-2);
        MAIN.on_actionSave_triggered();
        const QFileInfo saved(current);
        if (!saved.exists() || saved.size() <= 0 || saved.lastModified() < started)
            return McpTools::textResult("Saving failed: " + current + " was not updated.", true);
        return McpTools::jsonResult(QJsonObject{{"saved", current}});
    }

    const QString path = pathValue.toString();
    if (path.isEmpty() || isResourcePath(path) || !QDir::isAbsolutePath(path)
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

QJsonObject playTool(const QJsonObject &arguments)
{
    double speed = 1.0;
    const QJsonValue v = arguments.value("speed");
    if (!v.isUndefined()) {
        // Controller::isPaused() treats |speed| < 0.1 as paused, so such a play would not play.
        if (!v.isDouble() || qAbs(v.toDouble()) < 0.1) {
            return McpTools::textResult("speed must be a number with an absolute value of at least "
                                        "0.1 (use pause to stop).",
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
    int position = 0;
    const FrameArg result = readFrame(arguments, "position", position);
    if (result == FrameArg::Invalid)
        return McpTools::textResult("position must be a non-negative integer frame number.", true);
    if (!isLoaded())
        return McpTools::textResult("Nothing is loaded.", true);
    if (result == FrameArg::Ok)
        position = qMin(position, lastFrame());
    MLT.pause();
    // VideoWidget::pause() ignores its position argument, so move the playhead explicitly.
    if (result == FrameArg::Ok)
        movePlayhead(position);
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
    movePlayhead(position);
    return McpTools::jsonResult(QJsonObject{{"position", position}});
}

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
    QJsonObject result{{"key", key}};
    if (action->isCheckable())
        result["checked"] = action->isChecked();
    return McpTools::jsonResult(result);
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

static QList<McpTool> stage1Tools()
{
    return {
        {"get_state",
         "Report the current project: file path (or null), modified flag, whether media is loaded, "
         "whether it is playing, playhead position and duration in frames, frame rate, and the "
         "project video format as profile {width, height, fps_num, fps_den}. Position and "
         "duration describe the player's current producer, which can be a source clip while a "
         "timeline project exists.",
         schema(R"({"type":"object","properties":{}})"),
         getState},
        {"open_project",
         "Open a project (.mlt) or media file by absolute path. Fails if the current project has "
         "unsaved changes unless discard_changes is true; that flag only matters when a project "
         "file is opened or no project is loaded (opening a media file into a loaded project "
         "leaves the project and its changes alone). Call get_state afterwards to confirm.",
         schema(R"({"type":"object","properties":{
             "path":{"type":"string","description":"Absolute path to the file."},
             "discard_changes":{"type":"boolean","description":"Discard unsaved changes when a project file is opened or no project is loaded."}
           },"required":["path"]})"),
         openProject},
        {"save_project",
         "Save the project. Without path: saves to the current file (if the write itself fails, "
         "Shotcut may show its own save-error dialog). With path (absolute, ending "
         "in .mlt): writes a copy and leaves the current file unchanged.",
         schema(R"({"type":"object","properties":{
             "path":{"type":"string","description":"Absolute .mlt path for a copy."}
           }})"),
         saveProject},
        {"play",
         "Start playback. Optional speed (absolute value at least 0.1, default 1.0; negative plays "
         "backwards). Playing from the last frame ends immediately, so seek to an earlier frame "
         "first.",
         schema(R"({"type":"object","properties":{
             "speed":{"type":"number","description":"Playback speed, absolute value at least 0.1, default 1.0."}
           }})"),
         playTool},
        {"pause",
         "Pause playback. Optional position (frame number) to move the playhead to.",
         schema(R"({"type":"object","properties":{
             "position":{"type":"integer","minimum":0,"description":"Frame to move to."}
           }})"),
         pauseTool},
        {"seek",
         "Move the playhead to a frame number and return the frame it moved to, clamped to the "
         "last frame; use get_state to see where the player is. Whether playback continues after "
         "the seek follows Shotcut's 'pause after seek' player setting; call pause first for a "
         "guaranteed stop.",
         schema(R"({"type":"object","properties":{
             "position":{"type":"integer","minimum":0,"description":"Frame number."}
           },"required":["position"]})"),
         seekTool},
        {"list_actions",
         "List the player actions that trigger_action can run (key, text, enabled, checkable, checked).",
         schema(R"({"type":"object","properties":{}})"),
         listActions},
        {"trigger_action",
         "Run one of the player actions listed by list_actions, e.g. playerNextFrameAction. "
         "Returns before the effect completes; call get_state to see the result. For checkable actions "
         "(such as playerLoopAction) pass checked to set the state; omitting it toggles the action, "
         "and the result then includes checked.",
         schema(R"({"type":"object","properties":{
             "key":{"type":"string","description":"Action key from list_actions."},
             "checked":{"type":"boolean","description":"Desired state for a checkable action."}
           },"required":["key"]})"),
         triggerAction},
    };
}

namespace {

bool s_toolCallRunning = false;

std::deque<std::function<void()>> &idleQueue()
{
    static std::deque<std::function<void()>> queue;
    return queue;
}

void drainIdleQueue()
{
    static QTimer *timer = nullptr;
    auto &queue = idleQueue();
    const bool idle = !s_toolCallRunning && !QApplication::activeModalWidget()
                      && !QApplication::activePopupWidget();
    // One at a time: a queued function may itself open a nested event loop or a tool call may
    // arrive in between, so idleness is checked again before the next one.
    if (idle && !queue.empty()) {
        std::function<void()> fn = std::move(queue.front());
        queue.pop_front();
        fn();
    }
    if (queue.empty()) {
        if (timer)
            timer->stop();
        return;
    }
    if (!timer) {
        timer = new QTimer(qApp);
        QObject::connect(timer, &QTimer::timeout, timer, drainIdleQueue);
    }
    if (!timer->isActive())
        timer->start(100);
}

} // namespace

void McpTools::setToolCallRunning(bool running)
{
    s_toolCallRunning = running;
}

bool McpTools::toolCallRunning()
{
    return s_toolCallRunning;
}

void McpTools::runWhenIdle(std::function<void()> fn)
{
    idleQueue().push_back(std::move(fn));
    // Never run it from inside the caller's stack (which may be a job's finish handler).
    QTimer::singleShot(0, qApp, drainIdleQueue);
}

QList<McpTool> McpTools::all()
{
    QList<McpTool> tools = stage1Tools();
    tools += frameTools();
    tools += timelineTools();
    tools += exportTools();
    tools += analysisTools();
    tools += transcribeTools();
    tools += overviewTools();
    tools += structureTools();
    tools += filterTools();
    tools += audioTools();
    tools += pictureTools();
    tools += formatTools();
    return tools;
}

QJsonObject McpTools::imageResult(const QByteArray &bytes,
                                  const QString &mimeType,
                                  const QString &text)
{
    QJsonArray content;
    content.append(QJsonObject{{"type", "image"},
                               {"data", QString::fromLatin1(bytes.toBase64())},
                               {"mimeType", mimeType}});
    if (!text.isEmpty())
        content.append(QJsonObject{{"type", "text"}, {"text", text}});
    return QJsonObject{{"content", content}, {"isError", false}};
}
