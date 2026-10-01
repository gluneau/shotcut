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

#include "jobs/abstractjob.h"
#include "mainwindow.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonObject>
#include <QPointer>
#include <QStorageInfo>

#include <algorithm>
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
    if (!record->done && record->job) {
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

QJsonObject exportVideoTool(const QJsonObject &arguments)
{
    const QString path = arguments.value("path").toString();
    if (path.isEmpty() || isResourcePath(path) || !QDir::isAbsolutePath(path))
        return textResult("path must be an absolute file path.", true);
    QString preset = QStringLiteral("x264-medium");
    const QJsonValue presetValue = arguments.value("preset");
    if (!presetValue.isUndefined()) {
        preset = presetValue.toString();
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

    const QFileInfo target(path);
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

#if defined(Q_OS_UNIX) && !defined(Q_OS_MAC)
    const QFileInfo melt(QCoreApplication::applicationDirPath(), "melt-7");
#else
    const QFileInfo melt(QCoreApplication::applicationDirPath(), "melt");
#endif
    if (!melt.exists())
        return textResult("Shotcut's melt program was not found at " + melt.filePath()
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

QList<McpTool> McpTools::exportTools()
{
    return {
        {"export_video",
         "Render the project (the timeline if there is one, else the playlist, else the loaded "
         "clip) to a video file as a background job and return {job_id, output, preset} at once. "
         "Poll get_export_status. path must be absolute and end in the preset's extension (mp4 "
         "for x264-medium: H.264 + AAC, the project's resolution and frame rate); an existing "
         "file is refused unless overwrite is true. The preset is a stock preset name or a custom "
         "preset file name in Shotcut's presets folder. Only one export runs at a time. Side "
         "effect: the settings in Shotcut's Export panel are replaced by the preset.",
         schema(R"({"type":"object","properties":{
             "path":{"type":"string","description":"Absolute output path, e.g. /home/me/out.mp4."},
             "preset":{"type":"string","description":"Preset name, default x264-medium."},
             "overwrite":{"type":"boolean","description":"Replace an existing file."}
           },"required":["path"]})"),
         exportVideoTool},
        {"get_export_status",
         "Progress of an export: {job_id, state (pending, running, paused, succeeded, failed, "
         "stopped, idle), percent, output, output_exists, output_bytes, exit_code, elapsed_ms, "
         "error}. Without job_id it reports the latest export started through MCP; error holds the "
         "end of the render log when it failed.",
         schema(R"({"type":"object","properties":{
             "job_id":{"type":"integer","minimum":0}
           }})"),
         getExportStatusTool},
    };
}
