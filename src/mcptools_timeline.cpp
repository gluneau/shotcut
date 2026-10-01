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
#include "mcptoolsutil.h"

#include "commands/playlistcommands.h"
#include "commands/timelinecommands.h"
#include "docks/playlistdock.h"
#include "docks/timelinedock.h"
#include "mainwindow.h"
#include "mltcontroller.h"
#include "models/multitrackmodel.h"
#include "proxymanager.h"
#include "settings.h"
#include "shotcut_mlt_properties.h"

#include <MltPlaylist.h>

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QScopedPointer>
#include <QUndoStack>

#include <cmath>
#include <memory>

using namespace McpTools;

namespace {

bool itemFlag(MultitrackModel &model, int track, int clip, int role)
{
    return model.data(model.index(clip, 0, model.index(track)), role).toBool();
}

QString kindOf(MultitrackModel &model, int track, int clip)
{
    if (itemFlag(model, track, clip, MultitrackModel::IsBlankRole))
        return QStringLiteral("blank");
    if (itemFlag(model, track, clip, MultitrackModel::IsTransitionRole))
        return QStringLiteral("transition");
    return QStringLiteral("clip");
}

QJsonObject clipJson(MultitrackModel &model, int track, int clip)
{
    const QModelIndex index = model.index(clip, 0, model.index(track));
    QJsonObject o{{"index", clip}, {"kind", kindOf(model, track, clip)}};
    std::unique_ptr<Mlt::ClipInfo> info = model.getClipInfo(track, clip);
    if (info) {
        o["start"] = info->start;
        o["duration"] = info->frame_count;
        o["in"] = info->frame_in;
        o["out"] = info->frame_out;
    }
    o["name"] = model.data(index, MultitrackModel::NameRole).toString();
    if (o["kind"] == "clip" && info && info->producer && info->producer->is_valid())
        o["path"] = ProxyManager::resource(*info->producer);
    else
        o["path"] = model.data(index, MultitrackModel::ResourceRole).toString();
    o["fade_in"] = model.data(index, MultitrackModel::FadeInRole).toInt();
    o["fade_out"] = model.data(index, MultitrackModel::FadeOutRole).toInt();
    return o;
}

QJsonObject trackJson(MultitrackModel &model, int track)
{
    const QModelIndex index = model.index(track);
    QJsonArray clips;
    for (int c = 0; c < model.rowCount(index); ++c)
        clips.append(clipJson(model, track, c));
    return QJsonObject{
        {"index", track},
        {"name", model.data(index, MultitrackModel::NameRole).toString()},
        {"type", model.trackList().at(track).type == AudioTrackType ? "audio" : "video"},
        {"locked", model.data(index, MultitrackModel::IsLockedRole).toBool()},
        {"muted", model.data(index, MultitrackModel::IsMuteRole).toBool()},
        {"hidden", model.data(index, MultitrackModel::IsHiddenRole).toBool()},
        {"clips", clips}};
}

QJsonObject playlistJson()
{
    QJsonArray items;
    Mlt::Playlist *playlist = MAIN.playlist();
    if (playlist && playlist->is_valid()) {
        const QString projectFolder = MAIN.fileName().isEmpty()
                                          ? QString()
                                          : QFileInfo(MAIN.fileName()).canonicalPath();
        for (int i = 0; i < playlist->count(); ++i) {
            QScopedPointer<Mlt::ClipInfo> info(playlist->clip_info(i));
            if (!info || !info->producer || !info->producer->is_valid())
                continue;
            QString path = ProxyManager::resource(*info->producer);
            if (!path.isEmpty() && !QDir::isAbsolutePath(path) && !projectFolder.isEmpty())
                path = QDir(projectFolder).absoluteFilePath(path);
            QString name = QString::fromUtf8(info->producer->get(kShotcutCaptionProperty));
            if (name.isEmpty())
                name = QFileInfo(path).fileName();
            items.append(QJsonObject{{"index", i},
                                     {"name", name},
                                     {"path", path},
                                     {"in", info->frame_in},
                                     {"out", info->frame_out},
                                     {"duration", info->frame_count}});
        }
    }
    return QJsonObject{{"fps", MLT.profile().fps()}, {"items", items}};
}

QJsonObject getPlaylistTool(const QJsonObject &)
{
    return jsonResult(playlistJson());
}

QJsonObject getTimelineTool(const QJsonObject &)
{
    QJsonObject result{{"fps", MLT.profile().fps()},
                       {"position", 0},
                       {"duration", 0},
                       {"settings",
                        QJsonObject{{"allow_transitions", Settings.timelineAllowTransitions()},
                                    {"ripple_all_tracks", Settings.timelineRippleAllTracks()}}}};
    QJsonArray tracks;
    if (MAIN.isMultitrackValid()) {
        MultitrackModel &model = *MAIN.timelineDock()->model();
        result["position"] = MAIN.timelineDock()->position();
        result["duration"] = MAIN.multitrack()->get_playtime();
        for (int t = 0; t < model.trackList().size(); ++t)
            tracks.append(trackJson(model, t));
    }
    result["tracks"] = tracks;
    return jsonResult(result);
}

QJsonObject playlistAddTool(const QJsonObject &arguments)
{
    const QString path = arguments.value("path").toString();
    if (path.isEmpty() || isResourcePath(path) || !QDir::isAbsolutePath(path))
        return textResult("path must be an absolute file path.", true);
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile())
        return textResult("No such file: " + path, true);
    if (path.endsWith(".mlt") || path.endsWith(".xml"))
        return textResult("Project files cannot be added to the playlist; use open_project.", true);

    // Like PlaylistDock::addFiles: runs animation formats in a subprocess so a crashing file
    // cannot take Shotcut down. It shows no dialog.
    if (MLT.checkFile(path))
        return textResult("Shotcut cannot open " + path + " as media.", true);

    Mlt::Producer probe(MLT.profile(), path.toUtf8().constData());
    if (!probe.is_valid() || probe.get_length() <= 0)
        return textResult("Shotcut cannot open " + path + " as media.", true);

    // Same as Shotcut's Add Files: with nothing loaded the first file is opened so the project
    // profile follows it.
    Mlt::Producer *source = &probe;
    if (!isLoaded()) {
        // MainWindow::open() asks what to do with unsaved changes when no project is loaded.
        if (MAIN.isWindowModified()) {
            return textResult("The current project has unsaved changes. Save it first.", true);
        }
        Mlt::Properties properties;
        properties.set(kShotcutSkipConvertProperty, 1);
        MAIN.open(path, &properties, false);
        if (isLoaded())
            source = MLT.producer();
    }
    QScopedPointer<Mlt::Producer> producer(MLT.setupNewProducer(source));
    if (!producer || !producer->is_valid())
        return textResult("Shotcut could not prepare " + path + ".", true);
    producer->set(kShotcutSkipConvertProperty, 1);
    // Generators (producers without a finite length) would need Shotcut's duration dialog.
    // Still images do not: setupNewProducer gives them the default image duration. A probe is
    // not reported live before setupNewProducer, so this can only be checked here.
    if (MLT.isLiveProducer(producer.data()) && !producer->get_int(kShotcutVirtualClip))
        return textResult("Generators are not supported by playlist_add.", true);

    ProxyManager::generateIfNotExists(*producer);
    MAIN.undoStack()->push(
        new Playlist::AppendCommand(*MAIN.playlistDock()->model(), MLT.XML(producer.data())));
    return jsonResult(playlistJson());
}

// Validates a track index; with needVideo the track must be a video track.
bool validTrack(MultitrackModel &model, int track, QString &error, bool needVideo = false)
{
    if (track < 0 || track >= model.trackList().size()) {
        error = QStringLiteral("There is no track %1; call get_timeline.").arg(track);
        return false;
    }
    if (needVideo && model.trackList().at(track).type != VideoTrackType) {
        error = QStringLiteral("Track %1 is not a video track.").arg(track);
        return false;
    }
    if (MAIN.timelineDock()->isTrackLocked(track)) {
        error = QStringLiteral("Track %1 is locked.").arg(track);
        return false;
    }
    return true;
}

// Index 0 is the topmost video track, so V1 (the lowest) is the last video track.
int lowestVideoTrack(MultitrackModel &model)
{
    for (int t = model.trackList().size() - 1; t >= 0; --t) {
        if (model.trackList().at(t).type == VideoTrackType)
            return t;
    }
    return -1;
}

// Resolves (track, clip) to a plain clip; returns false with a message otherwise.
bool plainClip(MultitrackModel &model, int track, int clip, QString &error)
{
    if (!validTrack(model, track, error))
        return false;
    if (clip < 0 || clip >= model.rowCount(model.index(track))) {
        error = QStringLiteral("Track %1 has no clip %2; call get_timeline.").arg(track).arg(clip);
        return false;
    }
    if (kindOf(model, track, clip) != "clip") {
        error = QStringLiteral("Item %1 on track %2 is a %3, not a clip.")
                    .arg(clip)
                    .arg(track)
                    .arg(kindOf(model, track, clip));
        return false;
    }
    return true;
}

QJsonObject timelineAddPlaylistTool(const QJsonObject &arguments)
{
    int track = 0;
    const FrameArg trackArg = readFrame(arguments, "track", track);
    if (trackArg == FrameArg::Invalid)
        return textResult("track must be a non-negative integer.", true);
    Mlt::Playlist *playlist = MAIN.playlist();
    if (!playlist || !playlist->is_valid() || playlist->count() <= 0)
        return textResult("The playlist is empty; use playlist_add first.", true);

    TimelineDock *dock = MAIN.timelineDock();
    QString error;
    if (MAIN.isMultitrackValid()) {
        MultitrackModel &model = *dock->model();
        if (trackArg == FrameArg::Missing)
            track = lowestVideoTrack(model);
        if (track < 0 || !validTrack(model, track, error, true))
            return textResult(error.isEmpty() ? QStringLiteral("There is no video track.") : error,
                              true);
    } else if (trackArg == FrameArg::Ok && track != 0) {
        return textResult("There is no timeline yet; omit track (a video track is created).", true);
    }

    Mlt::Playlist copy(MLT.profile());
    for (int i = 0; i < playlist->count(); ++i) {
        QScopedPointer<Mlt::ClipInfo> info(playlist->clip_info(i));
        if (info && info->producer && info->producer->is_valid())
            copy.append(*info->producer, info->frame_in, info->frame_out);
    }
    if (copy.count() <= 0)
        return textResult("The playlist has no usable items.", true);

    if (MAIN.isMultitrackValid())
        dock->setCurrentTrack(track);
    MLT.pause();
    dock->appendFromPlaylist(&copy, false, false);
    if (!MAIN.isMultitrackValid())
        return textResult("Shotcut did not create the timeline.", true);
    MultitrackModel &model = *dock->model();
    const int target = trackArg == FrameArg::Ok ? track : lowestVideoTrack(model);
    return jsonResult(QJsonObject{{"appended", copy.count()}, {"track", trackJson(model, target)}});
}

QJsonObject timelineSplitTool(const QJsonObject &arguments)
{
    int track = 0, position = 0;
    if (readFrame(arguments, "track", track) != FrameArg::Ok
        || readFrame(arguments, "position", position) != FrameArg::Ok)
        return textResult("track and position must be non-negative integers.", true);
    if (!MAIN.isMultitrackValid())
        return textResult("There is no timeline.", true);
    TimelineDock *dock = MAIN.timelineDock();
    MultitrackModel &model = *dock->model();
    QString error;
    if (!validTrack(model, track, error))
        return textResult(error, true);
    const int clip = dock->clipIndexAtPosition(track, position);
    if (clip < 0 || !plainClip(model, track, clip, error))
        return textResult(error.isEmpty() ? QStringLiteral("No clip at frame %1.").arg(position)
                                          : error,
                          true);
    std::unique_ptr<Mlt::ClipInfo> info = model.getClipInfo(track, clip);
    if (!info || position <= info->start || position >= info->start + info->frame_count)
        return textResult("position must be strictly inside the clip (it starts at frame "
                              + QString::number(info ? info->start : 0) + ").",
                          true);
    MLT.pause();
    if (!dock->split(track, clip, position))
        return textResult("Shotcut refused the split.", true);
    return jsonResult(QJsonObject{{"track", trackJson(model, track)}});
}

QJsonObject timelineRemoveTool(const QJsonObject &arguments)
{
    int track = 0, clip = 0;
    if (readFrame(arguments, "track", track) != FrameArg::Ok
        || readFrame(arguments, "clip", clip) != FrameArg::Ok)
        return textResult("track and clip must be non-negative integers.", true);
    const QJsonValue rippleValue = arguments.value("ripple");
    if (!rippleValue.isUndefined() && !rippleValue.isBool())
        return textResult("ripple must be a boolean.", true);
    const bool ripple = rippleValue.toBool(true);
    if (!MAIN.isMultitrackValid())
        return textResult("There is no timeline.", true);
    TimelineDock *dock = MAIN.timelineDock();
    MultitrackModel &model = *dock->model();
    QString error;
    if (!plainClip(model, track, clip, error))
        return textResult(error, true);

    // Lifting consolidates blanks, so item indexes are not stable; count the clips instead.
    const auto clipCount = [&]() {
        int n = 0;
        for (int c = 0; c < model.rowCount(model.index(track)); ++c)
            n += kindOf(model, track, c) == "clip" ? 1 : 0;
        return n;
    };
    const int before = clipCount();
    MLT.pause();
    if (ripple)
        dock->remove(track, clip);
    else
        dock->lift(track, clip);
    if (clipCount() != before - 1)
        return textResult("Shotcut did not remove the clip.", true);
    return jsonResult(QJsonObject{{"track", trackJson(model, track)}});
}

QJsonObject timelineTrimTool(const QJsonObject &arguments)
{
    int track = 0, clip = 0;
    if (readFrame(arguments, "track", track) != FrameArg::Ok
        || readFrame(arguments, "clip", clip) != FrameArg::Ok)
        return textResult("track and clip must be non-negative integers.", true);
    const QString edge = arguments.value("edge").toString();
    if (edge != "in" && edge != "out")
        return textResult("edge must be \"in\" or \"out\".", true);
    const QJsonValue frameValue = arguments.value("frame");
    const QJsonValue deltaValue = arguments.value("delta");
    if (frameValue.isUndefined() == deltaValue.isUndefined())
        return textResult("Pass exactly one of frame or delta.", true);
    const QJsonValue rippleValue = arguments.value("ripple");
    if (!rippleValue.isUndefined() && !rippleValue.isBool())
        return textResult("ripple must be a boolean.", true);
    const bool ripple = rippleValue.toBool(false);

    if (!MAIN.isMultitrackValid())
        return textResult("There is no timeline.", true);
    TimelineDock *dock = MAIN.timelineDock();
    MultitrackModel &model = *dock->model();
    QString error;
    if (!plainClip(model, track, clip, error))
        return textResult(error, true);
    std::unique_ptr<Mlt::ClipInfo> info = model.getClipInfo(track, clip);
    if (!info)
        return textResult("Cannot read the clip.", true);

    int delta = 0;
    if (!frameValue.isUndefined()) {
        int frame = 0;
        if (readFrame(arguments, "frame", frame) != FrameArg::Ok)
            return textResult("frame must be a non-negative integer.", true);
        // The edge moves to `frame`; for out the clip then ends just before it.
        delta = edge == "in" ? frame - info->start : info->start + info->frame_count - frame;
    } else {
        const double d = deltaValue.toDouble();
        if (!deltaValue.isDouble() || d != std::floor(d) || qAbs(d) > 1e9)
            return textResult("delta must be an integer number of frames.", true);
        delta = static_cast<int>(d);
    }
    if (delta != 0) {
        const bool valid = edge == "in" ? model.trimClipInValid(track, clip, delta, ripple)
                                        : model.trimClipOutValid(track, clip, delta, ripple);
        if (!valid)
            return textResult("Shotcut cannot trim this edge by " + QString::number(delta)
                                  + " frames (source limits, a neighbour or a transition).", true);
        MLT.pause();
        if (edge == "in")
            MAIN.undoStack()->push(new Timeline::TrimClipInCommand(
                model, *dock->markersModel(), track, clip, delta, ripple));
        else
            MAIN.undoStack()->push(new Timeline::TrimClipOutCommand(
                model, *dock->markersModel(), track, clip, delta, ripple));
    }
    return jsonResult(QJsonObject{{"track", trackJson(model, track)}});
}

QJsonObject timelineMoveTool(const QJsonObject &arguments)
{
    int track = 0, clip = 0, toStart = 0;
    if (readFrame(arguments, "track", track) != FrameArg::Ok
        || readFrame(arguments, "clip", clip) != FrameArg::Ok
        || readFrame(arguments, "to_start", toStart) != FrameArg::Ok)
        return textResult("track, clip and to_start must be non-negative integers.", true);
    int toTrack = track;
    const FrameArg toTrackArg = readFrame(arguments, "to_track", toTrack);
    if (toTrackArg == FrameArg::Invalid)
        return textResult("to_track must be a non-negative integer.", true);

    if (!MAIN.isMultitrackValid())
        return textResult("There is no timeline.", true);
    TimelineDock *dock = MAIN.timelineDock();
    MultitrackModel &model = *dock->model();
    QString error;
    if (!plainClip(model, track, clip, error) || !validTrack(model, toTrack, error))
        return textResult(error, true);
    if (model.trackList().at(track).type != model.trackList().at(toTrack).type)
        return textResult("Clips cannot move between video and audio tracks.", true);
    std::unique_ptr<Mlt::ClipInfo> info = model.getClipInfo(track, clip);
    if (!info)
        return textResult("Cannot read the clip.", true);

    const int positionDelta = toStart - info->start;
    const int trackDelta = toTrack - track;
    if (positionDelta == 0 && trackDelta == 0)
        return jsonResult(QJsonObject{{"track", trackJson(model, track)}});

    // Only free space is allowed: refuse any overlap with a clip or transition on the target track.
    const int end = toStart + info->frame_count;
    const QModelIndex targetIndex = model.index(toTrack);
    for (int c = 0; c < model.rowCount(targetIndex); ++c) {
        if ((toTrack == track && c == clip) || kindOf(model, toTrack, c) == "blank")
            continue;
        std::unique_ptr<Mlt::ClipInfo> other = model.getClipInfo(toTrack, c);
        if (other && toStart < other->start + other->frame_count && end > other->start)
            return textResult("The clip would overlap item " + QString::number(c)
                                  + " on track " + QString::number(toTrack) + ".", true);
    }

    MLT.pause();
    auto *command = new Timeline::MoveClipCommand(*dock, trackDelta, positionDelta, false);
    command->addClip(track, clip);
    MAIN.undoStack()->push(command);
    QJsonObject result{{"track", trackJson(model, toTrack)}};
    if (toTrack != track)
        result["from_track"] = trackJson(model, track);
    return jsonResult(result);
}

QJsonObject historyTool(bool undo)
{
    QUndoStack *stack = MAIN.undoStack();
    if (undo ? !stack->canUndo() : !stack->canRedo())
        return textResult(undo ? "There is nothing to undo." : "There is nothing to redo.", true);
    const QString text = undo ? stack->undoText() : stack->redoText();
    MLT.pause();
    if (undo)
        stack->undo();
    else
        stack->redo();
    return jsonResult(QJsonObject{{"text", text},
                                  {"can_undo", stack->canUndo()},
                                  {"can_redo", stack->canRedo()}});
}

QJsonObject undoTool(const QJsonObject &)
{
    return historyTool(true);
}

QJsonObject redoTool(const QJsonObject &)
{
    return historyTool(false);
}

} // namespace

QList<McpTool> McpTools::timelineTools()
{
    return {
        {"get_playlist",
         "List the playlist items in order: index, name, path, in, out (source frames) and "
         "duration (frames).",
         schema(R"({"type":"object","properties":{}})"),
         getPlaylistTool},
        {"get_timeline",
         "Describe the timeline: fps, playhead position, duration and every track (index 0 is the "
         "TOPMOST video track) with its clips. Items have kind clip, blank or transition and "
         "start, duration, in, out, name, path, fade_in, fade_out. tracks is empty when no "
         "timeline exists.",
         schema(R"({"type":"object","properties":{}})"),
         getTimelineTool},
        {"playlist_add",
         "Add a media file (absolute path) to the end of the playlist. Still images are added "
         "with Shotcut's default image duration. Project files and generators are not supported. With nothing loaded the first file also becomes the "
         "loaded clip, as in Shotcut's Add Files. Returns the playlist.",
         schema(R"({"type":"object","properties":{
             "path":{"type":"string","description":"Absolute path of a media file."}
           },"required":["path"]})"),
         playlistAddTool},
        {"timeline_add_playlist",
         "Append every playlist item to the end of a video track as ONE undo step (default the "
         "lowest video track V1; the timeline is created if none exists). Track 0 is the TOPMOST "
         "video track. Returns {appended, track}.",
         schema(R"({"type":"object","properties":{
             "track":{"type":"integer","minimum":0,"description":"Video track index (0 is the topmost). Default V1."}
           }})"),
         timelineAddPlaylistTool},
        {"timeline_split",
         "Split the clip at an absolute timeline frame (strictly inside the clip). Track 0 is the "
         "TOPMOST video track. Clip indexes after the split shift by one; the returned track shows "
         "the new indexes.",
         schema(R"({"type":"object","properties":{
             "track":{"type":"integer","minimum":0},
             "position":{"type":"integer","minimum":0,"description":"Absolute timeline frame."}
           },"required":["track","position"]})"),
         timelineSplitTool},
        {"timeline_remove",
         "Remove a clip. ripple true (default) closes the gap; ripple false turns the lifted span "
         "into a blank (blanks next to it merge, so later item indexes can shift). Track 0 is the TOPMOST video track. Returns the updated track.",
         schema(R"({"type":"object","properties":{
             "track":{"type":"integer","minimum":0},
             "clip":{"type":"integer","minimum":0,"description":"Item index from get_timeline."},
             "ripple":{"type":"boolean"}
           },"required":["track","clip"]})"),
         timelineRemoveTool},
        {"timeline_trim",
         "Trim a clip edge (track 0 is the TOPMOST video track). Pass exactly one of frame (the absolute timeline frame the edge moves "
         "to; for out the clip then ends just before it) or delta (frames: for in positive moves "
         "the in-point later, for out positive shortens the clip). ripple false (default) leaves a "
         "gap. Consecutive trims of the same clip may merge into one undo step.",
         schema(R"({"type":"object","properties":{
             "track":{"type":"integer","minimum":0},
             "clip":{"type":"integer","minimum":0},
             "edge":{"type":"string","enum":["in","out"]},
             "frame":{"type":"integer","minimum":0},
             "delta":{"type":"integer"},
             "ripple":{"type":"boolean"}
           },"required":["track","clip","edge"]})"),
         timelineTrimTool},
        {"timeline_move",
         "Move a clip to an absolute start frame (track 0 is the TOPMOST video track), optionally to another track of the same type. "
         "Only free space is allowed (overlaps are refused); nothing ripples.",
         schema(R"({"type":"object","properties":{
             "track":{"type":"integer","minimum":0},
             "clip":{"type":"integer","minimum":0},
             "to_start":{"type":"integer","minimum":0},
             "to_track":{"type":"integer","minimum":0}
           },"required":["track","clip","to_start"]})"),
         timelineMoveTool},
        {"undo",
         "Undo the last edit in Shotcut's history. This also undoes edits made in the Shotcut "
         "window. Returns {text, can_undo, can_redo}.",
         schema(R"({"type":"object","properties":{}})"),
         undoTool},
        {"redo",
         "Redo the last undone edit. Returns {text, can_undo, can_redo}.",
         schema(R"({"type":"object","properties":{}})"),
         redoTool},
    };
}
