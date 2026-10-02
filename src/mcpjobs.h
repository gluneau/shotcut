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

#ifndef MCPJOBS_H
#define MCPJOBS_H

#include <QByteArray>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

// Background jobs of the MCP analysis tools: external programs run through QProcess, polled with
// get_job. Records outlive the project and are not persisted across restarts.
namespace McpJobs {

// The program in Shotcut's application folder, else on PATH; empty if neither has it.
QString findProgram(const QString &name);

struct Step
{
    QString program;
    QStringList args;
    // When set, the step's stdout goes to this function (on the GUI thread, in arrival order) and
    // is not buffered, so it is not subject to the 8 MiB cap; StepOutput::out is then empty.
    // Copies of the Step share the function, so keep its state in a shared_ptr.
    std::function<void(const QByteArray &)> onStdout;
    // Appended to the job's error when this step exits non-zero or crashes.
    QString failureHint;
};

struct StepOutput
{
    int exitCode;
    QByteArray out;
    QByteArray err;
};

struct Record
{
    int id = 0;
    QString kind;
    QString key;
    QString state = QStringLiteral("running"); // running | succeeded | failed | cancelled
    int percent = -1;
    QJsonObject result;
    QString error;
    bool done = false;
    QElapsedTimer timer;
    qint64 elapsedMs = 0;
};

// Runs on the GUI thread once every step exited with 0; returns false and an error to fail the job.
using Parser = std::function<bool(const QList<StepOutput> &, QJsonObject &result, QString &error)>;

// Starts the steps one after another in the background. A running record with the same kind and
// key is returned instead of starting another, so key must encode everything that affects the
// result (source, its mtime and size, parameters); reuse the cacheKeyFor() string. The result is
// cached under cacheKey when non-empty and succeeded (see cached()).
std::shared_ptr<Record> run(const QString &kind,
                            const QString &key,
                            const QList<Step> &steps,
                            Parser parse,
                            const QString &cacheKey = QString(),
                            int timeoutMs = 600000);

std::shared_ptr<Record> find(int id);

// A cached result; it gains "cached": true.
bool cached(const QString &cacheKey, QJsonObject &result);

// Runs a local event loop until the record is done or waitMs (at most 20000) passed; true if done.
bool waitFor(const std::shared_ptr<Record> &record, int waitMs);

// {job_id, kind, state, percent?, result?, error?, elapsed_ms}
QJsonObject statusJson(const Record &record);

// path|mtime|size|tool|params, so a changed file misses the cache.
QString cacheKeyFor(const QString &path, const QString &tool, const QString &params);

} // namespace McpJobs

#endif // MCPJOBS_H
