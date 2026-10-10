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
            QJsonObject item{{"index", i},
                             {"name", name},
                             {"path", path},
                             {"in", info->frame_in},
                             {"out", info->frame_out},
                             {"duration", info->frame_count}};
            addMediaFields(*info->producer, item);
            items.append(item);
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
                                    {"ripple_all_tracks", Settings.timelineRippleAllTracks()},
                                    {"ripple_markers", Settings.timelineRippleMarkers()}}}};
    QJsonArray tracks;
    if (MAIN.isMultitrackValid()) {
        MultitrackModel &model = *MAIN.timelineDock()->model();
        // The engine's playhead, not TimelineDock::position(): that is a display cache, -1 on a
        // new timeline until the preview shows its first frame.
        result["position"] = MAIN.multitrack()->position();
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
    QString error;
    if (!checkMediaPath(path, error))
        return textResult(error, true);
    if (path.endsWith(".mlt", Qt::CaseInsensitive) || path.endsWith(".xml", Qt::CaseInsensitive))
        return textResult("Project files cannot be added to the playlist; use open_project.", true);

    std::unique_ptr<Mlt::Producer> producer = newMediaProducer(path, "playlist_add", true, error);
    if (!producer)
        return textResult(error, true);

    ProxyManager::generateIfNotExists(*producer);
    MAIN.undoStack()->push(
        new Playlist::AppendCommand(*MAIN.playlistDock()->model(), MLT.XML(producer.get())));
    return jsonResult(playlistJson());
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
    EditPreviewGuard previewGuard(false);
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
    EditPreviewGuard previewGuard(false);
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
    EditPreviewGuard previewGuard(false);
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
        if (readFrame(arguments, "frame", frame) != FrameArg::Ok || frame > 1000000000)
            return textResult("frame must be a non-negative integer (at most 1e9).", true);
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
        if (delta < 0) {
            // trimClipIn/Out silently clamp an extension to the neighbouring blank.
            const int neighbour = edge == "in" ? clip - 1 : clip + 1;
            if (neighbour >= 0 && neighbour < model.rowCount(model.index(track))
                && kindOf(model, track, neighbour) == "blank") {
                std::unique_ptr<Mlt::ClipInfo> gap = model.getClipInfo(track, neighbour);
                if (gap && gap->frame_count < -delta)
                    return textResult("The neighbouring gap is only "
                                          + QString::number(gap->frame_count) + " frames.",
                                      true);
            }
        }
        EditPreviewGuard previewGuard(false);
        // A macro never merges with other trims (merging matches on clip index, which can shift).
        MAIN.undoStack()->beginMacro(edge == "in" ? QObject::tr("Trim clip in point")
                                                  : QObject::tr("Trim clip out point"));
        if (edge == "in")
            MAIN.undoStack()->push(new Timeline::TrimClipInCommand(
                model, *dock->markersModel(), track, clip, delta, ripple));
        else
            MAIN.undoStack()->push(new Timeline::TrimClipOutCommand(
                model, *dock->markersModel(), track, clip, delta, ripple));
        MAIN.undoStack()->endMacro();
    }
    return jsonResult(QJsonObject{{"track", trackJson(model, track)}});
}

QJsonObject timelineMoveTool(const QJsonObject &arguments)
{
    int track = 0, clip = 0, toStart = 0;
    if (readFrame(arguments, "track", track) != FrameArg::Ok
        || readFrame(arguments, "clip", clip) != FrameArg::Ok
        || readFrame(arguments, "to_start", toStart) != FrameArg::Ok || toStart > 1000000000)
        return textResult("track, clip and to_start must be non-negative integers "
                          "(to_start at most 1e9).",
                          true);
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

    for (const int neighbour : {clip - 1, clip + 1}) {
        if (neighbour >= 0 && neighbour < model.rowCount(model.index(track))
            && kindOf(model, track, neighbour) == "transition")
            return textResult("Clip " + QString::number(clip) + " on track "
                                  + QString::number(track)
                                  + " is joined to a transition; remove or trim the transition "
                                    "first.",
                              true);
    }
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

    EditPreviewGuard previewGuard(false);
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
    // Undoing or redoing a macro runs all its commands at once; one with more than 8 children
    // (a timeline_remove_ranges call of 3 or more cuts) gets the preview stand-in, smaller steps
    // only the refresh block, so that many small undos cause no preview flicker.
    const QUndoCommand *command = stack->command(undo ? stack->index() - 1 : stack->index());
    EditPreviewGuard previewGuard(command && command->childCount() > 8);
    if (undo)
        stack->undo();
    else
        stack->redo();
    return jsonResult(QJsonObject{{"text", text},
                                  {"can_undo", stack->canUndo()},
                                  {"can_redo", stack->canRedo()}});
}

QJsonObject timelineAddTransitionTool(const QJsonObject &arguments)
{
    int track = 0, clip = 0, duration = 0;
    if (readFrame(arguments, "track", track) != FrameArg::Ok
        || readFrame(arguments, "clip", clip) != FrameArg::Ok
        || readFrame(arguments, "duration", duration) != FrameArg::Ok || duration < 1)
        return textResult("track and clip must be non-negative integers and duration at least 1.",
                          true);
    if (!MAIN.isMultitrackValid())
        return textResult("There is no timeline.", true);
    TimelineDock *dock = MAIN.timelineDock();
    MultitrackModel &model = *dock->model();
    QString error;
    if (!plainClip(model, track, clip, error))
        return textResult(error, true);
    if (clip < 1 || kindOf(model, track, clip - 1) != "clip")
        return textResult("The item before the incoming clip must be a plain clip (no blank, "
                          "transition or first position).",
                          true);
    std::unique_ptr<Mlt::ClipInfo> previous = model.getClipInfo(track, clip - 1);
    std::unique_ptr<Mlt::ClipInfo> incoming = model.getClipInfo(track, clip);
    if (!previous || !incoming)
        return textResult("Cannot read the clips.", true);
    if (previous->start + previous->frame_count != incoming->start)
        return textResult("The two clips must touch (no gap between them).", true);
    if (duration >= previous->frame_count || duration >= incoming->frame_count)
        return textResult("duration must be shorter than both clips.", true);

    if (clip + 1 < model.rowCount(model.index(track))
        && kindOf(model, track, clip + 1) == "transition")
        return textResult("The incoming clip already has a transition after it.", true);

    const int position = incoming->start - duration;
    // Ripple, so the clips after the incoming one close up by duration (no gap opens).
    if (!model.addTransitionValid(track, track, clip, position, true))
        return textResult("Shotcut cannot add a transition between these clips.", true);

    EditPreviewGuard previewGuard(false);
    auto *command = new Timeline::AddTransitionCommand(*dock, track, clip, position, true);
    MAIN.undoStack()->push(command);
    const int transitionIndex = command->getTransitionIndex();
    if (transitionIndex < 0 || kindOf(model, track, transitionIndex) != "transition") {
        // Revert whatever was applied (undo() does nothing when no transition was created), then
        // mark the command obsolete so the stack drops it instead of keeping it on the redo stack.
        // QUndoStack::undo() skips the undo() of an obsolete command, so the order matters.
        command->undo();
        command->setObsolete(true);
        MAIN.undoStack()->undo();
        return textResult("Shotcut did not create the transition.", true);
    }
    return jsonResult(QJsonObject{{"transition_index", transitionIndex},
                                  {"track", trackJson(model, track)}});
}

QJsonObject timelineFadeTool(const QJsonObject &arguments)
{
    int track = 0, clip = 0, fadeIn = -1, fadeOut = -1;
    if (readFrame(arguments, "track", track) != FrameArg::Ok
        || readFrame(arguments, "clip", clip) != FrameArg::Ok)
        return textResult("track and clip must be non-negative integers.", true);
    const FrameArg inArg = readFrame(arguments, "fade_in", fadeIn);
    const FrameArg outArg = readFrame(arguments, "fade_out", fadeOut);
    if (inArg == FrameArg::Invalid || outArg == FrameArg::Invalid)
        return textResult("fade_in and fade_out must be non-negative integers.", true);
    if (inArg == FrameArg::Missing && outArg == FrameArg::Missing)
        return textResult("Pass fade_in and/or fade_out.", true);
    if (!MAIN.isMultitrackValid())
        return textResult("There is no timeline.", true);
    TimelineDock *dock = MAIN.timelineDock();
    MultitrackModel &model = *dock->model();
    QString error;
    if (!plainClip(model, track, clip, error))
        return textResult(error, true);
    std::unique_ptr<Mlt::ClipInfo> info = model.getClipInfo(track, clip);
    if (!info || (inArg == FrameArg::Ok && fadeIn > info->frame_count)
        || (outArg == FrameArg::Ok && fadeOut > info->frame_count))
        return textResult("A fade cannot be longer than the clip.", true);

    EditPreviewGuard previewGuard(false);
    MAIN.undoStack()->beginMacro(inArg != FrameArg::Ok    ? QObject::tr("Set clip fade out")
                                 : outArg != FrameArg::Ok ? QObject::tr("Set clip fade in")
                                                          : QObject::tr("Set clip fades"));
    if (inArg == FrameArg::Ok)
        dock->fadeIn(track, clip, fadeIn);
    if (outArg == FrameArg::Ok)
        dock->fadeOut(track, clip, fadeOut);
    MAIN.undoStack()->endMacro();
    return jsonResult(QJsonObject{{"track", trackJson(model, track)}});
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
         "duration (frames). Items also carry the source media's width, height, fps, has_audio and "
         "rotation when known (omitted otherwise). has_audio reflects the producer's selected "
         "audio stream, so a user-disabled audio stream may report false; proxy clips may report "
         "the proxy's dimensions.",
         schema(R"({"type":"object","properties":{}})"),
         getPlaylistTool},
        {"get_timeline",
         "Describe the timeline: fps, playhead position, duration and every track (index 0 is the "
         "TOPMOST video track) with its index, name, type, locked, muted, hidden, gain_db (track "
         "gain in dB, 0 = unchanged; with keyframes the level at the timeline's gain position), "
         "composite (video tracks only: blends over the tracks below) and clips. Items have "
         "kind clip, blank or transition and start, duration, in, out, name, path, fade_in, "
         "fade_out; items of kind clip also carry "
         "the source media's width, height, fps, has_audio and rotation when known (omitted "
         "otherwise; has_audio reflects the producer's selected audio stream, so a user-disabled "
         "audio stream may report false; proxy clips may report the proxy's dimensions). tracks "
         "is empty when no timeline exists. settings reports Shotcut's own allow_transitions, "
         "ripple_all_tracks and ripple_markers timeline settings.",
         schema(R"({"type":"object","properties":{}})"),
         getTimelineTool},
        {"playlist_add",
         "Add a media file (absolute path) to the end of the playlist. Still images are added "
         "with Shotcut's default image duration. Project files (.mlt/.xml, any case) and "
         "generators are not supported. With nothing loaded the first file also becomes the "
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
         "into a blank (blanks next to it merge, so later item indexes can shift). Track 0 is the "
         "TOPMOST video track. Returns the updated track.",
         schema(R"({"type":"object","properties":{
             "track":{"type":"integer","minimum":0},
             "clip":{"type":"integer","minimum":0,"description":"Item index from get_timeline."},
             "ripple":{"type":"boolean"}
           },"required":["track","clip"]})"),
         timelineRemoveTool},
        {"timeline_trim",
         "Trim the in or out edge of a clip. Pass EXACTLY ONE of frame or delta. "
         "frame is the absolute timeline frame the edge moves to; for edge out the clip then ends "
         "just before that frame. delta is the number of frames to move the edge: for in, "
         "positive moves the in-point later (the clip gets shorter at its start); for out, "
         "positive shortens the end. A negative delta extends the clip. With ripple false it "
         "needs an adjacent gap at least as long as the extension (for out on the last item the "
         "end of the track is free) and is refused next to a clip or transition. With ripple "
         "true the clip gets longer and later content is pushed later. Either way it is refused "
         "when an adjacent gap is shorter than the extension or the source runs out. With "
         "ripple false a positive delta is refused next to a transition. A trim that leaves "
         "zero length is refused; delta 0 does nothing. "
         "ripple false (default) leaves a gap where frames were removed; ripple true closes it "
         "(later clips move earlier). With edge in and ripple true the clip keeps its start "
         "frame, so the edge does not end at frame. Every trim is ONE undo step. Returns the "
         "updated track; clip indexes may shift. Track 0 is the TOPMOST video track.",
         schema(R"({"type":"object","properties":{
             "track":{"type":"integer","minimum":0},
             "clip":{"type":"integer","minimum":0},
             "edge":{"type":"string","enum":["in","out"]},
             "frame":{"type":"integer","minimum":0,"description":"Absolute timeline frame the edge moves to."},
             "delta":{"type":"integer","description":"Frames to move the edge; positive shortens the clip, negative extends it."},
             "ripple":{"type":"boolean"}
           },"required":["track","clip","edge"]})"),
         timelineTrimTool},
        {"timeline_move",
         "Move a clip to an absolute start frame (track 0 is the TOPMOST video track), optionally "
         "to another track of the same type. "
         "Only free space is allowed (overlaps are refused); nothing ripples. A clip joined to a "
         "transition is refused. Returns the updated track (and from_track, the source track, "
         "when the track changes).",
         schema(R"({"type":"object","properties":{
             "track":{"type":"integer","minimum":0},
             "clip":{"type":"integer","minimum":0},
             "to_start":{"type":"integer","minimum":0},
             "to_track":{"type":"integer","minimum":0}
           },"required":["track","clip","to_start"]})"),
         timelineMoveTool},
        {"timeline_add_transition",
         "Add Shotcut's default dissolve (with an audio cross-fade) between two touching clips on "
         "one track (track 0 is the TOPMOST video track). clip is the INCOMING (later) clip; "
         "duration frames of overlap, shorter than both clips. The clips overlap, so the timeline "
         "gets shorter by duration: later clips move earlier by duration (with Shotcut's "
         "ripple-all-tracks setting on, duration frames are also removed from the other "
         "unlocked tracks). "
         "The incoming clip must not already have a transition after it. Returns the updated "
         "track and transition_index.",
         schema(R"({"type":"object","properties":{
             "track":{"type":"integer","minimum":0},
             "clip":{"type":"integer","minimum":1,"description":"Index of the incoming clip."},
             "duration":{"type":"integer","minimum":1,"description":"Overlap in frames."}
           },"required":["track","clip","duration"]})"),
         timelineAddTransitionTool},
        {"timeline_fade",
         "Set a clip's fade-in and/or fade-out length in frames (0 removes the fade; track 0 is "
         "the TOPMOST video track). One call is one undo step. Returns the updated track.",
         schema(R"({"type":"object","properties":{
             "track":{"type":"integer","minimum":0},
             "clip":{"type":"integer","minimum":0},
             "fade_in":{"type":"integer","minimum":0},
             "fade_out":{"type":"integer","minimum":0}
           },"required":["track","clip"]})"),
         timelineFadeTool},
        {"undo",
         "Undo the last edit in Shotcut's history. This also undoes edits made in the Shotcut "
         "window. Returns {text, can_undo, can_redo}.",
         schema(R"({"type":"object","properties":{}})"),
         undoTool},
        {"redo",
         "Redo the last undone edit. This also redoes edits made in the Shotcut window. Returns "
         "{text, can_undo, can_redo}.",
         schema(R"({"type":"object","properties":{}})"),
         redoTool},
    };
}
