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

#include "mcpfiltercommands.h"
#include "mcpfilters.h"
#include "mcptools.h"
#include "mcptoolsutil.h"

#include "commands/timelinecommands.h"
#include "docks/timelinedock.h"
#include "mainwindow.h"
#include "mltcontroller.h"
#include "models/multitrackmodel.h"
#include "shotcut_mlt_properties.h"
#include "util.h"

#include <MltChain.h>

#include <QRectF>
#include <QRegularExpression>
#include <QStringList>
#include <QUndoStack>

#include <cmath>
#include <memory>

using namespace McpTools;
using namespace McpFilters;

namespace {

// ---- Picture recipes

QJsonObject clipSetColorTool(const QJsonObject &arguments)
{
    int track = 0, clip = 0;
    QString error;
    if (!checkKeys(arguments,
                   {"track", "clip", "brightness", "contrast", "saturation", "temperature_k"},
                   error)
        || !readTarget(arguments, track, clip, error))
        return textResult(error, true);
    double brightness = 0, contrast = 0, saturation = 1, temperature = 6500;
    const FrameArg b = readNumberArg(arguments, "brightness", -1, 1, brightness, error);
    const FrameArg c = readNumberArg(arguments, "contrast", -1, 1, contrast, error);
    const FrameArg s = readNumberArg(arguments, "saturation", 0, 3, saturation, error);
    const FrameArg t = readNumberArg(arguments, "temperature_k", 2000, 15000, temperature, error);
    if (b == FrameArg::Invalid || c == FrameArg::Invalid || s == FrameArg::Invalid
        || t == FrameArg::Invalid)
        return textResult(error, true);
    QList<RecipeStep> steps;
    QStringList summary;
    const auto step = [&](const char *id, const QJsonObject &params, const QString &named) {
        RecipeStep r;
        r.spec = recipeSpec(id, error);
        r.params = params;
        steps.append(r);
        summary << named;
        return r.spec != nullptr;
    };
    if (b == FrameArg::Ok
        && !step("brightness", {{"level", 1 + brightness}}, "brightness=" + num(brightness)))
        return textResult(error, true);
    if (c == FrameArg::Ok) {
        // The slider level of Shotcut's Contrast UI: 0 -> 0.5 (unchanged).
        const double level = (contrast + 1) / 2;
        const double gain = level * 2, gamma = (1 - level) * 2;
        const QJsonObject params{{"gamma_r", gamma},
                                 {"gamma_g", gamma},
                                 {"gamma_b", gamma},
                                 {"gain_r", gain},
                                 {"gain_g", gain},
                                 {"gain_b", gain}};
        if (!step("contrast", params, "contrast=" + num(contrast)))
            return textResult(error, true);
    }
    if (s == FrameArg::Ok
        && !step("saturation", {{"saturation", saturation}}, "saturation=" + num(saturation)))
        return textResult(error, true);
    if (t == FrameArg::Ok
        && !step("white_balance",
                 {{"temperature_k", temperature}},
                 "temperature_k=" + num(temperature)))
        return textResult(error, true);
    if (steps.isEmpty())
        return textResult("Give at least one of brightness, contrast, saturation, temperature_k.",
                          true);
    MultitrackModel &model = *MAIN.timelineDock()->model();
    if (!checkClip(model, track, clip, *steps.first().spec, error))
        return textResult(error, true);
    return runRecipe(track, clip, steps, QObject::tr("Set clip color: %1").arg(summary.join(", ")));
}

QJsonObject clipSetTransformTool(const QJsonObject &arguments)
{
    int track = 0, clip = 0;
    QString error;
    if (!checkKeys(arguments, {"track", "clip", "scale", "x", "y", "rotation", "crop"}, error)
        || !readTarget(arguments, track, clip, error))
        return textResult(error, true);
    double scale = 1, x = 0, y = 0, rotation = 0;
    const FrameArg sc = readNumberArg(arguments, "scale", 0.05, 8, scale, error);
    const FrameArg xa = readNumberArg(arguments, "x", -kMaxRectValue, kMaxRectValue, x, error);
    const FrameArg ya = readNumberArg(arguments, "y", -kMaxRectValue, kMaxRectValue, y, error);
    const FrameArg ro = readNumberArg(arguments, "rotation", -360, 360, rotation, error);
    if (sc == FrameArg::Invalid || xa == FrameArg::Invalid || ya == FrameArg::Invalid
        || ro == FrameArg::Invalid)
        return textResult(error, true);
    const QJsonValue cropArg = arguments.value("crop");
    if (!cropArg.isUndefined() && (!cropArg.isObject() || cropArg.toObject().isEmpty()))
        return textResult("crop must be an object with at least one of left, right, top, bottom "
                          "(integer pixels of the video mode).",
                          true);
    const bool geometry = sc == FrameArg::Ok || xa == FrameArg::Ok || ya == FrameArg::Ok;
    if (!geometry && ro != FrameArg::Ok && cropArg.isUndefined())
        return textResult("Give at least one of scale, x, y, rotation, crop.", true);

    QList<RecipeStep> steps;
    QStringList summary;
    if (cropArg.isObject()) {
        RecipeStep r;
        if (!(r.spec = recipeSpec("crop", error)))
            return textResult(error, true);
        r.params = cropArg.toObject();
        steps.append(r);
    }
    RecipeStep size;
    if (geometry || ro == FrameArg::Ok) {
        if (!(size.spec = recipeSpec("size_position", error)))
            return textResult(error, true);
    }
    MultitrackModel &model = *MAIN.timelineDock()->model();
    std::unique_ptr<Mlt::ClipInfo> info
        = checkClip(model, track, clip, size.spec ? *size.spec : *steps.first().spec, error);
    if (!info)
        return textResult(error, true);
    bool hasSize = false;
    for (int i : McpFilter::userFilterIndices(*info->producer)) {
        std::unique_ptr<Mlt::Filter> filter(info->producer->filter(i));
        hasSize = hasSize || (size.spec && filter && McpFilters::specOf(*filter) == size.spec);
    }
    // Shotcut's Fit mode (the picture keeps its shape inside the rect) for a new filter or a new
    // rect; rotation alone leaves an existing filter's mode alone.
    if (size.spec && (geometry || !hasSize)) {
        size.extra = [](Mlt::Properties &p) {
            p.set("transition.fill", 0);
            p.set("transition.distort", 0);
        };
    }
    if (sc == FrameArg::Ok)
        summary << "scale=" + num(scale);
    if (xa == FrameArg::Ok)
        summary << "x=" + num(x);
    if (ya == FrameArg::Ok)
        summary << "y=" + num(y);
    if (ro == FrameArg::Ok)
        summary << "rotation=" + num(rotation);
    if (geometry) {
        // The fit rect scaled about its centre, then moved by (x, y).
        const QRectF fit = fitRect(info->producer, FilterParam::AffineFit);
        const double w = fit.width() * scale, h = fit.height() * scale;
        const double left = fit.x() + (fit.width() - w) / 2 + x;
        const double top = fit.y() + (fit.height() - h) / 2 + y;
        if (std::abs(left) > kMaxRectValue || std::abs(top) > kMaxRectValue)
            return textResult(QStringLiteral("x and y move the picture too far: its top-left "
                                             "corner would be at (%1, %2) pixels; it must stay "
                                             "within %3 pixels of the frame's.")
                                  .arg(num(left), num(top))
                                  .arg(kMaxRectValue),
                              true);
        size.params["rect"] = QJsonObject{{"x", left}, {"y", top}, {"w", w}, {"h", h}};
    }
    if (ro == FrameArg::Ok)
        size.params["rotation"] = rotation;
    info.reset();
    if (size.spec)
        steps.append(size);
    if (cropArg.isObject()) {
        QStringList sides;
        const QJsonObject crop = cropArg.toObject();
        for (const char *side : {"left", "right", "top", "bottom"}) {
            if (crop.value(side).isDouble())
                sides << QStringLiteral("%1=%2").arg(side, num(crop.value(side).toDouble()));
        }
        summary << "crop " + sides.join(' ');
    }
    return runRecipe(track,
                     clip,
                     steps,
                     QObject::tr("Set clip transform: %1").arg(summary.join(", ")));
}

QJsonObject clipSetBlurTool(const QJsonObject &arguments)
{
    int track = 0, clip = 0;
    QString error;
    if (!checkKeys(arguments, {"track", "clip", "mode", "radius"}, error)
        || !readTarget(arguments, track, clip, error))
        return textResult(error, true);
    const QString mode = arguments.value("mode").toString();
    if (mode != "box" && mode != "pad")
        return textResult("mode must be \"box\" or \"pad\".", true);
    double radius = 5;
    const FrameArg ra = readNumberArg(arguments, "radius", 0, 100, radius, error);
    if (ra == FrameArg::Invalid || radius != std::floor(radius))
        return textResult("radius must be an integer from 0 to 100.", true);
    if (mode == "pad" && ra == FrameArg::Ok)
        return textResult("radius is for mode \"box\" only; the pad blur uses Shotcut's default.",
                          true);
    RecipeStep r;
    if (!(r.spec = recipeSpec(mode == "box" ? "box_blur" : "blur_pad", error)))
        return textResult(error, true);
    MultitrackModel &model = *MAIN.timelineDock()->model();
    std::unique_ptr<Mlt::ClipInfo> info = checkClip(model, track, clip, *r.spec, error);
    if (!info)
        return textResult(error, true);
    QString text;
    if (mode == "box") {
        r.params = QJsonObject{{"hradius", radius}, {"vradius", radius}};
        text = QObject::tr("Set clip blur: box radius=%1").arg(num(radius));
    } else {
        const QRectF fit = fitRect(info->producer, FilterParam::PillarEchoFit);
        r.params = QJsonObject{{"blur", 4},
                               {"rect",
                                QJsonObject{{"x", fit.x()},
                                            {"y", fit.y()},
                                            {"w", fit.width()},
                                            {"h", fit.height()}}}};
        text = QObject::tr("Set clip blur: pad");
    }
    info.reset();
    return runRecipe(track, clip, {r}, text);
}

// ---- Speed

// True when a user filter of the clip has a fade (shotcut:animIn/animOut) or a value in MLT's
// animation syntax ("0=1;24=1.5", "00:00:01.000~=2": a time, maybe a keyframe type, '=').
// Shotcut does not move keyframes when the speed changes.
bool hasFadesOrKeyframes(Mlt::Producer &producer)
{
    static const QRegularExpression re(QStringLiteral("^\\s*-?[0-9][0-9:.]*\\s*[^\\s0-9:.;=]?="));
    for (int i : McpFilter::userFilterIndices(producer)) {
        std::unique_ptr<Mlt::Filter> filter(producer.filter(i));
        if (!filter || !filter->is_valid())
            continue;
        if (filter->get_int(kShotcutAnimInProperty) > 0
            || filter->get_int(kShotcutAnimOutProperty) > 0)
            return true;
        for (int p = 0; p < filter->count(); ++p) {
            const char *value = filter->get(p);
            if (value && re.match(QString::fromUtf8(value)).hasMatch())
                return true;
        }
    }
    return false;
}

// Shotcut's speed change for a timeline clip (AvformatProducerWidget::recreateProducer and
// TimelineDock::onProducerChanged) without the Properties panel or the selection.
QJsonObject clipSetSpeedTool(const QJsonObject &arguments)
{
    int track = 0, clip = 0;
    QString error;
    if (!checkKeys(arguments, {"track", "clip", "speed", "keep_pitch"}, error)
        || !readTarget(arguments, track, clip, error))
        return textResult(error, true);
    double speed = 1;
    const FrameArg sp = readNumberArg(arguments, "speed", 0.1, 10, speed, error);
    if (sp != FrameArg::Ok)
        return textResult(sp == FrameArg::Missing
                              ? QStringLiteral("speed is required (0.1 to 10, 1 = normal).")
                              : error,
                          true);
    const QJsonValue pitchArg = arguments.value("keep_pitch");
    if (!pitchArg.isUndefined() && !pitchArg.isBool())
        return textResult("keep_pitch must be true or false.", true);
    const bool keepPitch = pitchArg.toBool(true);

    TimelineDock *dock = MAIN.timelineDock();
    MultitrackModel &model = *dock->model();
    std::unique_ptr<Mlt::ClipInfo> info = plainClip(model, track, clip, error);
    if (!info)
        return textResult(error, true);
    Mlt::Producer &producer = *info->producer;
    const QString service = QString::fromUtf8(producer.get("mlt_service"));
    if (producer.get_int(kIsProxyProperty))
        return textResult(QStringLiteral("Clip %1 on track %2 plays a proxy; turn proxy editing "
                                         "off (Settings > Proxy > Use Proxy) and try again.")
                              .arg(clip)
                              .arg(track),
                          true);
    if (!(service.startsWith("avformat") || service == "timewarp") || MLT.isImageProducer(&producer)
        || MLT.isLiveProducer(&producer))
        return textResult(QStringLiteral("Clip %1 on track %2 is not a video or audio file "
                                         "(a still image, generator, project or other "
                                         "producer); clip_set_speed changes media file clips "
                                         "only.")
                              .arg(clip)
                              .arg(track),
                          true);
    if (service == "timewarp" && producer.get_double("warp_speed") < 0)
        return textResult(QStringLiteral("Clip %1 on track %2 plays in reverse; clip_set_speed "
                                         "sets forward speeds only.")
                              .arg(clip)
                              .arg(track),
                          true);
    if (hasFadesOrKeyframes(producer))
        return textResult(QStringLiteral("Clip %1 on track %2 has fades or keyframed filters, "
                                         "which a speed change would not move. Remove fades and "
                                         "keyframes first.")
                              .arg(clip)
                              .arg(track),
                          true);
    const int count = model.rowCount(model.index(track));
    if ((clip > 0 && kindOf(model, track, clip - 1) == "transition")
        || (clip + 1 < count && kindOf(model, track, clip + 1) == "transition"))
        return textResult(QStringLiteral("Clip %1 on track %2 is part of a transition; remove the "
                                         "transition first.")
                              .arg(clip)
                              .arg(track),
                          true);

    const double oldSpeed = Util::GetSpeedFromProducer(&producer);
    if (std::abs(speed - oldSpeed) < 1e-6) // already at that speed: no undo step
        return jsonResult(QJsonObject{{"track", trackJson(model, track)},
                                      {"clip", clip},
                                      {"speed", speed},
                                      {"duration", info->frame_count},
                                      {"changed", false}});
    const QString filename = Util::GetFilenameFromProducer(&producer, false);
    std::unique_ptr<Mlt::Producer> np;
    if (speed == 1.0) {
        np.reset(new Mlt::Chain(MLT.profile(), filename.toUtf8().constData()));
    } else {
        // MLT formats the number (the locale of MLT and Qt can differ), as Shotcut does.
        Mlt::Properties temp;
        temp.set("speed", speed);
        const QString id = QStringLiteral("timewarp:%1:%2")
                               .arg(QString::fromLatin1(temp.get("speed")), filename);
        np.reset(new Mlt::Producer(MLT.profile(), id.toUtf8().constData()));
        np->set(kShotcutProducerProperty, "avformat");
    }
    if (!np->is_valid())
        return textResult("Shotcut cannot open " + filename + " to change its speed.", true);
    Util::passProducerProperties(&producer, np.get());
    // After passProducerProperties, which copies the old clip's warp_pitch (kept at speed 1,
    // where the plain chain does not use it, as in Shotcut).
    if (speed != 1.0)
        np->set("warp_pitch", keepPitch ? 1 : 0);
    Util::updateCaption(np.get());
    Mlt::Controller::copyFilters(producer, *np, false, Mlt::Controller::FILTER_INDEX_ALL);
    const double ratio = oldSpeed / speed;
    const int length = qRound(info->length * ratio);
    const int in = qMin(qRound(info->frame_in * ratio), length - 1);
    const int out = qMin(qRound(info->frame_out * ratio), length - 1);
    np->set("length", np->frames_to_time(length, mlt_time_clock));
    np->set_in_and_out(in, out);
    // As AvformatProducerWidget::recreateProducer: a frame fills in the media properties
    // (meta.media.*, audio_index), so the XML the command keeps does not depend on when the
    // preview first renders the clip.
    delete np->get_frame();
    Mlt::Controller::adjustClipFilters(*np,
                                       info->frame_in,
                                       info->frame_out,
                                       in - info->frame_in,
                                       info->frame_out - out,
                                       0);

    // The clip keeps its start and nothing else moves: a longer clip must fit the gap after it.
    const int frames = out - in + 1;
    if (frames > info->frame_count && clip + 1 < count) {
        const bool lastBlank = clip + 2 >= count && kindOf(model, track, clip + 1) == "blank";
        const int gap = kindOf(model, track, clip + 1) == "blank"
                            ? model.getClipInfo(track, clip + 1)->frame_count
                            : 0;
        if (!lastBlank && frames - info->frame_count > gap)
            return textResult(QStringLiteral("At speed %1 clip %2 on track %3 would last %4 frames "
                                             "(now %5), but only %6 frames are free after it "
                                             "before the next clip. Make room first (move or "
                                             "trim the next clip); nothing was changed.")
                                  .arg(num(speed))
                                  .arg(clip)
                                  .arg(track)
                                  .arg(frames)
                                  .arg(info->frame_count)
                                  .arg(gap),
                              true);
    }

    const int start = info->start;
    const QString text = QObject::tr("Set clip speed: %1x").arg(num(speed));
    EditPreviewGuard previewGuard(false);
    // As TimelineDock::onProducerChanged: Shotcut clears the selection before this command.
    if (!dock->selection().isEmpty())
        dock->setSelection();
    // UpdateCommand records the track before the change; never ripple, so only this clip and the
    // blank after it change.
    auto *command = new Timeline::UpdateCommand(*dock, track, clip, start);
    command->setXmlAfter(MLT.XML(np.get()));
    command->setRippleAllTracks(false);
    command->setRipple(false);
    info.reset();
    np.reset();
    QUndoStack *stack = MAIN.undoStack();
    stack->beginMacro(text);
    stack->push(command);
    std::unique_ptr<Mlt::ClipInfo> after = model.getClipInfo(track, clip);
    const bool ok = after && after->start == start && after->frame_count == frames
                    && after->frame_in == in
                    && std::abs(Util::GetSpeedFromProducer(after->producer) - speed) < 1e-6;
    after.reset();
    if (!finishMacro(stack, ok))
        return textResult("Shotcut did not change the clip's speed; nothing was changed.", true);
    return jsonResult(QJsonObject{{"track", trackJson(model, track)},
                                  {"clip", clip},
                                  {"speed", speed},
                                  {"duration", frames},
                                  {"changed", true}});
}

} // namespace

QList<McpTool> McpTools::pictureTools()
{
    return {
        {"clip_set_color",
         "Adjust a timeline clip's picture as ONE undo step (\"Set clip color: name=value, ...\") "
         "with Shotcut's CPU filters, one per value: brightness (-1 to 1, 0 unchanged; Brightness "
         "level = 1 + brightness), contrast (-1 to 1, 0 unchanged; Shotcut's Contrast filter, "
         "slider level (contrast + 1) / 2, so gain = level * 2 and gamma = (1 - level) * 2), "
         "saturation (0 to 3 multiplier, 1 unchanged, 0 grey; Saturation), temperature_k (2000 "
         "to 15000 kelvin, 6500 neutral; White Balance). Give at least one. Each value changes "
         "the clip's first filter of that kind or adds one, so repeated calls update rather than "
         "stack. Refused (nothing changed): a filter this Shotcut cannot use (saturation and "
         "temperature_k need MLT's frei0r plugins; the error says why), unknown arguments, a "
         "locked track, a blank or transition, an audio track or audio-only clip, a value with "
         "keyframes. Returns {filters:[{id, index}], track} (ids and indexes as "
         "filter_list_applied reports them).",
         schema(R"({"type":"object","properties":{
             "track":{"type":"integer","minimum":0},
             "clip":{"type":"integer","minimum":0},
             "brightness":{"type":"number","minimum":-1,"maximum":1},
             "contrast":{"type":"number","minimum":-1,"maximum":1},
             "saturation":{"type":"number","minimum":0,"maximum":3},
             "temperature_k":{"type":"number","minimum":2000,"maximum":15000}
           },"required":["track","clip"],"additionalProperties":false})"),
         clipSetColorTool},
        {"clip_set_transform",
         "Size, move, rotate and crop a timeline clip's picture as ONE undo step (\"Set clip "
         "transform: ...\"). scale (0.05 to 8; 1 = the picture fitted into the frame), x and y "
         "(pixels of the video mode: the picture's centre offset from the frame centre, default "
         "0) and rotation (-360 to 360 degrees) set Shotcut's Size, Position & Rotate filter: "
         "the rect is the fit rect scaled about its centre and moved by (x, y); when any of "
         "scale, x, y is given the others take their defaults and the filter is put in Fit mode "
         "(the picture keeps its shape; transparent around it, so lower tracks or black show), "
         "as is a new filter; rotation alone keeps an existing filter's rect and mode. crop "
         "{left, right, top, bottom} (integer pixels of the video mode, given sides only) sets "
         "the Crop: Source filter. Give at least one. Each filter is changed when the clip has "
         "one, else added (a new crop before a new size filter; filters are appended after the "
         "clip's existing ones, as Shotcut does, so a crop added after an existing Size/Position "
         "filter crops the composited frame). "
         "Picture-in-picture on a 1920x1080 project: {scale: 0.3, x: 560, y: -300} on the upper "
         "track's clip. Refused (nothing changed): unknown arguments, a crop that removes the "
         "whole picture, a locked track, a blank or transition, an audio track or audio-only "
         "clip, values with keyframes. Returns {filters:[{id, index}], track}.",
         schema(R"({"type":"object","properties":{
             "track":{"type":"integer","minimum":0},
             "clip":{"type":"integer","minimum":0},
             "scale":{"type":"number","minimum":0.05,"maximum":8},
             "x":{"type":"number"},
             "y":{"type":"number"},
             "rotation":{"type":"number","minimum":-360,"maximum":360},
             "crop":{"type":"object","minProperties":1,"properties":{
               "left":{"type":"integer","minimum":0},
               "right":{"type":"integer","minimum":0},
               "top":{"type":"integer","minimum":0},
               "bottom":{"type":"integer","minimum":0}},"additionalProperties":false}
           },"required":["track","clip"],"additionalProperties":false})"),
         clipSetTransformTool},
        {"clip_set_blur",
         "Blur a timeline clip's picture as ONE undo step: mode \"box\" sets Shotcut's Blur: Box "
         "(radius 0 to 100 pixels both ways, default 5); mode \"pad\" sets Blur: Pad (the picture "
         "fitted into the frame over a blurred, enlarged copy of itself that fills the bars; "
         "blur 4, no radius). A clip that already has that blur gets it replaced with these "
         "values (no second filter). Refused (nothing changed): unknown arguments, a locked "
         "track, a blank or transition, an audio track or audio-only clip, values with "
         "keyframes. Returns {filters:[{id, index}], track}.",
         schema(R"({"type":"object","properties":{
             "track":{"type":"integer","minimum":0},
             "clip":{"type":"integer","minimum":0},
             "mode":{"type":"string","enum":["box","pad"]},
             "radius":{"type":"integer","minimum":0,"maximum":100}
           },"required":["track","clip","mode"],"additionalProperties":false})"),
         clipSetBlurTool},
        {"clip_set_speed",
         "Change a timeline clip's playback speed as ONE undo step (\"Set clip speed: <speed>x\"), "
         "as Shotcut's Properties panel does (MLT's timewarp producer): speed 0.1 to 10 (1 = "
         "normal; no reverse), keep_pitch (default true: the sound keeps its pitch; ignored at "
         "speed 1). The clip keeps its start and its filters; its length, in and out scale by "
         "old speed / new speed (2 halves a clip, 0.5 doubles it). Nothing else moves, whatever "
         "Shotcut's ripple settings: a shorter clip leaves a gap after it, and a longer clip must "
         "fit the gap before the next clip on its track (the error names the free frames; make "
         "room first). Refused (nothing changed): unknown arguments, a locked track, a blank or "
         "transition, a clip next to a transition, a still image, generator, project or other "
         "non-file clip, a proxy clip (turn proxy editing off first), a clip playing in reverse, "
         "a clip with fades or keyframed filters (\"Remove fades and keyframes first\": Shotcut "
         "does not move keyframes either). Returns {track (as get_timeline), clip, speed, "
         "duration (the clip's new frames), changed (false, with no undo step, when the clip "
         "already plays at that speed)}.",
         schema(R"({"type":"object","properties":{
             "track":{"type":"integer","minimum":0},
             "clip":{"type":"integer","minimum":0},
             "speed":{"type":"number","minimum":0.1,"maximum":10},
             "keep_pitch":{"type":"boolean"}
           },"required":["track","clip","speed"],"additionalProperties":false})"),
         clipSetSpeedTool},
    };
}
