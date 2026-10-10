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

#include "mcpfilters.h"
#include "mcptools.h"
#include "mcptoolsutil.h"

#include "mainwindow.h"
#include "mltcontroller.h"

using namespace McpTools;

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
    };
}
