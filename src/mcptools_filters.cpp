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
#include "controllers/filtercontroller.h"
#include "docks/timelinedock.h"
#include "mainwindow.h"
#include "mltcontroller.h"
#include "models/metadatamodel.h"
#include "models/multitrackmodel.h"
#include "qmltypes/qmlmetadata.h"
#include "shotcut_mlt_properties.h"
#include "util.h"

#include <MltChain.h>
#include <MltPlaylist.h>
#include <MltTransition.h>
#include <framework/mlt_version.h>

#include <QPair>
#include <QRectF>
#include <QRegularExpression>
#include <QScopedPointer>
#include <QStringList>
#include <QUndoStack>
#include <QUuid>

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

    // contrast/meta.qml, ui.qml: the Contrast slider (level 0..1, 0.5 unchanged) sets every
    // gain_* to level * 2 and every gamma_* to (1 - level) * 2 (gainFactor, gammaFactor).
    s = FilterSpec();
    s.id = "contrast";
    s.service = "lift_gamma_gain";
    s.shotcutId = "contrast";
    s.label = "Contrast";
    for (const char *c : {"gamma_r", "gamma_g", "gamma_b", "gain_r", "gain_g", "gain_b"})
        s.params.append(number(c, 0, 2, 1, "x"));
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
    s.params[1].fit = FilterParam::PillarEchoFit;
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
    // Shotcut loads its filter list shortly after start; without it GPU-only rows are unknown.
    if (MAIN.filterController()->metadataModel()->sourceRowCount() == 0)
        return QStringLiteral("Shotcut's filter list is not loaded yet; try again in a moment");
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

// The fit rectangle of Shotcut's Size/Position (SizePositionUI.qml:316-328: the video mode's SAR,
// size not rounded) and Blur: Pad (pillar_echo/ui.qml:29-37: no SAR, size rounded) UIs: the
// clip's picture scaled into the video mode and centered.
QRectF fitRect(Mlt::Producer *clip, FilterParam::Fit fit)
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
    if (fit == FilterParam::PillarEchoFit) {
        if (dar > profile.dar()) {
            w = profile.width();
            h = std::round(w / dar);
        } else {
            h = profile.height();
            w = std::round(h * dar);
        }
    } else if (dar > profile.dar()) {
        w = profile.width();
        h = w * profile.sar() / dar;
    } else {
        h = profile.height();
        w = h / profile.sar() * dar;
    }
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
            props.set(property.constData(),
                      rectText(fitRect(clip, param.fit)).toUtf8().constData());
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
               int *userIndex,
               const std::function<void(Mlt::Properties &)> &extra = {})
{
    Mlt::Properties props;
    setDefaults(spec, info.producer, props);
    QStringList summary;
    if (!readParams(spec, params, props, summary, error))
        return false;
    if (extra)
        extra(props);
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
               QString &error,
               const std::function<void(Mlt::Properties &)> &extra = {})
{
    Mlt::Properties before, after;
    before.inherit(filter);
    after.inherit(filter);
    QStringList summary;
    if (!readParams(spec, params, after, summary, error, &filter))
        return false;
    if (extra)
        extra(after);
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
            // Shotcut's GPU crop (objectName movit.crop, service crop) counts as a crop too.
            if (filter
                && (McpFilters::specOf(*filter) == spec
                    || (spec->id == "crop" && uniqueIdOf(*filter) == "movit.crop")))
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
    if (!readTarget(arguments, track, clip, error))
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
    if (!readTarget(arguments, track, clip, error))
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
    if (readFrame(arguments, "track", track) != FrameArg::Ok)
        return textResult("track must be a non-negative integer.", true);
    QString error;
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
    if (!readTarget(arguments, track, clip, error))
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

// ---- Picture recipes

// Refuses argument names outside allowed (a misspelt value must not be ignored silently).
bool checkKeys(const QJsonObject &arguments, const QStringList &allowed, QString &error)
{
    for (auto it = arguments.begin(); it != arguments.end(); ++it) {
        if (!allowed.contains(it.key())) {
            error = QStringLiteral("Unknown argument '%1'; the arguments are: %2.")
                        .arg(it.key(), allowed.join(", "));
            return false;
        }
    }
    return true;
}

// The allowlist row id when it can be used, else null with the reason.
const FilterSpec *recipeSpec(const QString &id, QString &error)
{
    const FilterSpec *spec = McpFilters::findSpec(id);
    if (!spec) {
        for (const FilterSpec &s : McpFilters::allSpecs()) {
            if (s.id == id)
                error = QStringLiteral("The %1 filter is not available: %2.")
                            .arg(id, unavailableReason(s));
        }
    }
    return spec;
}

struct RecipeStep
{
    const FilterSpec *spec = nullptr;
    QJsonObject params;
    std::function<void(Mlt::Properties &)> extra;
};

// Applies every step to the clip (set on the first filter of its kind, else added) as ONE undo
// step named text; any failure rolls the whole call back. Returns {filters:[{id, index}], track}.
QJsonObject runRecipe(int track, int clip, const QList<RecipeStep> &steps, const QString &text)
{
    QString error;
    QUndoStack *stack = MAIN.undoStack();
    stack->beginMacro(text);
    QJsonArray filters;
    bool ok = true;
    for (const RecipeStep &step : steps) {
        int index = -1;
        ok = McpFilters::applyFilter(track,
                                     clip,
                                     step.spec->id,
                                     step.params,
                                     text,
                                     error,
                                     true,
                                     &index,
                                     step.extra);
        if (!ok)
            break;
        filters.append(QJsonObject{{"id", step.spec->id}, {"index", index}});
    }
    if (!finishMacro(stack, ok))
        return textResult(error, true);
    return jsonResult(
        QJsonObject{{"filters", filters},
                    {"track", trackJson(*MAIN.timelineDock()->model(), track)}});
}

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
    // After passProducerProperties, which copies the old clip's warp_pitch.
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
                                  {"duration", frames}});
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
                 int *userIndex,
                 const std::function<void(Mlt::Properties &)> &extra)
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
        return setFilter(model,
                         track,
                         clip,
                         i,
                         *filter,
                         *spec,
                         params,
                         undoText,
                         joinMacro,
                         error,
                         extra);
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
                     userIndex,
                     extra);
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
         "List a timeline clip's user filters in their MLT (processing) order; hidden internal "
         "filters and chain links (which Shotcut's Filters panel lists first) are not included: "
         "{filters:[{index, id, service, disabled, "
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
         "step, after its existing filters, in call order (Shotcut's panel puts video before "
         "audio filters; they act on different streams, so the result is the same); the clip's "
         "parent producer gets it, as in Shotcut, so it follows the clip when it is moved or "
         "trimmed. Missing params take Shotcut's "
         "defaults; unknown names, wrong types and out-of-range values are errors. Refused: a "
         "locked track, a blank or transition, an audio filter on a clip with no audio, a "
         "picture filter on an audio-only clip or on an audio track, and a second filter of a "
         "kind that allows only one (allow_multiple false). Keyframes are not set. Does not "
         "change the selection. Returns {index, track}.",
         schema(R"({"type":"object","properties":{
             "track":{"type":"integer","minimum":0},
             "clip":{"type":"integer","minimum":0},
             "filter":{"type":"string","description":"An id from filter_list_available."},
             "params":{"type":"object","description":"Parameter values by name; )"
                R"(rect params are {x, y, w, h} in pixels."}
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
           },"required":["track","clip","gain_db"]})"),
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
           },"required":["track","clip"]})"),
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
           },"required":["track"]})"),
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
           },"required":["track","clip"]})"),
         clipDetachAudioTool},
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
         "normal; no reverse), keep_pitch (default true: the sound keeps its pitch). The clip "
         "keeps its start and its filters; its length, in and out scale by old speed / new speed "
         "(2 halves a clip, 0.5 doubles it). Nothing else moves, whatever Shotcut's ripple "
         "settings: a shorter clip leaves a gap after it, and a longer clip must fit the gap "
         "before the next clip on its track (the error names the free frames; make room first). "
         "Refused (nothing changed): unknown arguments, a locked track, a blank or transition, a "
         "clip next to a transition, a still image, generator, project or other non-file clip, a "
         "proxy clip (turn proxy editing off first), a clip playing in reverse, a clip with fades "
         "or keyframed filters (\"Remove fades and keyframes first\": Shotcut does not move "
         "keyframes either). Returns {track (as get_timeline), clip, speed, duration (the clip's "
         "new frames)}.",
         schema(R"({"type":"object","properties":{
             "track":{"type":"integer","minimum":0},
             "clip":{"type":"integer","minimum":0},
             "speed":{"type":"number","minimum":0.1,"maximum":10},
             "keep_pitch":{"type":"boolean"}
           },"required":["track","clip","speed"],"additionalProperties":false})"),
         clipSetSpeedTool},
    };
}
