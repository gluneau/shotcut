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

#include "mcpjobs.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QEventLoop>
#include <QFileInfo>
#include <QHash>
#include <QPointer>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

#include <vector>

namespace McpJobs {

namespace {

constexpr qsizetype kStreamCap = 8 * 1024 * 1024;
constexpr size_t kMaxRecords = 200;
constexpr int kMaxCached = 200;
constexpr int kMaxWaitMs = 20000;

// The running state of one job; the lambdas connected to its process and timer keep it alive.
struct Runner
{
    std::shared_ptr<Record> record;
    QList<Step> steps;
    Parser parse;
    QString cacheKey;
    int index = 0;
    QList<StepOutput> outputs;
    QByteArray out;
    QByteArray err;
    QPointer<QProcess> process;
    QPointer<QTimer> timer;
};

// Function-static so records survive the project being closed.
std::vector<std::shared_ptr<Record>> &records()
{
    static std::vector<std::shared_ptr<Record>> list;
    return list;
}

struct Cache
{
    QHash<QString, QJsonObject> results;
    QStringList order;
};

Cache &cache()
{
    static Cache c;
    return c;
}

void store(const QString &key, const QJsonObject &result)
{
    Cache &c = cache();
    if (!c.results.contains(key)) {
        c.order.append(key);
        if (c.order.size() > kMaxCached)
            c.results.remove(c.order.takeFirst());
    }
    c.results.insert(key, result);
}

// stdout keeps its first bytes (parsers read from the start); stderr its last (errors end it).
void appendCapped(QByteArray &buffer, const QByteArray &data, bool keepTail)
{
    if (keepTail) {
        buffer += data;
        if (buffer.size() > kStreamCap)
            buffer.remove(0, buffer.size() - kStreamCap);
    } else if (buffer.size() < kStreamCap) {
        buffer += data.left(kStreamCap - buffer.size());
    }
}

// Idempotent: a timeout, a start failure and a process exit may all try to end the job.
void finish(const std::shared_ptr<Runner> &runner,
            const QString &state,
            const QString &error,
            const QJsonObject &result = QJsonObject())
{
    Record &record = *runner->record;
    if (record.done)
        return;
    record.done = true;
    record.elapsedMs = record.timer.elapsed();
    record.state = state;
    record.error = error;
    record.result = result;
    if (state == "succeeded") {
        record.percent = 100;
        if (!runner->cacheKey.isEmpty())
            store(runner->cacheKey, result);
    }
    if (runner->timer) {
        runner->timer->stop();
        runner->timer->deleteLater();
    }
    if (runner->process && runner->process->state() != QProcess::NotRunning)
        runner->process->kill(); // its finished handler deletes it
}

void startStep(const std::shared_ptr<Runner> &runner)
{
    const Step step = runner->steps.at(runner->index);
    runner->out.clear();
    runner->err.clear();
    auto *process = new QProcess(qApp);
    runner->process = process;
    process->setProcessChannelMode(QProcess::SeparateChannels);
    process->setStandardInputFile(QProcess::nullDevice());
    QObject::connect(process, &QProcess::readyReadStandardOutput, process, [runner, process] {
        appendCapped(runner->out, process->readAllStandardOutput(), false);
    });
    QObject::connect(process, &QProcess::readyReadStandardError, process, [runner, process] {
        appendCapped(runner->err, process->readAllStandardError(), true);
    });
    // A process that cannot start never emits finished.
    QObject::connect(process,
                     &QProcess::errorOccurred,
                     process,
                     [runner, process, step](QProcess::ProcessError e) {
                         if (e != QProcess::FailedToStart)
                             return;
                         process->deleteLater();
                         finish(runner,
                                "failed",
                                QStringLiteral("Could not start %1: %2")
                                    .arg(step.program, process->errorString()));
                     });
    QObject::connect(process,
                     &QProcess::finished,
                     process,
                     [runner, process, step](int exitCode, QProcess::ExitStatus status) {
                         process->deleteLater();
                         if (runner->record->done)
                             return;
                         appendCapped(runner->out, process->readAllStandardOutput(), false);
                         appendCapped(runner->err, process->readAllStandardError(), true);
                         const QString name = QFileInfo(step.program).fileName();
                         if (status != QProcess::NormalExit || exitCode != 0) {
                             QString error = status != QProcess::NormalExit
                                                 ? QStringLiteral("%1 crashed.").arg(name)
                                                 : QStringLiteral("%1 exited with code %2.")
                                                       .arg(name)
                                                       .arg(exitCode);
                             const QString tail = QString::fromUtf8(runner->err.right(2048))
                                                      .trimmed();
                             if (!tail.isEmpty())
                                 error += " " + tail;
                             finish(runner, "failed", error);
                             return;
                         }
                         runner->outputs.append(StepOutput{exitCode, runner->out, runner->err});
                         if (++runner->index < runner->steps.size()) {
                             startStep(runner);
                             return;
                         }
                         QJsonObject result;
                         QString error;
                         if (!runner->parse || runner->parse(runner->outputs, result, error))
                             finish(runner, "succeeded", QString(), result);
                         else
                             finish(runner,
                                    "failed",
                                    error.isEmpty() ? QStringLiteral("The output of %1 could not "
                                                                     "be read.")
                                                          .arg(name)
                                                    : error);
                     });
    process->start(step.program, step.args);
}

} // namespace

QString findProgram(const QString &name)
{
    const QString local = QStandardPaths::findExecutable(name,
                                                         {QCoreApplication::applicationDirPath()});
    return local.isEmpty() ? QStandardPaths::findExecutable(name) : local;
}

std::shared_ptr<Record> run(const QString &kind,
                            const QString &key,
                            const QList<Step> &steps,
                            Parser parse,
                            const QString &cacheKey,
                            int timeoutMs)
{
    auto &list = records();
    if (!key.isEmpty()) {
        for (const auto &record : list) {
            if (!record->done && record->kind == kind && record->key == key)
                return record;
        }
    }
    for (auto it = list.begin(); it != list.end() && list.size() >= kMaxRecords;) {
        if ((*it)->done)
            it = list.erase(it);
        else
            ++it;
    }

    static int nextId = 1;
    auto record = std::make_shared<Record>();
    record->id = nextId++;
    record->kind = kind;
    record->key = key;
    record->timer.start();
    list.push_back(record);
    if (steps.isEmpty()) {
        record->done = true;
        record->state = QStringLiteral("failed");
        record->error = QStringLiteral("The job has no steps.");
        return record;
    }

    auto runner = std::make_shared<Runner>();
    runner->record = record;
    runner->steps = steps;
    runner->parse = std::move(parse);
    runner->cacheKey = cacheKey;
    auto *timer = new QTimer(qApp);
    timer->setSingleShot(true);
    runner->timer = timer;
    QObject::connect(timer, &QTimer::timeout, timer, [runner, timeoutMs] {
        finish(runner,
               "failed",
               QStringLiteral("timed out after %1 s").arg(timeoutMs / 1000.0, 0, 'f', 1));
    });
    timer->start(timeoutMs);
    startStep(runner);
    return record;
}

std::shared_ptr<Record> find(int id)
{
    for (const auto &record : records()) {
        if (record->id == id)
            return record;
    }
    return nullptr;
}

bool cached(const QString &cacheKey, QJsonObject &result)
{
    if (cacheKey.isEmpty())
        return false;
    const auto it = cache().results.constFind(cacheKey);
    if (it == cache().results.constEnd())
        return false;
    result = it.value();
    result["cached"] = true;
    return true;
}

bool waitFor(const std::shared_ptr<Record> &record, int waitMs)
{
    waitMs = qMin(waitMs, kMaxWaitMs);
    if (!record || record->done || waitMs <= 0)
        return record && record->done;
    QEventLoop loop;
    QTimer poll;
    QElapsedTimer elapsed;
    elapsed.start();
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (record->done || elapsed.elapsed() >= waitMs)
            loop.quit();
    });
    poll.start(50);
    loop.exec();
    return record->done;
}

QJsonObject statusJson(const Record &record)
{
    QJsonObject o{{"job_id", record.id},
                  {"kind", record.kind},
                  {"state", record.state},
                  {"elapsed_ms",
                   static_cast<double>(record.done ? record.elapsedMs : record.timer.elapsed())}};
    if (record.percent >= 0)
        o["percent"] = record.percent;
    if (record.done && record.state == "succeeded")
        o["result"] = record.result;
    if (!record.error.isEmpty())
        o["error"] = record.error;
    return o;
}

QString cacheKeyFor(const QString &path, const QString &tool, const QString &params)
{
    const QFileInfo info(path);
    return QStringList{info.absoluteFilePath(),
                       QString::number(info.lastModified().toMSecsSinceEpoch()),
                       QString::number(info.size()),
                       tool,
                       params}
        .join('|');
}

} // namespace McpJobs
