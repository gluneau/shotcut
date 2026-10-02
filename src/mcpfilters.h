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
#include <QString>

#include <functional>

namespace Mlt {
class Filter;
class Producer;
class Properties;
} // namespace Mlt

// The filter allowlist of the MCP filter tools and helpers for the recipe, title, caption and
// reframe tools built on it (implemented in mcptools_filters.cpp).
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
// given) receives the filter's index in filter_list_applied.
bool applyFilter(int track,
                 int clip,
                 const QString &filterId,
                 const QJsonObject &params,
                 const QString &undoText,
                 QString &error,
                 bool joinMacro = false,
                 int *userIndex = nullptr);

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

} // namespace McpFilters

#endif // MCPFILTERS_H
