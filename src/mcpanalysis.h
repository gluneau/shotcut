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

#include <QString>

#include <memory>

namespace McpTools {

// Starts the loudness and level measurement of a source (the audio_levels tool): the result is
// {path, in_s, out_s, integrated_lufs, loudness_range_lu, true_peak_dbtp,
//  buckets:[{start_s, end_s, rms_db, peak_db}]} with source-time seconds. A cached result comes
// back as an already finished record with id 0 and "cached": true; otherwise the record is the
// running job. Returns null with an error for a source without audio, longer than 7200 s,
// without a known duration, or when ffmpeg or ffprobe is missing. buckets is 1..600.
std::shared_ptr<McpJobs::Record> startAudioLevels(const SourceRef &ref,
                                                  int buckets,
                                                  QString &error);

} // namespace McpTools

#endif // MCPANALYSIS_H
