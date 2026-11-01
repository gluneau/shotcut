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
// ClipInfo. The clip's producer gets the "clip:track" item property that
// MultitrackModel::filterAddedOrRemoved/onFilterChanged read to know which timeline item to
// refresh (TimelineDock::emitSelectedFromSelection sets it the same way). It is written afresh at
// every use; a stale value left after later edits is harmless, because it only selects which
// item's FadeIn/FadeOut/IsFiltered/Gain roles get a dataChanged signal (it is not saved: the "_"
// prefix keeps it out of the XML).
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

// The MLT index where a user filter goes to become user filter userIndex: the index of the
// current one there, else after the last user filter, else before the first hidden post filter
// (such as a track's level meter), else the end. AttachedFiltersModel's userFilterInsertIndex.
int insertIndex(Mlt::Service &service, int userIndex)
{
    int seen = 0;
    int lastUser = -1;
    for (int i = 0; i < service.filter_count(); ++i) {
        std::unique_ptr<Mlt::Filter> filter(service.filter(i));
        if (!Util::isUserFilter(filter.get()))
            continue;
        if (seen == userIndex)
            return i;
        lastUser = i;
        ++seen;
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

// Attaches filter (appended by MLT) and moves it to be user filter userIndex; returns its user
// index or -1.
int attachAt(Mlt::Producer &producer, Mlt::Filter &filter, int userIndex)
{
    const int mltIndex = insertIndex(producer, userIndex);
    if (producer.attach(filter) != 0)
        return -1;
    const int last = producer.filter_count() - 1;
    if (mltIndex < last)
        producer.move_filter(last, mltIndex);
    return userFilterIndices(producer).indexOf(qMin(mltIndex, last));
}

// The user filter at userIndex when it is the expected one (an empty service accepts any).
std::unique_ptr<Mlt::Filter> expectedFilter(Mlt::Producer &producer,
                                            int userIndex,
                                            const QByteArray &service,
                                            const QByteArray &shotcutId,
                                            const QString &what)
{
    const QList<int> indices = userFilterIndices(producer);
    if (userIndex < 0 || userIndex >= indices.size()) {
        LOG_ERROR() << what << "no filter" << userIndex << "(count" << indices.size() << ")";
        return nullptr;
    }
    std::unique_ptr<Mlt::Filter> filter(producer.filter(indices.at(userIndex)));
    if (!filter || !filter->is_valid()) {
        LOG_ERROR() << what << "invalid filter" << userIndex;
        return nullptr;
    }
    if (!service.isEmpty()
        && (service != filter->get("mlt_service")
            || shotcutId != QByteArray(filter->get(kShotcutFilterProperty)))) {
        LOG_ERROR() << what << "filter" << userIndex << "is" << filter->get("mlt_service")
                    << filter->get(kShotcutFilterProperty) << "not" << service << shotcutId;
        return nullptr;
    }
    return filter;
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
    if (m_attached)
        return;
    std::unique_ptr<Mlt::ClipInfo> info;
    Mlt::Producer producer = resolve(m_model, m_target, &info);
    if (!producer.is_valid()) {
        LOG_ERROR() << text() << "target not found:" << describe(m_target);
        return;
    }
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
    // The first redo appends; later ones put it back where it was.
    const int userIndex = m_userIndex >= 0 ? m_userIndex
                                           : int(userFilterIndices(producer).size());
    const int added = attachAt(producer, filter, userIndex);
    if (added < 0) {
        LOG_ERROR() << text() << "could not attach the filter to" << describe(m_target);
        return;
    }
    m_userIndex = added;
    m_attached = true;
    notifyAddedOrRemoved(m_model, producer);
}

void AddCommand::undo()
{
    // m_attached is cleared only once the filter is detached: after a failed undo the next redo
    // must be a no-op, not attach a second copy.
    if (!m_attached)
        return;
    Mlt::Producer producer = resolve(m_model, m_target);
    if (!producer.is_valid()) {
        LOG_ERROR() << text() << "target not found:" << describe(m_target);
        return;
    }
    std::unique_ptr<Mlt::Filter> filter = expectedFilter(producer,
                                                         m_userIndex,
                                                         m_service.toUtf8(),
                                                         m_shotcutId.toUtf8(),
                                                         text());
    if (!filter)
        return;
    if (producer.detach(*filter) != 0) {
        LOG_ERROR() << text() << "could not detach the filter from" << describe(m_target);
        return;
    }
    m_attached = false;
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
    , m_service(before.get("mlt_service"))
    , m_shotcutId(before.get(kShotcutFilterProperty))
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
    std::unique_ptr<Mlt::Filter> filter
        = expectedFilter(producer, m_userIndex, m_service, m_shotcutId, text());
    if (!filter)
        return;
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
    if (m_removed)
        return;
    Mlt::Producer producer = resolve(m_model, m_target);
    if (!producer.is_valid()) {
        LOG_ERROR() << text() << "target not found:" << describe(m_target);
        return;
    }
    // The first redo takes whatever filter is at the index; later ones check it is the same kind.
    std::unique_ptr<Mlt::Filter> filter
        = expectedFilter(producer, m_userIndex, m_service, m_shotcutId, text());
    if (!filter)
        return;
    if (producer.detach(*filter) != 0) {
        LOG_ERROR() << text() << "could not detach the filter from" << describe(m_target);
        return;
    }
    m_service = filter->get("mlt_service");
    m_shotcutId = filter->get(kShotcutFilterProperty);
    // The detached filter keeps all its properties for undo.
    m_filter = *filter;
    m_removed = true;
    notifyAddedOrRemoved(m_model, producer);
}

void RemoveCommand::undo()
{
    // m_removed and m_filter are cleared only once the filter is back: after a failed undo the
    // next redo is a no-op and the detached filter is still held.
    if (!m_removed || !m_filter.is_valid())
        return;
    Mlt::Producer producer = resolve(m_model, m_target);
    if (!producer.is_valid()) {
        LOG_ERROR() << text() << "target not found:" << describe(m_target);
        return;
    }
    const int attached = attachAt(producer, m_filter, m_userIndex);
    if (attached < 0) {
        LOG_ERROR() << text() << "could not re-attach the filter to" << describe(m_target);
        return;
    }
    if (attached != m_userIndex)
        LOG_ERROR() << text() << "re-attached the filter to" << describe(m_target) << "at"
                    << attached << "not" << m_userIndex;
    // Redo finds the filter by index again; no handle is kept while it is attached.
    m_filter = Mlt::Filter();
    m_removed = false;
    notifyAddedOrRemoved(m_model, producer);
}

} // namespace McpFilter
