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
    if (std::unique_ptr<Mlt::ClipInfo> info = model.getClipInfo(track, clip)) {
        o["start"] = info->start;
        o["duration"] = info->frame_count;
        o["in"] = info->frame_in;
        o["out"] = info->frame_out;
    }
    o["name"] = model.data(index, MultitrackModel::NameRole).toString();
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
    // Generators and still images need a duration, which Shotcut asks for with a dialog.
    if (MLT.isLiveProducer(producer.data()) && !producer->get_int(kShotcutVirtualClip))
        return textResult("Generators and still images are not supported by playlist_add.", true);

    ProxyManager::generateIfNotExists(*producer);
    MAIN.undoStack()->push(
        new Playlist::AppendCommand(*MAIN.playlistDock()->model(), MLT.XML(producer.data())));
    return jsonResult(playlistJson());
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
         "Add a media file (absolute path) to the end of the playlist. Project files, generators "
         "and still images are not supported. With nothing loaded the first file also becomes the "
         "loaded clip, as in Shotcut's Add Files. Returns the playlist.",
         schema(R"({"type":"object","properties":{
             "path":{"type":"string","description":"Absolute path of a media file."}
           },"required":["path"]})"),
         playlistAddTool},
    };
}
