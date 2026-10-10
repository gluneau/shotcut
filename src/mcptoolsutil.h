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
class QUndoStack;

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

// Reads a finite number from min to max from arguments[key]; value is left alone when it is
// missing. Invalid sets error to "<key> must be a number from <min> to <max>.".
FrameArg readNumberArg(const QJsonObject &arguments,
                       const char *key,
                       double min,
                       double max,
                       double &value,
                       QString &error);

// The multitrack when a timeline is open, otherwise null.
Mlt::Producer *timeline();

// Last valid frame of the timeline when one is open, else of the player's producer.
int lastFrame();

// Moves the playhead the same way for a plain clip and for a timeline.
void movePlayhead(int position);

// Wraps timeline edits (and undo/redo of them). While the outermost guard lives it pauses
// playback, blocks the preview refresh Shotcut issues after every edit and stops the preview
// consumer, which joins its read-ahead thread so no frame renders while the edit changes the
// graph (an in-flight frame crashed RemoveTrackCommand::undo). On a timeline it also connects the
// stopped consumer to a black stand-in producer at the same position, because the consumer does
// not stay stopped: a duration change makes Shotcut seek the player (Player::onDurationChanged)
// and Controller::seek starts a stopped consumer on whatever it is connected to. With standIn
// (large work) the consumer is also started on the stand-in, which shows black; without it the
// stand-in is only rendered if such a seek restarts the consumer. When the guard goes out of
// scope (on every exit path) it reconnects the timeline, restarts a consumer that was running (it
// stays stopped when there is no producer any more) and refreshes once.
// Why: Shotcut serializes clips for undo on the GUI thread while the preview thread may render
// them, which rewrites their meta.media.* properties; that race can corrupt the undo snapshot so
// that undo aborts (debug) or drops the clip (release). Every guard costs an audio device restart
// when the consumer was running.
// Nesting: inner guards are no-ops (a static depth counter). Shotcut's own RefreshBlocker and
// MultitrackModel::endBulkUpdate() also clear the same refresh-block flag when they finish; that
// only lets a refresh through early, which the stand-in makes harmless.
class EditPreviewGuard
{
public:
    explicit EditPreviewGuard(bool standIn);
    ~EditPreviewGuard();
    EditPreviewGuard(const EditPreviewGuard &) = delete;
    EditPreviewGuard &operator=(const EditPreviewGuard &) = delete;

private:
    static int s_depth;
    bool m_outer = false;
    bool m_wasStopped = false;
    bool m_consumer = false; // the outer guard saw a valid consumer (and stopped it if running)
    std::unique_ptr<Mlt::Producer> m_standIn;
};

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

// path is an absolute path of an existing file (not a Qt resource).
bool checkMediaPath(const QString &path, QString &error);

// A new producer for the media file at a checked path, prepared like Shotcut's Add Files. With
// nothing loaded the file is opened first so the project profile follows it. Generators (and
// still images unless allowImages) are refused, naming toolName. Null with a message otherwise.
std::unique_ptr<Mlt::Producer> newMediaProducer(const QString &path,
                                                const QString &toolName,
                                                bool allowImages,
                                                QString &error);

// A track a clip can be pushed to: an unlocked track, or track 0 when there is no timeline yet.
bool validTargetTrack(int track, QString &error);

// Pushes the single clip in xml (with its in and out) onto track at the absolute timeline frame
// position as ONE undo step named undoText, without moving the playhead or using the clipboard or
// the Source player. mode "insert" ripples per Shotcut's ripple settings; "overwrite" never
// ripples. With no timeline the first video track (and A1 when Shotcut's auto-add-tracks setting
// is on) is created in the same step. Refuses a position
// inside a transition (or an overwrite covering part of one) and a position more than one hour
// after the end of the track. Returns the new clip's index on the track, or -1 with a message;
// a push that did not produce the clip is rolled back and dropped from the undo stack.
int pushClipXml(const QString &mode,
                int track,
                int position,
                const QString &xml,
                const QString &undoText,
                QString &error);

// Undoes the command on top of the stack (usually a macro the caller just ended) and drops it,
// so it is neither applied nor left on the redo stack.
void rollbackLastMacro(QUndoStack *stack);

// Ends the caller's macro; when ok is false the macro is rolled back and dropped. Returns ok.
bool finishMacro(QUndoStack *stack, bool ok);

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
// never evicted; only the 50 most recent Whisper results are kept. At most kMaxSupplied paths hold
// a supplied transcript: store() refuses another path (returns false) but may replace one.
namespace McpTranscripts {

constexpr int kMaxSupplied = 200;

bool store(const QString &path, const QJsonObject &transcript);
bool get(const QString &path, QJsonObject &transcript);

} // namespace McpTranscripts

#endif // MCPTOOLSUTIL_H
