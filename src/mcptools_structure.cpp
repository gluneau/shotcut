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

#include "commands/timelinecommands.h"
#include "docks/timelinedock.h"
#include "mainwindow.h"
#include "mltcontroller.h"
#include "models/markersmodel.h"
#include "models/multitrackmodel.h"
#include "settings.h"

#include <MltPlaylist.h>

#include <QRegularExpression>
#include <QScopedPointer>
#include <QUndoStack>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <vector>

using namespace McpTools;

namespace {

// A finite number from arguments[key].
FrameArg readSeconds(const QJsonObject &arguments, const char *key, double &value)
{
    const QJsonValue v = arguments.value(key);
    if (v.isUndefined())
        return FrameArg::Missing;
    if (!v.isDouble() || !std::isfinite(v.toDouble()))
        return FrameArg::Invalid;
    value = v.toDouble();
    return FrameArg::Ok;
}

QJsonObject timelineInsertRangeTool(const QJsonObject &arguments)
{
    int track = 0, position = 0;
    if (readFrame(arguments, "track", track) != FrameArg::Ok
        || readFrame(arguments, "position", position) != FrameArg::Ok || position > 1000000000)
        return textResult("track and position must be non-negative integers (position at most "
                          "1e9).",
                          true);
    const QJsonValue modeValue = arguments.value("mode");
    const QString mode = modeValue.isUndefined() ? QStringLiteral("insert") : modeValue.toString();
    if (mode != "insert" && mode != "overwrite")
        return textResult("mode must be \"insert\" or \"overwrite\".", true);
    double inS = 0, outS = 0;
    const FrameArg inArg = readSeconds(arguments, "in_s", inS);
    const FrameArg outArg = readSeconds(arguments, "out_s", outS);
    if (inArg == FrameArg::Invalid || outArg == FrameArg::Invalid)
        return textResult("in_s and out_s must be numbers (seconds).", true);
    const bool hasPath = !arguments.value("path").isUndefined();
    const bool hasIndex = !arguments.value("playlist_index").isUndefined();
    if (hasPath == hasIndex)
        return textResult("Pass exactly one of path or playlist_index.", true);
    if ((inArg == FrameArg::Missing) != (outArg == FrameArg::Missing))
        return textResult("Pass both in_s and out_s, or neither (only with playlist_index).", true);
    if (hasPath && inArg == FrameArg::Missing)
        return textResult("in_s and out_s are required with path.", true);
    if (inArg == FrameArg::Ok && (inS < 0 || inS >= outS))
        return textResult("in_s must be at least 0 and less than out_s.", true);

    QString path;
    int in = -1, out = -1;
    if (hasIndex) {
        int index = 0;
        if (readFrame(arguments, "playlist_index", index) != FrameArg::Ok)
            return textResult("playlist_index must be a non-negative integer.", true);
        Mlt::Playlist *playlist = MAIN.playlist();
        QScopedPointer<Mlt::ClipInfo> info(
            playlist && playlist->is_valid() && index < playlist->count()
                ? playlist->clip_info(index)
                : nullptr);
        if (!info)
            return textResult(QStringLiteral("There is no playlist item %1; call get_playlist.")
                                  .arg(index),
                              true);
        // Rebuilt from its file at 1x, a speed-changed item would get the wrong range.
        if (!info->producer || QString(info->producer->get("mlt_service")) == "timewarp"
            || (info->producer->property_exists("warp_speed")
                && info->producer->get_double("warp_speed") != 1.0))
            return textResult(QStringLiteral("Playlist item %1 has a speed change, which is not "
                                             "carried over; insert its file by path instead.")
                                  .arg(index),
                              true);
        path = clipSourcePath(*info);
        in = info->frame_in;
        out = info->frame_out;
    } else {
        path = arguments.value("path").toString();
    }
    QString error;
    if (!checkMediaPath(path, error))
        return textResult(error, true);
    if (path.endsWith(".mlt", Qt::CaseInsensitive) || path.endsWith(".xml", Qt::CaseInsensitive))
        return textResult("Project files cannot be inserted; use open_project.", true);
    // Before newMediaProducer, which may open the file when nothing is loaded.
    if (!validTargetTrack(track, error))
        return textResult(error, true);
    std::unique_ptr<Mlt::Producer> producer = newMediaProducer(path,
                                                               "timeline_insert_range",
                                                               false,
                                                               error);
    if (!producer)
        return textResult(error, true);

    const double fps = MLT.profile().fps();
    const int length = producer->get_length();
    if (inArg == FrameArg::Ok) {
        if (outS * fps > length + 1e-6)
            return textResult(QStringLiteral("out_s is beyond the end of the file (%1 s).")
                                  .arg(length / fps),
                              true);
        in = qRound(inS * fps);
        if (qRound(outS * fps) <= in)
            return textResult("The range is shorter than one frame.", true);
        out = qMin(qRound(outS * fps) - 1, length - 1);
    }
    if (in < 0 || out < in || out >= length)
        return textResult(QStringLiteral("The range must cover at least one frame of the file "
                                         "(%1 s).")
                              .arg(length / fps),
                          true);

    // A copy, because with nothing loaded producer is the player's own clip.
    Mlt::Producer clip(MLT.profile(), "xml-string", MLT.XML(producer.get()).toUtf8().constData());
    clip.set_in_and_out(in, out);
    const int index = pushClipXml(mode,
                                  track,
                                  position,
                                  MLT.XML(&clip),
                                  mode == "insert" ? QObject::tr("Insert range into track")
                                                   : QObject::tr("Overwrite range onto track"),
                                  error);
    if (index < 0)
        return textResult(error.isEmpty() ? QStringLiteral("Shotcut did not insert the clip.")
                                          : error,
                          true);
    // InsertCommand read these in its constructor, during the push above.
    const bool insert = mode == "insert";
    const QJsonObject ripple{{"all_tracks", insert && Settings.timelineRippleAllTracks()},
                             {"markers", insert && Settings.timelineRippleMarkers()}};
    MultitrackModel &model = *MAIN.timelineDock()->model();
    std::unique_ptr<Mlt::ClipInfo> info = model.getClipInfo(track, index);
    if (!info)
        return textResult("The clip was inserted but cannot be read back; call get_timeline.",
                          true);
    const QJsonObject inserted{{"start", info->start}, {"duration", info->frame_count}};
    return jsonResult(
        QJsonObject{{"track", trackJson(model, track)}, {"inserted", inserted}, {"ripple", ripple}});
}

// Puts Shotcut's ripple settings back on every exit path. The remove commands read them in their
// constructors, so commands pushed while they were changed keep their own values on undo/redo.
// The settings are written to QSettings during the call, so a crash in the middle leaves
// ripple-all-tracks off (and ripple-markers possibly off) saved; accepted, because Shotcut offers
// no other way to choose the ripple of RemoveCommand and the user can turn them back on.
class RippleSettingsGuard
{
public:
    RippleSettingsGuard()
        : m_allTracks(Settings.timelineRippleAllTracks())
        , m_markers(Settings.timelineRippleMarkers())
    {}
    ~RippleSettingsGuard()
    {
        if (Settings.timelineRippleAllTracks() != m_allTracks)
            Settings.setTimelineRippleAllTracks(m_allTracks);
        if (Settings.timelineRippleMarkers() != m_markers)
            Settings.setTimelineRippleMarkers(m_markers);
    }
    bool markers() const { return m_markers; }

private:
    const bool m_allTracks;
    const bool m_markers;
};

// End of the last item that is not a blank; 0 for an empty track (it holds a blank placeholder).
int contentEnd(MultitrackModel &model, int track)
{
    for (int c = model.rowCount(model.index(track)) - 1; c >= 0; --c) {
        if (kindOf(model, track, c) == "blank")
            continue;
        std::unique_ptr<Mlt::ClipInfo> info = model.getClipInfo(track, c);
        return info ? info->start + info->frame_count : 0;
    }
    return 0;
}

// Splits the item under position (strictly inside it) so that position starts an item.
bool splitAt(TimelineDock *dock, MultitrackModel &model, int track, int position)
{
    const int clip = dock->clipIndexAtPosition(track, position);
    std::unique_ptr<Mlt::ClipInfo> info = clip >= 0 ? model.getClipInfo(track, clip) : nullptr;
    if (!info)
        return false;
    if (kindOf(model, track, clip) == "blank") {
        // TimelineDock::split refuses blanks; the model splits them like clips.
        MAIN.undoStack()->push(new Timeline::SplitCommand(model,
                                                          std::vector<int>{track},
                                                          std::vector<int>{clip},
                                                          position));
    } else if (!dock->split(track, clip, position)) {
        return false;
    }
    const int after = dock->clipIndexAtPosition(track, position);
    info = after >= 0 ? model.getClipInfo(track, after) : nullptr;
    return info && info->start == position;
}

// Removes (ripple) or lifts [start, end) on one track from its end backward, so the absolute
// frames before the piece being removed never move. With ripple, gaps inside the range are
// removed too, so every listed track loses exactly its part of the range.
bool cutRange(
    TimelineDock *dock, MultitrackModel &model, int track, int start, int end, bool ripple)
{
    int position = end;
    while (true) {
        const int hi = qMin(position, contentEnd(model, track));
        if (hi <= start)
            return true;
        int clip = dock->clipIndexAtPosition(track, hi - 1);
        std::unique_ptr<Mlt::ClipInfo> info = clip >= 0 ? model.getClipInfo(track, clip)
                                                        : nullptr;
        if (!info)
            return false;
        const bool blank = kindOf(model, track, clip) == "blank";
        if (blank && !ripple) {
            position = info->start;
            continue;
        }
        if ((info->start + info->frame_count > hi && !splitAt(dock, model, track, hi))
            || (info->start < start && !splitAt(dock, model, track, start)))
            return false;
        clip = dock->clipIndexAtPosition(track, hi - 1);
        info = clip >= 0 ? model.getClipInfo(track, clip) : nullptr;
        if (!info || info->start + info->frame_count != hi || info->start < start)
            return false;
        const int length = info->frame_count;
        position = info->start;
        const int before = contentEnd(model, track);
        if (ripple)
            dock->remove(track, clip, false);
        else
            dock->lift(track, clip, false);
        // Cheap checks of the one item: material after the piece moved left by its length (the
        // track can only end earlier when the piece was its last content: Shotcut drops trailing
        // gaps), or a lifted piece is now inside a gap.
        const int after = contentEnd(model, track);
        if (ripple) {
            if (hi < before ? after != before - length : after > position)
                return false;
        } else if (after > position) {
            clip = dock->clipIndexAtPosition(track, position);
            info = clip >= 0 ? model.getClipInfo(track, clip) : nullptr;
            if (!info || kindOf(model, track, clip) != "blank"
                || info->start + info->frame_count < hi)
                return false;
        }
    }
}

QJsonObject timelineRemoveRangesTool(const QJsonObject &arguments)
{
    const QJsonValue tracksValue = arguments.value("tracks");
    const QJsonArray tracksArray = tracksValue.toArray();
    if (!tracksValue.isArray() || tracksArray.isEmpty() || tracksArray.size() > 8)
        return textResult("tracks must be an array of 1 to 8 track indexes.", true);
    QList<int> tracks;
    for (const QJsonValue &v : tracksArray) {
        int track = 0;
        if (readFrame(QJsonObject{{"t", v}}, "t", track) != FrameArg::Ok)
            return textResult("tracks must hold non-negative integers.", true);
        if (tracks.contains(track))
            return textResult(QStringLiteral("Track %1 is listed twice.").arg(track), true);
        tracks.append(track);
    }
    const QJsonValue rangesValue = arguments.value("ranges");
    const QJsonArray rangesArray = rangesValue.toArray();
    if (!rangesValue.isArray() || rangesArray.isEmpty() || rangesArray.size() > 500)
        return textResult("ranges must be an array of 1 to 500 {start, end} objects.", true);
    std::vector<std::pair<int, int>> ranges;
    for (const QJsonValue &v : rangesArray) {
        int start = 0, end = 0;
        if (!v.isObject() || readFrame(v.toObject(), "start", start) != FrameArg::Ok
            || readFrame(v.toObject(), "end", end) != FrameArg::Ok || start >= end)
            return textResult("Each range must be {start, end} with integer frames, 0 <= start "
                              "< end (end exclusive).",
                              true);
        ranges.emplace_back(start, end);
    }
    const QJsonValue rippleValue = arguments.value("ripple");
    if (!rippleValue.isUndefined() && !rippleValue.isBool())
        return textResult("ripple must be a boolean.", true);
    const bool ripple = rippleValue.toBool(true);
    if (!MAIN.isMultitrackValid())
        return textResult("There is no timeline.", true);
    TimelineDock *dock = MAIN.timelineDock();
    MultitrackModel &model = *dock->model();
    QString error;
    QList<int> extents;
    int longest = 0;
    for (int track : tracks) {
        if (!validTrack(model, track, error))
            return textResult(error, true);
        extents.append(contentEnd(model, track));
        longest = qMax(longest, extents.last());
    }

    std::sort(ranges.begin(), ranges.end());
    std::vector<std::pair<int, int>> merged;
    for (const auto &range : ranges) {
        if (!merged.empty() && range.first <= merged.back().second)
            merged.back().second = qMax(merged.back().second, range.second);
        else
            merged.push_back(range);
    }
    std::vector<std::pair<int, int>> applied;
    for (const auto &range : merged) {
        if (range.first < longest)
            applied.emplace_back(range.first, qMin(range.second, longest));
    }
    if (applied.empty())
        return textResult("No range lies inside the timeline.", true);
    // Every split and remove snapshots its whole track for undo and a split opens a new producer,
    // so the cost grows faster than the number of cuts (debug build: 120 cuts take about 12 s).
    // Shotcut can corrupt such a snapshot when the preview renders the clip meanwhile (an
    // upstream race, docs/mcp-server.md). Measured on a debug build, call plus undo, redo and
    // undo: with refreshes blocked only, corrupt snapshots in about 5% of runs at every cap from
    // 20 to 120 cuts; with RefreshBlock (refreshes blocked and the preview fed a stand-in) none
    // in 75 runs at 120 cuts.
    const int maxCuts = 120;
    if (int(applied.size()) * tracks.size() > maxCuts)
        return textResult(QStringLiteral("Too much work for one call: %1 ranges (after merging) on "
                                         "%2 track(s) is more than %3 range-track cuts. Send at "
                                         "most %4 ranges per call, in chunks from the END of the "
                                         "timeline to the start (ripple shifts later material), "
                                         "or re-read get_timeline between chunks. Each call is "
                                         "one undo step; call save_project before many cuts (long "
                                         "edits can corrupt Shotcut's undo history). Nothing was "
                                         "changed.")
                              .arg(applied.size())
                              .arg(tracks.size())
                              .arg(maxCuts)
                              .arg(maxCuts / tracks.size()),
                          true);

    QJsonObject removedFrames;
    for (int i = 0; i < tracks.size(); ++i) {
        const int track = tracks.at(i);
        // Removing or lifting a clip a transition joins breaks the transition.
        QList<int> transitions;
        for (int c = 0; c < model.rowCount(model.index(track)); ++c) {
            if (kindOf(model, track, c) == "transition")
                transitions.append(c);
        }
        int removed = 0;
        for (const auto &range : applied) {
            const int end = qMin(range.second, extents.at(i));
            removed += qMax(0, end - range.first);
            if (range.first >= end)
                continue;
            for (int transition : transitions) {
                std::unique_ptr<Mlt::ClipInfo> t = model.getClipInfo(track, transition);
                if (!t || range.first > t->start + t->frame_count || end < t->start)
                    continue;
                return textResult(QStringLiteral("The range %1-%2 (end exclusive, after "
                                                 "clamping) touches the transition on track %3 "
                                                 "(item %4, frames %5-%6); keep ranges at least "
                                                 "one frame away from transitions.")
                                      .arg(range.first)
                                      .arg(end)
                                      .arg(track)
                                      .arg(transition)
                                      .arg(t->start)
                                      .arg(t->start + t->frame_count - 1),
                                  true);
            }
        }
        removedFrames[QString::number(track)] = removed;
    }

    RippleSettingsGuard guard;
    RefreshBlock refreshBlock;
    QUndoStack *stack = MAIN.undoStack();
    stack->beginMacro(QObject::tr("Remove %n range(s)", nullptr, int(applied.size())));
    // Each listed track is cut by itself, so other tracks must not ripple.
    if (Settings.timelineRippleAllTracks())
        Settings.setTimelineRippleAllTracks(false);
    bool ok = true;
    for (auto range = applied.rbegin(); ok && range != applied.rend(); ++range) {
        // Markers ripple once per range: with the track that loses the most of it.
        int markerTrack = -1, most = -1;
        for (int i = 0; i < tracks.size(); ++i) {
            const int frames = qMin(range->second, extents.at(i)) - range->first;
            if (frames > most) {
                most = frames;
                markerTrack = tracks.at(i);
            }
        }
        for (int track : tracks) {
            if (ripple && guard.markers()
                && Settings.timelineRippleMarkers() != (track == markerTrack))
                Settings.setTimelineRippleMarkers(track == markerTrack);
            if (!cutRange(dock, model, track, range->first, range->second, ripple)) {
                ok = false;
                break;
            }
        }
    }
    stack->endMacro();
    auto *macro = const_cast<QUndoCommand *>(stack->command(stack->index() - 1));
    if (!ok || macro->childCount() == 0) {
        // Roll the whole macro back and drop it from the stack (nothing to undo when it is
        // empty). QUndoStack::undo() skips the undo() of an obsolete command, so the order
        // matters.
        macro->undo();
        macro->setObsolete(true);
        stack->undo();
        if (!ok)
            return textResult("Shotcut did not remove a range; nothing was changed.", true);
    }

    QJsonArray tracksJson;
    for (int track : tracks)
        tracksJson.append(trackJson(model, track));
    QJsonArray rangesJson;
    for (const auto &range : applied)
        rangesJson.append(QJsonObject{{"start", range.first}, {"end", range.second}});
    return jsonResult(QJsonObject{{"tracks", tracksJson},
                                  {"removed_frames", removedFrames},
                                  {"ranges_applied", rangesJson},
                                  {"ripple",
                                   QJsonObject{{"all_tracks", false},
                                               {"markers", ripple && guard.markers()}}}});
}

QJsonArray allTracksJson(MultitrackModel &model)
{
    QJsonArray tracks;
    for (int t = 0; t < model.trackList().size(); ++t)
        tracks.append(trackJson(model, t));
    return tracks;
}

// Ends the caller's macro. When ok is false the macro is rolled back and dropped from the stack
// (QUndoStack::undo() skips the undo() of an obsolete command, so the order matters).
bool finishMacro(QUndoStack *stack, bool ok)
{
    stack->endMacro();
    if (!ok) {
        auto *macro = const_cast<QUndoCommand *>(stack->command(stack->index() - 1));
        macro->undo();
        macro->setObsolete(true);
        stack->undo();
    }
    return ok;
}

// A string of 1 to max characters (Unicode code points, so an emoji counts once) that is not only
// white space.
bool readText(const QJsonValue &v, const char *key, int max, QString &text, QString &error)
{
    text = v.toString();
    if (!v.isString() || text.isEmpty() || text.toUcs4().size() > max) {
        error = QStringLiteral("%1 must be a string of 1 to %2 characters.").arg(key).arg(max);
        return false;
    }
    if (text.trimmed().isEmpty()) {
        error = QStringLiteral("%1 must not be blank.").arg(key);
        return false;
    }
    return true;
}

// An optional track name. A null name when absent.
bool readTrackName(const QJsonObject &arguments, QString &name, QString &error)
{
    const QJsonValue v = arguments.value("name");
    return v.isUndefined() || readText(v, "name", 64, name, error);
}

QJsonObject trackAddTool(const QJsonObject &arguments)
{
    const QString type = arguments.value("type").toString();
    if (type != "video" && type != "audio")
        return textResult("type must be \"video\" or \"audio\".", true);
    QString name, error;
    if (!readTrackName(arguments, name, error))
        return textResult(error, true);
    if (!MAIN.isMultitrackValid())
        return textResult("There is no timeline; add a clip first.", true);
    TimelineDock *dock = MAIN.timelineDock();
    MultitrackModel &model = *dock->model();
    const bool video = type == "video";
    const int before = model.trackList().size();
    MLT.pause();
    // As TimelineDock::addVideoTrack and addAudioTrack do: a selected track head would point at
    // another track once the rows shift.
    if (dock->selectedTrack() != -1)
        dock->setSelection();
    QUndoStack *stack = MAIN.undoStack();
    stack->beginMacro(video ? QObject::tr("Add video track") : QObject::tr("Add audio track"));
    stack->push(new Timeline::AddTrackCommand(model, video));
    // Shotcut puts a new video track on top and a new audio track at the bottom.
    const int index = video ? 0 : model.trackList().size() - 1;
    const bool added = model.trackList().size() == before + 1
                       && model.trackList().at(index).type
                              == (video ? VideoTrackType : AudioTrackType);
    if (added && !name.isNull())
        stack->push(new Timeline::NameTrackCommand(model, index, name));
    if (!finishMacro(stack, added))
        return textResult("Shotcut did not add the track; nothing was changed.", true);
    const QString newName = model.data(model.index(index), MultitrackModel::NameRole).toString();
    return jsonResult(
        QJsonObject{{"tracks", allTracksJson(model)},
                    {"added", QJsonObject{{"index", index}, {"type", type}, {"name", newName}}}});
}

QJsonObject trackRemoveTool(const QJsonObject &arguments)
{
    int track = 0;
    if (readFrame(arguments, "track", track) != FrameArg::Ok)
        return textResult("track must be a non-negative integer.", true);
    const QJsonValue forceValue = arguments.value("force");
    if (!forceValue.isUndefined() && !forceValue.isBool())
        return textResult("force must be a boolean.", true);
    if (!MAIN.isMultitrackValid())
        return textResult("There is no timeline.", true);
    MultitrackModel &model = *MAIN.timelineDock()->model();
    QString error;
    if (!validTrack(model, track, error))
        return textResult(error, true);
    const bool video = model.trackList().at(track).type == VideoTrackType;
    // Undoing the removal of the bottom video track while other video tracks exist reconnects
    // their blend transitions to the wrong tracks (Shotcut's own Remove Track has this bug, also
    // when the track is first moved up), so the bottom video track is never removed.
    if (video && track == model.bottomVideoTrackIndex())
        return textResult(QStringLiteral("Track %1 is the bottom video track, which cannot be "
                                         "removed (a timeline needs a video track, and Shotcut "
                                         "cannot undo removing the bottom one correctly). Remove "
                                         "its clips with timeline_remove_ranges, or remove a "
                                         "video track above it. Nothing was changed.")
                              .arg(track),
                          true);
    if (!forceValue.toBool()) {
        for (int c = 0; c < model.rowCount(model.index(track)); ++c) {
            if (kindOf(model, track, c) != "blank")
                return textResult(QStringLiteral("Track %1 holds clips; pass force:true to remove "
                                                 "it with them (one undo restores them).")
                                      .arg(track),
                                  true);
        }
    }
    const QString name = model.data(model.index(track), MultitrackModel::NameRole).toString();
    const int before = model.trackList().size();
    RefreshBlock refreshBlock;
    QUndoStack *stack = MAIN.undoStack();
    stack->beginMacro(video ? QObject::tr("Remove video track")
                            : QObject::tr("Remove audio track"));
    // RemoveTrackCommand restores the clips, name and user filters but not mute, hide, gain or
    // compositing. These commands clear them on the track about to go, or change nothing, and
    // put them back when the macro is undone.
    const QModelIndex index = model.index(track);
    if (model.data(index, MultitrackModel::IsMuteRole).toBool())
        stack->push(new Timeline::MuteTrackCommand(model, track));
    // Audio tracks always report hidden (their video is hidden), so only video tracks count.
    if (video && model.data(index, MultitrackModel::IsHiddenRole).toBool())
        stack->push(new Timeline::HideTrackCommand(model, track));
    if (model.data(index, MultitrackModel::GainEnabledRole).toBool()
        && model.data(index, MultitrackModel::GainRole).toDouble() != 0)
        stack->push(new Timeline::ChangeTrackGainCommand(model, track, 0));
    if (video)
        stack->push(new Timeline::CompositeTrackCommand(
            model, track, model.data(index, MultitrackModel::IsCompositeRole).toBool()));
    stack->push(new Timeline::RemoveTrackCommand(model, track));
    if (!finishMacro(stack, model.trackList().size() == before - 1))
        return textResult("Shotcut did not remove the track; nothing was changed.", true);
    return jsonResult(QJsonObject{
        {"tracks", allTracksJson(model)},
        {"removed",
         QJsonObject{{"index", track}, {"type", video ? "video" : "audio"}, {"name", name}}}});
}

QJsonObject trackSetTool(const QJsonObject &arguments)
{
    int track = 0;
    if (readFrame(arguments, "track", track) != FrameArg::Ok)
        return textResult("track must be a non-negative integer.", true);
    QString name, error;
    if (!readTrackName(arguments, name, error))
        return textResult(error, true);
    QJsonObject flags;
    for (const char *key : {"mute", "lock", "hide", "composite"}) {
        const QJsonValue v = arguments.value(key);
        if (v.isUndefined())
            continue;
        if (!v.isBool())
            return textResult(QStringLiteral("%1 must be a boolean.").arg(key), true);
        flags[key] = v;
    }
    const QJsonValue gainValue = arguments.value("gain_db");
    const double gain = gainValue.toDouble();
    if (!gainValue.isUndefined()
        && (!gainValue.isDouble() || !std::isfinite(gain) || gain < -70 || gain > 24))
        return textResult("gain_db must be a number from -70 to 24.", true);
    if (name.isNull() && flags.isEmpty() && gainValue.isUndefined())
        return textResult("Pass at least one of name, mute, lock, hide, composite, gain_db.", true);
    if (!MAIN.isMultitrackValid())
        return textResult("There is no timeline.", true);
    MultitrackModel &model = *MAIN.timelineDock()->model();
    if (!validTrack(model, track, error, false, true))
        return textResult(error, true);
    const QModelIndex index = model.index(track);
    const bool video = model.trackList().at(track).type == VideoTrackType;
    if (!video && (flags.contains("hide") || flags.contains("composite")))
        return textResult(QStringLiteral("Track %1 is an audio track; hide and composite apply to "
                                         "video tracks only.")
                              .arg(track),
                          true);
    if (flags.value("composite").toBool() && track == model.bottomVideoTrackIndex())
        return textResult(QStringLiteral("Track %1 is the bottom video track, which has nothing "
                                         "below it to composite onto.")
                              .arg(track),
                          true);
    if (!gainValue.isUndefined() && !model.data(index, MultitrackModel::GainEnabledRole).toBool())
        return textResult(QStringLiteral("Track %1's gain has keyframes; gain_db cannot set it.")
                              .arg(track),
                          true);
    const bool locked = model.data(index, MultitrackModel::IsLockedRole).toBool();
    const bool unlock = flags.contains("lock") && !flags.value("lock").toBool();
    if (locked && !unlock
        && (!name.isNull() || flags.size() > flags.contains("lock") || !gainValue.isUndefined()))
        return textResult(QStringLiteral("Track %1 is locked; only lock:false is allowed (it may "
                                         "be combined with other properties, which are applied "
                                         "after unlocking).")
                              .arg(track),
                          true);

    // Only properties that differ are pushed. Mute and hide commands toggle, so they must not be
    // pushed when the value already matches. Unlock goes first and lock last, so the other
    // commands of the call apply to an unlocked track.
    struct Change
    {
        QString key;
        std::function<QUndoCommand *()> command;
    };
    std::vector<Change> changes;
    auto differs = [&](const char *key, int role) {
        return flags.contains(key) && flags.value(key).toBool() != model.data(index, role).toBool();
    };
    if (unlock && locked)
        changes.push_back(
            {"lock", [&] { return new Timeline::LockTrackCommand(model, track, false); }});
    if (!name.isNull() && name != model.data(index, MultitrackModel::NameRole).toString())
        changes.push_back(
            {"name", [&] { return new Timeline::NameTrackCommand(model, track, name); }});
    if (differs("mute", MultitrackModel::IsMuteRole))
        changes.push_back({"mute", [&] { return new Timeline::MuteTrackCommand(model, track); }});
    if (differs("hide", MultitrackModel::IsHiddenRole))
        changes.push_back({"hide", [&] { return new Timeline::HideTrackCommand(model, track); }});
    if (differs("composite", MultitrackModel::IsCompositeRole))
        changes.push_back({"composite", [&] {
                               return new Timeline::CompositeTrackCommand(
                                   model, track, flags.value("composite").toBool());
                           }});
    if (!gainValue.isUndefined()
        && std::abs(gain - model.data(index, MultitrackModel::GainRole).toDouble()) > 1e-9)
        changes.push_back({"gain_db", [&] {
                               return new Timeline::ChangeTrackGainCommand(model, track, gain);
                           }});
    if (flags.value("lock").toBool() && !locked)
        changes.push_back(
            {"lock", [&] { return new Timeline::LockTrackCommand(model, track, true); }});

    QJsonArray changed;
    if (!changes.empty()) {
        MLT.pause();
        QUndoStack *stack = MAIN.undoStack();
        // Every call is its own macro, so two calls setting gain stay two undo steps
        // (ChangeTrackGainCommand merges with a directly preceding one).
        stack->beginMacro(QObject::tr("Set track properties"));
        for (const Change &change : changes) {
            stack->push(change.command());
            changed.append(change.key);
        }
        const bool ok
            = (name.isNull() || model.data(index, MultitrackModel::NameRole).toString() == name)
              && (!flags.contains("mute")
                  || model.data(index, MultitrackModel::IsMuteRole).toBool()
                         == flags.value("mute").toBool())
              && (!flags.contains("hide")
                  || model.data(index, MultitrackModel::IsHiddenRole).toBool()
                         == flags.value("hide").toBool())
              && (!flags.contains("composite")
                  || model.data(index, MultitrackModel::IsCompositeRole).toBool()
                         == flags.value("composite").toBool())
              && (!flags.contains("lock")
                  || model.data(index, MultitrackModel::IsLockedRole).toBool()
                         == flags.value("lock").toBool())
              && (gainValue.isUndefined()
                  || std::abs(model.data(index, MultitrackModel::GainRole).toDouble() - gain)
                         < 1e-6);
        if (!finishMacro(stack, ok))
            return textResult("Shotcut did not apply the properties; nothing was changed.", true);
    }
    return jsonResult(QJsonObject{{"tracks", allTracksJson(model)}, {"changed", changed}});
}

QJsonObject markerJson(int index, const Markers::Marker &marker)
{
    return QJsonObject{{"index", index},
                       {"start", marker.start},
                       {"end", marker.end},
                       {"text", marker.text},
                       {"color", marker.color.name()}};
}

QJsonObject markerAddTool(const QJsonObject &arguments)
{
    Markers::Marker marker;
    if (readFrame(arguments, "start", marker.start) != FrameArg::Ok)
        return textResult("start must be a non-negative integer frame.", true);
    marker.end = marker.start;
    const FrameArg endArg = readFrame(arguments, "end", marker.end);
    if (endArg == FrameArg::Invalid || marker.end < marker.start)
        return textResult("end must be an integer frame at least start.", true);
    QString error;
    if (!readText(arguments.value("text"), "text", 200, marker.text, error))
        return textResult(error, true);
    const QJsonValue colorValue = arguments.value("color");
    if (colorValue.isUndefined()) {
        marker.color = Settings.markerColor();
    } else {
        static const QRegularExpression hex("^#[0-9a-fA-F]{6}$");
        if (!colorValue.isString() || !hex.match(colorValue.toString()).hasMatch())
            return textResult("color must be \"#rrggbb\".", true);
        marker.color = QColor(colorValue.toString());
    }
    if (!MAIN.isMultitrackValid())
        return textResult("There is no timeline; add a clip first.", true);
    const int maxFrame = MAIN.multitrack()->get_playtime() + qRound(3600 * MLT.profile().fps());
    if (marker.end > maxFrame)
        return textResult(QStringLiteral("start and end must be at most %1 (one hour after the end "
                                         "of the timeline).")
                              .arg(maxFrame),
                          true);
    MarkersModel &markers = *MAIN.timelineDock()->markersModel();
    const int before = markers.getMarkers().size();
    MLT.pause();
    markers.append(marker);
    const int count = markers.getMarkers().size();
    if (count != before + 1)
        return textResult("Shotcut did not add the marker.", true);
    return jsonResult(QJsonObject{{"index", count - 1},
                                  {"marker", markerJson(count - 1, markers.getMarker(count - 1))},
                                  {"markers_count", count}});
}

QJsonObject markerListTool(const QJsonObject &)
{
    QJsonArray list;
    if (MAIN.isMultitrackValid()) {
        const QList<Markers::Marker> markers = MAIN.timelineDock()->markersModel()->getMarkers();
        for (int i = 0; i < markers.size(); ++i)
            list.append(markerJson(i, markers.at(i)));
    }
    return jsonResult(QJsonObject{{"markers", list}});
}

QJsonObject markerRemoveTool(const QJsonObject &arguments)
{
    int index = 0;
    if (readFrame(arguments, "index", index) != FrameArg::Ok)
        return textResult("index must be a non-negative integer.", true);
    if (!MAIN.isMultitrackValid())
        return textResult("There is no timeline.", true);
    MarkersModel &markers = *MAIN.timelineDock()->markersModel();
    const int before = markers.getMarkers().size();
    if (index >= before)
        return textResult(QStringLiteral("There is no marker %1; call marker_list.").arg(index),
                          true);
    const QJsonObject removed = markerJson(index, markers.getMarker(index));
    MLT.pause();
    markers.remove(index);
    if (markers.getMarkers().size() != before - 1)
        return textResult("Shotcut did not remove the marker.", true);
    return jsonResult(QJsonObject{{"removed", removed}, {"markers_count", before - 1}});
}

} // namespace

QList<McpTool> McpTools::structureTools()
{
    return {
        {"timeline_insert_range",
         "Put a range of a media file on a track at an absolute timeline frame as ONE undo step. "
         "Source: path (absolute media file) OR playlist_index (that playlist item's file; its "
         "filters are not carried over, and an item with a speed change is refused). in_s and "
         "out_s are source seconds (out_s exclusive, 0 <= in_s < out_s <= file duration, at least "
         "one frame); required with path, optional with playlist_index (default: the item's own "
         "in and out). mode insert (default) splits a clip at position and pushes later items on "
         "the track right by the inserted duration; whether other tracks and markers also move "
         "follows Shotcut's own ripple settings, reported in the result as ripple:{all_tracks, "
         "markers} (also in get_timeline settings). When all_tracks is true the other unlocked "
         "tracks gained a gap at position, so re-read get_timeline. mode overwrite replaces what "
         "is under the range and never ripples. From the track's last frame on, the clip is "
         "appended at the end of the track (a position on the last frame lands one frame later; "
         "beyond the end a gap is left before it). position may be at most one hour after the end "
         "of the track; a position inside a transition, or an overwrite covering part of one, is "
         "refused. Inserting exactly at a transition's start puts the new clip before the "
         "transition, which then plays after the new clip instead of at the end of the previous "
         "clip; inserting exactly at its end puts the new clip between the transition and the "
         "next clip. Item indexes after the new clip shift; the returned track shows them. Track 0 "
         "is the TOPMOST video track; any media may go on video or audio tracks, as in Shotcut. "
         "With no timeline, track must be 0 and a video track is created. With nothing loaded "
         "the file is first opened as playlist_add does (the project then follows its format, "
         "even when the range is then refused); the insert itself never moves the playhead. "
         "Locked tracks, still images, generators and project files are refused. Returns {track, "
         "inserted:{start, duration}, ripple:{all_tracks, markers}}.",
         schema(R"({"type":"object","properties":{
             "path":{"type":"string","description":"Absolute path of a media file."},
             "playlist_index":{"type":"integer","minimum":0,"description":"Playlist item index from get_playlist."},
             "in_s":{"type":"number","minimum":0,"description":"Source start in seconds."},
             "out_s":{"type":"number","description":"Source end in seconds (exclusive)."},
             "track":{"type":"integer","minimum":0},
             "position":{"type":"integer","minimum":0,"description":"Absolute timeline frame."},
             "mode":{"type":"string","enum":["insert","overwrite"]}
           },"required":["track","position"]})"),
         timelineInsertRangeTool},
        {"timeline_remove_ranges",
         "Cut the same frame ranges out of every listed track as ONE undo step (to cut audio and "
         "video together, list both tracks; unlisted tracks are never touched, whatever Shotcut's "
         "ripple-all-tracks setting is). ranges are absolute timeline frames {start, end} with "
         "end exclusive (0 <= start < end, up to 500); they are sorted, overlapping or touching "
         "ones are merged, end is clamped to the end of the longest listed track and ranges past "
         "it are dropped (error when none is left). All ranges are checked before anything "
         "changes: a range that overlaps or touches a transition is refused, naming it. ripple "
         "true (default) closes each range on each listed track, gaps inside it included, so "
         "listed tracks stay aligned; whether markers move follows Shotcut's ripple-markers "
         "setting (markers inside a range are deleted, later ones shift once). ripple false lifts "
         "the clips and leaves gaps of the same length (Shotcut drops a gap at the end of a "
         "track). Item indexes change; use the returned tracks. Locked tracks are refused. At "
         "most 120 cuts per call (ranges after merging times listed tracks, e.g. 60 ranges on 2 "
         "tracks): long edits can make Shotcut corrupt its undo history, so call save_project "
         "before many cuts; for more, send chunks from the END of the timeline to the start "
         "(ripple shifts later material) or re-read get_timeline between chunks; each call is "
         "one undo step. Returns {tracks:[...] in the listed order, removed_frames:{\"<track>\": "
         "frames of the applied ranges inside that track up to its last clip, gaps included (with "
         "ripple false also the frames of gaps that stay gaps)}, ranges_applied:[{start, end}], "
         "ripple:{all_tracks, markers}}.",
         schema(R"({"type":"object","properties":{
             "tracks":{"type":"array","minItems":1,"maxItems":8,"items":{"type":"integer","minimum":0},"description":"Distinct track indexes (0 is the topmost video track)."},
             "ranges":{"type":"array","minItems":1,"maxItems":500,"items":{"type":"object","properties":{
               "start":{"type":"integer","minimum":0},
               "end":{"type":"integer","minimum":1,"description":"Exclusive."}
             },"required":["start","end"]}},
             "ripple":{"type":"boolean","description":"Default true."}
           },"required":["tracks","ranges"]})"),
         timelineRemoveRangesTool},
        {"track_add",
         "Add an empty timeline track as ONE undo step, optionally named (otherwise Shotcut names "
         "it V<n> or A<n>). A new video track goes on TOP, at index 0, so every existing track "
         "index shifts up by one (re-read indexes from the result); a new audio track goes at the "
         "bottom (the last index). Needs a timeline (create one with timeline_insert_range or "
         "timeline_add_playlist). name is 1 to 64 characters, not blank. A track head selected "
         "in the timeline is deselected, as in Shotcut. Returns {tracks:[...all tracks], "
         "added:{index, type, name}}.",
         schema(R"({"type":"object","properties":{
             "type":{"type":"string","enum":["video","audio"]},
             "name":{"type":"string","minLength":1,"maxLength":64}
           },"required":["type"]})"),
         trackAddTool},
        {"track_remove",
         "Remove a track as ONE undo step (undo restores its clips, filters, name, mute, hide, "
         "gain and compositing on/off (not a custom blend mode); a gain with keyframes comes back "
         "as an ordinary Gain/Volume filter with the same keyframes, so it sounds the same but "
         "gain_db then reports 0). A track that holds clips or transitions is refused unless force "
         "is true. Locked tracks are refused, and so is the bottom video track (the last video "
         "track listed): a timeline needs one, and Shotcut cannot undo removing it correctly "
         "while other video tracks exist; remove its clips instead. Audio tracks may all be "
         "removed. Later track indexes shift down by one. Returns {tracks:[...all tracks], "
         "removed:{index, type, name}}.",
         schema(R"({"type":"object","properties":{
             "track":{"type":"integer","minimum":0},
             "force":{"type":"boolean","description":"Remove a track that still holds clips."}
           },"required":["track"]})"),
         trackRemoveTool},
        {"track_set",
         "Set track properties as ONE undo step; pass at least one. Values are set, not "
         "toggled: properties that already have the value are skipped, and a call where nothing "
         "differs succeeds without adding an undo step. hide and composite (blend over the "
         "tracks below) are for video tracks only, and the bottom video track cannot composite. "
         "gain_db (-70 to 24, 0 = unchanged level) applies to any track, since video tracks carry "
         "sound too; a gain with keyframes is refused. A locked track only accepts lock:false, "
         "which may be combined with other properties: it unlocks first, then applies them; "
         "lock:true is applied after the other properties. Returns {tracks:[...all tracks], "
         "changed:[property names applied]}.",
         schema(R"({"type":"object","properties":{
             "track":{"type":"integer","minimum":0},
             "name":{"type":"string","minLength":1,"maxLength":64},
             "mute":{"type":"boolean"},
             "lock":{"type":"boolean"},
             "hide":{"type":"boolean","description":"Video tracks only."},
             "composite":{"type":"boolean","description":"Video tracks only."},
             "gain_db":{"type":"number","minimum":-70,"maximum":24}
           },"required":["track"]})"),
         trackSetTool},
        {"marker_add",
         "Add a timeline marker as ONE undo step: a point (end omitted or equal to start) or a "
         "range (end > start, inclusive frames as in Shotcut); start and end may be at most one "
         "hour of frames after the end of the timeline. text is 1 to 200 characters, not blank. "
         "Markers are listed in the order they were added, not by time; the new one is last. "
         "Needs a timeline. Returns {index, marker:{index, start, end, text, color}, "
         "markers_count}.",
         schema(R"({"type":"object","properties":{
             "start":{"type":"integer","minimum":0,"description":"Timeline frame."},
             "end":{"type":"integer","minimum":0,"description":"Timeline frame, at least start. Default start."},
             "text":{"type":"string","minLength":1,"maxLength":200},
             "color":{"type":"string","pattern":"^#[0-9a-fA-F]{6}$","description":"Default: Shotcut's marker color."}
           },"required":["start","text"]})"),
         markerAddTool},
        {"marker_list",
         "List the timeline markers in index order: {markers:[{index, start, end, text, "
         "color}]} (empty without a timeline).",
         schema(R"({"type":"object","properties":{}})"),
         markerListTool},
        {"marker_remove",
         "Remove the marker at index (from marker_list) as ONE undo step; later markers' indexes "
         "shift down by one. Returns {removed:{index, start, end, text, color}, markers_count}.",
         schema(R"({"type":"object","properties":{
             "index":{"type":"integer","minimum":0}
           },"required":["index"]})"),
         markerRemoveTool},
    };
}
