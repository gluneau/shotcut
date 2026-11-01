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

#ifndef MCPFILTERCOMMANDS_H
#define MCPFILTERCOMMANDS_H

#include <MltFilter.h>
#include <MltProperties.h>
#include <QByteArray>
#include <QList>
#include <QString>
#include <QUndoCommand>

class MultitrackModel;

namespace Mlt {
class Service;
}

// Undo commands that add, change and remove a filter on a timeline clip or on the multitrack
// output without the Filters panel (Shotcut's Filter:: commands depend on its selection). The
// target is resolved again by (track, clip) at every redo and undo, as Shotcut's fade commands do,
// so no producer is held between calls. They keep the default id() (-1): they never merge.
namespace McpFilter {

// track >= 0 and clip >= 0: the clip's parent producer (where Shotcut attaches clip filters);
// track == -1: the multitrack tractor (output filters).
struct Target
{
    int track = -1;
    int clip = -1;
};

// MLT indexes of the service's filters that the Filters panel lists (Util::isUserFilter), in order.
QList<int> userFilterIndices(Mlt::Service &service);

class AddCommand : public QUndoCommand
{
public:
    // props are copied; shotcut:filter is set to shotcutId when it is not empty.
    AddCommand(MultitrackModel &model,
               Target target,
               const QString &text,
               const QString &service,
               const QString &shotcutId,
               Mlt::Properties &props);
    // Every redo creates the filter afresh from props at the same user index; undo detaches the
    // user filter at that index after checking it is this command's (service and shotcut:filter).
    // No filter object is kept, because Shotcut's undo of many timeline edits rebuilds the clip's
    // producer (and its filters) from XML.
    void redo() override;
    void undo() override;
    // The new filter's user index after the last redo, or -1 when it is not attached.
    int userIndex() const { return m_attached ? m_userIndex : -1; }

private:
    MultitrackModel &m_model;
    Target m_target;
    QString m_service;
    QString m_shotcutId;
    Mlt::Properties m_props;
    int m_userIndex = -1;
    bool m_attached = false;
};

// Replaces the properties of the user filter at userIndex with after (redo) or before (undo).
class SetCommand : public QUndoCommand
{
public:
    SetCommand(MultitrackModel &model,
               Target target,
               int userIndex,
               const QString &text,
               Mlt::Properties &before,
               Mlt::Properties &after);
    void redo() override;
    void undo() override;
    // Whether the last redo or undo found the filter and applied its properties.
    bool applied() const { return m_applied; }

private:
    void apply(Mlt::Properties &properties);

    MultitrackModel &m_model;
    Target m_target;
    int m_userIndex;
    bool m_applied = false;
    QByteArray m_service;
    QByteArray m_shotcutId;
    Mlt::Properties m_before;
    Mlt::Properties m_after;
};

// Detaches the user filter at userIndex (found by index at every redo, and checked against the
// filter the first redo removed) and keeps the detached filter, with all its properties, to put it
// back at the same user index on undo.
class RemoveCommand : public QUndoCommand
{
public:
    RemoveCommand(MultitrackModel &model, Target target, int userIndex, const QString &text);
    void redo() override;
    void undo() override;
    // Whether the last redo detached the filter.
    bool removed() const { return m_removed; }

private:
    MultitrackModel &m_model;
    Target m_target;
    int m_userIndex;
    bool m_removed = false;
    QByteArray m_service;
    QByteArray m_shotcutId;
    Mlt::Filter m_filter;
};

} // namespace McpFilter

#endif // MCPFILTERCOMMANDS_H
