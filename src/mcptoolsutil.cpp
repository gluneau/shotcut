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

#include "commands/timelinecommands.h"
#include "docks/timelinedock.h"
#include "mainwindow.h"
#include "mltcontroller.h"
#include "models/multitrackmodel.h"
#include "proxymanager.h"
#include "settings.h"
#include "sharedframe.h"
#include "shotcut_mlt_properties.h"
#include "videowidget.h"

#include <MltPlaylist.h>

#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QScopedPointer>
#include <QStringList>
#include <QTimer>
#include <QUndoStack>

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

FrameArg readNumberArg(const QJsonObject &arguments,
                       const char *key,
                       double min,
                       double max,
                       double &value,
                       QString &error)
{
    const QJsonValue v = arguments.value(key);
    if (v.isUndefined())
        return FrameArg::Missing;
    if (!v.isDouble() || !std::isfinite(v.toDouble()) || v.toDouble() < min || v.toDouble() > max) {
        error = QStringLiteral("%1 must be a number from %2 to %3.").arg(key).arg(min).arg(max);
        return FrameArg::Invalid;
    }
    value = v.toDouble();
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

int EditPreviewGuard::s_depth = 0;

EditPreviewGuard::EditPreviewGuard(bool standIn)
{
    if (s_depth++ > 0)
        return; // only the outermost guard acts
    m_outer = true;
    MLT.pause();
    MLT.blockRefresh(true);
    Mlt::Consumer *consumer = MLT.consumer();
    if (!consumer || !consumer->is_valid())
        return;
    // Stopping joins the preview's read-ahead thread: a frame it is still rendering (for example
    // after the previous call's refresh) must not run while the edit changes tracks and
    // transitions on the GUI thread (seen: SIGSEGV in transition_get_frame, heap corruption,
    // during RemoveTrackCommand::undo).
    m_wasStopped = consumer->is_stopped();
    if (!m_wasStopped)
        consumer->stop();
    m_consumer = true;
    if (!MLT.isMultitrack())
        return;
    // A stopped consumer does not stay stopped: every duration change makes Shotcut seek the
    // player (Player::onDurationChanged), and Controller::seek starts a stopped consumer, on
    // whatever it is connected to. So it is always connected to a black stand-in at the same
    // position while the guard lives; only large work also starts it on the stand-in.
    const int position = MLT.producer()->position();
    m_standIn = std::make_unique<Mlt::Producer>(MLT.profile(), "color:black");
    if (!m_standIn->is_valid()) {
        m_standIn.reset();
        return;
    }
    m_standIn->set("length", position + 1000);
    m_standIn->set_in_and_out(0, position + 999);
    m_standIn->set_speed(0); // producers play at speed 1 by default
    m_standIn->seek(position);
    consumer->connect(*m_standIn);
    if (standIn && !m_wasStopped)
        consumer->start();
}

EditPreviewGuard::~EditPreviewGuard()
{
    --s_depth;
    if (!m_outer)
        return;
    MLT.blockRefresh(false);
    Mlt::Consumer *consumer = MLT.consumer();
    if (m_consumer && consumer && consumer->is_valid()) {
        if (m_standIn) {
            consumer->stop();
            if (MLT.producer() && MLT.producer()->is_valid())
                consumer->connect(*MLT.producer());
        }
        // Without a producer the consumer stays stopped rather than running on the stand-in.
        // start() does nothing when the edit already restarted it (a new timeline).
        if (!m_wasStopped && MLT.producer() && MLT.producer()->is_valid())
            consumer->start();
    }
    MLT.refreshConsumer();
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
    const bool audio = model.trackList().at(track).type == AudioTrackType;
    // Every Shotcut track carries audio (video tracks mix their clips' sound), so all have gain.
    QJsonObject o{{"index", track},
                  {"name", model.data(index, MultitrackModel::NameRole).toString()},
                  {"type", audio ? "audio" : "video"},
                  {"locked", model.data(index, MultitrackModel::IsLockedRole).toBool()},
                  {"muted", model.data(index, MultitrackModel::IsMuteRole).toBool()},
                  {"hidden", model.data(index, MultitrackModel::IsHiddenRole).toBool()},
                  {"gain_db", model.data(index, MultitrackModel::GainRole).toDouble()},
                  {"clips", clips}};
    if (!audio)
        o["composite"] = model.data(index, MultitrackModel::IsCompositeRole).toBool();
    return o;
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

bool checkMediaPath(const QString &path, QString &error)
{
    if (path.isEmpty() || isResourcePath(path) || !QDir::isAbsolutePath(path)) {
        error = QStringLiteral("path must be an absolute file path.");
        return false;
    }
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile()) {
        error = "No such file: " + path;
        return false;
    }
    return true;
}

std::unique_ptr<Mlt::Producer> newMediaProducer(const QString &path,
                                                const QString &toolName,
                                                bool allowImages,
                                                QString &error)
{
    // Like PlaylistDock::addFiles: runs animation formats in a subprocess so a crashing file
    // cannot take Shotcut down. It shows no dialog.
    if (MLT.checkFile(path)) {
        error = "Shotcut cannot open " + path + " as media.";
        return nullptr;
    }
    Mlt::Producer probe(MLT.profile(), path.toUtf8().constData());
    if (!probe.is_valid() || probe.get_length() <= 0) {
        error = "Shotcut cannot open " + path + " as media.";
        return nullptr;
    }
    if (!allowImages && MLT.isImageProducer(&probe)) {
        error = "Still images are not supported by " + toolName + ".";
        return nullptr;
    }

    // Same as Shotcut's Add Files: with nothing loaded the first file is opened so the project
    // profile follows it.
    Mlt::Producer *source = &probe;
    if (!isLoaded()) {
        // MainWindow::open() asks what to do with unsaved changes when no project is loaded.
        if (MAIN.isWindowModified()) {
            error = QStringLiteral("The current project has unsaved changes. Save it first.");
            return nullptr;
        }
        Mlt::Properties properties;
        properties.set(kShotcutSkipConvertProperty, 1);
        MAIN.open(path, &properties, false);
        if (isLoaded())
            source = MLT.producer();
    }
    std::unique_ptr<Mlt::Producer> producer(MLT.setupNewProducer(source));
    if (!producer || !producer->is_valid()) {
        error = "Shotcut could not prepare " + path + ".";
        return nullptr;
    }
    producer->set(kShotcutSkipConvertProperty, 1);
    // Generators (producers without a finite length) would need Shotcut's duration dialog.
    // Still images do not: setupNewProducer gives them the default image duration. A probe is
    // not reported live before setupNewProducer, so this can only be checked here.
    if (MLT.isLiveProducer(producer.get()) && !producer->get_int(kShotcutVirtualClip)) {
        error = "Generators are not supported by " + toolName + ".";
        return nullptr;
    }
    return producer;
}

bool validTargetTrack(int track, QString &error)
{
    if (MAIN.isMultitrackValid())
        return validTrack(*MAIN.timelineDock()->model(), track, error);
    if (track != 0) {
        error = QStringLiteral("There is no timeline yet; use track 0 (a video track is created).");
        return false;
    }
    return true;
}

int pushClipXml(const QString &mode,
                int track,
                int position,
                const QString &xml,
                const QString &undoText,
                QString &error)
{
    if (mode != "insert" && mode != "overwrite") {
        error = QStringLiteral("mode must be \"insert\" or \"overwrite\".");
        return -1;
    }
    if (!validTargetTrack(track, error))
        return -1;
    TimelineDock *dock = MAIN.timelineDock();
    MultitrackModel &model = *dock->model();
    Mlt::Producer clip(MLT.profile(), "xml-string", xml.toUtf8().constData());
    const int duration = clip.is_valid() ? clip.get_playtime() : 0;
    if (duration <= 0) {
        error = QStringLiteral("The clip is empty.");
        return -1;
    }
    const bool create = !MAIN.isMultitrackValid();
    int playtime = 0;
    if (!create) {
        QScopedPointer<Mlt::Producer> trackProducer(
            model.tractor()->track(model.trackList().at(track).mlt_index));
        if (trackProducer)
            playtime = trackProducer->get_playtime();
        // Splitting a transition, or overwriting part of one, breaks the clips it joins.
        for (int c = 0; c < model.rowCount(model.index(track)); ++c) {
            if (kindOf(model, track, c) != "transition")
                continue;
            std::unique_ptr<Mlt::ClipInfo> t = model.getClipInfo(track, c);
            if (!t)
                continue;
            const int tStart = t->start, tEnd = t->start + t->frame_count;
            const int end = position + duration;
            const bool inside = position > tStart && position < tEnd;
            const bool partial = mode == "overwrite" && position < tEnd && end > tStart
                                 && (position > tStart || end < tEnd);
            if (inside || partial) {
                error = QStringLiteral("position is inside a transition (item %1, frames %2-%3); "
                                       "choose a position outside it.")
                            .arg(c)
                            .arg(tStart)
                            .arg(tEnd - 1);
                return -1;
            }
        }
    }
    const int maxPosition = playtime + qRound(3600 * MLT.profile().fps());
    if (position > maxPosition) {
        error = QStringLiteral("position must be at most %1 (one hour after the end of the "
                               "track).")
                    .arg(maxPosition);
        return -1;
    }

    EditPreviewGuard previewGuard(false); // one insert: no stand-in, so no preview flicker
    MAIN.undoStack()->beginMacro(undoText);
    if (create) {
        // The tracks timeline_add_playlist creates on an empty timeline (V1, plus A1 when
        // Shotcut's auto-add-tracks setting is on); the video track is index 0.
        MAIN.undoStack()->push(new Timeline::AddTrackCommand(model, true));
        if (Settings.timelineAutoAddTracks())
            MAIN.undoStack()->push(new Timeline::AddTrackCommand(model, false));
    }
    if (mode == "insert")
        MAIN.undoStack()->push(new Timeline::InsertCommand(model,
                                                           *dock->markersModel(),
                                                           track,
                                                           position,
                                                           xml,
                                                           false));
    else
        MAIN.undoStack()->push(new Timeline::OverwriteCommand(model, track, position, xml, false));
    MAIN.undoStack()->endMacro();
    // From the track's last frame on (always on a new track), Shotcut appends the clip at the end
    // of the track (after a gap when position is beyond it), so it is then the last item.
    const int index = position >= playtime - 1 ? model.rowCount(model.index(track)) - 1
                                               : dock->clipIndexAtPosition(track, position);
    std::unique_ptr<Mlt::ClipInfo> info = index >= 0 && kindOf(model, track, index) == "clip"
                                              ? model.getClipInfo(track, index)
                                              : nullptr;
    if (!info || info->frame_count != duration) {
        rollbackLastMacro(MAIN.undoStack()); // a created track too
        error = QStringLiteral("Shotcut did not insert the clip; nothing was changed.");
        return -1;
    }
    return index;
}

void rollbackLastMacro(QUndoStack *stack)
{
    // Qt 6.10 QUndoStack::undo() calls undo() only on a command that is not obsolete, then deletes
    // the command if it is obsolete. So undo it by hand first, then mark it obsolete so that
    // stack->undo() only removes it (marking it first would leave its changes applied).
    auto *command = const_cast<QUndoCommand *>(stack->command(stack->index() - 1));
    if (!command)
        return; // nothing below the stack index (no macro was pushed)
    command->undo();
    command->setObsolete(true);
    stack->undo();
}

bool finishMacro(QUndoStack *stack, bool ok)
{
    stack->endMacro();
    if (!ok)
        rollbackLastMacro(stack);
    return ok;
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
        // A non-string path reads as empty, which checkMediaPath refuses.
        const QString path = pathValue.toString();
        if (!checkMediaPath(path, error))
            return false;
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

bool store(const QString &path, const QJsonObject &transcript)
{
    Store &s = transcriptStore();
    const QString key = keyOf(path);
    const auto it = s.transcripts.constFind(key);
    const bool replacesSupplied = it != s.transcripts.constEnd() && it->value("supplied").toBool();
    // Only Whisper results are evicted; a supplied transcript stays until Shotcut quits.
    if (transcript.value("supplied").toBool()) {
        // Every entry outside order is a supplied one.
        if (!replacesSupplied && s.transcripts.size() - s.order.size() >= kMaxSupplied)
            return false;
        s.order.removeOne(key);
        s.transcripts.insert(key, transcript);
        return true;
    }
    if (replacesSupplied)
        return true;
    s.order.removeOne(key);
    s.order.append(key);
    if (s.order.size() > kMaxTranscripts)
        s.transcripts.remove(s.order.takeFirst());
    s.transcripts.insert(key, transcript);
    return true;
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
