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

#include "mcpfilters.h"
#include "mcpanalysis.h"
#include "mcpjobs.h"
#include "mcptools.h"
#include "mcptoolsutil.h"

#include "settings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QThread>

#include <memory>

using namespace McpTools;

namespace {

constexpr int kMaxSegments = 5000;
constexpr int kMaxWordsPerSegment = 2000;
constexpr double kMaxTranscriptSeconds = 86400;
constexpr int kMaxTextChars = 2000;
constexpr qint64 kMaxWhisperJson = 64 * 1024 * 1024;

// Reads {start_s, end_s, text} of a supplied segment or word; text comes back trimmed. Words may
// have no length (Whisper gives some words start_s == end_s).
bool readTimedText(const QJsonValue &value,
                   const QString &where,
                   bool isWord,
                   double &start,
                   double &end,
                   QString &text,
                   QString &error)
{
    const QJsonObject o = value.toObject();
    const QJsonValue s = o.value("start_s");
    const QJsonValue e = o.value("end_s");
    const QJsonValue t = o.value("text");
    if (!value.isObject() || !s.isDouble() || !e.isDouble() || s.toDouble() < 0
        || s.toDouble() > e.toDouble() || (!isWord && s.toDouble() == e.toDouble())
        || e.toDouble() > kMaxTranscriptSeconds) {
        error = QStringLiteral("%1 needs numbers start_s and end_s with 0 <= start_s %2 end_s "
                               "<= %3.")
                    .arg(where, isWord ? "<=" : "<")
                    .arg(kMaxTranscriptSeconds);
        return false;
    }
    // Characters are Unicode code points, as for track names and marker texts.
    text = t.toString().trimmed();
    if (!t.isString() || text.isEmpty() || t.toString().toUcs4().size() > kMaxTextChars) {
        error = QStringLiteral("%1 needs a non-empty text of at most %2 characters.")
                    .arg(where)
                    .arg(kMaxTextChars);
        return false;
    }
    start = s.toDouble();
    end = e.toDouble();
    return true;
}

bool readTranscript(const QJsonValue &value,
                    const QString &path,
                    QJsonObject &transcript,
                    QString &error)
{
    const QJsonValue list = value.toObject().value("segments");
    if (!value.isObject() || !list.isArray() || list.toArray().isEmpty()
        || list.toArray().size() > kMaxSegments) {
        error = QStringLiteral("transcript must be {segments:[{start_s, end_s, text, words?}]} "
                               "with 1 to %1 segments.")
                    .arg(kMaxSegments);
        return false;
    }
    QJsonArray segments;
    bool allWords = true;
    double previousStart = 0;
    const QJsonArray input = list.toArray();
    for (int i = 0; i < input.size(); ++i) {
        const QString where = QStringLiteral("segments[%1]").arg(i);
        double start = 0, end = 0;
        QString text;
        if (!readTimedText(input.at(i), where, false, start, end, text, error))
            return false;
        if (start < previousStart) {
            error = QStringLiteral("segments must be sorted by start_s (%1).").arg(where);
            return false;
        }
        previousStart = start;
        QJsonObject segment{{"start_s", start}, {"end_s", end}, {"text", text}};
        const QJsonValue wordsValue = input.at(i).toObject().value("words");
        QJsonArray words;
        if (!wordsValue.isUndefined()) {
            if (!wordsValue.isArray() || wordsValue.toArray().size() > kMaxWordsPerSegment) {
                error = QStringLiteral("%1.words must be an array of at most %2 words.")
                            .arg(where)
                            .arg(kMaxWordsPerSegment);
                return false;
            }
            double previousWord = start;
            const QJsonArray wordList = wordsValue.toArray();
            for (int j = 0; j < wordList.size(); ++j) {
                const QString wordWhere = QStringLiteral("%1.words[%2]").arg(where).arg(j);
                double wordStart = 0, wordEnd = 0;
                QString wordText;
                if (!readTimedText(wordList.at(j),
                                   wordWhere,
                                   true,
                                   wordStart,
                                   wordEnd,
                                   wordText,
                                   error)) {
                    return false;
                }
                if (wordStart < start || wordEnd > end) {
                    error = QStringLiteral("%1 must lie inside its segment.").arg(wordWhere);
                    return false;
                }
                if (wordStart < previousWord) {
                    error = QStringLiteral("%1 starts before the word before it.").arg(wordWhere);
                    return false;
                }
                previousWord = wordStart;
                words.append(QJsonObject{{"start_s", wordStart},
                                         {"end_s", wordEnd},
                                         {"text", wordText}});
            }
        }
        if (words.isEmpty())
            allWords = false;
        else
            segment["words"] = words;
        segments.append(segment);
    }
    transcript = QJsonObject{{"path", path},
                             {"granularity", allWords ? "word" : "segment"},
                             {"segments", segments},
                             {"supplied", true}};
    return true;
}

// The segments with timeline_frames on every segment and word for a clip source, or without any
// (the form kept in McpTranscripts).
QJsonArray segmentsFor(const SourceRef &ref, const QJsonArray &segments, bool withFrames)
{
    auto place = [&ref, withFrames](QJsonObject &o) {
        if (withFrames && ref.isClip) {
            o["timeline_frames"] = mapRangeToTimeline(ref,
                                                      o.value("start_s").toDouble(),
                                                      o.value("end_s").toDouble());
        } else {
            o.remove("timeline_frames");
        }
    };
    QJsonArray list;
    for (const QJsonValue &value : segments) {
        QJsonObject segment = value.toObject();
        place(segment);
        if (segment.contains("words")) {
            QJsonArray words;
            for (const QJsonValue &word : segment.value("words").toArray()) {
                QJsonObject w = word.toObject();
                place(w);
                words.append(w);
            }
            segment["words"] = words;
        }
        list.append(segment);
    }
    return list;
}

// Keeps a Whisper result for captions_from_transcript (a supplied transcript wins).
void rememberTranscript(const SourceRef &ref, QJsonObject result)
{
    result.remove("cached");
    result["segments"] = segmentsFor(ref, result.value("segments").toArray(), false);
    McpTranscripts::store(ref.path, result);
}

// whisper.cpp writes token texts byte by byte, so one UTF-8 character can be split over two
// tokens. The file is read as Latin-1 (one character per byte) and texts are decoded as UTF-8
// once joined.
QString utf8Of(const QString &bytes)
{
    return QString::fromUtf8(bytes.toLatin1());
}

// whisper-cli can exit 0 without a transcript (an unknown language, for one) and then prints its
// help after the error, so the error lines are looked for before falling back to the output tail.
QString whisperDiagnostics(const McpJobs::StepOutput &output)
{
    const QString text = QString::fromUtf8(output.err + "\n" + output.out);
    QStringList found;
    for (const QString &line : text.split('\n')) {
        if (line.contains("error", Qt::CaseInsensitive) || line.contains("unknown")
            || line.contains("failed", Qt::CaseInsensitive)) {
            found.append(line.trimmed());
        }
    }
    if (!found.isEmpty())
        return found.mid(qMax(0, int(found.size()) - 5)).join(' ');
    return QString::fromUtf8(output.err.right(2048) + output.out.right(2048)).trimmed();
}

// A punctuation token (ASCII without letters or digits, or Unicode punctuation such as an
// ellipsis or a dash) joins the word's text but not its time. A token holding only part of a
// split UTF-8 character decodes to U+FFFD and counts as a word piece.
bool isPunctuation(const QString &bytes)
{
    for (const QChar c : utf8Of(bytes)) {
        if (c.unicode() < 0x80 ? c.isLetterOrNumber() : !c.isPunct())
            return false;
    }
    return true;
}

bool parseWhisper(const SourceRef &ref,
                  const AudioRange &range,
                  const QString &language,
                  const QString &jsonPath,
                  const McpJobs::StepOutput &whisperOutput,
                  QJsonObject &result,
                  QString &error)
{
    QFile file(jsonPath);
    if (!file.open(QIODevice::ReadOnly) || file.size() > kMaxWhisperJson) {
        error = QStringLiteral("Whisper wrote no readable transcript.");
        const QString details = whisperDiagnostics(whisperOutput);
        if (!details.isEmpty())
            error += " " + details;
        return false;
    }
    QJsonParseError parseError;
    const QJsonObject root = QJsonDocument::fromJson(QString::fromLatin1(file.readAll()).toUtf8(),
                                                     &parseError)
                                 .object();
    if (parseError.error != QJsonParseError::NoError || !root.value("transcription").isArray()) {
        error = QStringLiteral("The transcript written by Whisper could not be read.");
        return false;
    }
    struct Word
    {
        QString bytes;
        double from = -1, to = -1;
    };
    bool anyTimed = false;
    QJsonArray segments;
    for (const QJsonValue &value : root.value("transcription").toArray()) {
        const QJsonObject segment = value.toObject();
        const QString text = utf8Of(segment.value("text").toString()).trimmed();
        const QJsonObject offsets = segment.value("offsets").toObject();
        if (text.isEmpty() || !offsets.value("from").isDouble() || !offsets.value("to").isDouble())
            continue;
        const double start = qBound(range.in,
                                    range.in + offsets.value("from").toDouble() / 1000,
                                    range.out);
        const double end = qBound(start,
                                  range.in + offsets.value("to").toDouble() / 1000,
                                  range.out);
        // A segment past the end of the range is clamped to nothing; a zero-length segment would
        // also be refused when the result is supplied back as a transcript.
        if (round3(end) <= round3(start))
            continue;
        QList<Word> words;
        for (const QJsonValue &tokenValue : segment.value("tokens").toArray()) {
            const QJsonObject token = tokenValue.toObject();
            QString piece = token.value("text").toString();
            if (piece.isEmpty() || piece.startsWith("[_"))
                continue;
            if (piece.startsWith(' ') || words.isEmpty()) {
                words.append(Word());
                if (piece.startsWith(' '))
                    piece.remove(0, 1);
            }
            Word &word = words.last();
            word.bytes += piece;
            const QJsonObject times = token.value("offsets").toObject();
            if (times.value("from").isDouble() && times.value("to").isDouble()
                && (word.from < 0 || !isPunctuation(piece))) {
                if (word.from < 0)
                    word.from = times.value("from").toDouble();
                word.to = times.value("to").toDouble();
                anyTimed = true;
            }
        }
        QJsonArray wordList;
        double previous = start;
        for (const Word &word : words) {
            const QString wordText = utf8Of(word.bytes).trimmed();
            if (word.from < 0 || wordText.isEmpty())
                continue;
            const double wordStart = qBound(previous, range.in + word.from / 1000, end);
            const double wordEnd = qBound(wordStart, range.in + word.to / 1000, end);
            previous = wordStart;
            wordList.append(QJsonObject{{"start_s", round3(wordStart)},
                                        {"end_s", round3(wordEnd)},
                                        {"text", wordText}});
        }
        segments.append(QJsonObject{{"start_s", round3(start)},
                                    {"end_s", round3(end)},
                                    {"text", text},
                                    {"words", wordList}});
    }
    // No speech at all still has word granularity: there are no words to time.
    if (segments.isEmpty())
        anyTimed = true;
    if (!anyTimed) {
        for (auto it = segments.begin(); it != segments.end(); ++it) {
            QJsonObject segment = it->toObject();
            segment.remove("words");
            *it = segment;
        }
    }
    const QString detected = root.value("result").toObject().value("language").toString();
    result = QJsonObject{{"path", ref.path},
                         {"in_s", round3(range.in)},
                         {"out_s", round3(range.out)},
                         {"language", detected.isEmpty() ? language : detected},
                         {"granularity", anyTimed ? "word" : "segment"},
                         {"segments", segments}};
    rememberTranscript(ref, result);
    result["segments"] = segmentsFor(ref, segments, true);
    return true;
}

QJsonObject storeTranscript(const QJsonObject &arguments)
{
    for (const char *key : {"track", "clip", "language", "wait_ms"}) {
        if (arguments.contains(QLatin1String(key))) {
            return textResult(QStringLiteral("Pass transcript with path only, not with %1.")
                                  .arg(key),
                              true);
        }
    }
    if (!arguments.contains("path"))
        return textResult("Pass path, the file the transcript belongs to, with transcript.", true);
    SourceRef ref;
    QString error;
    QJsonObject transcript;
    if (!resolveSource(QJsonObject{{"path", arguments.value("path")}}, ref, error)
        || !readTranscript(arguments.value("transcript"), ref.path, transcript, error)) {
        return textResult(error, true);
    }
    if (!McpTranscripts::store(ref.path, transcript)) {
        return textResult(QStringLiteral("Transcripts are already supplied for %1 files, the "
                                         "most kept; nothing was stored. Replace the transcript "
                                         "of one of those files instead.")
                              .arg(McpTranscripts::kMaxSupplied),
                          true);
    }
    return jsonResult(transcript);
}

QJsonObject transcribeTool(const QJsonObject &arguments)
{
    QString keyError;
    if (!McpFilters::checkKeys(arguments,
                               {"path", "track", "clip", "language", "wait_ms", "transcript"},
                               keyError))
        return textResult(keyError, true);
    if (arguments.contains("transcript"))
        return storeTranscript(arguments);
    QString error;
    QString language = QStringLiteral("auto");
    const QJsonValue languageValue = arguments.value("language");
    if (!languageValue.isUndefined()) {
        // The shape of Whisper's own codes; whisper-cli itself rejects an unknown one.
        static const QRegularExpression code(
            QRegularExpression::anchoredPattern(QStringLiteral("[a-z]{2}|auto|haw|yue")));
        language = languageValue.toString();
        if (!languageValue.isString() || !code.match(language).hasMatch()) {
            return textResult("language must be auto or a Whisper language code (ISO 639-1, e.g. "
                              "en, fr).",
                              true);
        }
    }
    int waitMs = 0;
    if (!readWaitMs(arguments, waitMs, error))
        return textResult(error, true);
    SourceRef ref;
    if (!resolveSource(arguments, ref, error))
        return textResult(error, true);
    QJsonObject stored;
    if (!ref.isClip && McpTranscripts::get(ref.path, stored) && stored.value("supplied").toBool())
        return jsonResult(stored);

    const QString exe = Settings.whisperExe();
    const QFileInfo exeInfo(exe);
    if (exe.isEmpty() || !exeInfo.isFile() || !exeInfo.isExecutable()) {
        return textResult("Whisper is not configured: set the Whisper.cpp executable in Shotcut "
                          "(Subtitles panel > Speech to Text... > Configuration), or supply a "
                          "transcript with transcribe {path, transcript}.",
                          true);
    }
    const QString model = Settings.whisperModel();
    if (model.isEmpty() || !QFileInfo(model).isFile()) {
        return textResult("Whisper is not configured: choose a GGML model in Shotcut (Subtitles "
                          "panel > Speech to Text... > Configuration), or supply a transcript "
                          "with transcribe {path, transcript}.",
                          true);
    }
    const QString program = McpJobs::findProgram("ffmpeg");
    if (program.isEmpty()) {
        return textResult("ffmpeg was not found in Shotcut's application folder or on PATH; "
                          "install FFmpeg to transcribe.",
                          true);
    }
    AudioRange range;
    if (!audioRange(ref, range, error))
        return textResult(error, true);

    const QString key = McpJobs::cacheKeyFor(ref.path,
                                             "transcribe",
                                             QStringLiteral("%1|%2|%3")
                                                 .arg(rangeParams(ref, range), language, model));
    QJsonObject result;
    if (McpJobs::cached(key, result)) {
        rememberTranscript(ref, result);
        return jsonResult(result);
    }
    // Removed when the job lets go of its parser, whichever way it ends.
    auto dir = std::make_shared<QTemporaryDir>(
        QDir::temp().filePath(QStringLiteral("shotcut-transcribe-XXXXXX")));
    if (!dir->isValid())
        return textResult("Could not create a temporary folder: " + dir->errorString(), true);
    const QString wav = dir->filePath("audio.wav");
    const QString out = dir->filePath("out");
    QStringList decode{"-hide_banner", "-nostdin", "-y"};
    decode += range.seek;
    decode += {"-i", ref.path, "-vn", "-ac", "1", "-ar", "16000", wav};
    // whisper-cli assumes English without --language, so auto is passed too.
    QStringList whisper{"--file",
                        wav,
                        "--model",
                        model,
                        "--language",
                        language,
                        "--threads",
                        QString::number(qMax(1, QThread::idealThreadCount() - 1)),
                        "-ojf",
                        "--output-file",
                        out};
    McpJobs::Step whisperStep{exe, whisper};
    if (Settings.whisperUseGpu()) {
        whisperStep.failureHint = QStringLiteral("If the GPU is the problem, turn off Use GPU in "
                                                 "Shotcut's Speech to Text dialog.");
    } else {
        whisperStep.args << "--no-gpu";
    }
    const int timeoutMs = int(qMin(4.0 * 3600 * 1000, 600000 + (range.out - range.in) * 1000));
    const auto record = McpJobs::run(
        "transcribe",
        key,
        {{program, decode}, whisperStep},
        [ref, range, language, dir, out](const QList<McpJobs::StepOutput> &outputs,
                                         QJsonObject &transcript,
                                         QString &err) {
            return parseWhisper(ref,
                                range,
                                language,
                                out + ".json",
                                outputs.value(1),
                                transcript,
                                err);
        },
        error,
        key,
        timeoutMs);
    if (!record)
        return textResult(error, true);
    return jobResult(record, waitMs);
}

} // namespace

QList<McpTool> McpTools::transcribeTools()
{
    return {
        {"transcribe",
         "Speech to text with word times, using the Whisper.cpp program and model configured in "
         "Shotcut (Subtitles panel > Speech to Text... > Configuration); nothing is downloaded. "
         "Give path (absolute file) or track and clip (a timeline clip: only its trimmed range is "
         "transcribed; a locked track is fine). Returns {path, in_s, out_s, language, granularity, "
         "segments} where segments is [{start_s, end_s, text, words:[{start_s, end_s, text}]}], "
         "all times in seconds in the SOURCE file (words inside their segment, in order; a word "
         "may have start_s == end_s; a segment that has no length once clamped to the analysed "
         "range is dropped; punctuation, also Unicode such as an ellipsis or a dash, joins a "
         "word's text but does not extend its end). granularity is \"word\", or \"segment\" "
         "(no words) when the program gave no word times; audio without speech gives segments "
         "[]. For a clip source every segment and word also has timeline_frames, [{start, end "
         "(exclusive)}] on "
         "the timeline, empty when it does not map (speed changed). language (default auto) is "
         "auto or a Whisper language code (ISO 639-1, e.g. en, fr); the result names the "
         "detected one. A source without audio is an error, and so is Whisper not being "
         "configured (checked before any job starts). wait_ms (0 to 20000, default 0) waits for "
         "the result; without it, or when slower, {job_id, state} is returned to poll with "
         "get_job. The first call on a file may take up to 10 s while ffprobe runs, even with "
         "wait_ms 0. Only one transcription runs at a time. Results are cached (\"cached\": "
         "true). A failing run is not retried: the "
         "error has the program's own message. Bring your own transcript instead: pass path and "
         "transcript (and nothing else) with transcript {segments:[{start_s, end_s, text, "
         "words?}]}: 1 to 5000 segments sorted by start_s, 0 <= start_s < end_s <= 86400 (words "
         "start_s <= end_s), text non-empty (at most 2000 characters, counted as Unicode code "
         "points), words inside their "
         "segment and in order. It is kept in memory for that file until Shotcut quits (for at "
         "most 200 files: another file is then refused, replacing one is allowed), with "
         "\"supplied\": true and granularity \"word\" when every segment has words, else "
         "\"segment\"; transcribe with only that path then returns it, and "
         "captions_from_transcript uses it. A track and clip call always runs Whisper; it does "
         "not read a supplied transcript.",
         schema(R"({"type":"object","properties":{
             "path":{"type":"string","description":"Absolute path of the media file."},
             "track":{"type":"integer","minimum":0,"description":"Track index (with clip)."},
             "clip":{"type":"integer","minimum":0,"description":"Clip index on the track."},
             "language":{"type":"string","default":"auto",
               "description":"auto or a Whisper language code (ISO 639-1, e.g. en, fr)."},
             "wait_ms":{"type":"integer","minimum":0,"maximum":20000,"default":0},
             "transcript":{"type":"object","properties":{
               "segments":{"type":"array","minItems":1,"maxItems":5000,"items":{
                 "type":"object","properties":{
                   "start_s":{"type":"number","minimum":0,"maximum":86400},
                   "end_s":{"type":"number","minimum":0,"maximum":86400},
                   "text":{"type":"string","minLength":1,"maxLength":2000},
                   "words":{"type":"array","items":{"type":"object","properties":{
                     "start_s":{"type":"number"},"end_s":{"type":"number"},
                     "text":{"type":"string"}},"required":["start_s","end_s","text"]}}},
                 "required":["start_s","end_s","text"]}}},
               "required":["segments"],
               "description":"A transcript to keep for path instead of running Whisper."}
           },"additionalProperties":false})"),
         transcribeTool},
    };
}
