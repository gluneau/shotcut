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

#include "docks/timelinedock.h"
#include "mainwindow.h"
#include "mltcontroller.h"
#include "models/multitrackmodel.h"
#include "settings.h"

#include <MltPlaylist.h>

#include <QScopedPointer>

#include <cmath>
#include <memory>

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
    };
}
