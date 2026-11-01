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

#ifndef MCPANALYSIS_H
#define MCPANALYSIS_H

#include "mcpjobs.h"
#include "mcptoolsutil.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <memory>

// Helpers shared by the analysis tools (mcptools_analysis.cpp, mcptools_transcribe.cpp).
namespace McpTools {

// The part of a source to analyse in source seconds, and the ffmpeg input options selecting it.
struct AudioRange
{
    double in = 0, out = 0;
    QStringList seek;
};

// A number of seconds with 6 decimals, for ffmpeg arguments.
QString seconds(double value);

double round3(double value);

// Starts the analyze_media probe (ffprobe at program) of path; its result is cached under
// cacheKey. Null with an error when McpJobs refuses another running job.
std::shared_ptr<McpJobs::Record> startProbe(const QString &path,
                                            const QString &cacheKey,
                                            const QString &program,
                                            QString &error);

// The probe of a source, from the cache or waited on for up to 10 s even when wait_ms is 0.
bool probeInfo(const SourceRef &ref, QJsonObject &info, QString &error);

// The range of a source with an audio (video) stream: a clip's trimmed range, else the file.
bool audioRange(const SourceRef &ref, AudioRange &range, QString &error);
bool videoRange(const SourceRef &ref, AudioRange &range, QString &error);

// The part of a cache key that depends on the range and, for a clip, its place on the timeline.
QString rangeParams(const SourceRef &ref, const AudioRange &range);

// wait_ms: an integer from 0 to 20000, 0 when missing.
bool readWaitMs(const QJsonObject &arguments, int &waitMs, QString &error);

// The tool result of a job: its result or error when done within waitMs, else its status.
QJsonObject jobResult(const std::shared_ptr<McpJobs::Record> &record, int waitMs);

// Starts the loudness and level measurement of a source (the audio_levels tool): the result is
// {path, in_s, out_s, integrated_lufs, loudness_range_lu, true_peak_dbtp,
//  buckets:[{start_s, end_s, rms_db, peak_db}]} with source-time seconds. A cached result comes
// back as an already finished record with id 0 and "cached": true; otherwise the record is the
// running job. Returns null with an error for a source without audio, longer than 7200 s,
// without a known duration, when ffmpeg or ffprobe is missing, or when McpJobs refuses another
// running job. buckets is 1..600.
std::shared_ptr<McpJobs::Record> startAudioLevels(const SourceRef &ref,
                                                  int buckets,
                                                  QString &error);

} // namespace McpTools

#endif // MCPANALYSIS_H
