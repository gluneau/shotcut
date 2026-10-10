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

#ifndef MCPFILTERS_H
#define MCPFILTERS_H

#include <QJsonObject>
#include <QList>
#include <QRectF>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

namespace Mlt {
class ClipInfo;
class Filter;
class Producer;
class Properties;
} // namespace Mlt
class MultitrackModel;
class QUndoCommand;

// The filter allowlist of the MCP filter tools and helpers for the recipe, title, caption and
// reframe tools built on it (implemented in mcptools_filters.cpp; the audio and picture recipes
// live in mcptools_audio.cpp and mcptools_picture.cpp).
namespace McpFilters {

struct FilterParam
{
    enum Type { Number, Integer, Color, Rect };
    enum MaxFrom { FixedMax, ProfileWidth, ProfileHeight };

    QString name;     // tool-facing name
    QString property; // MLT property
    Type type = Number;
    double min = 0, max = 0, def = 0;
    QString unit;
    // MLT value = tool value * scale (saturation multiplier, temperature in kelvin).
    double scale = 1.0;
    MaxFrom maxFrom = FixedMax;
    QString defText; // Color default
    // Rect default "fit": Size, Position & Rotate's formula (video mode SAR, size not rounded)
    // or Blur: Pad's (no SAR, size rounded).
    enum Fit { AffineFit, PillarEchoFit };
    Fit fit = AffineFit;
};

struct FilterSpec
{
    QString id;        // tool-facing, e.g. "gain"
    QString service;   // mlt_service
    QString shotcutId; // the QML objectName written to shotcut:filter; empty when Shotcut has none
    QString label;     // Shotcut's name of the filter
    bool audio = false;
    bool clipOnly = false;
    bool allowMultiple = true;
    QList<FilterParam> params;
    // Fixed extras the Shotcut UI sets on a new filter (rgb_only, threads, ...).
    std::function<void(Mlt::Properties &)> setup;
    // Checks a complete set of MLT values (defaults or current values plus the change); returns
    // an error message or an empty string.
    std::function<QString(Mlt::Properties &)> check;
};

// The whole allowlist, including rows whose service this MLT lacks.
const QList<FilterSpec> &allSpecs();

// The allowlist row with this id when its service is available (in MLT, not GPU-only); else null.
const FilterSpec *findSpec(const QString &id);

// The allowlist row an attached filter was made from (by shotcut:filter, else mlt_service, as
// Shotcut identifies filters), or null.
const FilterSpec *specOf(Mlt::Filter &filter);

// Applies filterId to the timeline clip (track, clip): adds it with params (defaults for the
// missing ones) when the clip has none of that filter, otherwise sets the given params on the
// first one. Validates the clip like filter_add (unlocked track, a clip, audio/picture match) and
// the params like filter_add/filter_set. Without joinMacro the change is ONE undo step named
// undoText; with joinMacro the commands join the macro the caller has open, and on false the
// caller must roll its macro back (McpTools::finishMacro(stack, false)). On success userIndex (if
// given) receives the filter's index in filter_list_applied. extra (if given) may set further MLT
// properties after the params are written (the picture recipes' Fit mode of Size/Position).
bool applyFilter(int track,
                 int clip,
                 const QString &filterId,
                 const QJsonObject &params,
                 const QString &undoText,
                 QString &error,
                 bool joinMacro = false,
                 int *userIndex = nullptr,
                 const std::function<void(Mlt::Properties &)> &extra = {});

// Adds a filter of service (shotcut:filter = shotcutId when not empty) with props to the
// multitrack output (McpFilter::Target{-1, -1}), appended after the existing output filters,
// with no in and out (it covers the whole timeline). Undo and joinMacro as applyFilter.
// Not used by any tool yet (the caption burn-in will be the first).
bool addOutputFilter(const QString &service,
                     const QString &shotcutId,
                     Mlt::Properties &props,
                     const QString &undoText,
                     QString &error,
                     bool joinMacro = false,
                     int *userIndex = nullptr);

// ---- Helpers shared by the filter, audio and picture tools (mcptools_filters.cpp)

// Largest rect coordinate (pixels) a tool accepts.
constexpr double kMaxRectValue = 100000;

// A number as the tools print it (up to 12 significant digits).
QString num(double v);

// The fit rectangle of Shotcut's Size/Position (SizePositionUI.qml:316-328: the video mode's SAR,
// size not rounded) and Blur: Pad (pillar_echo/ui.qml:29-37: no SAR, size rounded) UIs: the
// clip's picture scaled into the video mode and centered.
QRectF fitRect(Mlt::Producer *clip, FilterParam::Fit fit);

// Whether the filter's property holds keyframes (MLT's "frame=value" syntax).
bool isKeyframed(Mlt::Filter &filter, const QString &property);

// filter_add's clip rules: an unlocked track, a clip, and audio/picture that fit the clip.
std::unique_ptr<Mlt::ClipInfo> checkClip(
    MultitrackModel &model, int track, int clip, const FilterSpec &spec, QString &error);

// Pushes command as one undo step named text (or into the caller's open macro), after pausing.
// ok tells whether the command did its work; a failed step is rolled back.
bool pushCommand(QUndoCommand *command,
                 const QString &text,
                 bool joinMacro,
                 const std::function<bool()> &ok,
                 QString &error);

// Adds a new filter of spec (defaults for the params not given, then extra) to the clip described
// by info at (track, clip), through pushCommand; userIndex receives its filter_list_applied index.
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
               const std::function<void(Mlt::Properties &)> &extra = {});

// Sets params (then extra) on the clip's existing filter at userIndex, through pushCommand.
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
               const std::function<void(Mlt::Properties &)> &extra = {});

// Refuses argument names outside allowed (a misspelt value must not be ignored silently).
bool checkKeys(const QJsonObject &arguments, const QStringList &allowed, QString &error);

// The allowlist row id when it can be used, else null with the reason.
const FilterSpec *recipeSpec(const QString &id, QString &error);

struct RecipeStep
{
    const FilterSpec *spec = nullptr;
    QJsonObject params;
    std::function<void(Mlt::Properties &)> extra;
};

// Applies every step to the clip (set on the first filter of its kind, else added) as ONE undo
// step named text; any failure rolls the whole call back. Returns {filters:[{id, index}], track}.
QJsonObject runRecipe(int track, int clip, const QList<RecipeStep> &steps, const QString &text);

} // namespace McpFilters

#endif // MCPFILTERS_H
