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

#include "mcptools.h"
#include "mcptoolsutil.h"

#include "mainwindow.h"
#include "mltcontroller.h"
#include "videowidget.h"

#include <QBuffer>
#include <QEventLoop>
#include <QImage>
#include <QJsonDocument>
#include <QTimer>

#include <cmath>

using namespace McpTools;

namespace {

QJsonObject getFrameTool(const QJsonObject &arguments)
{
    int position = 0;
    const FrameArg positionArg = readFrame(arguments, "position", position);
    if (positionArg == FrameArg::Invalid)
        return textResult("position must be a non-negative integer frame number.", true);

    int maxWidth = 640;
    const QJsonValue maxWidthValue = arguments.value("max_width");
    if (!maxWidthValue.isUndefined()) {
        const double d = maxWidthValue.toDouble();
        if (!maxWidthValue.isDouble() || d != std::floor(d) || d < 64 || d > 1920)
            return textResult("max_width must be an integer from 64 to 1920.", true);
        maxWidth = static_cast<int>(d);
    }

    QString format = QStringLiteral("jpeg");
    const QJsonValue formatValue = arguments.value("format");
    if (!formatValue.isUndefined()) {
        format = formatValue.toString();
        if (!formatValue.isString() || (format != "jpeg" && format != "png"))
            return textResult("format must be \"jpeg\" or \"png\".", true);
    }

    if (!isLoaded())
        return textResult("Nothing is loaded.", true);
    auto *widget = qobject_cast<Mlt::VideoWidget *>(MLT.videoWidget());
    if (!widget)
        return textResult("There is no video widget to read a frame from.", true);

    if (positionArg == FrameArg::Ok) {
        // The display lags a seek, so ask for the next displayed frame and wait for it
        // (the way Shotcut's own Export Frame does); a timeout is an error, not a stale image.
        position = qMin(position, lastFrame());
        bool ready = false;
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(widget, &Mlt::VideoWidget::imageReady, &loop, [&]() {
            ready = true;
            loop.quit();
        });
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        movePlayhead(position);
        widget->requestImage();
        MLT.refreshConsumer();
        if (!ready) {
            timeout.start(2000);
            loop.exec();
        }
        if (!ready)
            return textResult("The frame was not ready in time; try again.", true);
    }

    QImage image = widget->image();
    if (image.isNull())
        return textResult("No frame is available yet.", true);

    // Frame pixels may not be square: correct to the display aspect ratio, then never upscale.
    const double dar = MLT.profile().dar();
    const int displayWidth = qRound(image.height() * dar);
    if (displayWidth > 0 && displayWidth != image.width())
        image = image.scaled(displayWidth, image.height(), Qt::IgnoreAspectRatio,
                             Qt::SmoothTransformation);
    if (image.width() > maxWidth)
        image = image.scaledToWidth(maxWidth, Qt::SmoothTransformation);

    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    const bool jpeg = format == "jpeg";
    const bool saved = jpeg ? image.convertToFormat(QImage::Format_RGB32).save(&buffer, "JPEG", 80)
                            : image.save(&buffer, "PNG");
    if (!saved)
        return textResult("Could not encode the frame.", true);

    const QJsonObject info{{"position", position},
                           {"width", image.width()},
                           {"height", image.height()},
                           {"format", format},
                           {"proxy", widget->imageIsProxy()}};
    return imageResult(bytes,
                       jpeg ? "image/jpeg" : "image/png",
                       QString::fromUtf8(QJsonDocument(info).toJson(QJsonDocument::Compact)));
}

} // namespace

QList<McpTool> McpTools::frameTools()
{
    return {
        {"get_frame",
         "Return an image of the video frame so you can see it. Without position it is the frame "
         "currently shown in the player (with timeline filters and transitions applied); with "
         "position it moves the playhead to that frame first (clamped to the last frame). The text "
         "item reports position, width, height, format and whether the frame came from a proxy. "
         "Images are never upscaled.",
         schema(R"({"type":"object","properties":{
             "position":{"type":"integer","minimum":0,"description":"Frame to show; moves the playhead. Omit for the current frame."},
             "max_width":{"type":"integer","minimum":64,"maximum":1920,"description":"Maximum image width in pixels, default 640."},
             "format":{"type":"string","enum":["jpeg","png"],"description":"jpeg (default, smaller) or png."}
           }})"),
         getFrameTool},
    };
}
