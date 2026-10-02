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

#include "controllers/filtercontroller.h"
#include "docks/timelinedock.h"
#include "mainwindow.h"
#include "mltcontroller.h"
#include "models/multitrackmodel.h"
#include "qmltypes/qmlmetadata.h"
#include "shotcut_mlt_properties.h"

#include <MltPlaylist.h>

#include <QRectF>
#include <QRegularExpression>
#include <QScopedPointer>
#include <QStringList>
#include <QUndoStack>

#include <cmath>
#include <memory>

using namespace McpTools;
using McpFilters::FilterParam;
using McpFilters::FilterSpec;

namespace {

constexpr double kMaxRectValue = 100000;

FilterParam number(const char *name,
                   double min,
                   double max,
                   double def,
                   const char *unit,
                   const char *property = nullptr,
                   double scale = 1.0)
{
    FilterParam p;
    p.name = name;
    p.property = property ? property : name;
    p.type = FilterParam::Number;
    p.min = min;
    p.max = max;
    p.def = def;
    p.unit = unit;
    p.scale = scale;
    return p;
}

FilterParam integer(const char *name,
                    double min,
                    double max,
                    double def,
                    const char *unit,
                    FilterParam::MaxFrom maxFrom = FilterParam::FixedMax)
{
    FilterParam p = number(name, min, max, def, unit);
    p.type = FilterParam::Integer;
    p.maxFrom = maxFrom;
    return p;
}

FilterParam color(const char *name, const char *property, const char *def)
{
    FilterParam p;
    p.name = name;
    p.property = property;
    p.type = FilterParam::Color;
    p.defText = def;
    return p;
}

FilterParam rect(const char *name, const char *property)
{
    FilterParam p;
    p.name = name;
    p.property = property;
    p.type = FilterParam::Rect;
    p.unit = QStringLiteral("px");
    return p;
}

// Parameters, ranges and defaults come from each filter's Shotcut QML (src/qml/filters/<dir>/):
// the meta file's keyframe ranges, the ui file's sliders and its "if (filter.isNew)" defaults.
QList<FilterSpec> buildSpecs()
{
    QList<FilterSpec> specs;
    FilterSpec s;

    // audio_gain/meta.qml, ui.qml
    s = FilterSpec();
    s.id = "gain";
    s.service = "volume";
    s.shotcutId = "audioGain";
    s.label = "Gain / Volume";
    s.audio = true;
    s.params = {number("level", -70, 24, 0, "dB")};
    specs.append(s);

    // audio_normalize_1p/meta.qml, ui.qml (no objectName)
    s = FilterSpec();
    s.id = "normalize_one_pass";
    s.service = "dynamic_loudness";
    s.label = "Normalize: One Pass";
    s.audio = true;
    s.params = {number("target_loudness", -50, -10, -23, "LUFS"),
                number("window", 2, 600, 10, "s"),
                number("max_gain", 0, 30, 15, "dB"),
                number("min_gain", -30, 0, -15, "dB"),
                number("max_rate", 0.5, 9, 3, "dB/s")};
    s.setup = [](Mlt::Properties &p) { p.set("discontinuity_reset", 1); };
    specs.append(s);

    // brightness/meta.qml, ui.qml (no objectName; the UI always sets rgb_only)
    s = FilterSpec();
    s.id = "brightness";
    s.service = "brightness";
    s.label = "Brightness";
    s.params = {number("level", 0, 2, 1, "x")};
    s.setup = [](Mlt::Properties &p) { p.set("rgb_only", 1); };
    specs.append(s);

    // color/meta.qml, ui.qml: raw MLT values of filter version 1 (the UI's wheels map lift to
    // -1..1 and gamma/gain to 0.5..2; scaleWheelToValueV1).
    s = FilterSpec();
    s.id = "color_grading";
    s.service = "lift_gamma_gain";
    s.label = "Color Grading";
    for (const char *c : {"lift_r", "lift_g", "lift_b"})
        s.params.append(number(c, -1, 1, 0, ""));
    for (const char *c : {"gamma_r", "gamma_g", "gamma_b", "gain_r", "gain_g", "gain_b"})
        s.params.append(number(c, 0.5, 2, 1, "x"));
    s.setup = [](Mlt::Properties &p) { p.set("shotcut:filter_version", 1); };
    specs.append(s);

    // saturation/meta_frei0r.qml, ui_frei0r.qml: property "0" = percent / 800, UI 0..300 %.
    s = FilterSpec();
    s.id = "saturation";
    s.service = "frei0r.saturat0r";
    s.label = "Saturation";
    s.params = {number("saturation", 0, 3, 1, "x", "0", 0.125)};
    s.setup = [](Mlt::Properties &p) { p.set("threads", 0); };
    specs.append(s);

    // white/meta_frei0r.qml, ui.qml: "0" neutral color, "1" = kelvin / 15000, UI 1000..15000 K.
    s = FilterSpec();
    s.id = "white_balance";
    s.service = "frei0r.colgate";
    s.label = "White Balance";
    s.params = {color("neutral_color", "0", "#7f7f7f"),
                number("temperature_k", 1000, 15000, 6500, "K", "1", 1.0 / 15000)};
    s.setup = [](Mlt::Properties &p) { p.set("threads", 0); };
    specs.append(s);

    // blur/meta_box_blur.qml, ui_box_blur.qml
    s = FilterSpec();
    s.id = "box_blur";
    s.service = "box_blur";
    s.label = "Blur: Box";
    s.params = {integer("hradius", 0, 100, 5, "px"), integer("vradius", 0, 100, 5, "px")};
    specs.append(s);

    // pillar_echo/meta.qml, ui.qml
    s = FilterSpec();
    s.id = "blur_pad";
    s.service = "pillar_echo";
    s.shotcutId = "blur_pad";
    s.label = "Blur: Pad";
    s.params = {number("blur", 0, 10, 4, "%"), rect("rect", "rect")};
    specs.append(s);

    // size_position/meta_affine.qml, ui_affine.qml, SizePositionUI.qml: a new filter ends in
    // Fill mode (fill 1, distort 0), centered, with the fit rectangle.
    s = FilterSpec();
    s.id = "size_position";
    s.service = "affine";
    s.shotcutId = "affineSizePosition";
    s.label = "Size, Position & Rotate";
    s.params = {rect("rect", "transition.rect"),
                number("rotation", -360, 360, 0, "deg", "transition.fix_rotate_x")};
    s.setup = [](Mlt::Properties &p) {
        p.set("transition.fill", 1);
        p.set("transition.distort", 0);
        p.set("transition.valign", "middle");
        p.set("transition.halign", "center");
        p.set("background", "color:#00000000");
        p.set("transition.threads", 0);
    };
    specs.append(s);

    // crop/meta.qml, ui.qml: pixels of the video mode.
    s = FilterSpec();
    s.id = "crop";
    s.service = "crop";
    s.label = "Crop: Source";
    s.clipOnly = true;
    s.allowMultiple = false;
    s.params = {integer("left", 0, 0, 0, "px", FilterParam::ProfileWidth),
                integer("right", 0, 0, 0, "px", FilterParam::ProfileWidth),
                integer("top", 0, 0, 0, "px", FilterParam::ProfileHeight),
                integer("bottom", 0, 0, 0, "px", FilterParam::ProfileHeight)};
    s.setup = [](Mlt::Properties &p) {
        p.set("use_profile", "1");
        p.set("center", 0);
        p.set("center_bias", 0);
    };
    s.check = [](Mlt::Properties &p) {
        if (p.get_int("left") + p.get_int("right") >= MLT.profile().width()
            || p.get_int("top") + p.get_int("bottom") >= MLT.profile().height())
            return QStringLiteral("Crop would remove the whole picture: left + right must be less "
                                  "than %1 and top + bottom less than %2.")
                .arg(MLT.profile().width())
                .arg(MLT.profile().height());
        return QString();
    };
    specs.append(s);

    return specs;
}

double maxOf(const FilterParam &param)
{
    switch (param.maxFrom) {
    case FilterParam::ProfileWidth:
        return MLT.profile().width();
    case FilterParam::ProfileHeight:
        return MLT.profile().height();
    default:
        return param.max;
    }
}

QString uniqueIdOf(Mlt::Filter &filter)
{
    const QString id = QString::fromUtf8(filter.get(kShotcutFilterProperty));
    return id.isEmpty() ? QString::fromUtf8(filter.get("mlt_service")) : id;
}

// Why a row's service cannot be used here, or an empty string.
QString unavailableReason(const FilterSpec &spec)
{
    QScopedPointer<Mlt::Properties> filters(MLT.repository()->filters());
    if (!filters || !filters->get_data(spec.service.toUtf8().constData()))
        return QStringLiteral("this MLT has no %1 service").arg(spec.service);
    QmlMetadata *meta = MAIN.filterController()->metadata(
        spec.shotcutId.isEmpty() ? spec.service : spec.shotcutId);
    if (meta && meta->needsGPU())
        return QStringLiteral("it needs GPU processing");
    return QString();
}

// Six significant digits: values that went through a scale come back as 6500, not 6499.995.
double round6(double v)
{
    return QString::number(v, 'g', 6).toDouble();
}

QString num(double v)
{
    return QString::number(v, 'g', 12);
}

// The fit rectangle of Shotcut's Size/Position and Blur: Pad UIs: the clip's picture scaled into
// the video mode, centered, keeping its display aspect ratio.
QRectF fitRect(Mlt::Producer *clip)
{
    Mlt::Profile &profile = MLT.profile();
    double dar = profile.dar();
    if (clip && clip->get_int("meta.media.width") > 0 && clip->get_int("meta.media.height") > 0) {
        double sar = 1.0;
        if (clip->get_double("meta.media.sample_aspect_den") > 0)
            sar = clip->get_double("meta.media.sample_aspect_num")
                  / clip->get_double("meta.media.sample_aspect_den");
        if (sar > 0)
            dar = sar * clip->get_double("meta.media.width")
                  / clip->get_double("meta.media.height");
    }
    double w, h;
    if (dar > profile.dar()) {
        w = profile.width();
        h = w * profile.sar() / dar;
    } else {
        h = profile.height();
        w = h / profile.sar() * dar;
    }
    w = std::round(w);
    h = std::round(h);
    return QRectF(std::round((profile.width() - w) / 2),
                  std::round((profile.height() - h) / 2),
                  w,
                  h);
}

QString rectText(const QRectF &r)
{
    return QStringLiteral("%1 %2 %3 %4 1")
        .arg(num(r.x()), num(r.y()), num(r.width()), num(r.height()));
}

bool isKeyframed(Mlt::Filter &filter, const QString &property)
{
    const char *s = filter.get(property.toUtf8().constData());
    return s && ::strchr(s, '=');
}

// The params of an allowlisted filter as filter_list_applied reports them: tool-facing names and
// units; a value with keyframes as its raw MLT string.
QJsonObject paramsJson(const FilterSpec &spec, Mlt::Filter &filter)
{
    QJsonObject o;
    for (const FilterParam &param : spec.params) {
        const QByteArray property = param.property.toUtf8();
        const char *s = filter.get(property.constData());
        if (!s)
            continue;
        if (::strchr(s, '=')) {
            o[param.name] = QString::fromUtf8(s);
            continue;
        }
        switch (param.type) {
        case FilterParam::Color:
            o[param.name] = QString::fromUtf8(s);
            break;
        case FilterParam::Rect: {
            mlt_rect r = filter.get_rect(property.constData());
            const double sx = ::strchr(s, '%') ? MLT.profile().width() : 1.0;
            const double sy = ::strchr(s, '%') ? MLT.profile().height() : 1.0;
            o[param.name] = QJsonObject{{"x", round6(r.x * sx)},
                                        {"y", round6(r.y * sy)},
                                        {"w", round6(r.w * sx)},
                                        {"h", round6(r.h * sy)}};
            break;
        }
        default:
            o[param.name] = round6(filter.get_double(property.constData()) / param.scale);
        }
    }
    return o;
}

QJsonObject specJson(const FilterSpec &spec)
{
    QJsonArray params;
    for (const FilterParam &param : spec.params) {
        QJsonObject p{{"name", param.name}};
        switch (param.type) {
        case FilterParam::Color:
            p["type"] = "color";
            p["default"] = param.defText;
            break;
        case FilterParam::Rect:
            p["type"] = "rect";
            p["default"] = "fit";
            break;
        default:
            p["type"] = param.type == FilterParam::Integer ? "int" : "number";
            p["min"] = param.min;
            p["max"] = maxOf(param);
            p["default"] = param.def;
        }
        if (!param.unit.isEmpty())
            p["unit"] = param.unit;
        params.append(p);
    }
    return QJsonObject{{"id", spec.id},
                       {"service", spec.service},
                       {"label", spec.label},
                       {"kind", spec.audio ? "audio" : "video"},
                       {"allow_multiple", spec.allowMultiple},
                       {"clip_only", spec.clipOnly},
                       {"params", params}};
}

// Writes the MLT value of every param named in params into props (validated); summary receives
// "name=value" in the order of the spec. With existing (filter_set), a param with keyframes on
// that filter is refused.
bool readParams(const FilterSpec &spec,
                const QJsonObject &params,
                Mlt::Properties &props,
                QStringList &summary,
                QString &error,
                Mlt::Filter *existing = nullptr)
{
    for (auto it = params.begin(); it != params.end(); ++it) {
        bool known = false;
        for (const FilterParam &param : spec.params)
            known = known || param.name == it.key();
        if (!known) {
            QStringList names;
            for (const FilterParam &param : spec.params)
                names << param.name;
            error = QStringLiteral("%1 has no parameter '%2'; its parameters are: %3.")
                        .arg(spec.id, it.key(), names.join(", "));
            return false;
        }
    }
    for (const FilterParam &param : spec.params) {
        if (!params.contains(param.name))
            continue;
        const QJsonValue v = params.value(param.name);
        const QByteArray property = param.property.toUtf8();
        if (existing && isKeyframed(*existing, param.property)) {
            error = QStringLiteral("%1 has keyframes on this filter; filter_set cannot change it.")
                        .arg(param.name);
            return false;
        }
        switch (param.type) {
        case FilterParam::Number:
        case FilterParam::Integer: {
            const double d = v.toDouble();
            const bool isInt = param.type == FilterParam::Integer;
            if (!v.isDouble() || !std::isfinite(d) || d < param.min || d > maxOf(param)
                || (isInt && d != std::floor(d))) {
                error = QStringLiteral("%1 must be %2 from %3 to %4.")
                            .arg(param.name, isInt ? "an integer" : "a number")
                            .arg(param.min)
                            .arg(maxOf(param));
                return false;
            }
            props.set(property.constData(), num(d * param.scale).toUtf8().constData());
            summary << QStringLiteral("%1=%2").arg(param.name, num(d));
            break;
        }
        case FilterParam::Color: {
            static const QRegularExpression re(QStringLiteral("^#[0-9a-fA-F]{6}$"));
            if (!v.isString() || !re.match(v.toString()).hasMatch()) {
                error = QStringLiteral("%1 must be a color like \"#7f7f7f\".").arg(param.name);
                return false;
            }
            props.set(property.constData(), v.toString().toUtf8().constData());
            summary << QStringLiteral("%1=%2").arg(param.name, v.toString());
            break;
        }
        case FilterParam::Rect: {
            const QJsonObject r = v.toObject();
            bool ok = v.isObject() && r.size() == 4;
            for (const char *key : {"x", "y", "w", "h"}) {
                const QJsonValue c = r.value(key);
                ok = ok && c.isDouble() && std::isfinite(c.toDouble())
                     && std::abs(c.toDouble()) <= kMaxRectValue;
            }
            if (!ok || r.value("w").toDouble() <= 0 || r.value("h").toDouble() <= 0) {
                error = QStringLiteral("%1 must be an object {x, y, w, h} in pixels of the video "
                                       "mode, w and h greater than 0, each value at most %2 in "
                                       "size.")
                            .arg(param.name)
                            .arg(kMaxRectValue);
                return false;
            }
            const QRectF rf(r.value("x").toDouble(),
                            r.value("y").toDouble(),
                            r.value("w").toDouble(),
                            r.value("h").toDouble());
            props.set(property.constData(), rectText(rf).toUtf8().constData());
            summary << QStringLiteral("%1=%2 %3 %4 %5")
                           .arg(param.name, num(rf.x()), num(rf.y()), num(rf.width()),
                                num(rf.height()));
            break;
        }
        }
    }
    return true;
}

void setDefaults(const FilterSpec &spec, Mlt::Producer *clip, Mlt::Properties &props)
{
    for (const FilterParam &param : spec.params) {
        const QByteArray property = param.property.toUtf8();
        switch (param.type) {
        case FilterParam::Color:
            props.set(property.constData(), param.defText.toUtf8().constData());
            break;
        case FilterParam::Rect:
            props.set(property.constData(), rectText(fitRect(clip)).toUtf8().constData());
            break;
        default:
            props.set(property.constData(), num(param.def * param.scale).toUtf8().constData());
        }
    }
    if (spec.setup)
        spec.setup(props);
}

// filter_add's clip rules: an unlocked track, a clip, and audio/picture that fit the clip.
std::unique_ptr<Mlt::ClipInfo> checkClip(
    MultitrackModel &model, int track, int clip, const FilterSpec &spec, QString &error)
{
    std::unique_ptr<Mlt::ClipInfo> info = plainClip(model, track, clip, error);
    if (!info)
        return nullptr;
    Mlt::Producer &producer = *info->producer;
    if (spec.audio) {
        if (producer.property_exists("audio_index") && producer.get_int("audio_index") < 0) {
            error = QStringLiteral("Clip %1 on track %2 has no audio; %3 is an audio filter.")
                        .arg(clip)
                        .arg(track)
                        .arg(spec.id);
            return nullptr;
        }
    } else if (model.trackList().at(track).type == AudioTrackType) {
        error = QStringLiteral("Track %1 is an audio track; %2 is a picture filter.")
                    .arg(track)
                    .arg(spec.id);
        return nullptr;
    } else if (producer.property_exists("video_index") && producer.get_int("video_index") < 0) {
        error = QStringLiteral("Clip %1 on track %2 has no picture (audio only); %3 is a picture "
                               "filter.")
                    .arg(clip)
                    .arg(track)
                    .arg(spec.id);
        return nullptr;
    }
    return info;
}

// The user filter at userIndex of the clip, or null with a message.
std::unique_ptr<Mlt::Filter> userFilter(Mlt::Producer &producer,
                                        int track,
                                        int clip,
                                        int userIndex,
                                        QString &error)
{
    const QList<int> indices = McpFilter::userFilterIndices(producer);
    if (userIndex < 0 || userIndex >= indices.size()) {
        error = QStringLiteral("Clip %1 on track %2 has no filter %3 (it has %4); call "
                               "filter_list_applied.")
                    .arg(clip)
                    .arg(track)
                    .arg(userIndex)
                    .arg(indices.size());
        return nullptr;
    }
    std::unique_ptr<Mlt::Filter> filter(producer.filter(indices.at(userIndex)));
    if (!filter || !filter->is_valid()) {
        error = QStringLiteral("Filter %1 of clip %2 on track %3 is not valid.")
                    .arg(userIndex)
                    .arg(clip)
                    .arg(track);
        return nullptr;
    }
    return filter;
}

// Pushes command as one undo step named text (or into the caller's open macro), after pausing.
// ok tells whether the command did its work; a failed step is rolled back.
bool pushCommand(QUndoCommand *command,
                 const QString &text,
                 bool joinMacro,
                 const std::function<bool()> &ok,
                 QString &error)
{
    QUndoStack *stack = MAIN.undoStack();
    MLT.pause();
    if (!joinMacro)
        stack->beginMacro(text);
    stack->push(command);
    const bool done = ok();
    if (!joinMacro)
        finishMacro(stack, done);
    if (!done)
        error = QStringLiteral("Shotcut could not apply the filter change; nothing was changed.");
    return done;
}

bool addFilter(MultitrackModel &model,
               int track,
               int clip,
               Mlt::ClipInfo &info,
               const FilterSpec &spec,
               const QJsonObject &params,
               QString text,
               bool joinMacro,
               QString &error,
               int *userIndex)
{
    Mlt::Properties props;
    setDefaults(spec, info.producer, props);
    QStringList summary;
    if (!readParams(spec, params, props, summary, error))
        return false;
    if (spec.check) {
        error = spec.check(props);
        if (!error.isEmpty())
            return false;
    }
    if (text.isEmpty())
        text = QObject::tr("Add %1 filter").arg(spec.label);
    auto *command = new McpFilter::AddCommand(model,
                                              McpFilter::Target{track, clip},
                                              text,
                                              spec.service,
                                              spec.shotcutId,
                                              props);
    const auto added = [command] { return command->userIndex() >= 0; };
    if (!pushCommand(command, text, joinMacro, added, error))
        return false;
    if (userIndex)
        *userIndex = command->userIndex();
    return true;
}

bool setFilter(MultitrackModel &model,
               int track,
               int clip,
               int userIndex,
               Mlt::Filter &filter,
               const FilterSpec &spec,
               const QJsonObject &params,
               QString text,
               bool joinMacro,
               QString &error)
{
    Mlt::Properties before, after;
    before.inherit(filter);
    after.inherit(filter);
    QStringList summary;
    if (!readParams(spec, params, after, summary, error, &filter))
        return false;
    if (spec.check) {
        error = spec.check(after);
        if (!error.isEmpty())
            return false;
    }
    if (text.isEmpty())
        text = QObject::tr("Set %1: %2").arg(spec.label, summary.join(", "));
    auto *command = new McpFilter::SetCommand(model,
                                              McpFilter::Target{track, clip},
                                              userIndex,
                                              text,
                                              before,
                                              after);
    return pushCommand(command, text, joinMacro, [command] { return command->applied(); }, error);
}

bool readTarget(const QJsonObject &arguments, int &track, int &clip, QString &error)
{
    if (readFrame(arguments, "track", track) != FrameArg::Ok
        || readFrame(arguments, "clip", clip) != FrameArg::Ok) {
        error = QStringLiteral("track and clip must be non-negative integers.");
        return false;
    }
    if (!MAIN.isMultitrackValid()) {
        error = QStringLiteral("There is no timeline.");
        return false;
    }
    return true;
}

bool readParamsArg(const QJsonObject &arguments, bool required, QJsonObject &params, QString &error)
{
    const QJsonValue v = arguments.value("params");
    if (v.isUndefined() && !required)
        return true;
    if (!v.isObject() || (required && v.toObject().isEmpty())) {
        error = required ? QStringLiteral("params must be an object with at least one parameter.")
                         : QStringLiteral("params must be an object.");
        return false;
    }
    params = v.toObject();
    return true;
}

QJsonObject filterListAvailableTool(const QJsonObject &)
{
    QJsonArray filters, unavailable;
    for (const FilterSpec &spec : McpFilters::allSpecs()) {
        const QString reason = unavailableReason(spec);
        if (reason.isEmpty())
            filters.append(specJson(spec));
        else
            unavailable.append(
                QJsonObject{{"id", spec.id}, {"service", spec.service}, {"reason", reason}});
    }
    return jsonResult(QJsonObject{{"filters", filters}, {"unavailable", unavailable}});
}

QJsonObject filterListAppliedTool(const QJsonObject &arguments)
{
    int track = 0, clip = 0;
    QString error;
    if (!readTarget(arguments, track, clip, error))
        return textResult(error, true);
    MultitrackModel &model = *MAIN.timelineDock()->model();
    std::unique_ptr<Mlt::ClipInfo> info = plainClip(model, track, clip, error, true);
    if (!info)
        return textResult(error, true);
    QJsonArray filters;
    const QList<int> indices = McpFilter::userFilterIndices(*info->producer);
    for (int i = 0; i < indices.size(); ++i) {
        std::unique_ptr<Mlt::Filter> filter(info->producer->filter(indices.at(i)));
        if (!filter || !filter->is_valid())
            continue;
        const FilterSpec *spec = McpFilters::specOf(*filter);
        filters.append(QJsonObject{{"index", i},
                                   {"id", spec ? spec->id : uniqueIdOf(*filter)},
                                   {"service", QString::fromUtf8(filter->get("mlt_service"))},
                                   {"disabled", filter->get_int("disable") != 0},
                                   {"params", spec ? paramsJson(*spec, *filter) : QJsonObject()}});
    }
    return jsonResult(QJsonObject{{"filters", filters}});
}

QJsonObject filterAddTool(const QJsonObject &arguments)
{
    int track = 0, clip = 0;
    QString error;
    if (!readTarget(arguments, track, clip, error))
        return textResult(error, true);
    const QString id = arguments.value("filter").toString();
    if (!arguments.value("filter").isString() || id.isEmpty())
        return textResult("filter must be an id from filter_list_available.", true);
    QJsonObject params;
    if (!readParamsArg(arguments, false, params, error))
        return textResult(error, true);
    const FilterSpec *spec = McpFilters::findSpec(id);
    if (!spec)
        return textResult(QStringLiteral("There is no available filter '%1'; call "
                                         "filter_list_available.")
                              .arg(id),
                          true);
    MultitrackModel &model = *MAIN.timelineDock()->model();
    std::unique_ptr<Mlt::ClipInfo> info = checkClip(model, track, clip, *spec, error);
    if (!info)
        return textResult(error, true);
    if (!spec->allowMultiple) {
        const QList<int> indices = McpFilter::userFilterIndices(*info->producer);
        for (int i = 0; i < indices.size(); ++i) {
            std::unique_ptr<Mlt::Filter> filter(info->producer->filter(indices.at(i)));
            if (filter && McpFilters::specOf(*filter) == spec)
                return textResult(QStringLiteral("Clip %1 on track %2 already has a %3 filter "
                                                 "(index %4), which Shotcut allows only once; "
                                                 "change it with filter_set.")
                                      .arg(clip)
                                      .arg(track)
                                      .arg(spec->id)
                                      .arg(i),
                                  true);
        }
    }
    int index = -1;
    if (!addFilter(model, track, clip, *info, *spec, params, QString(), false, error, &index))
        return textResult(error, true);
    return jsonResult(QJsonObject{{"index", index}, {"track", trackJson(model, track)}});
}

QJsonObject filterSetTool(const QJsonObject &arguments)
{
    int track = 0, clip = 0, index = 0;
    QString error;
    if (!readTarget(arguments, track, clip, error))
        return textResult(error, true);
    if (readFrame(arguments, "index", index) != FrameArg::Ok)
        return textResult("index must be a non-negative integer from filter_list_applied.", true);
    QJsonObject params;
    if (!readParamsArg(arguments, true, params, error))
        return textResult(error, true);
    MultitrackModel &model = *MAIN.timelineDock()->model();
    std::unique_ptr<Mlt::ClipInfo> info = plainClip(model, track, clip, error);
    if (!info)
        return textResult(error, true);
    std::unique_ptr<Mlt::Filter> filter = userFilter(*info->producer, track, clip, index, error);
    if (!filter)
        return textResult(error, true);
    const FilterSpec *spec = McpFilters::specOf(*filter);
    if (!spec)
        return textResult(QStringLiteral("Filter %1 (%2) is not one of filter_list_available's "
                                         "filters, so filter_set cannot change it; "
                                         "filter_remove can remove it.")
                              .arg(index)
                              .arg(uniqueIdOf(*filter)),
                          true);
    if (!setFilter(model, track, clip, index, *filter, *spec, params, QString(), false, error))
        return textResult(error, true);
    return jsonResult(QJsonObject{{"index", index}, {"track", trackJson(model, track)}});
}

QJsonObject filterRemoveTool(const QJsonObject &arguments)
{
    int track = 0, clip = 0, index = 0;
    QString error;
    if (!readTarget(arguments, track, clip, error))
        return textResult(error, true);
    if (readFrame(arguments, "index", index) != FrameArg::Ok)
        return textResult("index must be a non-negative integer from filter_list_applied.", true);
    MultitrackModel &model = *MAIN.timelineDock()->model();
    std::unique_ptr<Mlt::ClipInfo> info = plainClip(model, track, clip, error);
    if (!info)
        return textResult(error, true);
    std::unique_ptr<Mlt::Filter> filter = userFilter(*info->producer, track, clip, index, error);
    if (!filter)
        return textResult(error, true);
    const FilterSpec *spec = McpFilters::specOf(*filter);
    const QString text = QObject::tr("Remove %1 filter")
                             .arg(spec ? spec->label : uniqueIdOf(*filter));
    filter.reset();
    info.reset();
    auto *command = new McpFilter::RemoveCommand(model,
                                                 McpFilter::Target{track, clip},
                                                 index,
                                                 text);
    if (!pushCommand(command, text, false, [command] { return command->removed(); }, error))
        return textResult(error, true);
    return jsonResult(QJsonObject{{"index", index}, {"track", trackJson(model, track)}});
}

} // namespace

namespace McpFilters {

const QList<FilterSpec> &allSpecs()
{
    static const QList<FilterSpec> specs = buildSpecs();
    return specs;
}

const FilterSpec *findSpec(const QString &id)
{
    for (const FilterSpec &spec : allSpecs()) {
        if (spec.id == id)
            return unavailableReason(spec).isEmpty() ? &spec : nullptr;
    }
    return nullptr;
}

const FilterSpec *specOf(Mlt::Filter &filter)
{
    const QString service = QString::fromUtf8(filter.get("mlt_service"));
    const QString id = uniqueIdOf(filter);
    for (const FilterSpec &spec : allSpecs()) {
        if (spec.service == service
            && (spec.shotcutId.isEmpty() ? spec.service : spec.shotcutId) == id)
            return &spec;
    }
    return nullptr;
}

bool applyFilter(int track,
                 int clip,
                 const QString &filterId,
                 const QJsonObject &params,
                 const QString &undoText,
                 QString &error,
                 bool joinMacro,
                 int *userIndex)
{
    if (!MAIN.isMultitrackValid()) {
        error = QStringLiteral("There is no timeline.");
        return false;
    }
    const FilterSpec *spec = findSpec(filterId);
    if (!spec) {
        error = QStringLiteral("The filter '%1' is not available.").arg(filterId);
        return false;
    }
    MultitrackModel &model = *MAIN.timelineDock()->model();
    std::unique_ptr<Mlt::ClipInfo> info = checkClip(model, track, clip, *spec, error);
    if (!info)
        return false;
    const QList<int> indices = McpFilter::userFilterIndices(*info->producer);
    for (int i = 0; i < indices.size(); ++i) {
        std::unique_ptr<Mlt::Filter> filter(info->producer->filter(indices.at(i)));
        if (!filter || specOf(*filter) != spec)
            continue;
        if (userIndex)
            *userIndex = i;
        if (params.isEmpty())
            return true;
        return setFilter(model, track, clip, i, *filter, *spec, params, undoText, joinMacro, error);
    }
    return addFilter(model,
                     track,
                     clip,
                     *info,
                     *spec,
                     params,
                     undoText,
                     joinMacro,
                     error,
                     userIndex);
}

bool addOutputFilter(const QString &service,
                     const QString &shotcutId,
                     Mlt::Properties &props,
                     const QString &undoText,
                     QString &error,
                     bool joinMacro,
                     int *userIndex)
{
    if (!MAIN.isMultitrackValid()) {
        error = QStringLiteral("There is no timeline.");
        return false;
    }
    QScopedPointer<Mlt::Properties> filters(MLT.repository()->filters());
    if (!filters || !filters->get_data(service.toUtf8().constData())) {
        error = QStringLiteral("This MLT has no %1 service.").arg(service);
        return false;
    }
    MultitrackModel &model = *MAIN.timelineDock()->model();
    auto *command = new McpFilter::AddCommand(model,
                                              McpFilter::Target{},
                                              undoText,
                                              service,
                                              shotcutId,
                                              props);
    const auto added = [command] { return command->userIndex() >= 0; };
    if (!pushCommand(command, undoText, joinMacro, added, error))
        return false;
    if (userIndex)
        *userIndex = command->userIndex();
    return true;
}

} // namespace McpFilters

QList<McpTool> McpTools::filterTools()
{
    return {
        {"filter_list_available",
         "List the filters the filter tools can add (an allowlist taken from Shotcut's own filter "
         "UIs; GPU-only filters are never offered): {filters:[{id, service, label, kind "
         "(audio|video), allow_multiple, clip_only, params:[{name, type (number|int|color|rect), "
         "min, max, default, unit}]}], unavailable:[{id, service, reason}]}. A rect param is an "
         "object {x, y, w, h} in pixels of the video mode; its default \"fit\" is the clip's "
         "picture fitted into the frame and centered. crop's maxima are the video mode's width "
         "and height. Filters whose MLT service is missing in this Shotcut are listed under "
         "unavailable.",
         schema(R"({"type":"object","properties":{}})"),
         filterListAvailableTool},
        {"filter_list_applied",
         "List the filters of a timeline clip in order, as Shotcut's Filters panel shows them "
         "(hidden internal filters are skipped): {filters:[{index, id, service, disabled, "
         "params}]}. id is the filter_list_available id for allowlisted filters, else Shotcut's "
         "filter id or MLT service; params uses the tool-facing names and units of "
         "filter_list_available (a value with keyframes is its raw MLT string) and is empty for "
         "filters outside the allowlist. Works on locked tracks. Indexes are what filter_set "
         "and filter_remove take.",
         schema(R"({"type":"object","properties":{
             "track":{"type":"integer","minimum":0},
             "clip":{"type":"integer","minimum":0}
           },"required":["track","clip"]})"),
         filterListAppliedTool},
        {"filter_add",
         "Add an allowlisted filter (see filter_list_available) to a timeline clip as ONE undo "
         "step, after its existing filters; the clip's parent producer gets it, as in Shotcut, "
         "so it follows the clip when it is moved or trimmed. Missing params take Shotcut's "
         "defaults; unknown names, wrong types and out-of-range values are errors. Refused: a "
         "locked track, a blank or transition, an audio filter on a clip with no audio, a "
         "picture filter on an audio-only clip or on an audio track, and a second filter of a "
         "kind that allows only one (allow_multiple false). Keyframes are not set. Does not "
         "change the selection. Returns {index, track}.",
         schema(R"({"type":"object","properties":{
             "track":{"type":"integer","minimum":0},
             "clip":{"type":"integer","minimum":0},
             "filter":{"type":"string","description":"An id from filter_list_available."},
             "params":{"type":"object","description":"Parameter values by name; rect params are {x, y, w, h} in pixels."}
           },"required":["track","clip","filter"]})"),
         filterAddTool},
        {"filter_set",
         "Change parameters of a clip's allowlisted filter (index from filter_list_applied) as "
         "ONE undo step named after the values (\"Set <label>: name=value, ...\"); every call is "
         "its own undo step, also when it repeats a value. params needs at least one entry and "
         "is checked like filter_add's; other params keep their values. A parameter with "
         "keyframes is refused (keyframes are not edited). Locked tracks are refused. Returns "
         "{index, track}.",
         schema(R"({"type":"object","properties":{
             "track":{"type":"integer","minimum":0},
             "clip":{"type":"integer","minimum":0},
             "index":{"type":"integer","minimum":0},
             "params":{"type":"object","minProperties":1}
           },"required":["track","clip","index","params"]})"),
         filterSetTool},
        {"filter_remove",
         "Remove the filter at index (from filter_list_applied; any listed filter, allowlisted "
         "or not) from a timeline clip as ONE undo step; undo puts it back at the same place "
         "with all its settings. Later indexes shift down by one. Locked tracks are refused. "
         "Returns {index, track}.",
         schema(R"({"type":"object","properties":{
             "track":{"type":"integer","minimum":0},
             "clip":{"type":"integer","minimum":0},
             "index":{"type":"integer","minimum":0}
           },"required":["track","clip","index"]})"),
         filterRemoveTool},
    };
}
