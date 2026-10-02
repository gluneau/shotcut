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

#include "mcpfiltercommands.h"

#include "Logger.h"
#include "controllers/filtercontroller.h"
#include "mainwindow.h"
#include "mltcontroller.h"
#include "models/multitrackmodel.h"
#include "shotcut_mlt_properties.h"
#include "util.h"

#include <MltPlaylist.h>

#include <memory>

namespace McpFilter {

namespace {

// The target's producer as it is now; invalid when it is gone. For a clip, info receives its
// ClipInfo, and the producer gets the "clip:track" item property that the timeline model needs
// to report filter changes on the clip (TimelineDock::emitSelectedFromSelection sets it too).
Mlt::Producer resolve(MultitrackModel &model,
                      const Target &target,
                      std::unique_ptr<Mlt::ClipInfo> *info = nullptr)
{
    if (target.track < 0) {
        Mlt::Tractor *tractor = model.tractor();
        if (!tractor || !tractor->is_valid())
            return Mlt::Producer();
        return Mlt::Producer(*tractor);
    }
    std::unique_ptr<Mlt::ClipInfo> clipInfo = model.getClipInfo(target.track, target.clip);
    if (!clipInfo || !clipInfo->producer || !clipInfo->producer->is_valid() || !clipInfo->cut
        || clipInfo->cut->is_blank() || clipInfo->producer->get(kShotcutTransitionProperty))
        return Mlt::Producer();
    Mlt::Producer producer(*clipInfo->producer);
    producer.set(kMultitrackItemProperty,
                 QStringLiteral("%1:%2").arg(target.clip).arg(target.track).toLatin1().constData());
    if (info)
        *info = std::move(clipInfo);
    return producer;
}

// Where an appended user filter belongs: after the last user filter, else before the first
// hidden post filter (such as a track's level meter), else at the end. This is
// AttachedFiltersModel's rule for adding a row at the end.
int appendIndex(Mlt::Service &service)
{
    int lastUser = -1;
    for (int i = 0; i < service.filter_count(); ++i) {
        std::unique_ptr<Mlt::Filter> filter(service.filter(i));
        if (Util::isUserFilter(filter.get()))
            lastUser = i;
    }
    if (lastUser >= 0)
        return lastUser + 1;
    for (int i = 0; i < service.filter_count(); ++i) {
        std::unique_ptr<Mlt::Filter> filter(service.filter(i));
        if (Util::isPostUserFilter(filter.get()))
            return i;
    }
    return service.filter_count();
}

// Attaches filter (appended by MLT) and moves it to mltIndex.
bool attachAt(Mlt::Producer &producer, Mlt::Filter &filter, int mltIndex)
{
    if (producer.attach(filter) != 0)
        return false;
    const int last = producer.filter_count() - 1;
    if (mltIndex >= 0 && mltIndex < last)
        producer.move_filter(last, mltIndex);
    return true;
}

void notifyAddedOrRemoved(MultitrackModel &model, Mlt::Producer &producer)
{
    model.filterAddedOrRemoved(&producer);
    MAIN.setWindowModified(true);
    MLT.refreshConsumer();
}

QString describe(const Target &target)
{
    if (target.track < 0)
        return QStringLiteral("the multitrack output");
    return QStringLiteral("clip %1 on track %2").arg(target.clip).arg(target.track);
}

} // namespace

QList<int> userFilterIndices(Mlt::Service &service)
{
    QList<int> indices;
    if (!service.is_valid())
        return indices;
    for (int i = 0; i < service.filter_count(); ++i) {
        std::unique_ptr<Mlt::Filter> filter(service.filter(i));
        if (Util::isUserFilter(filter.get()))
            indices.append(i);
    }
    return indices;
}

AddCommand::AddCommand(MultitrackModel &model,
                       Target target,
                       const QString &text,
                       const QString &service,
                       const QString &shotcutId,
                       Mlt::Properties &props)
    : m_model(model)
    , m_target(target)
    , m_service(service)
    , m_shotcutId(shotcutId)
{
    setText(text);
    m_props.inherit(props);
}

void AddCommand::redo()
{
    m_userIndex = -1;
    std::unique_ptr<Mlt::ClipInfo> info;
    Mlt::Producer producer = resolve(m_model, m_target, &info);
    if (!producer.is_valid()) {
        LOG_ERROR() << text() << "target not found:" << describe(m_target);
        return;
    }
    if (!m_filter.is_valid()) {
        Mlt::Filter filter(MLT.profile(), m_service.toUtf8().constData());
        if (!filter.is_valid()) {
            LOG_ERROR() << text() << "could not create the filter" << m_service;
            return;
        }
        filter.inherit(m_props);
        if (!m_shotcutId.isEmpty())
            filter.set(kShotcutFilterProperty, m_shotcutId.toUtf8().constData());
        if (info)
            filter.set_in_and_out(info->frame_in, info->frame_out);
        m_filter = filter;
    }
    const int mltIndex = appendIndex(producer);
    if (!attachAt(producer, m_filter, mltIndex)) {
        LOG_ERROR() << text() << "could not attach the filter to" << describe(m_target);
        return;
    }
    m_userIndex = userFilterIndices(producer).indexOf(mltIndex);
    notifyAddedOrRemoved(m_model, producer);
}

void AddCommand::undo()
{
    if (m_userIndex < 0 || !m_filter.is_valid())
        return;
    m_userIndex = -1;
    Mlt::Producer producer = resolve(m_model, m_target);
    if (!producer.is_valid()) {
        LOG_ERROR() << text() << "target not found:" << describe(m_target);
        return;
    }
    if (producer.detach(m_filter) != 0) {
        LOG_ERROR() << text() << "the filter is no longer on" << describe(m_target);
        return;
    }
    notifyAddedOrRemoved(m_model, producer);
}

SetCommand::SetCommand(MultitrackModel &model,
                       Target target,
                       int userIndex,
                       const QString &text,
                       Mlt::Properties &before,
                       Mlt::Properties &after)
    : m_model(model)
    , m_target(target)
    , m_userIndex(userIndex)
{
    setText(text);
    m_before.inherit(before);
    m_after.inherit(after);
}

void SetCommand::redo()
{
    apply(m_after);
}

void SetCommand::undo()
{
    apply(m_before);
}

void SetCommand::apply(Mlt::Properties &properties)
{
    m_applied = false;
    Mlt::Producer producer = resolve(m_model, m_target);
    if (!producer.is_valid()) {
        LOG_ERROR() << text() << "target not found:" << describe(m_target);
        return;
    }
    const QList<int> indices = userFilterIndices(producer);
    if (m_userIndex < 0 || m_userIndex >= indices.size()) {
        LOG_ERROR() << text() << "no filter" << m_userIndex << "on" << describe(m_target);
        return;
    }
    std::unique_ptr<Mlt::Filter> filter(producer.filter(indices.at(m_userIndex)));
    if (!filter || !filter->is_valid()) {
        LOG_ERROR() << text() << "invalid filter" << m_userIndex << "on" << describe(m_target);
        return;
    }
    filter->inherit(properties);
    m_applied = true;
    m_model.onFilterChanged(filter.get());
    MAIN.setWindowModified(true);
    // Refreshes the preview, and the Filters panel when it shows this filter.
    MAIN.filterController()->onUndoOrRedo(*filter);
}

RemoveCommand::RemoveCommand(MultitrackModel &model,
                             Target target,
                             int userIndex,
                             const QString &text)
    : m_model(model)
    , m_target(target)
    , m_userIndex(userIndex)
{
    setText(text);
}

void RemoveCommand::redo()
{
    m_mltIndex = -1;
    Mlt::Producer producer = resolve(m_model, m_target);
    if (!producer.is_valid()) {
        LOG_ERROR() << text() << "target not found:" << describe(m_target);
        return;
    }
    const QList<int> indices = userFilterIndices(producer);
    if (m_userIndex < 0 || m_userIndex >= indices.size()) {
        LOG_ERROR() << text() << "no filter" << m_userIndex << "on" << describe(m_target);
        return;
    }
    std::unique_ptr<Mlt::Filter> filter(producer.filter(indices.at(m_userIndex)));
    if (!filter || !filter->is_valid()) {
        LOG_ERROR() << text() << "invalid filter" << m_userIndex << "on" << describe(m_target);
        return;
    }
    // Keep the detached filter itself (with all its properties) for undo.
    m_filter = *filter;
    if (producer.detach(m_filter) != 0) {
        LOG_ERROR() << text() << "could not detach the filter from" << describe(m_target);
        return;
    }
    m_mltIndex = indices.at(m_userIndex);
    notifyAddedOrRemoved(m_model, producer);
}

void RemoveCommand::undo()
{
    if (m_mltIndex < 0 || !m_filter.is_valid())
        return;
    const int mltIndex = m_mltIndex;
    m_mltIndex = -1;
    Mlt::Producer producer = resolve(m_model, m_target);
    if (!producer.is_valid()) {
        LOG_ERROR() << text() << "target not found:" << describe(m_target);
        return;
    }
    if (!attachAt(producer, m_filter, mltIndex)) {
        LOG_ERROR() << text() << "could not re-attach the filter to" << describe(m_target);
        return;
    }
    notifyAddedOrRemoved(m_model, producer);
}

} // namespace McpFilter
