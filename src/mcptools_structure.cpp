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
#include "models/multitrackmodel.h"
#include "settings.h"

#include <MltPlaylist.h>

#include <QScopedPointer>
#include <QUndoStack>

#include <algorithm>
#include <cmath>
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

int trackPlaytime(MultitrackModel &model, int track)
{
    QScopedPointer<Mlt::Producer> producer(
        model.tractor()->track(model.trackList().at(track).mlt_index));
    return producer ? producer->get_playtime() : 0;
}

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

int itemFrames(MultitrackModel &model, int track)
{
    int frames = 0;
    for (int c = 0; c < model.rowCount(model.index(track)); ++c) {
        std::unique_ptr<Mlt::ClipInfo> info = kindOf(model, track, c) == "blank"
                                                  ? nullptr
                                                  : model.getClipInfo(track, c);
        frames += info ? info->frame_count : 0;
    }
    return frames;
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
        if (blank) {
            const int before = trackPlaytime(model, track);
            dock->remove(track, clip, false);
            if (trackPlaytime(model, track) != before - length)
                return false;
        } else {
            const int before = itemFrames(model, track);
            if (ripple)
                dock->remove(track, clip, false);
            else
                dock->lift(track, clip, false);
            if (itemFrames(model, track) != before - length)
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

    QJsonObject removedFrames;
    for (int i = 0; i < tracks.size(); ++i) {
        const int track = tracks.at(i);
        int removed = 0;
        for (const auto &range : applied) {
            const int end = qMin(range.second, extents.at(i));
            removed += qMax(0, end - range.first);
            if (range.first >= end)
                continue;
            // Removing or lifting a clip a transition joins breaks the transition.
            for (int c = 0; c < model.rowCount(model.index(track)); ++c) {
                if (kindOf(model, track, c) != "transition")
                    continue;
                std::unique_ptr<Mlt::ClipInfo> t = model.getClipInfo(track, c);
                if (!t || range.first > t->start + t->frame_count || end < t->start)
                    continue;
                return textResult(QStringLiteral("The range %1-%2 touches the transition on track "
                                                 "%3 (item %4, frames %5-%6); keep ranges at "
                                                 "least one frame away from transitions.")
                                      .arg(range.first)
                                      .arg(range.second)
                                      .arg(track)
                                      .arg(c)
                                      .arg(t->start)
                                      .arg(t->start + t->frame_count - 1),
                                  true);
            }
        }
        removedFrames[QString::number(track)] = removed;
    }

    RippleSettingsGuard guard;
    MLT.pause();
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
         "refused. Item indexes after the new clip shift; the returned track shows them. Track 0 "
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
         "track). Item indexes change; use the returned tracks. Locked tracks are refused. "
         "Returns {tracks:[...] in the listed order, removed_frames:{\"<track>\": frames of the "
         "applied ranges inside that track}, ranges_applied:[{start, end}], ripple:{all_tracks, "
         "markers}}.",
         schema(R"({"type":"object","properties":{
             "tracks":{"type":"array","minItems":1,"maxItems":8,"items":{"type":"integer","minimum":0},"description":"Distinct track indexes (0 is the topmost video track)."},
             "ranges":{"type":"array","minItems":1,"maxItems":500,"items":{"type":"object","properties":{
               "start":{"type":"integer","minimum":0},
               "end":{"type":"integer","minimum":1,"description":"Exclusive."}
             },"required":["start","end"]}},
             "ripple":{"type":"boolean","description":"Default true."}
           },"required":["tracks","ranges"]})"),
         timelineRemoveRangesTool},
    };
}
