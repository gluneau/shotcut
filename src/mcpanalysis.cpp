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

#include "mcpanalysis.h"
#include "mcptools.h"

#include <QJsonArray>
#include <QJsonDocument>

#include <cmath>

namespace McpTools {

namespace {

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

} // namespace

std::shared_ptr<McpJobs::Record> startProbe(const QString &path,
                                            const QString &cacheKey,
                                            const QString &program,
                                            QString &error)
{
    return McpJobs::run(
        "analyze_media",
        cacheKey,
        {{program,
          {"-v", "error", "-print_format", "json", "-show_format", "-show_streams", path}}},
        [path](const QList<McpJobs::StepOutput> &outputs, QJsonObject &out, QString &err) {
            return parseProbe(path, outputs, out, err);
        },
        error,
        cacheKey);
}

QString seconds(double value)
{
    return QString::number(value, 'f', 6);
}

double round3(double value)
{
    return std::round(value * 1000) / 1000;
}

// Uses the analyze_media probe (and its cache) to learn a source's streams and duration. An
// uncached probe may be waited on for up to 10 s even when wait_ms is 0.
bool probeInfo(const SourceRef &ref, QJsonObject &info, QString &error)
{
    const QString cacheKey = McpJobs::cacheKeyFor(ref.path, "analyze_media", QString());
    if (McpJobs::cached(cacheKey, info))
        return true;
    const QString program = McpJobs::findProgram("ffprobe");
    if (program.isEmpty()) {
        error = QStringLiteral("ffprobe was not found in Shotcut's application folder or on "
                               "PATH; install FFmpeg to analyze media.");
        return false;
    }
    const auto record = startProbe(ref.path, cacheKey, program, error);
    if (!record)
        return false;
    if (!McpJobs::waitFor(record, 10000)) {
        error = QStringLiteral("ffprobe is still probing %1; try again shortly.").arg(ref.path);
        return false;
    }
    if (record->state != "succeeded") {
        error = record->error;
        return false;
    }
    info = record->result;
    return true;
}

namespace {

// Fills the range of a source: the trimmed range of a clip, else the whole file.
bool sourceRange(const SourceRef &ref, const QJsonObject &info, AudioRange &range, QString &error)
{
    if (ref.isClip) {
        range.in = ref.inSeconds;
        range.out = ref.outSeconds;
        range.seek = {"-ss", seconds(range.in), "-t", seconds(range.out - range.in)};
    } else {
        const QJsonValue duration = info.value("duration_s");
        if (!duration.isDouble() || duration.toDouble() <= 0) {
            error = QStringLiteral("%1 has no known duration.").arg(ref.path);
            return false;
        }
        range.in = 0;
        range.out = duration.toDouble();
        range.seek.clear();
    }
    return true;
}

} // namespace

bool audioRange(const SourceRef &ref, AudioRange &range, QString &error)
{
    QJsonObject info;
    if (!probeInfo(ref, info, error))
        return false;
    if (info.value("audio").isNull()) {
        error = QStringLiteral("%1 has no audio stream.").arg(ref.path);
        return false;
    }
    return sourceRange(ref, info, range, error);
}

bool videoRange(const SourceRef &ref, AudioRange &range, QString &error)
{
    QJsonObject info;
    if (!probeInfo(ref, info, error))
        return false;
    if (info.value("video").isNull()) {
        error = QStringLiteral("%1 has no video stream.").arg(ref.path);
        return false;
    }
    return sourceRange(ref, info, range, error);
}

// A cache key part that changes when the clip moves or is retrimmed, as the result maps times to
// timeline frames.
QString rangeParams(const SourceRef &ref, const AudioRange &range)
{
    QString params = seconds(range.in) + "," + seconds(range.out);
    if (ref.isClip) {
        params += QStringLiteral(",%1,%2,%3,%4")
                      .arg(ref.clipStart)
                      .arg(ref.clipFrameIn)
                      .arg(ref.clipFrameCount)
                      .arg(ref.speedChanged);
    }
    return params;
}

bool readWaitMs(const QJsonObject &arguments, int &waitMs, QString &error)
{
    waitMs = 0;
    const FrameArg state = readFrame(arguments, "wait_ms", waitMs);
    if (state == FrameArg::Invalid || waitMs > 20000) {
        error = QStringLiteral("wait_ms must be an integer from 0 to 20000.");
        return false;
    }
    return true;
}

// The finished result when the job is done within waitMs, else {job_id, ...} to poll.
QJsonObject jobResult(const std::shared_ptr<McpJobs::Record> &record, int waitMs)
{
    if (record->id == 0)
        return jsonResult(record->result);
    if (waitMs > 0 && McpJobs::waitFor(record, waitMs)) {
        if (record->state == "succeeded")
            return jsonResult(record->result);
        return textResult(record->error, true);
    }
    return jsonResult(McpJobs::statusJson(*record));
}

} // namespace McpTools
