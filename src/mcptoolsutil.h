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

#include <QJsonObject>
#include <QString>

namespace Mlt {
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

} // namespace McpTools

#endif // MCPTOOLSUTIL_H
