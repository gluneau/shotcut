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
#include "mcpjobs.h"
#include "mcptools.h"
#include "mcptoolsutil.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <cstring>

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

std::shared_ptr<McpJobs::Record> startProbe(const QString &path,
                                            const QString &cacheKey,
                                            const QString &program)
{
    return McpJobs::run(
        "analyze_media",
        cacheKey,
        {{program,
          {"-v", "error", "-print_format", "json", "-show_format", "-show_streams", path}}},
        [path](const QList<McpJobs::StepOutput> &outputs, QJsonObject &out, QString &err) {
            return parseProbe(path, outputs, out, err);
        },
        cacheKey);
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
    const auto record = startProbe(ref.path, cacheKey, program);
    if (McpJobs::waitFor(record, 5000)) {
        if (record->state == "succeeded")
            return jsonResult(record->result);
        return textResult(record->error, true);
    }
    return jsonResult(McpJobs::statusJson(*record));
}

constexpr double kMaxLevelsSeconds = 7200;
constexpr int kLevelsRate = 8000;
constexpr double kFloorDb = -120;

// The part of a source to analyse in source seconds, and the ffmpeg input options selecting it.
struct AudioRange
{
    double in = 0, out = 0;
    QStringList seek;
};

QString seconds(double value)
{
    return QString::number(value, 'f', 6);
}

double round3(double value)
{
    return std::round(value * 1000) / 1000;
}

double round2(double value)
{
    return std::round(value * 100) / 100;
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
    const auto record = startProbe(ref.path, cacheKey, program);
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

bool readNumber(const QJsonObject &arguments,
                const char *key,
                double min,
                double max,
                double &value,
                QString &error)
{
    const QJsonValue v = arguments.value(key);
    if (v.isUndefined())
        return true;
    if (!v.isDouble() || !std::isfinite(v.toDouble()) || v.toDouble() < min || v.toDouble() > max) {
        error = QStringLiteral("%1 must be a number from %2 to %3.").arg(key).arg(min).arg(max);
        return false;
    }
    value = v.toDouble();
    return true;
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

// A finished record for a cached result.
std::shared_ptr<McpJobs::Record> cachedRecord(const QJsonObject &result)
{
    auto record = std::make_shared<McpJobs::Record>();
    record->done = true;
    record->state = QStringLiteral("succeeded");
    record->percent = 100;
    record->result = result;
    return record;
}

QJsonObject rangeJson(const SourceRef &ref, double start, double end)
{
    QJsonObject o{{"start_s", round3(start)},
                  {"end_s", round3(end)},
                  {"duration_s", round3(end - start)}};
    if (ref.isClip)
        o["timeline_frames"] = mapRangeToTimeline(ref, start, end);
    return o;
}

bool parseSilence(const SourceRef &ref,
                  const AudioRange &range,
                  const QByteArray &stderrText,
                  QJsonObject &result)
{
    static const QRegularExpression startRe(QStringLiteral("silence_start: (-?[0-9.]+)"));
    static const QRegularExpression endRe(QStringLiteral("silence_end: (-?[0-9.]+)"));
    QList<QPair<double, double>> silent;
    double pending = -1;
    // ffmpeg times are relative to the seek position.
    for (const QByteArray &line : stderrText.split('\n')) {
        const QString text = QString::fromUtf8(line);
        const auto start = startRe.match(text);
        if (start.hasMatch()) {
            pending = range.in + qMax(0.0, start.captured(1).toDouble());
            continue;
        }
        const auto end = endRe.match(text);
        if (end.hasMatch() && pending >= 0) {
            silent.append({pending, qMin(range.out, range.in + end.captured(1).toDouble())});
            pending = -1;
        }
    }
    if (pending >= 0)
        silent.append({pending, range.out});

    QJsonArray ranges;
    QJsonArray speech;
    double total = 0;
    double cursor = range.in;
    for (const auto &r : silent) {
        if (r.second <= r.first)
            continue;
        if (r.first - cursor >= 0.01)
            speech.append(rangeJson(ref, cursor, r.first));
        ranges.append(rangeJson(ref, r.first, r.second));
        total += r.second - r.first;
        cursor = qMax(cursor, r.second);
    }
    if (range.out - cursor >= 0.01)
        speech.append(rangeJson(ref, cursor, range.out));
    result = QJsonObject{{"path", ref.path},
                         {"in_s", round3(range.in)},
                         {"out_s", round3(range.out)},
                         {"ranges", ranges},
                         {"speech_ranges", speech},
                         {"total_silence_s", round3(total)}};
    return true;
}

QJsonObject detectSilenceTool(const QJsonObject &arguments)
{
    QString error;
    double noise = -35;
    double minimum = 0.4;
    int waitMs = 0;
    if (!readNumber(arguments, "noise_db", -90, -10, noise, error)
        || !readNumber(arguments, "min_s", 0.05, 30, minimum, error)
        || !readWaitMs(arguments, waitMs, error)) {
        return textResult(error, true);
    }
    SourceRef ref;
    if (!resolveSource(arguments, ref, error))
        return textResult(error, true);
    const QString program = McpJobs::findProgram("ffmpeg");
    if (program.isEmpty()) {
        return textResult("ffmpeg was not found in Shotcut's application folder or on PATH; "
                          "install FFmpeg to analyze audio.",
                          true);
    }
    AudioRange range;
    if (!audioRange(ref, range, error))
        return textResult(error, true);

    const QString key = McpJobs::cacheKeyFor(ref.path,
                                             "silence",
                                             QStringLiteral("%1|%2|%3")
                                                 .arg(rangeParams(ref, range))
                                                 .arg(noise)
                                                 .arg(minimum));
    QJsonObject result;
    if (McpJobs::cached(key, result))
        return jsonResult(result);
    QStringList args{"-hide_banner", "-nostats", "-nostdin"};
    args += range.seek;
    args += {"-i",
             ref.path,
             "-vn",
             "-af",
             QStringLiteral("silencedetect=noise=%1dB:d=%2").arg(noise).arg(minimum),
             "-f",
             "null",
             "-"};
    const auto record = McpJobs::run(
        "detect_silence",
        key,
        {{program, args}},
        [ref, range](const QList<McpJobs::StepOutput> &outputs, QJsonObject &out, QString &) {
            return parseSilence(ref, range, outputs.value(0).err, out);
        },
        key);
    return jobResult(record, waitMs);
}

bool parseScenes(const SourceRef &ref,
                 const AudioRange &range,
                 const QByteArray &stdoutText,
                 QJsonObject &result)
{
    static const QRegularExpression timeRe(QStringLiteral("pts_time:(-?[0-9.eE+-]+)"));
    static const QRegularExpression scoreRe(QStringLiteral("lavfi\\.scene_score=([0-9.eE+-]+)"));
    QJsonArray scenes;
    double pending = -1;
    // ffmpeg times are relative to the seek position.
    for (const QByteArray &line : stdoutText.split('\n')) {
        const QString text = QString::fromUtf8(line);
        const auto time = timeRe.match(text);
        if (time.hasMatch()) {
            pending = range.in + qMax(0.0, time.captured(1).toDouble());
            continue;
        }
        const auto score = scoreRe.match(text);
        if (!score.hasMatch() || pending < 0)
            continue;
        QJsonObject scene{{"time_s", round3(pending)},
                          {"score", round3(score.captured(1).toDouble())}};
        if (ref.isClip) {
            const QJsonArray frames = mapRangeToTimeline(ref, pending, range.out);
            if (!frames.isEmpty())
                scene["timeline_frames"] = frames.first().toObject().value("start");
        }
        scenes.append(scene);
        pending = -1;
    }
    result = QJsonObject{{"path", ref.path},
                         {"in_s", round3(range.in)},
                         {"out_s", round3(range.out)},
                         {"scenes", scenes}};
    return true;
}

QJsonObject detectScenesTool(const QJsonObject &arguments)
{
    QString error;
    double threshold = 0.3;
    int waitMs = 0;
    if (!readNumber(arguments, "threshold", 0.05, 0.9, threshold, error)
        || !readWaitMs(arguments, waitMs, error)) {
        return textResult(error, true);
    }
    SourceRef ref;
    if (!resolveSource(arguments, ref, error))
        return textResult(error, true);
    const QString program = McpJobs::findProgram("ffmpeg");
    if (program.isEmpty()) {
        return textResult("ffmpeg was not found in Shotcut's application folder or on PATH; "
                          "install FFmpeg to analyze video.",
                          true);
    }
    AudioRange range;
    if (!videoRange(ref, range, error))
        return textResult(error, true);

    const QString key = McpJobs::cacheKeyFor(ref.path,
                                             "scenes",
                                             QStringLiteral("%1|%2")
                                                 .arg(rangeParams(ref, range))
                                                 .arg(threshold));
    QJsonObject result;
    if (McpJobs::cached(key, result))
        return jsonResult(result);
    QStringList args{"-hide_banner", "-nostats", "-nostdin"};
    args += range.seek;
    args += {"-i",
             ref.path,
             "-an",
             "-vf",
             QStringLiteral("select='gt(scene,%1)',metadata=print:file=-").arg(threshold),
             "-f",
             "null",
             "-"};
    const auto record = McpJobs::run(
        "detect_scenes",
        key,
        {{program, args}},
        [ref, range](const QList<McpJobs::StepOutput> &outputs, QJsonObject &out, QString &) {
            return parseScenes(ref, range, outputs.value(0).out, out);
        },
        key);
    return jobResult(record, waitMs);
}

// Buckets the mono float samples as they stream in, so the length of the source does not matter.
struct LevelsAccumulator
{
    int buckets = 1;
    qint64 total = 1; // expected number of samples
    qint64 index = 0;
    QByteArray carry;
    QVector<double> sumSquares;
    QVector<float> peak;
    QVector<qint64> count;

    LevelsAccumulator(int bucketCount, qint64 expected)
        : buckets(bucketCount)
        , total(qMax<qint64>(1, expected))
        , sumSquares(bucketCount, 0)
        , peak(bucketCount, 0)
        , count(bucketCount, 0)
    {}

    void add(const QByteArray &data)
    {
        const QByteArray bytes = carry.isEmpty() ? data : carry + data;
        const qsizetype samples = bytes.size() / 4;
        for (qsizetype i = 0; i < samples; ++i, ++index) {
            float x;
            std::memcpy(&x, bytes.constData() + i * 4, sizeof x);
            if (!std::isfinite(x))
                continue;
            const int b = int(qMin<qint64>(buckets - 1, index * buckets / total));
            sumSquares[b] += double(x) * x;
            peak[b] = qMax(peak[b], std::fabs(x));
            ++count[b];
        }
        carry = bytes.mid(samples * 4);
    }
};

double dbOf(double amplitude)
{
    return amplitude > 0 ? qMax(kFloorDb, 20 * std::log10(amplitude)) : kFloorDb;
}

// The last "<label> <number> <unit>" line of the ebur128 summary; -inf counts as the floor.
bool summaryValue(const QString &summary, const QString &label, double &value)
{
    const QRegularExpression re(QStringLiteral("^\\s*%1:\\s+(-?inf|[-+]?[0-9.]+)\\s")
                                    .arg(label),
                                QRegularExpression::MultilineOption);
    QRegularExpressionMatchIterator it = re.globalMatch(summary);
    if (!it.hasNext())
        return false;
    QRegularExpressionMatch last;
    while (it.hasNext())
        last = it.next();
    const QString number = last.captured(1);
    value = number.endsWith("inf") ? kFloorDb : number.toDouble();
    return true;
}

bool parseLevels(const SourceRef &ref,
                 const AudioRange &range,
                 const std::shared_ptr<LevelsAccumulator> &acc,
                 const QByteArray &stderrText,
                 QJsonObject &result,
                 QString &error)
{
    const QString text = QString::fromUtf8(stderrText);
    const int at = text.lastIndexOf("Summary:");
    double lufs = 0, lra = 0, peakDb = 0;
    if (at < 0 || !summaryValue(text.mid(at), "I", lufs)
        || !summaryValue(text.mid(at), "LRA", lra)
        || !summaryValue(text.mid(at), "Peak", peakDb)) {
        error = QStringLiteral("The loudness summary of ffmpeg could not be read.");
        return false;
    }
    QJsonArray list;
    const double length = range.out - range.in;
    for (int b = 0; b < acc->buckets; ++b) {
        const qint64 n = acc->count[b];
        const double rms = n > 0 ? std::sqrt(acc->sumSquares[b] / n) : 0;
        list.append(QJsonObject{{"start_s", round3(range.in + length * b / acc->buckets)},
                                {"end_s", round3(range.in + length * (b + 1) / acc->buckets)},
                                {"rms_db", round2(dbOf(rms))},
                                {"peak_db", round2(dbOf(acc->peak[b]))}});
    }
    result = QJsonObject{{"path", ref.path},
                         {"in_s", round3(range.in)},
                         {"out_s", round3(range.out)},
                         {"integrated_lufs", lufs},
                         {"loudness_range_lu", lra},
                         {"true_peak_dbtp", peakDb},
                         {"buckets", list}};
    return true;
}

QJsonObject audioLevelsTool(const QJsonObject &arguments)
{
    QString error;
    int waitMs = 0;
    int buckets = 60;
    if (!readWaitMs(arguments, waitMs, error))
        return textResult(error, true);
    const FrameArg state = readFrame(arguments, "buckets", buckets);
    if (state == FrameArg::Invalid || (state == FrameArg::Ok && (buckets < 1 || buckets > 600)))
        return textResult("buckets must be an integer from 1 to 600.", true);
    SourceRef ref;
    if (!resolveSource(arguments, ref, error))
        return textResult(error, true);
    const auto record = startAudioLevels(ref, buckets, error);
    if (!record)
        return textResult(error, true);
    return jobResult(record, waitMs);
}

} // namespace

std::shared_ptr<McpJobs::Record> McpTools::startAudioLevels(const SourceRef &ref,
                                                            int buckets,
                                                            QString &error)
{
    const QString program = McpJobs::findProgram("ffmpeg");
    if (program.isEmpty()) {
        error = QStringLiteral("ffmpeg was not found in Shotcut's application folder or on PATH; "
                               "install FFmpeg to analyze audio.");
        return nullptr;
    }
    AudioRange range;
    if (!audioRange(ref, range, error))
        return nullptr;
    if (range.out - range.in > kMaxLevelsSeconds) {
        error = QStringLiteral("%1 is longer than %2 s; analyse a clip range of it instead.")
                    .arg(ref.path)
                    .arg(kMaxLevelsSeconds);
        return nullptr;
    }
    const QString key = McpJobs::cacheKeyFor(ref.path,
                                             "levels",
                                             QStringLiteral("%1|%2").arg(rangeParams(ref, range))
                                                 .arg(buckets));
    QJsonObject result;
    if (McpJobs::cached(key, result))
        return cachedRecord(result);

    // 8 kHz mono keeps 32 kB per second, so the samples are bucketed as they arrive rather than
    // collected (McpJobs keeps only the first 8 MiB of a step's stdout).
    auto acc = std::make_shared<LevelsAccumulator>(buckets,
                                                   qRound64((range.out - range.in)
                                                            * kLevelsRate));
    QStringList loudness{"-hide_banner", "-nostats", "-nostdin"};
    loudness += range.seek;
    loudness += {"-i",
                 ref.path,
                 "-vn",
                 "-af",
                 "ebur128=peak=true:framelog=quiet",
                 "-f",
                 "null",
                 "-"};
    QStringList decode{"-hide_banner", "-nostats", "-nostdin"};
    decode += range.seek;
    decode += {"-i", ref.path, "-vn", "-ac", "1", "-ar", QString::number(kLevelsRate), "-f",
               "f32le", "-"};
    McpJobs::Step decodeStep{program, decode};
    decodeStep.onStdout = [acc](const QByteArray &data) { acc->add(data); };
    return McpJobs::run(
        "audio_levels",
        key,
        {{program, loudness}, decodeStep},
        [ref, range, acc](const QList<McpJobs::StepOutput> &outputs,
                          QJsonObject &out,
                          QString &err) {
            return parseLevels(ref, range, acc, outputs.value(0).err, out, err);
        },
        key);
}

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
        {"detect_silence",
         "Find silent and spoken stretches of a source's audio with ffmpeg silencedetect. Give "
         "path (absolute file) or track and clip (a timeline clip: only its trimmed range is "
         "analysed; a locked track is fine). Returns {path, in_s, out_s, ranges, speech_ranges, "
         "total_silence_s}; ranges are the silent stretches and speech_ranges the rest of "
         "[in_s, out_s], each {start_s, end_s, duration_s}. All times are seconds in the SOURCE "
         "file. For a clip source every range also has timeline_frames, [{start, end (exclusive)}] "
         "on the timeline, empty when it does not map (speed changed). Silence boundaries are "
         "approximate (about 0.1 s). noise_db (-90 to -10, default -35) is the level below which "
         "audio counts as silent; min_s (0.05 to 30, default 0.4) the shortest silence reported. "
         "A source without audio is an error. wait_ms (0 to 20000, default 0) waits for the "
         "result; without it, or when slower, {job_id, state} is returned to poll with get_job. Results "
         "are cached (\"cached\": true).",
         schema(R"({"type":"object","properties":{
             "path":{"type":"string","description":"Absolute path of the media file."},
             "track":{"type":"integer","minimum":0,"description":"Track index (with clip)."},
             "clip":{"type":"integer","minimum":0,"description":"Clip index on the track."},
             "noise_db":{"type":"number","minimum":-90,"maximum":-10,"default":-35},
             "min_s":{"type":"number","minimum":0.05,"maximum":30,"default":0.4},
             "wait_ms":{"type":"integer","minimum":0,"maximum":20000,"default":0}
           }})"),
         detectSilenceTool},
        {"audio_levels",
         "Measure the loudness of a source's audio with ffmpeg: {path, in_s, out_s, "
         "integrated_lufs, loudness_range_lu, true_peak_dbtp, buckets} where buckets is "
         "[{start_s, end_s, rms_db, peak_db}] of equal slices of [in_s, out_s] (dBFS, floor -120), "
         "e.g. for a waveform or to find quiet parts. Give path or track and clip (a clip: only "
         "its trimmed range; a locked track is fine); times are seconds in the SOURCE file. "
         "buckets (1 to 600, default 60). A source without audio is an error, and so is one "
         "(or a clip range) longer than 7200 s. integrated_lufs reads -70 for silence (ebur128's own "
         "floor) and true_peak_dbtp is floored at -120; buckets with no decoded samples (audio "
         "shorter than the requested range) report -120. wait_ms (0 to 20000, default 0) waits for the "
         "result; without it, or when slower, {job_id, state} is returned to poll with get_job. "
         "Results are cached (\"cached\": true).",
         schema(R"({"type":"object","properties":{
             "path":{"type":"string","description":"Absolute path of the media file."},
             "track":{"type":"integer","minimum":0,"description":"Track index (with clip)."},
             "clip":{"type":"integer","minimum":0,"description":"Clip index on the track."},
             "buckets":{"type":"integer","minimum":1,"maximum":600,"default":60},
             "wait_ms":{"type":"integer","minimum":0,"maximum":20000,"default":0}
           }})"),
         audioLevelsTool},
        {"detect_scenes",
         "Find scene changes (cuts) in a source's video with ffmpeg's scene score. Give path "
         "(absolute file) or track and clip (a timeline clip: only its trimmed range is "
         "analysed; a locked track is fine). Returns {path, in_s, out_s, scenes} where scenes is "
         "[{time_s, score, timeline_frames}]: time_s is the first frame of the new scene in "
         "seconds in the SOURCE file, score (0 to 1, 3 decimals) how different it is from the "
         "frame before, and for a clip source timeline_frames the timeline frame number of that "
         "time (omitted when it does not map, e.g. speed changed). threshold (0.05 to 0.9, "
         "default 0.3) is the lowest score reported; a gradual change scores lower than a hard "
         "cut, so a high threshold misses soft cuts. A source without video is an error. "
         "wait_ms (0 to 20000, default 0) waits for the result; without it, or when slower, "
         "{job_id, state} is returned to poll with get_job. Results are cached (\"cached\": true).",
         schema(R"({"type":"object","properties":{
             "path":{"type":"string","description":"Absolute path of the media file."},
             "track":{"type":"integer","minimum":0,"description":"Track index (with clip)."},
             "clip":{"type":"integer","minimum":0,"description":"Clip index on the track."},
             "threshold":{"type":"number","minimum":0.05,"maximum":0.9,"default":0.3},
             "wait_ms":{"type":"integer","minimum":0,"maximum":20000,"default":0}
           }})"),
         detectScenesTool},
    };
}
