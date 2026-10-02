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

#include "mcpjobs.h"
#include "mcptools.h"
#include "mcptoolsutil.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>

#include <cmath>

using namespace McpTools;

namespace {

QJsonObject getJobTool(const QJsonObject &arguments)
{
    int id = 0;
    if (readFrame(arguments, "job_id", id) != FrameArg::Ok)
        return textResult("job_id must be a non-negative integer.", true);
    const auto record = McpJobs::find(id);
    if (!record)
        return textResult("No such job.", true);
    return jsonResult(McpJobs::statusJson(*record));
}

int rotationOf(const QJsonObject &stream)
{
    const QJsonArray sideData = stream.value("side_data_list").toArray();
    for (const QJsonValue &entry : sideData) {
        const QJsonValue rotation = entry.toObject().value("rotation");
        if (rotation.isDouble())
            return qRound(rotation.toDouble());
    }
    return stream.value("tags").toObject().value("rotate").toString().toInt();
}

double frameRateOf(const QString &rate)
{
    const QStringList parts = rate.split('/');
    if (parts.size() != 2)
        return 0;
    const double num = parts[0].toDouble();
    const double den = parts[1].toDouble();
    return den > 0 ? num / den : 0;
}

bool parseProbe(const QString &path,
                const QList<McpJobs::StepOutput> &outputs,
                QJsonObject &result,
                QString &error)
{
    const QJsonObject root = QJsonDocument::fromJson(outputs.value(0).out).object();
    const QJsonObject format = root.value("format").toObject();
    if (format.isEmpty()) {
        error = QStringLiteral("ffprobe found no media in %1.").arg(path);
        return false;
    }
    QJsonValue video(QJsonValue::Null);
    QJsonValue audio(QJsonValue::Null);
    for (const QJsonValue &entry : root.value("streams").toArray()) {
        const QJsonObject stream = entry.toObject();
        const QString type = stream.value("codec_type").toString();
        if (type == "video" && video.isNull()) {
            video = QJsonObject{{"width", stream.value("width").toInt()},
                                {"height", stream.value("height").toInt()},
                                {"fps", frameRateOf(stream.value("avg_frame_rate").toString())},
                                {"codec", stream.value("codec_name").toString()},
                                {"rotation", rotationOf(stream)},
                                {"pix_fmt", stream.value("pix_fmt").toString()}};
        } else if (type == "audio" && audio.isNull()) {
            audio = QJsonObject{{"channels", stream.value("channels").toInt()},
                                {"sample_rate", stream.value("sample_rate").toString().toInt()},
                                {"codec", stream.value("codec_name").toString()}};
        }
    }
    if (video.isNull() && audio.isNull()) {
        error = QStringLiteral("%1 has no audio or video stream.").arg(path);
        return false;
    }
    bool durationOk = false;
    const double duration = format.value("duration").toString().toDouble(&durationOk);
    result = QJsonObject{{"path", path},
                         {"duration_s",
                          durationOk && duration >= 0 ? QJsonValue(duration) : QJsonValue()},
                         {"size_bytes", format.value("size").toString().toLongLong()},
                         {"video", video},
                         {"audio", audio}};
    return true;
}

QJsonObject analyzeMediaTool(const QJsonObject &arguments)
{
    if (arguments.value("path").isUndefined())
        return textResult("path must be an absolute file path.", true);
    SourceRef ref;
    QString error;
    if (!resolveSource(QJsonObject{{"path", arguments.value("path")}}, ref, error))
        return textResult(error, true);
    const QString cacheKey = McpJobs::cacheKeyFor(ref.path, "analyze_media", QString());
    QJsonObject result;
    if (McpJobs::cached(cacheKey, result))
        return jsonResult(result);
    const QString program = McpJobs::findProgram("ffprobe");
    if (program.isEmpty()) {
        return textResult("ffprobe was not found in Shotcut's application folder or on PATH; "
                          "install FFmpeg to analyze media.",
                          true);
    }
    const QString path = ref.path;
    const auto record = McpJobs::run(
        "analyze_media",
        cacheKey,
        {{program,
          {"-v", "error", "-print_format", "json", "-show_format", "-show_streams", path}}},
        [path](const QList<McpJobs::StepOutput> &outputs, QJsonObject &out, QString &err) {
            return parseProbe(path, outputs, out, err);
        },
        cacheKey);
    if (McpJobs::waitFor(record, 5000)) {
        if (record->state == "succeeded")
            return jsonResult(record->result);
        return textResult(record->error, true);
    }
    return jsonResult(McpJobs::statusJson(*record));
}

} // namespace

QList<McpTool> McpTools::analysisTools()
{
    return {
        {"get_job",
         "Status of a background analysis job (the tools that return {job_id}): {job_id, kind, "
         "state (running, succeeded, failed, cancelled), percent (when known), result (once "
         "succeeded), error (once failed), elapsed_ms}. elapsed_ms is the run time so far, or "
         "the total once finished. Jobs are kept in memory until Shotcut quits, also after the "
         "project is closed; they are not files on disk.",
         schema(R"({"type":"object","properties":{
             "job_id":{"type":"integer","minimum":0}
           },"required":["job_id"]})"),
         getJobTool},
        {"analyze_media",
         "Probe a media file with ffprobe: {path, duration_s, size_bytes, video, audio}. video is "
         "null for audio-only files, else {width, height, fps, codec, rotation, pix_fmt}; "
         "rotation is in degrees as ffprobe reports it (display rotation, may be negative, e.g. "
         "-90), and an audio file with embedded cover art reports that picture as its video "
         "stream; audio is null for files without sound, else {channels, sample_rate, codec}. "
         "duration_s is null when ffprobe reports none. Only the first video and first audio stream "
         "are reported. path must be an absolute path "
         "of an existing file. The call waits up to 5 seconds; a slower probe returns {job_id, "
         "state} to poll with get_job. Results are cached per file (path, size and modification "
         "time). An error is returned when ffprobe is missing or the file is not media.",
         schema(R"({"type":"object","properties":{
             "path":{"type":"string","description":"Absolute path of the media file."}
           },"required":["path"]})"),
         analyzeMediaTool},
    };
}
