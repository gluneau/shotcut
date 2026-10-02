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

#include "autosavefile.h"
#include "docks/timelinedock.h"
#include "jobs/abstractjob.h"
#include "mainwindow.h"
#include "mltcontroller.h"
#include "models/multitrackmodel.h"
#include "proxymanager.h"

#include <MltPlaylist.h>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonObject>
#include <QPointer>
#include <QProcess>
#include <QSet>
#include <QStorageInfo>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <vector>

using namespace McpTools;

namespace {

struct ExportRecord
{
    int id = 0;
    QString output;
    QPointer<AbstractJob> job;
    QString state = QStringLiteral("pending");
    int percent = 0;
    int exitCode = -1;
    QString error;
    bool done = false;
    QElapsedTimer timer;
    qint64 elapsedMs = 0;
};

// Records survive the Jobs dock removing finished jobs; ids are never queue rows.
std::vector<std::shared_ptr<ExportRecord>> &records()
{
    static std::vector<std::shared_ptr<ExportRecord>> list;
    return list;
}

std::shared_ptr<ExportRecord> findRecord(int id)
{
    for (const auto &record : records()) {
        if (record->id == id)
            return record;
    }
    return nullptr;
}

QJsonObject statusJson(const std::shared_ptr<ExportRecord> &record)
{
    QString state = record->state;
    if (!record->done && !record->job) {
        state = QStringLiteral("removed"); // taken out of the Jobs dock before it finished
    } else if (!record->done && record->job) {
        if (!record->job->ran())
            state = QStringLiteral("pending");
        else
            state = record->job->paused() ? QStringLiteral("paused") : QStringLiteral("running");
    }
    const QFileInfo output(record->output);
    return QJsonObject{
        {"job_id", record->id},
        {"state", state},
        {"percent", record->percent},
        {"output", record->output},
        {"output_exists", output.exists()},
        {"output_bytes", static_cast<double>(output.exists() ? output.size() : 0)},
        {"exit_code", record->exitCode},
        {"elapsed_ms",
         static_cast<double>(record->done ? record->elapsedMs : record->timer.elapsed())},
        {"error",
         record->error.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(record->error)}};
}

// Canonical form of a path that may not exist yet: the parent folder is resolved instead.
QString canonicalTarget(const QString &path)
{
    const QFileInfo info(path);
    if (info.exists())
        return info.canonicalFilePath();
    const QString folder = QFileInfo(info.absolutePath()).canonicalFilePath();
    return folder.isEmpty() ? QString() : folder + "/" + info.fileName();
}

// Every media file and the project file that an export must never overwrite.
QSet<QString> protectedFiles()
{
    QSet<QString> files;
    const QString projectFolder = MAIN.fileName().isEmpty()
                                      ? QString()
                                      : QFileInfo(MAIN.fileName()).canonicalPath();
    auto add = [&](QString path) {
        if (path.isEmpty())
            return;
        if (!QDir::isAbsolutePath(path) && !projectFolder.isEmpty())
            path = QDir(projectFolder).absoluteFilePath(path);
        const QString canonical = canonicalTarget(path);
        if (!canonical.isEmpty())
            files.insert(canonical);
    };
    auto addProducer = [&](Mlt::Producer *producer) {
        if (!producer || !producer->is_valid())
            return;
        add(ProxyManager::resource(*producer));
        add(QString::fromUtf8(producer->get("resource")));
    };
    if (MAIN.isMultitrackValid()) {
        MultitrackModel &model = *MAIN.timelineDock()->model();
        for (int t = 0; t < model.trackList().size(); ++t) {
            const int count = model.rowCount(model.index(t));
            for (int c = 0; c < count; ++c) {
                std::unique_ptr<Mlt::ClipInfo> info = model.getClipInfo(t, c);
                if (info)
                    addProducer(info->producer);
            }
        }
    }
    Mlt::Playlist *playlist = MAIN.playlist();
    if (playlist && playlist->is_valid()) {
        for (int i = 0; i < playlist->count(); ++i) {
            std::unique_ptr<Mlt::ClipInfo> info(playlist->clip_info(i));
            if (info)
                addProducer(info->producer);
        }
    }
    add(MLT.resource());
    add(MAIN.fileName());
    return files;
}

QJsonObject exportVideoTool(const QJsonObject &arguments)
{
    const QString path = arguments.value("path").toString();
    if (path.isEmpty() || isResourcePath(path) || !QDir::isAbsolutePath(path))
        return textResult("path must be an absolute file path.", true);
    QString preset = QStringLiteral("x264-medium");
    const QJsonValue presetValue = arguments.value("preset");
    if (!presetValue.isUndefined()) {
        preset = presetValue.toString();
        // ".." anywhere is rejected, which also rejects (harmless) names such as "my..preset".
        if (!presetValue.isString() || preset.isEmpty() || preset.contains("..")
            || preset.contains('\\') || preset.contains(QChar(0)) || preset.startsWith('.')
            || QDir::isAbsolutePath(preset) || QFileInfo(preset).isAbsolute())
            return textResult("preset must be a preset name such as x264-medium.", true);
    }
    const QJsonValue overwriteValue = arguments.value("overwrite");
    if (!overwriteValue.isUndefined() && !overwriteValue.isBool())
        return textResult("overwrite must be a boolean.", true);

    for (const auto &record : records()) {
        if (!record->done && record->job)
            return textResult("An export is already running (job_id " + QString::number(record->id)
                                  + "); wait for it to finish.",
                              true);
    }

    const QString slashPath = QDir::fromNativeSeparators(path);
    if (slashPath != QDir::cleanPath(slashPath))
        return textResult("path must be normalized (no ./, ../ or doubled slashes).", true);
    const QFileInfo target(path);
    if (target.isSymLink())
        return textResult("path must not be a symbolic link.", true);
    if (target.isDir())
        return textResult("path is a directory: " + path, true);
    if (target.exists() && !overwriteValue.toBool(false))
        return textResult(path + " already exists; pass overwrite: true to replace it.", true);
    const QFileInfo folder(target.dir().path());
    if (!folder.exists() || !folder.isDir() || !folder.isWritable())
        return textResult("The folder " + folder.filePath()
                              + " does not exist or is not writable.",
                          true);
    if (QStorageInfo(folder.filePath()).bytesAvailable() < qint64(1) << 30)
        return textResult("Less than 1 GiB of free space in " + folder.filePath() + ".", true);
    const QSet<QString> protectedSet = protectedFiles();
    const QString canonical = canonicalTarget(path);
    const QString autosave = QFileInfo(AutoSaveFile::path()).canonicalFilePath();
    bool isProtected = protectedSet.contains(canonical)
                       || (!autosave.isEmpty() && canonical.startsWith(autosave + "/"));
    if (!isProtected && target.exists()) {
        // Same file under another name (hard link, bind mount): compare file identity.
        const std::filesystem::path targetPath(path.toStdString());
        std::error_code ec;
        const auto links = std::filesystem::hard_link_count(targetPath, ec);
        if (ec || links > 1)
            return textResult("The output file has other names (hard links); choose another "
                              "path.",
                              true);
        for (const QString &file : protectedSet) {
            if (!QFileInfo::exists(file))
                continue;
            ec.clear();
            const bool same = std::filesystem::equivalent(targetPath, file.toStdString(), ec);
            if (ec || same) {
                isProtected = true;
                break;
            }
        }
    }
    if (isProtected)
        return textResult("The output path is a media file used by the project; "
                          "choose another path.",
                          true);

#if defined(Q_OS_UNIX) && !defined(Q_OS_MAC)
    const QFileInfo melt(QCoreApplication::applicationDirPath(), "melt-7");
#else
    const QFileInfo melt(QCoreApplication::applicationDirPath(), "melt");
#endif
    if (!melt.isExecutable())
        return textResult("Shotcut's melt program was not found or is not executable at "
                              + melt.filePath()
                              + ". In a development build link it in, for example: ln -s "
                                "<mlt prefix>/bin/melt-7 "
                              + melt.filePath(),
                          true);

    QString error;
    AbstractJob *job = MAIN.exportToFile(preset, path, error);
    if (!job)
        return textResult(error.isEmpty() ? QStringLiteral("Shotcut could not start the export.")
                                          : error,
                          true);

    static int nextId = 1;
    auto record = std::make_shared<ExportRecord>();
    record->id = nextId++;
    record->output = path;
    record->job = job;
    record->timer.start();
    records().push_back(record);
    // The job is the context, so these connections end with it; `done` makes finishing idempotent
    // (MeltJob can emit finished twice).
    QObject::connect(job, &AbstractJob::progressUpdated, job, [record](QStandardItem *, int pct) {
        if (pct >= 0 && pct <= 100)
            record->percent = std::max(record->percent, pct);
    });
    QObject::connect(job, &AbstractJob::finished, job, [record](AbstractJob *j, bool ok, QString) {
        if (record->done)
            return;
        record->done = true;
        record->elapsedMs = record->timer.elapsed();
        record->exitCode = j->exitCode();
        record->state = ok ? "succeeded" : j->stopped() ? "stopped" : "failed";
        if (ok)
            record->percent = 100;
        else
            record->error = j->log().right(2048);
    });
    // A process that cannot start never emits finished.
    auto startFailed = [record](AbstractJob *j) {
        if (record->done)
            return;
        record->done = true;
        record->elapsedMs = record->timer.elapsed();
        record->state = "failed";
        record->error = "melt could not be started (or nice, which Shotcut runs it through "
                        "for some job priorities): "
                        + j->errorString();
    };
    QObject::connect(job,
                     &QProcess::errorOccurred,
                     job,
                     [job, startFailed](QProcess::ProcessError e) {
                         if (e == QProcess::FailedToStart)
                             startFailed(job);
                     });
    if (job->error() == QProcess::FailedToStart)
        startFailed(job);
    return jsonResult(QJsonObject{{"job_id", record->id}, {"output", path}, {"preset", preset}});
}

QJsonObject getExportStatusTool(const QJsonObject &arguments)
{
    const QJsonValue idValue = arguments.value("job_id");
    if (idValue.isUndefined()) {
        if (records().empty())
            return jsonResult(QJsonObject{{"state", "idle"}});
        return jsonResult(statusJson(records().back()));
    }
    int id = 0;
    if (readFrame(arguments, "job_id", id) != FrameArg::Ok)
        return textResult("job_id must be a non-negative integer.", true);
    const auto record = findRecord(id);
    if (!record)
        return textResult("There is no export with job_id " + QString::number(id) + ".", true);
    return jsonResult(statusJson(record));
}

} // namespace

bool McpTools::exportBusy()
{
    for (const auto &record : records()) {
        if (!record->done && record->job)
            return true;
    }
    return false;
}

QList<McpTool> McpTools::exportTools()
{
    return {
        {"export_video",
         "Render the project (the timeline if there is one, else the current playlist bin, else "
         "the loaded clip) to a video file as a background job and return {job_id, output, "
         "preset} at once. Poll get_export_status. path must be absolute and end in the preset's "
         "extension (mp4 for x264-medium: H.264 + AAC, the project's resolution and frame "
         "rate); an existing file is refused unless overwrite is true, and the path must not be "
         "a media file or the project file used by the project, nor a symbolic link. The preset "
         "is a stock preset name or a custom preset file name in Shotcut's presets folder. Only "
         "one export runs at a time. Side effects: the Export panel's settings and its "
         "\"export from\" choice are overwritten by the preset; the panel's hardware-encoder and "
         "proxy/preview-scale choices may apply; the missing-files check and the analysis "
         "prompts are skipped, so analysis filters are not run. The guard protects media used by "
         "the project (timeline clips, playlist items, the loaded clip, the project file, by "
         "path and by file identity) but cannot see files referenced only inside filters or "
         "nested project clips: do not pass overwrite: true for a file you did not create.",
         schema(R"({"type":"object","properties":{
             "path":{"type":"string","description":"Absolute output path, e.g. /home/me/out.mp4."},
             "preset":{"type":"string","description":"Preset name, default x264-medium."},
             "overwrite":{"type":"boolean","description":"Replace an existing file."}
           },"required":["path"]})"),
         exportVideoTool},
        {"get_export_status",
         "Progress of an export: {job_id, state (pending, running, paused, succeeded, failed, "
         "stopped, removed, idle), percent, output, output_exists, output_bytes, exit_code, "
         "elapsed_ms, error}. Without job_id it reports the latest export started through MCP; "
         "error holds the end of the render log when it failed. removed means the job was taken "
         "out of the Jobs dock before it finished. output_exists and output_bytes describe the "
         "file on disk now; elapsed_ms is the run time so far, or the total once finished. "
         "exit_code is the raw process exit code; a render that reached 99% or more counts as "
         "succeeded.",
         schema(R"({"type":"object","properties":{
             "job_id":{"type":"integer","minimum":0}
           }})"),
         getExportStatusTool},
    };
}
