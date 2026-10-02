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

#ifndef MCPTOOLSUTIL_H
#define MCPTOOLSUTIL_H

#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <memory>

class MultitrackModel;

namespace Mlt {
class ClipInfo;
class Producer;
}

namespace McpTools {

// Parses a JSON literal (used for tool input schemas).
QJsonObject schema(const char *json);

// Qt resource paths (":/...") are accepted by QDir::isAbsolutePath but are not files.
bool isResourcePath(const QString &path);

// True when the player has a valid producer.
bool isLoaded();

enum class FrameArg { Missing, Ok, Invalid };

// Reads a non-negative integer from arguments[key]; also used for track and clip indexes.
FrameArg readFrame(const QJsonObject &arguments, const char *key, int &value);

// The multitrack when a timeline is open, otherwise null.
Mlt::Producer *timeline();

// Last valid frame of the timeline when one is open, else of the player's producer.
int lastFrame();

// Moves the playhead the same way for a plain clip and for a timeline.
void movePlayhead(int position);

// Pauses, moves the playhead to `position` (a valid frame) and returns that frame as the player
// displays it, taken only after refreshes issued by this call so it is never a stale picture.
// A null image and `error` when there is no video widget or the frame is not ready in time.
QImage fetchSettledFrame(int position, QString &error, int timeoutMs = 3000);

// Adds width, height, has_audio, rotation and fps of the source media from the producer's
// properties; fields that are unknown are left out.
void addMediaFields(Mlt::Producer &producer, QJsonObject &o);

// A timeline track as get_timeline reports it.
QJsonObject trackJson(MultitrackModel &model, int track);

// Validates a track index (range, not locked unless allowLocked); with needVideo the track must
// be a video track. Read-only tools pass allowLocked.
bool validTrack(MultitrackModel &model,
                int track,
                QString &error,
                bool needVideo = false,
                bool allowLocked = false);

// Resolves (track, clip) to a plain clip; null with a message otherwise.
std::unique_ptr<Mlt::ClipInfo> plainClip(MultitrackModel &model,
                                         int track,
                                         int clip,
                                         QString &error,
                                         bool allowLocked = false);

// "clip", "blank" or "transition".
QString kindOf(MultitrackModel &model, int track, int clip);

// The clip's real source file (not its proxy), absolute against the project folder.
QString clipSourcePath(Mlt::ClipInfo &info);

// A source file to analyse: given by path, or the source of a timeline clip and its range.
struct SourceRef
{
    QString path;
    double inSeconds = 0, outSeconds = 0;
    bool isClip = false;
    int track = -1, clip = -1;
    int clipStart = 0, clipFrameIn = 0, clipFrameCount = 0;
    bool speedChanged = false;
};

// Reads exactly one of path or (track, clip) from arguments.
bool resolveSource(const QJsonObject &arguments, SourceRef &ref, QString &error);

// The timeline frames [{start, end (exclusive)}] of a source range inside the clip; empty when the
// range is outside the clip, the source was given by path, or the clip's speed is changed.
QJsonArray mapRangeToTimeline(const SourceRef &ref, double startSec, double endSec);

} // namespace McpTools

// Transcripts per source file in memory (until Shotcut quits), as transcribe returns them:
// {path, granularity, segments:[{start_s, end_s, text, words?}], ...} in source seconds. A
// supplied transcript carries "supplied": true, is not replaced by a later Whisper result and is
// never evicted; only the 50 most recent Whisper results are kept.
namespace McpTranscripts {

void store(const QString &path, const QJsonObject &transcript);
bool get(const QString &path, QJsonObject &transcript);

} // namespace McpTranscripts

#endif // MCPTOOLSUTIL_H
