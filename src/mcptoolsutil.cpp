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

#include "mcptoolsutil.h"

#include "docks/timelinedock.h"
#include "mainwindow.h"
#include "mltcontroller.h"
#include "models/multitrackmodel.h"
#include "proxymanager.h"

#include <MltPlaylist.h>

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>

#include <cmath>
#include <limits>

namespace McpTools {

namespace {

bool itemFlag(MultitrackModel &model, int track, int clip, int role)
{
    return model.data(model.index(clip, 0, model.index(track)), role).toBool();
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
    if (o["kind"] == "clip" && info && info->producer && info->producer->is_valid()) {
        o["path"] = ProxyManager::resource(*info->producer);
        addMediaFields(*info->producer, o);
    } else
        o["path"] = model.data(index, MultitrackModel::ResourceRole).toString();
    o["fade_in"] = model.data(index, MultitrackModel::FadeInRole).toInt();
    o["fade_out"] = model.data(index, MultitrackModel::FadeOutRole).toInt();
    return o;
}

} // namespace

// Source media fields from the producer's probe results; each is omitted when unknown.
void addMediaFields(Mlt::Producer &producer, QJsonObject &o)
{
    const int width = producer.get_int("meta.media.width");
    const int height = producer.get_int("meta.media.height");
    if (width > 0 && height > 0) {
        o["width"] = width;
        o["height"] = height;
    }
    if (producer.property_exists("audio_index"))
        o["has_audio"] = producer.get_int("audio_index") >= 0;
    const int videoIndex = producer.get_int("video_index");
    if (videoIndex >= 0) {
        const QByteArray key = QStringLiteral("meta.media.%1.codec.rotate")
                                   .arg(videoIndex)
                                   .toUtf8();
        if (producer.property_exists(key.constData()))
            o["rotation"] = producer.get_int(key.constData());
    }
    const double den = producer.get_double("meta.media.frame_rate_den");
    if (den > 0)
        o["fps"] = producer.get_double("meta.media.frame_rate_num") / den;
}

QJsonObject schema(const char *json)
{
    return QJsonDocument::fromJson(json).object();
}

// Qt resource paths (":/...") are accepted by QDir::isAbsolutePath but are not files.
bool isResourcePath(const QString &path)
{
    return path.startsWith(':');
}

bool isLoaded()
{
    return MLT.producer() && MLT.producer()->is_valid();
}

// Reads a non-negative integer frame number from `arguments[key]`.
FrameArg readFrame(const QJsonObject &arguments, const char *key, int &value)
{
    const QJsonValue v = arguments.value(key);
    if (v.isUndefined())
        return FrameArg::Missing;
    const double d = v.toDouble();
    if (!v.isDouble() || d != std::floor(d) || d < 0
        || d > std::numeric_limits<int>::max()) {
        return FrameArg::Invalid;
    }
    value = static_cast<int>(d);
    return FrameArg::Ok;
}

// The multitrack when a timeline is open, otherwise null.
Mlt::Producer *timeline()
{
    return MAIN.isMultitrackValid() ? MAIN.multitrack() : nullptr;
}

// seekTimeline() switches the player to the multitrack, so clamp against its length then.
int lastFrame()
{
    Mlt::Producer *tl = timeline();
    Mlt::Producer *producer = tl ? tl : MLT.producer();
    return producer ? qMax(0, producer->get_playtime() - 1) : 0;
}

void movePlayhead(int position)
{
    if (timeline())
        MAIN.seekTimeline(position);
    else
        MLT.seek(position);
}

QString kindOf(MultitrackModel &model, int track, int clip)
{
    if (itemFlag(model, track, clip, MultitrackModel::IsBlankRole))
        return QStringLiteral("blank");
    if (itemFlag(model, track, clip, MultitrackModel::IsTransitionRole))
        return QStringLiteral("transition");
    return QStringLiteral("clip");
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

bool validTrack(
    MultitrackModel &model, int track, QString &error, bool needVideo, bool allowLocked)
{
    if (track < 0 || track >= model.trackList().size()) {
        error = QStringLiteral("There is no track %1; call get_timeline.").arg(track);
        return false;
    }
    if (needVideo && model.trackList().at(track).type != VideoTrackType) {
        error = QStringLiteral("Track %1 is not a video track.").arg(track);
        return false;
    }
    if (!allowLocked && MAIN.timelineDock()->isTrackLocked(track)) {
        error = QStringLiteral("Track %1 is locked.").arg(track);
        return false;
    }
    return true;
}

std::unique_ptr<Mlt::ClipInfo> plainClip(MultitrackModel &model,
                                         int track,
                                         int clip,
                                         QString &error,
                                         bool allowLocked)
{
    if (!validTrack(model, track, error, false, allowLocked))
        return nullptr;
    if (clip < 0 || clip >= model.rowCount(model.index(track))) {
        error = QStringLiteral("Track %1 has no clip %2; call get_timeline.").arg(track).arg(clip);
        return nullptr;
    }
    if (kindOf(model, track, clip) != "clip") {
        error = QStringLiteral("Item %1 on track %2 is a %3, not a clip.")
                    .arg(clip)
                    .arg(track)
                    .arg(kindOf(model, track, clip));
        return nullptr;
    }
    std::unique_ptr<Mlt::ClipInfo> info = model.getClipInfo(track, clip);
    if (!info)
        error = QStringLiteral("Track %1 has no clip %2; call get_timeline.").arg(track).arg(clip);
    return info;
}

QString clipSourcePath(Mlt::ClipInfo &info)
{
    if (!info.producer || !info.producer->is_valid())
        return QString();
    QString path = ProxyManager::resource(*info.producer);
    if (!path.isEmpty() && !QDir::isAbsolutePath(path) && !MAIN.fileName().isEmpty())
        path = QDir(QFileInfo(MAIN.fileName()).canonicalPath()).absoluteFilePath(path);
    return path;
}

bool resolveSource(const QJsonObject &arguments, SourceRef &ref, QString &error)
{
    const QJsonValue pathValue = arguments.value("path");
    const bool hasTrack = !arguments.value("track").isUndefined();
    const bool hasClip = !arguments.value("clip").isUndefined();
    if (!pathValue.isUndefined() && (hasTrack || hasClip)) {
        error = QStringLiteral("Pass either path or track and clip, not both.");
        return false;
    }
    if (!pathValue.isUndefined()) {
        const QString path = pathValue.toString();
        if (!pathValue.isString() || path.isEmpty() || isResourcePath(path)
            || !QDir::isAbsolutePath(path)) {
            error = QStringLiteral("path must be an absolute file path.");
            return false;
        }
        const QFileInfo info(path);
        if (!info.exists() || !info.isFile()) {
            error = "No such file: " + path;
            return false;
        }
        ref = SourceRef();
        ref.path = path;
        return true;
    }
    if (!hasTrack || !hasClip) {
        error = QStringLiteral("Pass path, or track and clip.");
        return false;
    }
    int track = 0;
    int clip = 0;
    if (readFrame(arguments, "track", track) != FrameArg::Ok
        || readFrame(arguments, "clip", clip) != FrameArg::Ok) {
        error = QStringLiteral("track and clip must be non-negative integers.");
        return false;
    }
    if (!MAIN.isMultitrackValid()) {
        error = QStringLiteral("There is no timeline.");
        return false;
    }
    std::unique_ptr<Mlt::ClipInfo> info = plainClip(*MAIN.timelineDock()->model(),
                                                    track,
                                                    clip,
                                                    error,
                                                    true);
    if (!info)
        return false;
    const QString path = clipSourcePath(*info);
    const QFileInfo file(path);
    if (path.isEmpty() || !file.exists() || !file.isFile()) {
        error = QStringLiteral("Clip %1 on track %2 has no source file.").arg(clip).arg(track);
        return false;
    }
    const double fps = MLT.profile().fps();
    ref = SourceRef();
    ref.path = path;
    ref.inSeconds = info->frame_in / fps;
    ref.outSeconds = (info->frame_out + 1) / fps;
    ref.isClip = true;
    ref.track = track;
    ref.clip = clip;
    ref.clipStart = info->start;
    ref.clipFrameIn = info->frame_in;
    ref.clipFrameCount = info->frame_count;
    ref.speedChanged = info->producer->property_exists("warp_speed")
                       && info->producer->get_double("warp_speed") != 1.0;
    return true;
}

QJsonArray mapRangeToTimeline(const SourceRef &ref, double startSec, double endSec)
{
    if (!ref.isClip || ref.speedChanged)
        return QJsonArray();
    const double fps = MLT.profile().fps();
    const int a = qMax(qRound(startSec * fps), ref.clipFrameIn);
    const int b = qMin(qRound(endSec * fps), ref.clipFrameIn + ref.clipFrameCount);
    if (a >= b)
        return QJsonArray();
    return QJsonArray{QJsonObject{{"start", ref.clipStart + (a - ref.clipFrameIn)},
                                  {"end", ref.clipStart + (b - ref.clipFrameIn)}}};
}

} // namespace McpTools
