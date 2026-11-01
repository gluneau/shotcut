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
#include "mcpfiltercommands.h"
#include "mcpfilters.h"
#include "mcpjobs.h"
#include "mcptools.h"
#include "mcptoolsutil.h"

#include "commands/timelinecommands.h"
#include "docks/timelinedock.h"
#include "mainwindow.h"
#include "mltcontroller.h"
#include "models/multitrackmodel.h"
#include "shotcut_mlt_properties.h"

#include <MltPlaylist.h>
#include <MltTransition.h>
#include <framework/mlt_version.h>

#include <QPair>
#include <QStringList>
#include <QUndoStack>
#include <QUuid>

#include <cmath>
#include <memory>

using namespace McpTools;
using namespace McpFilters;

namespace {

// ---- Audio recipes

QString dbText(double v)
{
    return num(std::round(v * 100) / 100);
}

// Sets the clip's volume (an audioGain "gain" filter, never a fade or mute filter) to gainDb as
// ONE undo step named text: changes the first enabled gain filter when there is one, else adds
// one; 0 dB removes it. changed is false when the clip already had that volume (no undo step).
// index receives the gain filter's index afterwards, or -1 when there is none.
bool setClipGain(int track,
                 int clip,
                 double gainDb,
                 const QString &text,
                 bool &changed,
                 int &index,
                 QString &error)
{
    changed = false;
    index = -1;
    if (!MAIN.isMultitrackValid()) {
        error = QStringLiteral("There is no timeline.");
        return false;
    }
    const FilterSpec *spec = McpFilters::findSpec("gain");
    if (!spec) {
        error = QStringLiteral("The gain filter is not available.");
        return false;
    }
    MultitrackModel &model = *MAIN.timelineDock()->model();
    std::unique_ptr<Mlt::ClipInfo> info = checkClip(model, track, clip, *spec, error);
    if (!info)
        return false;
    std::unique_ptr<Mlt::Filter> gain;
    const QList<int> indices = McpFilter::userFilterIndices(*info->producer);
    for (int i = 0; i < indices.size() && !gain; ++i) {
        std::unique_ptr<Mlt::Filter> filter(info->producer->filter(indices.at(i)));
        if (filter && McpFilters::specOf(*filter) == spec && !filter->get_int("disable")) {
            gain = std::move(filter);
            index = i;
        }
    }
    if (gain && isKeyframed(*gain, "level")) {
        error = QStringLiteral("The volume of clip %1 on track %2 has keyframes (gain filter "
                               "%3); remove them in Shotcut or with filter_remove first.")
                    .arg(clip)
                    .arg(track)
                    .arg(index);
        return false;
    }
    if (gainDb == 0) {
        if (!gain)
            return true;
        const int at = index;
        gain.reset();
        info.reset();
        auto *command = new McpFilter::RemoveCommand(model,
                                                     McpFilter::Target{track, clip},
                                                     at,
                                                     text);
        if (!pushCommand(command, text, false, [command] { return command->removed(); }, error))
            return false;
        index = -1;
        changed = true;
        return true;
    }
    const QJsonObject params{{"level", gainDb}};
    if (gain) {
        if (std::abs(gain->get_double("level") - gainDb) < 1e-9)
            return true;
        if (!setFilter(model, track, clip, index, *gain, *spec, params, text, false, error))
            return false;
    } else if (!addFilter(model, track, clip, *info, *spec, params, text, false, error, &index)) {
        return false;
    }
    changed = true;
    return true;
}

QJsonObject clipSetVolumeTool(const QJsonObject &arguments)
{
    int track = 0, clip = 0;
    QString error;
    if (!checkKeys(arguments, {"track", "clip", "gain_db"}, error)
        || !readTarget(arguments, track, clip, error))
        return textResult(error, true);
    double gain = 0;
    if (readNumberArg(arguments, "gain_db", -70, 24, gain, error) != FrameArg::Ok)
        return textResult(error.isEmpty() ? QStringLiteral("gain_db must be a number from -70 "
                                                           "to 24.")
                                          : error,
                          true);
    const QString text = gain == 0 ? QObject::tr("Reset clip volume to 0 dB")
                                   : QObject::tr("Set clip volume to %1 dB").arg(dbText(gain));
    bool changed = false;
    int index = -1;
    if (!setClipGain(track, clip, gain, text, changed, index, error))
        return textResult(error, true);
    QJsonObject result{{"track", trackJson(*MAIN.timelineDock()->model(), track)},
                       {"gain_db", gain},
                       {"changed", changed}};
    if (index >= 0)
        result["index"] = index;
    return jsonResult(result);
}

// Counts timeline creations and closings, so a clip_normalize that finishes later can tell that
// the project it measured is gone even when a new timeline reuses the same address.
int timelineGeneration()
{
    static int generation = 0;
    static bool connected = false;
    if (!connected) {
        connected = true;
        MultitrackModel *model = MAIN.timelineDock()->model();
        QObject::connect(model, &MultitrackModel::aboutToClose, model, [] { ++generation; });
        QObject::connect(model, &MultitrackModel::created, model, [] { ++generation; });
    }
    return generation;
}

// A clip_normalize call: what it measured, and the clip as it was when the call started.
struct Normalize
{
    int track = 0, clip = 0;
    int start = 0, frameIn = 0, frameOut = 0;
    QString path;
    QUuid uuid; // of the clip's parent producer, stamped at call time
    int generation = 0;
    double target = -16;
    std::shared_ptr<McpJobs::Record> levels;
    std::shared_ptr<McpJobs::Record> job; // set when the result is delivered through get_job
    bool handled = false;
};

// The timeline the call measured is still open (saving, also under a new name, keeps it).
bool sameProject(const Normalize &n)
{
    return MAIN.isMultitrackValid() && timelineGeneration() == n.generation;
}

// Finds the measured clip by identity, so edits elsewhere (a split of an earlier clip, a new
// video track shifting the rows) do not matter: same in, out and source file, and either the same
// start (and no other uuid) or, when it was moved, the same parent-producer uuid (Shotcut keeps it
// across moves). Matches on the recorded track win; exactly one match is required.
bool findMeasuredClip(const Normalize &n, int &track, int &clip)
{
    MultitrackModel &model = *MAIN.timelineDock()->model();
    QList<QPair<int, int>> exact, moved;
    for (int t = 0; t < model.trackList().size(); ++t) {
        for (int c = 0; c < model.rowCount(model.index(t)); ++c) {
            if (kindOf(model, t, c) != "clip")
                continue;
            std::unique_ptr<Mlt::ClipInfo> info = model.getClipInfo(t, c);
            if (!info || !info->producer || info->frame_in != n.frameIn
                || info->frame_out != n.frameOut || clipSourcePath(*info) != n.path)
                continue;
            const QUuid uuid = MLT.uuid(*info->producer);
            if (info->start == n.start && (uuid.isNull() || uuid == n.uuid))
                exact.append({t, c});
            else if (info->start != n.start && !n.uuid.isNull() && uuid == n.uuid)
                moved.append({t, c});
        }
    }
    for (const QList<QPair<int, int>> &found : {exact, moved}) {
        QList<QPair<int, int>> onTrack;
        for (const auto &f : found) {
            if (f.first == n.track)
                onTrack.append(f);
        }
        const QList<QPair<int, int>> &pick = onTrack.isEmpty() ? found : onTrack;
        if (pick.size() == 1) {
            track = pick.first().first;
            clip = pick.first().second;
            return true;
        }
        if (!pick.isEmpty())
            return false;
    }
    return false;
}

// Applies the measured gain; the result or an error. Runs on the GUI thread outside any other
// tool call (in the clip_normalize call itself or through runWhenIdle).
bool applyNormalize(Normalize &n, QJsonObject &result, QString &error)
{
    n.handled = true;
    if (n.levels->state != "succeeded") {
        error = QStringLiteral("The loudness measurement failed: %1").arg(n.levels->error);
        return false;
    }
    const QJsonValue lufs = n.levels->result.value("integrated_lufs");
    if (!lufs.isDouble() || !std::isfinite(lufs.toDouble()) || lufs.toDouble() <= -70) {
        error = QStringLiteral("The source of the clip is silent (integrated loudness at or "
                               "below -70 LUFS); nothing was changed.");
        return false;
    }
    if (!sameProject(n)) {
        error = QStringLiteral("The project was closed or replaced while measuring; nothing was "
                               "changed.");
        return false;
    }
    int track = n.track, clip = n.clip;
    if (!findMeasuredClip(n, track, clip)) {
        error = QStringLiteral("The clip changed while measuring; nothing was changed.");
        return false;
    }
    const double measured = lufs.toDouble();
    const double gain = qBound(-70.0, std::round((n.target - measured) * 100) / 100, 24.0);
    bool changed = false;
    int index = -1;
    const QString text = QObject::tr("Normalize to %1 LUFS").arg(num(n.target));
    if (!setClipGain(track, clip, gain, text, changed, index, error))
        return false;
    result = QJsonObject{{"measured_lufs", measured},
                         {"applied_gain_db", gain},
                         {"target_lufs", n.target},
                         {"changed", changed},
                         {"clip", clip},
                         {"track", trackJson(*MAIN.timelineDock()->model(), track)}};
    if (index >= 0)
        result["index"] = index;
    return true;
}

void finishNormalize(const std::shared_ptr<Normalize> &n)
{
    if (n->handled)
        return;
    QJsonObject result;
    QString error;
    if (applyNormalize(*n, result, error))
        McpJobs::complete(n->job, "succeeded", result, QString());
    else
        McpJobs::complete(n->job, "failed", QJsonObject(), error);
}

QJsonObject clipNormalizeTool(const QJsonObject &arguments)
{
    int track = 0, clip = 0, waitMs = 0;
    QString error;
    if (!checkKeys(arguments, {"track", "clip", "target_lufs", "wait_ms"}, error)
        || !readTarget(arguments, track, clip, error))
        return textResult(error, true);
    auto n = std::make_shared<Normalize>();
    if (readNumberArg(arguments, "target_lufs", -30, -6, n->target, error) == FrameArg::Invalid
        || !readWaitMs(arguments, waitMs, error))
        return textResult(error, true);
    const FilterSpec *spec = McpFilters::findSpec("gain");
    if (!spec)
        return textResult("The gain filter is not available.", true);
    MultitrackModel &model = *MAIN.timelineDock()->model();
    std::unique_ptr<Mlt::ClipInfo> info = checkClip(model, track, clip, *spec, error);
    if (!info)
        return textResult(error, true);
    SourceRef ref;
    if (!resolveSource(QJsonObject{{"track", track}, {"clip", clip}}, ref, error))
        return textResult(error, true);
    if (ref.speedChanged)
        return textResult(QStringLiteral("Clip %1 on track %2 has a speed change; clip_normalize "
                                         "cannot measure it (use audio_levels and "
                                         "clip_set_volume).")
                              .arg(clip)
                              .arg(track),
                          true);
    n->track = track;
    n->clip = clip;
    n->start = info->start;
    n->frameIn = info->frame_in;
    n->frameOut = info->frame_out;
    n->path = clipSourcePath(*info);
    // The same stamp UndoHelper gives every clip it records (not saved: "_" property).
    n->uuid = MLT.ensureHasUuid(*info->producer);
    n->generation = timelineGeneration();
    info.reset();
    // The default bucket count shares audio_levels' cache.
    n->levels = startAudioLevels(ref, 60, error);
    if (!n->levels)
        return textResult(error, true);
    if (McpJobs::waitFor(n->levels, waitMs)) {
        QJsonObject result;
        if (!applyNormalize(*n, result, error))
            return textResult(error, true);
        return jsonResult(result);
    }
    n->job = McpJobs::addFollower("clip_normalize");
    // finish() may run inside another tool call's nested event loop: apply when idle.
    n->levels->onDone.append([n] { runWhenIdle([n] { finishNormalize(n); }); });
    return jsonResult(QJsonObject{{"job_id", n->job->id},
                                  {"state", n->job->state},
                                  {"measure_job_id", n->levels->id}});
}

// The "mix" transition that mixes the audio of the track with this MLT index into the tracks
// below, found as Shotcut's ducking commands find it (Timeline::getMixTransitionByTrackIndex),
// but in the timeline model's tractor rather than the player's producer.
std::unique_ptr<Mlt::Transition> mixTransition(MultitrackModel &model, int mltIndex)
{
    Mlt::Tractor *tractor = model.tractor();
    if (!tractor || !tractor->is_valid())
        return nullptr;
    std::unique_ptr<Mlt::Service> service(tractor->producer());
    while (service && service->is_valid()) {
        if (service->type() == mlt_service_transition_type) {
            Mlt::Transition transition((mlt_transition) service->get_service());
            if (QByteArray(transition.get("mlt_service")) == "mix"
                && transition.get_b_track() == mltIndex)
                return std::make_unique<Mlt::Transition>(transition);
        }
        service.reset(service->producer());
    }
    return nullptr;
}

QJsonObject trackSetDuckingTool(const QJsonObject &arguments)
{
    int track = 0;
    QString error;
    const QStringList keys{"track", "threshold_db", "reduction_db", "fade_in_ms", "fade_out_ms",
                           "enabled"};
    if (!checkKeys(arguments, keys, error))
        return textResult(error, true);
    if (readFrame(arguments, "track", track) != FrameArg::Ok)
        return textResult("track must be a non-negative integer.", true);
    double threshold = -12, reduction = -12, fadeIn = 1500, fadeOut = 250;
    if (readNumberArg(arguments, "threshold_db", -70, 0, threshold, error) == FrameArg::Invalid
        || readNumberArg(arguments, "reduction_db", -70, 0, reduction, error) == FrameArg::Invalid
        || readNumberArg(arguments, "fade_in_ms", 0, 5000, fadeIn, error) == FrameArg::Invalid
        || readNumberArg(arguments, "fade_out_ms", 0, 5000, fadeOut, error) == FrameArg::Invalid)
        return textResult(error, true);
    const QJsonValue enabledArg = arguments.value("enabled");
    if (!enabledArg.isUndefined() && !enabledArg.isBool())
        return textResult("enabled must be a boolean.", true);
    const bool enabled = enabledArg.toBool(true);
    if (!MAIN.isMultitrackValid())
        return textResult("There is no timeline.", true);
    MultitrackModel &model = *MAIN.timelineDock()->model();
    if (!validTrack(model, track, error))
        return textResult(error, true);
    if (track == 0)
        return textResult("Track 0 is the top track; ducking lowers a track while the tracks "
                          "above it are loud, so it applies to tracks below the top one.",
                          true);
    const int mltIndex = model.trackList().at(track).mlt_index;
    if (!mixTransition(model, mltIndex))
        return textResult(QStringLiteral("Track %1 has no audio mix transition.").arg(track), true);
    constexpr int kDuckingVersion = (7 << 16) + (41 << 8);
    if (mlt_version_get_int() < kDuckingVersion)
        return textResult(QStringLiteral("Audio ducking needs MLT 7.41 or newer (this is %1).")
                              .arg(mlt_version_get_string()),
                          true);
    // Shotcut's ducking commands look the transition up in the player's producer, so the player
    // must show the timeline (it may show a source clip).
    if (!MLT.isMultitrack())
        MAIN.seekTimeline(MAIN.multitrack()->position());
    if (!MLT.isMultitrack())
        return textResult("The player could not be switched to the timeline.", true);

    MLT.pause();
    QUndoStack *stack = MAIN.undoStack();
    const QString text = enabled ? QObject::tr("Set track %1 audio ducking").arg(track)
                                 : QObject::tr("Disable track %1 audio ducking").arg(track);
    stack->beginMacro(text);
    stack->push(new Timeline::ChangeDuckThresholdCommand(mltIndex, threshold, enabled, text));
    stack->push(new Timeline::ChangeTransitionPropertyCommand(mltIndex,
                                                              "duck_attenuation",
                                                              reduction,
                                                              text,
                                                              false));
    stack->push(new Timeline::ChangeTransitionPropertyCommand(mltIndex,
                                                              "duck_fade_in",
                                                              fadeIn,
                                                              text,
                                                              false));
    stack->push(new Timeline::ChangeTransitionPropertyCommand(mltIndex,
                                                              "duck_fade_out",
                                                              fadeOut,
                                                              text,
                                                              false));
    std::unique_ptr<Mlt::Transition> mix = mixTransition(model, mltIndex);
    const bool ok = mix && mix->get_double("duck_threshold") == (enabled ? threshold : 0.0)
                    && mix->get_double("duck_attenuation") == reduction
                    && mix->get_double("duck_fade_in") == fadeIn
                    && mix->get_double("duck_fade_out") == fadeOut;
    if (!finishMacro(stack, ok))
        return textResult("Shotcut did not apply the ducking settings; nothing was changed.",
                          true);
    return jsonResult(
        QJsonObject{{"track", trackJson(model, track)},
                    {"ducking",
                     QJsonObject{{"enabled", enabled},
                                 {"threshold_db", threshold},
                                 {"reduction_db", reduction},
                                 {"fade_in_ms", fadeIn},
                                 {"fade_out_ms", fadeOut},
                                 {"duck_threshold", mix->get_double("duck_threshold")}}}});
}

// The audio track DetachAudioCommand will use for an audio clip of playtime frames at start: the
// first one blank over the whole range (Timeline::DetachAudioCommand::redo); -1 when it will add
// one.
int detachTargetTrack(MultitrackModel &model, int start, int playtime)
{
    for (int i = 0; i < model.trackList().size(); ++i) {
        std::unique_ptr<Mlt::Producer> track(
            model.tractor()->track(model.trackList().at(i).mlt_index));
        if (!track || !track->is_valid() || !track->get(kAudioTrackProperty))
            continue;
        Mlt::Playlist playlist(*track);
        const int end = start + playtime - 1;
        if (playlist.is_blank_at(start) && playlist.is_blank_at(end)
            && playlist.get_clip_index_at(start) == playlist.get_clip_index_at(end))
            return i;
    }
    return -1;
}

QJsonObject clipDetachAudioTool(const QJsonObject &arguments)
{
    int track = 0, clip = 0;
    QString error;
    if (!checkKeys(arguments, {"track", "clip"}, error)
        || !readTarget(arguments, track, clip, error))
        return textResult(error, true);
    TimelineDock *dock = MAIN.timelineDock();
    MultitrackModel &model = *dock->model();
    std::unique_ptr<Mlt::ClipInfo> info = plainClip(model, track, clip, error);
    if (!info)
        return textResult(error, true);
    if (model.trackList().at(track).type != VideoTrackType)
        return textResult(QStringLiteral("Track %1 is an audio track; clip_detach_audio takes a "
                                         "clip on a video track.")
                              .arg(track),
                          true);
    Mlt::Producer &producer = *info->producer;
    if (!producer.get("audio_index") || producer.get_int("audio_index") < 0)
        return textResult(QStringLiteral("Clip %1 on track %2 has no audio to detach.")
                              .arg(clip)
                              .arg(track),
                          true);
    const int target = detachTargetTrack(model, info->start, info->frame_count);
    if (target >= 0 && dock->isTrackLocked(target))
        return textResult(QStringLiteral("The audio would go to track %1, which is locked; unlock "
                                         "it first.")
                              .arg(target),
                          true);
    const int audioTrack = target >= 0 ? target : int(model.trackList().size());
    info.reset();

    EditPreviewGuard previewGuard(false);
    QUndoStack *stack = MAIN.undoStack();
    stack->beginMacro(QObject::tr("Detach audio"));
    dock->detachAudio(track, clip);
    std::unique_ptr<Mlt::ClipInfo> after = model.getClipInfo(track, clip);
    const bool ok = after && after->producer && after->producer->get_int("audio_index") < 0;
    if (!finishMacro(stack, ok))
        return textResult("Shotcut did not detach the audio; nothing was changed.", true);
    QJsonArray tracks;
    for (int t = 0; t < model.trackList().size(); ++t)
        tracks.append(trackJson(model, t));
    return jsonResult(QJsonObject{{"tracks", tracks},
                                  {"audio_track", audioTrack}});
}

} // namespace

QList<McpTool> McpTools::audioTools()
{
    return {
        {"clip_set_volume",
         "Set a timeline clip's volume in dB (gain_db -70 to 24) as ONE undo step (\"Set clip "
         "volume to <n> dB\"): changes the clip's first enabled gain filter (filter_list_available "
         "id gain; fade and mute filters are never touched) or adds one; gain_db 0 removes it "
         "(\"Reset clip volume to 0 dB\"). A call that changes nothing (same value, or 0 with no "
         "gain filter) adds no undo step and returns changed false. Refused: a locked track, a "
         "blank or transition, a clip without audio, a gain filter with keyframes. Returns "
         "{track, gain_db, changed, index?} (index: the gain filter's filter_list_applied index "
         "when one remains).",
         schema(R"({"type":"object","properties":{
             "track":{"type":"integer","minimum":0},
             "clip":{"type":"integer","minimum":0},
             "gain_db":{"type":"number","minimum":-70,"maximum":24}
           },"required":["track","clip","gain_db"],"additionalProperties":false})"),
         clipSetVolumeTool},
        {"clip_normalize",
         "Measure the loudness of a timeline clip's source range (the audio_levels measurement, "
         "EBU R128 integrated loudness of the clip's own audio, before any filter) and set the "
         "clip's volume so it reaches target_lufs (-30 to -6, default -16): gain = target - "
         "measured, clamped to -70..24 dB, applied like clip_set_volume as ONE undo step "
         "\"Normalize to <t> LUFS\". If the measurement finishes within wait_ms (0 to 20000, "
         "default 0; a cached measurement counts) the call returns {measured_lufs, "
         "applied_gain_db, target_lufs, changed, clip, track, index?}; otherwise it returns "
         "{job_id, state, measure_job_id} at once and the gain is applied AUTOMATICALLY when the "
         "measurement ends (as soon as no other tool call is running): get_job(job_id) then "
         "shows that result, and the change is an ordinary undo step made at that moment (it "
         "discards any redo history). The clip is found again by identity (source, trim, and "
         "start or Shotcut's clip id when it was moved), so edits elsewhere do not matter; the "
         "result's clip and track give where it was applied. "
         "Nothing is edited (the job fails with \"The clip changed while measuring\") when the "
         "clip was trimmed, removed or replaced, or no single clip matches, or (\"The project "
         "was closed or replaced while measuring\") the timeline was closed or another project "
         "opened. A gain of exactly 0 dB removes an existing gain filter. Refused: a locked "
         "track, a blank or transition, a clip without audio, a speed-changed clip, and a "
         "silent source (integrated loudness at or below -70 LUFS). Fades, track gain and other "
         "filters are not taken into account.",
         schema(R"({"type":"object","properties":{
             "track":{"type":"integer","minimum":0},
             "clip":{"type":"integer","minimum":0},
             "target_lufs":{"type":"number","minimum":-30,"maximum":-6},
             "wait_ms":{"type":"integer","minimum":0,"maximum":20000}
           },"required":["track","clip"],"additionalProperties":false})"),
         clipNormalizeTool},
        {"track_set_ducking",
         "Make a track (usually music) dip automatically while the tracks above it are louder "
         "than threshold_db, with Shotcut's track audio ducking (its audio mix transition): "
         "threshold_db (-70 to 0, default -12), reduction_db (maximum reduction, -70 to 0, "
         "default -12), fade_in_ms and fade_out_ms (0 to 5000, defaults 1500 and 250; as "
         "Shotcut's Properties panel names them), enabled (default true; false keeps the "
         "settings but turns ducking off). ONE undo step. Refused: track 0 (the top track), a "
         "locked track, a track without an audio mix transition, and an MLT older than 7.41 "
         "(which has no ducking). The player is switched to the timeline if it shows a source "
         "clip. Undo of an enabled:false call leaves ducking off (Shotcut's ducking undo command "
         "reads the enabled flag after its own redo changed it; Shotcut's checkbox has the same "
         "bug); undo of an enabling call works. Returns {track, ducking:{enabled, threshold_db, "
         "reduction_db, fade_in_ms, fade_out_ms, duck_threshold}} (duck_threshold: the value "
         "MLT uses, 0 when disabled).",
         schema(R"({"type":"object","properties":{
             "track":{"type":"integer","minimum":1},
             "threshold_db":{"type":"number","minimum":-70,"maximum":0},
             "reduction_db":{"type":"number","minimum":-70,"maximum":0},
             "fade_in_ms":{"type":"number","minimum":0,"maximum":5000},
             "fade_out_ms":{"type":"number","minimum":0,"maximum":5000},
             "enabled":{"type":"boolean"}
           },"required":["track"],"additionalProperties":false})"),
         trackSetDuckingTool},
        {"clip_detach_audio",
         "Split a video clip's sound onto an audio track, as Shotcut's Detach Audio does, as ONE "
         "undo step: the video clip stays with its audio switched off (its audio filters are "
         "removed) and an audio-only copy of the same range goes to the first audio track that "
         "is empty over that range, or to a new audio track added at the bottom. Track indexes "
         "may change when a track is added: re-read the returned tracks. Refused: an audio "
         "track, a locked track (or a locked target audio track), a blank or transition, a clip "
         "without audio. Shotcut selects the video clip afterwards. Returns {tracks (every "
         "track, as get_timeline), audio_track (the index of the track that got the audio)}.",
         schema(R"({"type":"object","properties":{
             "track":{"type":"integer","minimum":0},
             "clip":{"type":"integer","minimum":0}
           },"required":["track","clip"],"additionalProperties":false})"),
         clipDetachAudioTool},
    };
}
