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

#include "docks/timelinedock.h"
#include "mainwindow.h"
#include "mltcontroller.h"
#include "models/multitrackmodel.h"
#include "shotcut_mlt_properties.h"

#include <QGuiApplication>
#include <QJsonArray>
#include <QRectF>
#include <QRegularExpression>
#include <QScreen>
#include <QUndoStack>

#include <algorithm>
#include <cmath>
#include <memory>

using namespace McpTools;
using namespace McpFilters;

namespace {

QJsonObject projectSetFormatTool(const QJsonObject &arguments)
{
    QString error;
    if (!McpFilters::checkKeys(arguments, {"preset", "width", "height", "fps"}, error))
        return textResult(error, true);

    const QJsonValue preset = arguments.value("preset");
    const bool hasSize = arguments.contains("width") || arguments.contains("height");
    if (preset.isUndefined() == !hasSize)
        return textResult("Pass either preset or width and height.", true);
    int width = 0, height = 0;
    if (!preset.isUndefined()) {
        const QString name = preset.toString();
        if (name == "16:9")
            width = 1920, height = 1080;
        else if (name == "9:16")
            width = 1080, height = 1920;
        else if (name == "1:1")
            width = 1080, height = 1080;
        else if (name == "4:5")
            width = 1080, height = 1350;
        else
            return textResult("preset must be one of 16:9, 9:16, 1:1, 4:5.", true);
    } else if (readFrame(arguments, "width", width) != FrameArg::Ok
               || readFrame(arguments, "height", height) != FrameArg::Ok || width < 64
               || height < 64 || width > 7680 || height > 7680 || width % 2 || height % 2) {
        return textResult("width and height must both be even integers from 64 to 7680.", true);
    }

    int fpsNum = 30, fpsDen = 1;
    const QJsonValue fps = arguments.value("fps");
    if (!fps.isUndefined()) {
        // The NTSC rates are exact fractions; the others whole numbers.
        static const struct
        {
            double fps;
            int num, den;
        } rates[] = {{23.976, 24000, 1001},
                     {24, 24, 1},
                     {25, 25, 1},
                     {29.97, 30000, 1001},
                     {30, 30, 1},
                     {50, 50, 1},
                     {59.94, 60000, 1001},
                     {60, 60, 1}};
        bool found = false;
        for (const auto &rate : rates) {
            if (fps.isDouble() && fps.toDouble() == rate.fps) {
                fpsNum = rate.num;
                fpsDen = rate.den;
                found = true;
            }
        }
        if (!found)
            return textResult("fps must be one of 23.976, 24, 25, 29.97, 30, 50, 59.94, 60.", true);
    }

    if (!MAIN.setVideoMode(width, height, fpsNum, fpsDen, error))
        return textResult(error, true);
    Mlt::Profile &profile = MLT.profile();
    const auto ratio = [](int num, int den) { return QStringLiteral("%1:%2").arg(num).arg(den); };
    return jsonResult({{"width", profile.width()},
                       {"height", profile.height()},
                       {"fps_num", profile.frame_rate_num()},
                       {"fps_den", profile.frame_rate_den()},
                       {"sar", ratio(profile.sample_aspect_num(), profile.sample_aspect_den())},
                       {"dar", ratio(profile.display_aspect_num(), profile.display_aspect_den())}});
}

// Marks the filters clip_reframe added (filter_set keeps it): a later call replaces only these.
const char *kReframeProperty = "shotcut:reframe";

QString rectValue(double x, double y, double w, double h)
{
    return QStringLiteral("%1 %2 %3 %4 1").arg(num(x), num(y), num(w), num(h));
}

QJsonObject clipReframeTool(const QJsonObject &arguments)
{
    int track = 0, clip = 0;
    QString error;
    if (!checkKeys(arguments,
                   {"track", "clip", "mode", "focus_x", "keyframes", "interpolation"},
                   error)
        || !readTarget(arguments, track, clip, error))
        return textResult(error, true);
    const QString mode = arguments.value("mode").toString();
    if (mode != "fill" && mode != "pad")
        return textResult("mode must be \"fill\" or \"pad\".", true);
    const bool fill = mode == "fill";
    double focus = 0.5;
    const FrameArg fo = readNumberArg(arguments, "focus_x", 0, 1, focus, error);
    if (fo == FrameArg::Invalid)
        return textResult(error, true);
    const QJsonValue keyArg = arguments.value("keyframes");
    const QJsonValue interpolation = arguments.value("interpolation");
    if (!fill && (fo == FrameArg::Ok || !keyArg.isUndefined() || !interpolation.isUndefined()))
        return textResult("focus_x, keyframes and interpolation are for mode \"fill\" only.",
                          true);
    if (fo == FrameArg::Ok && !keyArg.isUndefined())
        return textResult("Give focus_x or keyframes, not both.", true);
    if (!interpolation.isUndefined()
        && (keyArg.isUndefined()
            || (interpolation.toString() != "linear" && interpolation.toString() != "smooth")))
        return textResult("interpolation (\"linear\" or \"smooth\") goes with keyframes only.",
                          true);
    const QString keyError = QStringLiteral(
        "keyframes must be an array of 1 to 50 objects {frame, focus_x}: frame an integer of the "
        "clip (0 is its first frame), strictly increasing; focus_x a number from 0 to 1.");
    QList<QPair<int, double>> keys;
    if (!keyArg.isUndefined()) {
        const QJsonArray array = keyArg.toArray();
        if (!keyArg.isArray() || array.isEmpty() || array.size() > 50)
            return textResult(keyError, true);
        for (const QJsonValue &v : array) {
            const QJsonObject key = v.toObject();
            int frame = 0;
            double f = 0;
            QString ignored;
            if (!v.isObject() || key.size() != 2 || readFrame(key, "frame", frame) != FrameArg::Ok
                || readNumberArg(key, "focus_x", 0, 1, f, ignored) != FrameArg::Ok
                || (!keys.isEmpty() && frame <= keys.last().first))
                return textResult(keyError, true);
            keys.append({frame, f});
        }
    }

    const FilterSpec *spec = recipeSpec(fill ? "size_position" : "blur_pad", error);
    if (!spec)
        return textResult(error, true);
    MultitrackModel &model = *MAIN.timelineDock()->model();
    std::unique_ptr<Mlt::ClipInfo> info = checkClip(model, track, clip, *spec, error);
    if (!info)
        return textResult(error, true);
    if (!keys.isEmpty() && keys.last().first >= info->frame_count)
        return textResult(QStringLiteral("Keyframe frame %1 is after the clip's last frame (%2).")
                              .arg(keys.last().first)
                              .arg(info->frame_count - 1),
                          true);
    // The clip's display aspect, from Size/Position's fit rect (w = h / sar * dar).
    Mlt::Profile &profile = MLT.profile();
    const double width = profile.width(), height = profile.height(), sar = profile.sar();
    const QRectF fit = fitRect(info->producer, FilterParam::AffineFit);
    const double srcDar = fit.width() * sar / fit.height();
    if (std::abs(srcDar / profile.dar() - 1) < 0.01)
        return textResult("The clip already matches the project format.", true);

    QString value;
    if (fill) {
        // The fill (cover) rect: the picture scaled to cover the frame, its focus_x point at the
        // frame's centre as far as the picture's edges allow.
        double w, h;
        if (srcDar > profile.dar()) {
            h = height;
            w = h / sar * srcDar;
        } else {
            w = width;
            h = w * sar / srcDar;
        }
        const double y = (height - h) / 2;
        const double minX = std::min(width - w, 0.0);
        const auto xAt = [&](double f) { return std::clamp(width / 2 - w * f, minX, 0.0); };
        if (keys.isEmpty()) {
            value = rectValue(xAt(focus), y, w, h);
        } else {
            // MLT's keyframe type is on the keyframe a segment starts from: "~" on every one.
            const QString type = interpolation.toString() == "smooth" ? "~" : "";
            QStringList parts;
            for (const auto &key : keys)
                parts << QStringLiteral("%1%2=%3")
                             .arg(key.first)
                             .arg(type, rectValue(xAt(key.second), y, w, h));
            value = parts.join(';');
        }
    } else {
        const QRectF pad = fitRect(info->producer, FilterParam::PillarEchoFit);
        value = rectValue(pad.x(), pad.y(), pad.width(), pad.height());
    }
    // The whole reframe on top of the spec's defaults, so a replaced one is reset too.
    const auto extra = [fill, value](Mlt::Properties &p) {
        p.set(kReframeProperty, 1);
        if (fill) {
            p.set("transition.rect", value.toUtf8().constData());
            p.set("transition.fill", 1);
            p.set("transition.distort", 0);
            p.set("transition.fix_rotate_x", 0);
            p.set("transition.valign", "middle");
            p.set("transition.halign", "center");
        } else {
            p.set("rect", value.toUtf8().constData());
            p.set("blur", 4);
        }
    };

    // This tool's own filters: the first of this mode's kind is set, the others removed. A
    // Size/Position or Blur: Pad the tool did not add already renders the frame at the project's
    // shape, so a reframe after it would compute its rect for the wrong picture: refused.
    const FilterSpec *sizeSpec = findSpec("size_position");
    const FilterSpec *padSpec = findSpec("blur_pad");
    int target = -1;
    QList<int> removals;
    const QList<int> indices = McpFilter::userFilterIndices(*info->producer);
    for (int i = 0; i < indices.size(); ++i) {
        std::unique_ptr<Mlt::Filter> filter(info->producer->filter(indices.at(i)));
        if (!filter)
            continue;
        const FilterSpec *kind = specOf(*filter);
        if (!filter->get_int(kReframeProperty)) {
            if (kind && (kind == sizeSpec || kind == padSpec))
                return textResult(QStringLiteral("Clip %1 on track %2 has its own %3 filter "
                                                 "(index %4). Remove or adjust the clip's own "
                                                 "Size, Position & Rotate / Blur: Pad first; "
                                                 "nothing was changed.")
                                      .arg(clip)
                                      .arg(track)
                                      .arg(kind->label)
                                      .arg(i),
                                  true);
            continue;
        }
        if (target < 0 && kind == spec)
            target = i;
        else
            removals.prepend(i);
    }
    info.reset();
    const QString text = QObject::tr("Reframe clip (%1)").arg(mode);
    QUndoStack *stack = MAIN.undoStack();
    stack->beginMacro(text);
    bool ok = true;
    for (int i : removals) {
        auto *command = new McpFilter::RemoveCommand(model,
                                                     McpFilter::Target{track, clip},
                                                     i,
                                                     text);
        ok = pushCommand(command, text, true, [command] { return command->removed(); }, error);
        if (!ok)
            break;
        if (i < target)
            --target;
    }
    int index = target;
    if (ok) {
        std::unique_ptr<Mlt::ClipInfo> now = model.getClipInfo(track, clip);
        if (target >= 0) {
            const QList<int> nowIndices = McpFilter::userFilterIndices(*now->producer);
            std::unique_ptr<Mlt::Filter> filter(now->producer->filter(nowIndices.at(target)));
            ok = setFilter(model,
                           track,
                           clip,
                           target,
                           *filter,
                           *spec,
                           {},
                           text,
                           true,
                           error,
                           extra);
        } else {
            ok = addFilter(model,
                           track,
                           clip,
                           *now,
                           *spec,
                           {},
                           text,
                           true,
                           error,
                           &index,
                           extra);
        }
    }
    if (!finishMacro(stack, ok))
        return textResult(error, true);
    const QJsonObject filter{{"id", spec->id}, {"index", index}};
    return jsonResult(
        QJsonObject{{"filters", QJsonArray{filter}}, {"track", trackJson(model, track)}});
}

// An optional integer from min to max; value keeps its default when missing.
bool readIntArg(
    const QJsonObject &arguments, const char *key, int min, int max, int &value, QString &error)
{
    double d = value;
    if (readNumberArg(arguments, key, min, max, d, error) == FrameArg::Invalid
        || d != std::floor(d)) {
        error = QStringLiteral("%1 must be an integer from %2 to %3.").arg(key).arg(min).arg(max);
        return false;
    }
    value = int(d);
    return true;
}

// An optional "#rrggbb" or "#aarrggbb" as MLT's "#aarrggbb" (lower case, opaque without alpha).
bool readColorArg(const QJsonObject &arguments, const char *key, QString &color, QString &error)
{
    const QJsonValue v = arguments.value(key);
    if (v.isUndefined())
        return true;
    static const QRegularExpression re(QStringLiteral("^#([0-9a-fA-F]{6}|[0-9a-fA-F]{8})$"));
    if (!v.isString() || !re.match(v.toString()).hasMatch()) {
        error = QStringLiteral("%1 must be a color \"#rrggbb\" or \"#aarrggbb\".").arg(key);
        return false;
    }
    color = v.toString().toLower();
    if (color.size() == 7)
        color.insert(1, QStringLiteral("ff"));
    return true;
}

// An optional string that must be one of choices; value keeps its default when missing.
bool readChoiceArg(const QJsonObject &arguments,
                   const char *key,
                   const QStringList &choices,
                   QString &value,
                   QString &error)
{
    const QJsonValue v = arguments.value(key);
    if (v.isUndefined())
        return true;
    if (!choices.contains(v.toString())) {
        error = QStringLiteral("%1 must be one of %2.").arg(key, choices.join(", "));
        return false;
    }
    value = v.toString();
    return true;
}

// A transparent clip carrying Shotcut's Text: Simple filter (dynamictext), as Open Other > Text
// makes it.
struct TextClip
{
    QString text; // as shown; may hold newlines
    QString family;
    int size = 0;
    QString fgcolour = QStringLiteral("#ffffffff");
    QString bgcolour = QStringLiteral("#00000000");
    int outline = 3;
    QString geometry = QStringLiteral("0%/0%:100%x100%");
    QString halign = QStringLiteral("center");
    QString valign = QStringLiteral("middle");
};

// Reads track, position, duration, mode and font, shared by the text tools.
bool readTextPlacement(const QJsonObject &arguments,
                       int &track,
                       int &position,
                       int &duration,
                       QString &mode,
                       TextClip &clip,
                       QString &error)
{
    if (readFrame(arguments, "track", track) != FrameArg::Ok) {
        error = QStringLiteral("track must be a non-negative integer.");
        return false;
    }
    if (readFrame(arguments, "position", position) != FrameArg::Ok || position > 1000000000) {
        error = QStringLiteral("position must be a non-negative integer (at most 1e9).");
        return false;
    }
    if (readFrame(arguments, "duration", duration) != FrameArg::Ok || duration < 1
        || duration > 100000) {
        error = QStringLiteral("duration must be an integer from 1 to 100000 frames.");
        return false;
    }
    if (!readChoiceArg(arguments, "mode", {"insert", "overwrite"}, mode, error))
        return false;
    const QJsonValue font = arguments.value("font");
    return font.isUndefined() || readText(font, "font", 100, clip.family, error);
}

QJsonObject addTextClip(const TextClip &clip,
                        int track,
                        int position,
                        int duration,
                        const QString &mode,
                        const QString &undoText,
                        const QString &what)
{
    // "#" starts a keyword of the filter (#timecode# and so on); "\#" shows it as text.
    QString argument = clip.text;
    argument.replace('#', QStringLiteral("\\#"));
    // Shotcut's Filters panel cuts the text to 256 (UTF-16) characters when it shows the filter,
    // and MLT's filter renders at most 511 bytes of it.
    if (argument.size() > 256 || argument.toUtf8().size() > 511)
        return textResult(QStringLiteral("Too long for Shotcut's text filter: %1 may have at most "
                                         "256 characters (each # and emoji counts twice) and 511 "
                                         "bytes in UTF-8.")
                              .arg(what),
                          true);
    QString error;
    // A video track, or track 0 when there is no timeline yet (pushClipXml creates it).
    if (!validTargetTrack(track, error)
        || (MAIN.isMultitrackValid()
            && !validTrack(*MAIN.timelineDock()->model(), track, error, true)))
        return textResult(error, true);

    // The first line that is not blank names the clip.
    QString caption;
    for (const QString &line : clip.text.split('\n')) {
        if (!line.trimmed().isEmpty()) {
            caption = line;
            break;
        }
    }
    Mlt::Producer producer(MLT.profile(), "color:");
    Mlt::Filter filter(MLT.profile(), "dynamictext");
    if (!producer.is_valid() || !filter.is_valid())
        return textResult("Shotcut's text filter (dynamictext) is not available.", true);
    producer.set("resource", "#00000000");
    producer.set("mlt_image_format", "rgba");
    producer.set("length", duration);
    producer.set_in_and_out(0, duration - 1);
    producer.set(kShotcutCaptionProperty, caption.toUtf8().constData());
    producer.set(kShotcutDetailProperty, caption.toUtf8().constData());
    filter.set(kShotcutFilterProperty, "dynamicText");
    filter.set("argument", argument.toUtf8().constData());
    if (!clip.family.isEmpty())
        filter.set("family", clip.family.toUtf8().constData());
    filter.set("size", clip.size);
    filter.set("fgcolour", clip.fgcolour.toLatin1().constData());
    filter.set("bgcolour", clip.bgcolour.toLatin1().constData());
    filter.set("olcolour", "#aa000000");
    filter.set("outline", clip.outline);
    filter.set("weight", 700);
    filter.set("style", "normal");
    filter.set("geometry", clip.geometry.toLatin1().constData());
    filter.set("valign", clip.valign.toLatin1().constData());
    filter.set("halign", clip.halign.toLatin1().constData());
    // A fixed size, as Open Other > Text: with usePointSize 0 the Filters panel replaces size by
    // height / lines when it shows the filter; with 1 it sets size from the point size, so that is
    // the size in points at the screen's resolution (it may then move size by about a pixel).
    const QScreen *screen = QGuiApplication::primaryScreen();
    const double dpi = screen ? screen->logicalDotsPerInchY() : 96.0;
    filter.set("shotcut:usePointSize", 1);
    filter.set("shotcut:pointSize", qMax(1, qRound(clip.size * 72 / dpi)));
    filter.set_in_and_out(producer.get_in(), producer.get_out());
    producer.attach(filter);

    const int index = pushClipXml(mode, track, position, MLT.XML(&producer), undoText, error);
    if (index < 0)
        return textResult(error, true);
    MultitrackModel &model = *MAIN.timelineDock()->model();
    std::unique_ptr<Mlt::ClipInfo> info = model.getClipInfo(track, index);
    if (!info)
        return textResult("The clip was added but cannot be read back; call get_timeline.", true);
    return jsonResult(QJsonObject{{"track", trackJson(model, track)},
                                  {"clip",
                                   QJsonObject{{"start", info->start},
                                               {"duration", info->frame_count}}}});
}

QJsonObject titleAddTool(const QJsonObject &arguments)
{
    QString error;
    if (!checkKeys(arguments,
                   {"text",
                    "track",
                    "position",
                    "duration",
                    "font",
                    "size",
                    "color",
                    "bg_color",
                    "outline",
                    "align",
                    "valign",
                    "mode"},
                   error))
        return textResult(error, true);
    TextClip clip;
    // At most the largest size the argument accepts, also on a tall video mode.
    clip.size = qMin(400, MLT.profile().height() / 12);
    int track = 0, position = 0, duration = 0;
    QString mode = QStringLiteral("overwrite");
    if (!readText(arguments.value("text"), "text", 256, clip.text, error)
        || !readTextPlacement(arguments, track, position, duration, mode, clip, error)
        || !readIntArg(arguments, "size", 8, 400, clip.size, error)
        || !readColorArg(arguments, "color", clip.fgcolour, error)
        || !readColorArg(arguments, "bg_color", clip.bgcolour, error)
        || !readIntArg(arguments, "outline", 0, 20, clip.outline, error)
        || !readChoiceArg(arguments, "align", {"left", "center", "right"}, clip.halign, error)
        || !readChoiceArg(arguments, "valign", {"top", "middle", "bottom"}, clip.valign, error))
        return textResult(error, true);
    // Windows line ends (and a lone carriage return) become plain newlines.
    clip.text.replace(QStringLiteral("\r\n"), QStringLiteral("\n")).replace('\r', '\n');
    return addTextClip(clip, track, position, duration, mode, QObject::tr("Add title"), "text");
}

QJsonObject lowerThirdAddTool(const QJsonObject &arguments)
{
    QString error;
    if (!checkKeys(arguments,
                   {"line1",
                    "line2",
                    "track",
                    "position",
                    "duration",
                    "font",
                    "color",
                    "bar_color",
                    "mode"},
                   error))
        return textResult(error, true);
    TextClip clip;
    clip.size = MLT.profile().height() / 20;
    clip.bgcolour = QStringLiteral("#99000000");
    clip.geometry = QStringLiteral("0%/76%:100%x14%");
    clip.halign = QStringLiteral("left");
    clip.valign = QStringLiteral("bottom");
    int track = 0, position = 0, duration = 0;
    QString mode = QStringLiteral("overwrite");
    QString line1, line2;
    const QJsonValue line2Value = arguments.value("line2");
    if (!readText(arguments.value("line1"), "line1", 200, line1, error)
        || (!line2Value.isUndefined() && !readText(line2Value, "line2", 200, line2, error)))
        return textResult(error, true);
    if (line1.contains('\n') || line2.contains('\n') || line1.contains('\r')
        || line2.contains('\r'))
        return textResult("line1 and line2 must not contain line breaks.", true);
    if (!readTextPlacement(arguments, track, position, duration, mode, clip, error)
        || !readColorArg(arguments, "color", clip.fgcolour, error)
        || !readColorArg(arguments, "bar_color", clip.bgcolour, error))
        return textResult(error, true);
    clip.text = line2.isEmpty() ? line1 : line1 + '\n' + line2;
    return addTextClip(clip,
                       track,
                       position,
                       duration,
                       mode,
                       QObject::tr("Add lower third"),
                       "line1 and line2 together");
}

} // namespace

QList<McpTool> McpTools::formatTools()
{
    return {
        {"project_set_format",
         "Set the project's video format (Shotcut's Video Mode) on an EMPTY project: pass preset "
         "(16:9 = 1920x1080, 9:16 = 1080x1920, 1:1 = 1080x1080, 4:5 = 1080x1350) or width and "
         "height (even integers 64 to 7680), and optionally fps (23.976, 24, 25, 29.97, 30, 50, "
         "59.94 or 60; default 30). The format is square-pixel, progressive, BT.709, like an "
         "unnamed custom Video Mode. Call it BEFORE adding media: media added later keeps the "
         "format instead of setting it. Refused, changing nothing, once anything is loaded (a "
         "clip, a playlist item or a timeline): start a new project first. Not an undo step; the "
         "saved Video Mode setting is not changed, so a new project after closing this one uses "
         "the saved mode again, and the Video Mode menu shows no mode checked meanwhile. Returns "
         "{width, height, fps_num, fps_den, sar, dar} (sar and dar as \"num:den\").",
         schema(R"({"type":"object","properties":{
             "preset":{"type":"string","enum":["16:9","9:16","1:1","4:5"]},
             "width":{"type":"integer","minimum":64,"maximum":7680,"multipleOf":2},
             "height":{"type":"integer","minimum":64,"maximum":7680,"multipleOf":2},
             "fps":{"type":"number","enum":[23.976,24,25,29.97,30,50,59.94,60]}
           },"additionalProperties":false})"),
         projectSetFormatTool},
        {"clip_reframe",
         "Make a timeline clip of another shape fill the project's frame (say a 16:9 or 4:3 clip "
         "in a 9:16 project) as ONE undo step (\"Reframe clip (fill)\" or \"(pad)\"). mode "
         "\"fill\" crops: Shotcut's Size, Position & Rotate filter in Fill mode with the rect "
         "that covers the frame (the picture scaled until its shorter side fits, centred "
         "vertically); focus_x (0 to 1, default 0.5) is the point across the picture that is put "
         "at the frame's centre as far as the picture's edges allow (0 = keep the left edge, 1 = "
         "the right edge; no effect when the picture is taller than the frame). Or keyframes: 1 "
         "to 50 {frame, focus_x} (frame counted from the clip's first frame, strictly "
         "increasing, within the clip) to pan, with interpolation \"linear\" (default) or "
         "\"smooth\". mode \"pad\" fits the whole picture over a blurred, enlarged copy of "
         "itself (Shotcut's Blur: Pad, blur 4; no focus_x or keyframes). The filter is marked as "
         "the clip's reframe (shotcut:reframe=1, kept by filter_set): a later call replaces it "
         "(also when switching between fill and pad) instead of adding another. Refused "
         "(nothing changed): a clip that has a Size, Position & Rotate or Blur: Pad filter the "
         "tool did not add (it already renders the frame at the project's shape: remove or "
         "adjust it first), a clip whose shape is within 1% of the project's "
         "(\"The clip already matches the project format.\"), unknown arguments, focus_x with "
         "keyframes, focus_x, keyframes or interpolation with pad, a locked track, a blank or "
         "transition, an audio track or audio-only clip. Returns {filters:[{id, index}], track} "
         "(as filter_list_applied reports them).",
         schema(R"({"type":"object","properties":{
             "track":{"type":"integer","minimum":0},
             "clip":{"type":"integer","minimum":0},
             "mode":{"type":"string","enum":["fill","pad"]},
             "focus_x":{"type":"number","minimum":0,"maximum":1},
             "keyframes":{"type":"array","minItems":1,"maxItems":50,"items":{
               "type":"object","properties":{
                 "frame":{"type":"integer","minimum":0},
                 "focus_x":{"type":"number","minimum":0,"maximum":1}},
               "required":["frame","focus_x"],"additionalProperties":false}},
             "interpolation":{"type":"string","enum":["linear","smooth"]}
           },"required":["track","clip","mode"],"additionalProperties":false})"),
         clipReframeTool},
        {"title_add",
         "Put a text clip (a title) on a VIDEO track at an absolute timeline frame as ONE undo "
         "step (\"Add title\"): a transparent clip of duration frames with Shotcut's Text: Simple "
         "filter, as Open Other > Text makes it, so it shows over the tracks below (put it on an "
         "upper track, e.g. one made with track_add). text is 1 to 256 characters (as Shotcut's "
         "text editor counts them: each # and emoji counts twice) and at most 511 bytes in UTF-8; "
         "newlines make more lines (\\r\\n and \\r count as newlines); # is shown as typed (not a "
         "keyword such as #timecode#). The clip is named after its first line that is not blank. "
         "font: a font family name (default the text filter's, Sans); size: 8 to 400 pixels "
         "(default the project height / 12, at most 400); color "
         "(default \"#ffffffff\") and bg_color (a box behind the text, default \"#00000000\" "
         "none): \"#rrggbb\" or \"#aarrggbb\"; outline: 0 to 20 pixels (default 3, colour "
         "#aa000000); bold. align left|center|right (default center) and valign top|middle|bottom "
         "(default middle) place it in the whole frame. mode overwrite (default) replaces what is "
         "under it and never ripples; insert splits and pushes later items right (and other "
         "tracks/markers per Shotcut's ripple settings), as timeline_insert_range. From the "
         "track's end on, the clip is appended (after a gap); position may be at most one hour "
         "after the end of the track; a position inside a transition is refused. With no timeline "
         "only track 0 is accepted and a video track is created. Refused (nothing changed): "
         "unknown arguments, an audio or locked track. Edit it later in Shotcut's Filters panel. "
         "Returns {track, clip:{start, duration}}.",
         schema(R"({"type":"object","properties":{
             "text":{"type":"string","minLength":1,"maxLength":256},
             "track":{"type":"integer","minimum":0},
             "position":{"type":"integer","minimum":0},
             "duration":{"type":"integer","minimum":1,"maximum":100000},
             "font":{"type":"string","minLength":1,"maxLength":100},
             "size":{"type":"integer","minimum":8,"maximum":400},
             "color":{"type":"string","pattern":"^#([0-9a-fA-F]{6}|[0-9a-fA-F]{8})$"},
             "bg_color":{"type":"string","pattern":"^#([0-9a-fA-F]{6}|[0-9a-fA-F]{8})$"},
             "outline":{"type":"integer","minimum":0,"maximum":20},
             "align":{"type":"string","enum":["left","center","right"]},
             "valign":{"type":"string","enum":["top","middle","bottom"]},
             "mode":{"type":"string","enum":["insert","overwrite"]}
           },"required":["text","track","position","duration"],"additionalProperties":false})"),
         titleAddTool},
        {"lower_third_add",
         "Put a lower third (a name and an optional second line on a bar across the lower part of "
         "the frame) on a VIDEO track as ONE undo step (\"Add lower third\"): the same transparent "
         "text clip as title_add with the text \"line1\\nline2\" left-aligned at the bottom of the "
         "band from 76% to 90% of the frame height (Shotcut's Lower Third geometry "
         "0%/76%:100%x14%), size the project height / 20, bold, outline 3, on a bar of bar_color "
         "(default \"#99000000\", translucent black). line1 and line2 are 1 to 200 characters "
         "each, without line breaks, and together within title_add's text limits; # is shown as "
         "typed. color is the text colour (default "
         "\"#ffffffff\"); colours are \"#rrggbb\" or \"#aarrggbb\"; font as in title_add. track, "
         "position, duration (1 to 100000 frames) and mode (overwrite by default, or insert) as in "
         "title_add. The clip is named after line1. Refused (nothing changed): unknown arguments, "
         "an audio or locked track. Returns {track, clip:{start, duration}}.",
         schema(R"({"type":"object","properties":{
             "line1":{"type":"string","minLength":1,"maxLength":200},
             "line2":{"type":"string","minLength":1,"maxLength":200},
             "track":{"type":"integer","minimum":0},
             "position":{"type":"integer","minimum":0},
             "duration":{"type":"integer","minimum":1,"maximum":100000},
             "font":{"type":"string","minLength":1,"maxLength":100},
             "color":{"type":"string","pattern":"^#([0-9a-fA-F]{6}|[0-9a-fA-F]{8})$"},
             "bar_color":{"type":"string","pattern":"^#([0-9a-fA-F]{6}|[0-9a-fA-F]{8})$"},
             "mode":{"type":"string","enum":["insert","overwrite"]}
           },"required":["line1","track","position","duration"],"additionalProperties":false})"),
         lowerThirdAddTool},
    };
}
