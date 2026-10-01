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

#include "mcptoolsutil.h"

#include "mainwindow.h"
#include "mltcontroller.h"

#include <QJsonDocument>

#include <cmath>
#include <limits>

namespace McpTools {

QJsonObject schema(const char *json)
{
    return QJsonDocument::fromJson(json).object();
}

// Qt resource paths (":/...") are accepted by QDir::isAbsolutePath but are not files.
bool isResourcePath(const QString &path)
{
    return path.startsWith(':');
}

bool isLoaded()
{
    return MLT.producer() && MLT.producer()->is_valid();
}

// Reads a non-negative integer frame number from `arguments[key]`.
FrameArg readFrame(const QJsonObject &arguments, const char *key, int &value)
{
    const QJsonValue v = arguments.value(key);
    if (v.isUndefined())
        return FrameArg::Missing;
    const double d = v.toDouble();
    if (!v.isDouble() || d != std::floor(d) || d < 0
        || d > std::numeric_limits<int>::max()) {
        return FrameArg::Invalid;
    }
    value = static_cast<int>(d);
    return FrameArg::Ok;
}

// The multitrack when a timeline is open, otherwise null.
Mlt::Producer *timeline()
{
    return MAIN.isMultitrackValid() ? MAIN.multitrack() : nullptr;
}

// seekTimeline() switches the player to the multitrack, so clamp against its length then.
int lastFrame()
{
    Mlt::Producer *tl = timeline();
    Mlt::Producer *producer = tl ? tl : MLT.producer();
    return producer ? qMax(0, producer->get_playtime() - 1) : 0;
}

void movePlayhead(int position)
{
    if (timeline())
        MAIN.seekTimeline(position);
    else
        MLT.seek(position);
}

} // namespace McpTools
