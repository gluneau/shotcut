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

#include "docks/timelinedock.h"
#include "mainwindow.h"
#include "mltcontroller.h"
#include "models/multitrackmodel.h"

#include <QBuffer>
#include <QElapsedTimer>
#include <QFont>
#include <QFontMetrics>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QVector>

using namespace McpTools;

namespace {

// Every tile waits at most this long for its frame, and the whole sheet at most the budget, so a
// call stays well below a client's usual 60 s request timeout.
constexpr int kOverviewTileTimeoutMs = 3000;
constexpr int kOverviewBudgetMs = 45000;

bool readCount(
    const QJsonObject &arguments, const char *key, int min, int max, int &value, QString &error)
{
    if (readFrame(arguments, key, value) == FrameArg::Invalid || value < min || value > max) {
        error = QStringLiteral("%1 must be an integer from %2 to %3.").arg(key).arg(min).arg(max);
        return false;
    }
    return true;
}

// H:MM:SS:FF counting whole frames per second (non-drop-frame for fractional rates).
QString timecode(int frame, double fps)
{
    const int rate = qMax(1, qRound(fps));
    const int seconds = frame / rate;
    return QStringLiteral("%1:%2:%3:%4")
        .arg(seconds / 3600)
        .arg(seconds / 60 % 60, 2, 10, QChar('0'))
        .arg(seconds % 60, 2, 10, QChar('0'))
        .arg(frame % rate, 2, 10, QChar('0'));
}

constexpr int kOverviewMaxSide = 4096;
constexpr int kLabelMinPx = 8;

struct OverviewLabel
{
    int pixelSize;
    bool withTimecode;
};

// The tile label font: sized from both tile sides, then shrunk until the timecode, a gap and the
// frame number fit across the tile; below the floor only the frame number is drawn.
OverviewLabel overviewLabel(QFont font,
                            int tileWidth,
                            int tileHeight,
                            const QString &timecodeText,
                            const QString &numberText)
{
    const int start = qBound(10, qMin(tileHeight / 12, tileWidth / 24), 32);
    for (int size = start; size >= kLabelMinPx; --size) {
        font.setPixelSize(size);
        const QFontMetrics metrics(font);
        const int needed = metrics.horizontalAdvance(timecodeText)
                           + metrics.horizontalAdvance(numberText) + size + 8;
        if (needed <= tileWidth)
            return {size, true};
    }
    return {start, false};
}

QJsonObject getOverviewTool(const QJsonObject &arguments)
{
    QString error;
    int columns = 4;
    int rows = 4;
    int tileWidth = 320;
    if (!readCount(arguments, "columns", 1, 8, columns, error)
        || !readCount(arguments, "rows", 1, 8, rows, error)
        || !readCount(arguments, "tile_width", 96, 640, tileWidth, error))
        return textResult(error, true);
    QString format = QStringLiteral("jpeg");
    const QJsonValue formatValue = arguments.value("format");
    if (!formatValue.isUndefined()) {
        format = formatValue.toString();
        if (!formatValue.isString() || (format != "jpeg" && format != "png"))
            return textResult("format must be \"jpeg\" or \"png\".", true);
    }
    Mlt::Producer *tl = timeline();
    if (!tl)
        return textResult("There is no timeline; add clips with timeline_add_playlist first.",
                          true);
    // A timeline whose clips were all removed keeps a one-frame blank.
    MultitrackModel &model = *MAIN.timelineDock()->model();
    bool empty = true;
    for (int t = 0; t < model.trackList().size() && empty; ++t) {
        for (int c = 0; c < model.rowCount(model.index(t)) && empty; ++c)
            empty = kindOf(model, t, c) == "blank";
    }
    if (empty || tl->get_playtime() <= 0)
        return textResult("The timeline is empty.", true);
    if (exportBusy())
        return textResult("An export is running; get_overview uses the player, so try again when "
                          "it has finished.",
                          true);

    const int last = lastFrame();
    const int count = qMin(columns * rows, last + 1);
    columns = qMin(columns, count);
    rows = (count + columns - 1) / columns;
    QVector<int> positions;
    QJsonArray positionsJson;
    for (int i = 0; i < count; ++i) {
        positions.append(count == 1 ? 0 : qRound(double(i) * last / (count - 1)));
        positionsJson.append(positions.last());
    }

    const double dar = MLT.profile().dar();
    const int gap = 2;
    auto heightOf = [dar](int width) { return qMax(1, qRound(width / dar)); };
    while (tileWidth > 96
           && (columns * tileWidth + gap * (columns - 1) > kOverviewMaxSide
               || rows * heightOf(tileWidth) + gap * (rows - 1) > kOverviewMaxSide))
        --tileWidth;
    const int tileHeight = heightOf(tileWidth);
    QImage sheet(columns * tileWidth + gap * (columns - 1),
                 rows * tileHeight + gap * (rows - 1),
                 QImage::Format_RGB32);
    sheet.fill(Qt::black);
    QPainter painter(&sheet);
    const double fps = MLT.profile().fps();
    // Sized for the last frame, whose timecode and number are the widest.
    const OverviewLabel label = overviewLabel(painter.font(),
                                              tileWidth,
                                              tileHeight,
                                              timecode(last, fps),
                                              QString::number(last));
    QFont font = painter.font();
    font.setPixelSize(label.pixelSize);
    painter.setFont(font);
    painter.setPen(Qt::white);
    const int barHeight = label.pixelSize + 8;
    const int playtime = tl->get_playtime();

    // The playhead moves for every tile; put it back, and resume playback only when the player
    // was playing the timeline (a source clip stays replaced by the timeline, as with get_frame).
    const bool onTimeline = MLT.producer()
                            && MLT.producer()->get_producer() == tl->get_producer();
    const bool wasPlaying = onTimeline && !MLT.isPaused();
    const double speed = wasPlaying ? MLT.producer()->get_speed() : 1.0;
    const int before = qMin(tl->position(), last);
    QElapsedTimer clock;
    clock.start();
    const QString budgetError = QStringLiteral("The overview took longer than %1 s; ask for "
                                               "fewer tiles.")
                                    .arg(kOverviewBudgetMs / 1000);
    const QString changedError = QStringLiteral("The timeline changed while building the "
                                                "overview.");
    auto changed = [&]() {
        Mlt::Producer *now = timeline();
        return now != tl || now->get_playtime() != playtime;
    };
    for (int i = 0; i < count && error.isEmpty(); ++i) {
        if (changed()) {
            error = changedError;
            break;
        }
        const int remaining = kOverviewBudgetMs - int(clock.elapsed());
        if (remaining <= 0) {
            error = budgetError;
            break;
        }
        const QImage frame = fetchSettledFrame(positions[i],
                                               error,
                                               qMin(kOverviewTileTimeoutMs, remaining));
        if (frame.isNull()) {
            // A tile whose wait the budget cut short is a budget failure, not a slow frame.
            error = remaining < kOverviewTileTimeoutMs
                        ? budgetError
                        : QStringLiteral("Frame %1: %2").arg(positions[i]).arg(error);
            break;
        }
        const int x = (i % columns) * (tileWidth + gap);
        const int y = (i / columns) * (tileHeight + gap);
        painter.drawImage(x,
                          y,
                          frame.scaled(tileWidth,
                                       tileHeight,
                                       Qt::IgnoreAspectRatio,
                                       Qt::SmoothTransformation));
        const QRect bar(x, y + tileHeight - barHeight, tileWidth, barHeight);
        painter.fillRect(bar, QColor(0, 0, 0, 160));
        const QRect text = bar.adjusted(4, 0, -4, 0);
        if (label.withTimecode)
            painter.drawText(text, Qt::AlignVCenter | Qt::AlignLeft, timecode(positions[i], fps));
        painter.drawText(text, Qt::AlignVCenter | Qt::AlignRight, QString::number(positions[i]));
    }
    painter.end();
    if (error.isEmpty() && changed())
        error = changedError;
    if (timeline()) {
        const int restore = qMin(before, lastFrame());
        if (wasPlaying) {
            // Resuming at once lets a frame of the last tile that is still in flight be shown at
            // playing speed; at the last frame Player then ends playback (paused one past the
            // end). Wait until the restored frame is shown, so those frames have drained.
            QString ignored;
            fetchSettledFrame(restore, ignored);
            MLT.play(speed);
        } else {
            movePlayhead(restore);
        }
    }
    if (!error.isEmpty())
        return textResult(error, true);

    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    const bool jpeg = format == "jpeg";
    const bool saved = jpeg ? sheet.save(&buffer, "JPEG", 80) : sheet.save(&buffer, "PNG");
    if (!saved)
        return textResult("Could not encode the overview.", true);
    const QJsonObject info{{"columns", columns},
                           {"rows", rows},
                           {"tile_width", tileWidth},
                           {"positions", positionsJson}};
    return imageResult(bytes,
                       jpeg ? "image/jpeg" : "image/png",
                       QString::fromUtf8(QJsonDocument(info).toJson(QJsonDocument::Compact)));
}

} // namespace

QList<McpTool> McpTools::overviewTools()
{
    return {
        {"get_overview",
         "One image of the whole timeline (a contact sheet) so you can see it at a glance: "
         "columns*rows frames evenly spaced from frame 0 to the last frame, left to right then "
         "top to bottom, each labelled with its timecode (H:MM:SS:FF) and frame number, tiles "
         "separated by 2 px. columns and rows (1 to 8, default 4), tile_width (96 to 640, "
         "default 320; the tile height follows the display aspect ratio), format jpeg (default) "
         "or png. A timeline with fewer frames than tiles gets one tile per frame and fewer "
         "columns or rows, and tile_width is reduced (not below 96) so that neither side of the "
         "image exceeds 4096 px. Narrow tiles show only the frame number. Returns the image and "
         "{columns, rows, tile_width, positions} with the columns, rows and tile_width actually "
         "used and the frame of every tile. Each tile is read like "
         "get_frame with a position (with timeline filters and transitions), so the call takes a "
         "few seconds: at most 3 s per tile and 45 s in all, else an error. The playhead is put "
         "back afterwards and playback of the timeline resumes; a source clip shown in the "
         "player is replaced by the timeline. Refused when there is no timeline, the timeline "
         "has no clips or an export started with export_video runs (an export started in the "
         "Shotcut window is not detected), and stopped with an error if the timeline changes "
         "meanwhile.",
         schema(R"({"type":"object","properties":{
             "columns":{"type":"integer","minimum":1,"maximum":8,"default":4},
             "rows":{"type":"integer","minimum":1,"maximum":8,"default":4},
             "tile_width":{"type":"integer","minimum":96,"maximum":640,"default":320},
             "format":{"type":"string","enum":["jpeg","png"],"default":"jpeg"}
           }})"),
         getOverviewTool},
    };
}
