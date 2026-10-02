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
#include "sharedframe.h"
#include "videowidget.h"

#include <MltPlaylist.h>

#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QStringList>
#include <QTimer>

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

QImage fetchSettledFrame(int position, QString &error, int timeoutMs)
{
    auto *widget = qobject_cast<Mlt::VideoWidget *>(MLT.videoWidget());
    if (!widget) {
        error = QStringLiteral("There is no video widget to read a frame from.");
        return QImage();
    }
    // After a seek the consumer shows the frame it rendered BEFORE the seek first (the display
    // lags one request), and Mlt::VideoWidget::requestImage() is consumed by whichever frame is
    // displayed next, so imageReady can deliver a stale picture. Instead take the first
    // displayed frame whose position is the requested one (the way a position-less call reads
    // what is shown), re-pushing the frame while waiting; a timeout is an error, never a
    // wrong image.
    bool ready = false;
    int matches = 0;
    // Two skipped displays by default; with Player > Realtime off MLT.realTime() is minus the
    // parallel worker count (-1 to -4), so more frames can be in flight.
    const int requiredMatches = 3 + qMax(0, -MLT.realTime() - 1);
    QImage matched;
    QEventLoop loop;
    QTimer timeout;
    QTimer refresh;
    timeout.setSingleShot(true);
    QObject::connect(widget,
                     &Mlt::VideoWidget::frameDisplayed,
                     &loop,
                     [&](const SharedFrame &frame) {
                         if (ready || frame.get_position() != position)
                             return;
                         // The first match can predate an edit made just before this call
                         // (same position, so the seek is a no-op); only a match shown after
                         // a refresh issued by this call is trusted.
                         if (++matches < requiredMatches) {
                             MLT.refreshConsumer();
                             return;
                         }
                         const uint8_t *data = frame.get_image(mlt_image_rgba);
                         if (!data)
                             return;
                         matched = QImage(data,
                                          frame.get_image_width(),
                                          frame.get_image_height(),
                                          QImage::Format_RGBA8888)
                                       .copy();
                         ready = true;
                         loop.quit();
                     });
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&refresh, &QTimer::timeout, &loop, []() { MLT.refreshConsumer(); });
    MLT.pause();
    movePlayhead(position);
    MLT.refreshConsumer();
    timeout.start(timeoutMs);
    refresh.start(200);
    loop.exec();
    if (!ready) {
        error = QStringLiteral("The frame was not ready in time; try again.");
        return QImage();
    }
    return matched;
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

namespace McpTranscripts {

namespace {

constexpr int kMaxTranscripts = 50;

struct Store
{
    QHash<QString, QJsonObject> transcripts;
    QStringList order;
};

Store &transcriptStore()
{
    static Store s;
    return s;
}

QString keyOf(const QString &path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

} // namespace

void store(const QString &path, const QJsonObject &transcript)
{
    Store &s = transcriptStore();
    const QString key = keyOf(path);
    // Only Whisper results are evicted; a supplied transcript stays until Shotcut quits.
    if (transcript.value("supplied").toBool()) {
        s.order.removeOne(key);
        s.transcripts.insert(key, transcript);
        return;
    }
    const auto it = s.transcripts.constFind(key);
    if (it != s.transcripts.constEnd() && it->value("supplied").toBool())
        return;
    s.order.removeOne(key);
    s.order.append(key);
    if (s.order.size() > kMaxTranscripts)
        s.transcripts.remove(s.order.takeFirst());
    s.transcripts.insert(key, transcript);
}

bool get(const QString &path, QJsonObject &transcript)
{
    const Store &s = transcriptStore();
    const auto it = s.transcripts.constFind(keyOf(path));
    if (it == s.transcripts.constEnd())
        return false;
    transcript = it.value();
    return true;
}

} // namespace McpTranscripts
