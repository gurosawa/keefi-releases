#include "keefi/ChartWorkspace.h"
#include "keefi/KeefiTheme.h"
#include "keefi/KeefiVisuals.h"
#include "keefi/WatchlistWorkspaceList.h"

#include <QAccessible>
#include <QAction>
#include <QItemSelectionModel>
#include <QMenu>
#include <QPainter>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <algorithm>

namespace keefi {
namespace {
constexpr int IdentityRole = Qt::UserRole;
constexpr int SectionRole = Qt::UserRole + 1;

class InstrumentRowMenu final : public QMenu {
public:
    using QMenu::QMenu;
protected:
    void paintEvent(QPaintEvent* event) override {
        QMenu::paintEvent(event);
        for (auto* action : actions()) if (action->objectName() == "chartInstrumentRowRemove" && action->isVisible()) {
            const auto area = actionGeometry(action);
            if (area.isEmpty()) continue;
            QPainter painter(this);
            painter.setClipRect(area);
            painter.fillRect(area, activeAction() == action ? themeColors().surface : themeColors().input);
            painter.setPen(action->isEnabled() ? themeColors().danger : themeColors().text3);
            drawLedgerText(painter, QRectF(area).adjusted(12, 0, -12, 0), action->text(), FontRole::Caption12);
        }
    }
};

bool sectionExists(const WatchlistUserList& list, const QString& section) {
    return section.isEmpty() || std::any_of(list.sections.cbegin(), list.sections.cend(),
        [&](const auto& value) { return value.id == section; });
}
QStringList orderedMembers(const WatchlistUserList& list, const QStringList& requested) {
    QSet<QString> keys;
    for (const auto& entry : list.entries) keys.insert(entry.identity.key());
    QStringList result;
    for (const auto& key : requested) if (keys.remove(key)) result.append(key);
    return result;
}
}

void ChartWorkspace::restoreBrowserSelection(const QStringList& keys, const QString& section) {
    if (!watchlistRows_) return;
    const QSet<QString> wanted(keys.cbegin(), keys.cend());
    const QSignalBlocker block(watchlistRows_);
    const QSignalBlocker selectionBlock(watchlistRows_->selectionModel());
    watchlistRows_->clearSelection();
    QTreeWidgetItem* current = nullptr;
    for (QTreeWidgetItemIterator it(watchlistRows_); *it; ++it) {
        const auto key = (*it)->data(0, IdentityRole).toString();
        const bool selected = !key.isEmpty() ? wanted.contains(key) :
            keys.isEmpty() && !section.isEmpty() && (*it)->data(0, SectionRole).toString() == section;
        if (!selected) continue;
        (*it)->setSelected(true);
        if (!current) current = *it;
    }
    watchlistRows_->setCurrentItem(current, 0, QItemSelectionModel::NoUpdate);
}

void ChartWorkspace::announceBrowserMove(const QStringList& keys, const QString& section) {
    if (!watchlists_ || !watchlistRows_) return;
    const auto list = watchlists_->userList(currentListId());
    QString sectionId = section, label;
    int position = 0;
    if (!keys.isEmpty()) {
        for (const auto& entry : list.entries) if (entry.identity.key() == keys.first()) {
            sectionId = entry.sectionId;
            label = entry.identity.code;
            if (entry.identity.source == "upbit") label = label.section('-', 1);
            else if (entry.identity.source == "binance" && label.endsWith("USDT")) label.chop(4);
            break;
        }
        for (const auto& entry : list.entries) if (entry.sectionId == sectionId) {
            ++position;
            if (entry.identity.key() == keys.first()) break;
        }
    } else {
        for (int i = 0; i < list.sections.size(); ++i) if (list.sections[i].id == sectionId) {
            label = list.sections[i].name; position = i + 1; break;
        }
    }
    QString destination = tr("섹션 없음");
    for (const auto& value : list.sections) if (value.id == sectionId) { destination = value.name; break; }
    const auto message = keys.isEmpty() ? tr("%1 섹션을 %2번째로 옮김").arg(label).arg(position) :
        keys.size() == 1 ? tr("%1를 %2 %3번째로 옮김").arg(label, destination).arg(position) :
        tr("%1개 종목을 %2 %3번째로 옮김").arg(keys.size()).arg(destination).arg(position);
    watchlistRows_->setAccessibleDescription(message);
    if (auto* item = watchlistRows_->currentItem()) item->setData(0, Qt::AccessibleDescriptionRole, message);
    QAccessibleEvent event(watchlistRows_, QAccessible::DescriptionChanged);
    QAccessible::updateAccessibility(&event);
}

bool ChartWorkspace::applyBrowserEntryOrder(const QString& listId, const QList<WatchlistEntry>& wanted,
                                             const QStringList& selected) {
    QPointer<WatchlistWorkspaceList> source = watchlists_;
    if (!source || listId != currentListId() || browserScope_ != "watch") return false;
    const auto before = source->userList(listId);
    if (before.id.isEmpty() || before.entries == wanted || before.entries.size() != wanted.size()) return false;
    QSet<QString> oldKeys, newKeys;
    for (const auto& entry : before.entries) oldKeys.insert(entry.identity.key());
    for (const auto& entry : wanted) {
        if (!sectionExists(before, entry.sectionId)) return false;
        newKeys.insert(entry.identity.key());
    }
    if (oldKeys != newKeys || newKeys.size() != wanted.size()) return false;
    bool accepted = true;
    {
        // Other model listeners still receive the original changes, including
        // profile persistence. Only this view waits for the whole operation.
        const QScopedValueRollback guard(mutatingLists_, true);
        for (int i = 0; i < wanted.size(); ++i) {
            if (!source || source != watchlists_) { accepted = false; break; }
            const auto current = source->userList(listId).entries;
            if (i < current.size() && current[i] == wanted[i]) continue;
            if (!source->moveIdentity(listId, wanted[i].identity, i, wanted[i].sectionId)) {
                accepted = false; break;
            }
        }
    }
    refreshWatchlists();
    if (!accepted || !source || source != watchlists_ || listId != currentListId()) return false;
    restoreBrowserSelection(selected);
    announceBrowserMove(selected);
    return true;
}

void ChartWorkspace::moveBrowserRows(const QString& listId, const QStringList& requested,
                                      const QString& target, const QString& section, bool after) {
    if (!watchlists_ || listId != currentListId() || browserScope_ != "watch") return;
    const auto list = watchlists_->userList(listId);
    const auto keys = orderedMembers(list, requested);
    if (keys.isEmpty() || keys.contains(target) || !sectionExists(list, section)) return;
    QList<WatchlistEntry> remaining, moved;
    QString destination = section;
    if (!target.isEmpty()) {
        const auto entry = std::find_if(list.entries.cbegin(), list.entries.cend(),
            [&](const auto& value) { return value.identity.key() == target; });
        if (entry == list.entries.cend()) return;
        destination = entry->sectionId;
    }
    for (const auto& entry : list.entries) {
        if (!keys.contains(entry.identity.key())) remaining.append(entry);
    }
    for (const auto& key : keys) {
        const auto source = std::find_if(list.entries.cbegin(), list.entries.cend(),
            [&](const auto& entry) { return entry.identity.key() == key; });
        auto entry = *source;
        entry.sectionId = destination;
        moved.append(entry);
    }
    int insertion = remaining.size();
    if (!target.isEmpty()) {
        for (int i = 0; i < remaining.size(); ++i) if (remaining[i].identity.key() == target) {
            insertion = i + (after ? 1 : 0); break;
        }
    } else {
        for (int i = 0; i < remaining.size(); ++i) if (remaining[i].sectionId == destination) insertion = i + 1;
    }
    for (const auto& entry : moved) remaining.insert(insertion++, entry);
    applyBrowserEntryOrder(listId, remaining, keys);
}

void ChartWorkspace::moveBrowserSection(const QString& listId, const QString& sourceSection,
                                         const QString& targetSection, bool after) {
    if (!watchlists_ || listId != currentListId() || browserScope_ != "watch" ||
        sourceSection == targetSection || sourceSection.isEmpty() || targetSection.isEmpty()) return;
    const auto list = watchlists_->userList(listId);
    QStringList sections;
    for (const auto& section : list.sections) sections.append(section.id);
    if (!sections.contains(sourceSection) || !sections.contains(targetSection)) return;
    sections.removeAll(sourceSection);
    if (watchlists_->moveSection(listId, sourceSection, sections.indexOf(targetSection) + (after ? 1 : 0))) {
        restoreBrowserSelection({}, sourceSection);
        announceBrowserMove({}, sourceSection);
    }
}

void ChartWorkspace::moveBrowserSelection(const QString& listId, const QStringList& requested,
                                           const QString& section, int delta) {
    if (!watchlists_ || listId != currentListId() || browserScope_ != "watch") return;
    const auto list = watchlists_->userList(listId);
    const auto keys = orderedMembers(list, requested);
    if (keys.isEmpty()) {
        if (section.isEmpty() || delta == 0) return;
        for (int i = 0; i < list.sections.size(); ++i) if (list.sections[i].id == section) {
            const int destination = qBound(0, i + delta, int(list.sections.size()) - 1);
            if (destination != i && watchlists_->moveSection(listId, section, destination)) {
                restoreBrowserSelection({}, section); announceBrowserMove({}, section);
            }
            return;
        }
        return;
    }
    auto wanted = list.entries;
    QSet<QString> sections;
    for (const auto& entry : list.entries) if (keys.contains(entry.identity.key())) sections.insert(entry.sectionId);
    for (const auto& sectionId : sections) {
        QList<int> positions;
        QList<WatchlistEntry> entries;
        for (int i = 0; i < wanted.size(); ++i) if (wanted[i].sectionId == sectionId) {
            positions.append(i); entries.append(wanted[i]);
        }
        if (delta == 0) {
            std::stable_partition(entries.begin(), entries.end(),
                [&](const auto& entry) { return keys.contains(entry.identity.key()); });
        } else if (delta < 0) {
            for (int i = 1; i < entries.size(); ++i)
                if (keys.contains(entries[i].identity.key()) && !keys.contains(entries[i - 1].identity.key()))
                    entries.swapItemsAt(i, i - 1);
        } else {
            for (int i = int(entries.size()) - 2; i >= 0; --i)
                if (keys.contains(entries[i].identity.key()) && !keys.contains(entries[i + 1].identity.key()))
                    entries.swapItemsAt(i, i + 1);
        }
        for (int i = 0; i < positions.size(); ++i) wanted[positions[i]] = entries[i];
    }
    applyBrowserEntryOrder(listId, wanted, keys);
}

void ChartWorkspace::removeBrowserSelection(const QString& listId, const QStringList& requested,
                                             const QString& section) {
    QPointer<WatchlistWorkspaceList> source = watchlists_;
    if (!source || listId != currentListId() || browserScope_ != "watch") return;
    const auto list = source->userList(listId);
    const auto keys = orderedMembers(list, requested);
    if (keys.isEmpty()) {
        if (!section.isEmpty()) source->removeSection(listId, section);
        return;
    }
    {
        const QScopedValueRollback guard(mutatingLists_, true);
        for (const auto& key : keys) {
            if (!source || source != watchlists_) break;
            source->removeIdentity(listId, WatchlistWorkspaceList::identityFromKey(key));
        }
    }
    refreshWatchlists();
}

void ChartWorkspace::openBrowserContextMenu(const QString& listId, const QStringList& requested,
                                             const QPoint& position) {
    QPointer<WatchlistWorkspaceList> source = watchlists_;
    if (!source || listId != currentListId() || browserScope_ != "watch") return;
    const auto list = source->userList(listId);
    const auto keys = orderedMembers(list, requested);
    if (keys.isEmpty()) return;
    if (browserContextMenu_) browserContextMenu_->close();
    auto* menu = new InstrumentRowMenu(watchlistRows_);
    browserContextMenu_ = menu;
    menu->setObjectName("chartInstrumentRowMenu");
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->setFont(KeefiFonts::font(FontRole::Caption12));
    setThemeStyle(menu, listMenu_->property("keefiThemeTemplate").toString());
    const auto valid = [this, source, listId, keys] {
        return source && source == watchlists_ && listId == currentListId() && browserScope_ == "watch" &&
            orderedMembers(source->userList(listId), keys).size() == keys.size();
    };
    auto* sections = menu->addMenu(tr("섹션으로 이동"));
    const auto destination = [this, sections, valid, listId, keys](const QString& name, const QString& id) {
        connect(sections->addAction(name), &QAction::triggered, this, [this, valid, listId, keys, id] {
            if (valid()) moveBrowserRows(listId, keys, {}, id, true);
        });
    };
    destination(tr("섹션 없음"), {});
    for (const auto& section : list.sections) destination(section.name, section.id);
    connect(menu->addAction(tr("맨 위로")), &QAction::triggered, this, [this, valid, listId, keys] {
        if (valid()) moveBrowserSelection(listId, keys, {}, 0);
    });
    connect(menu->addAction(tr("알림 설정")), &QAction::triggered, this, [this, valid, listId] {
        if (valid()) emit listAlertsRequested(listId);
    });
    menu->addSeparator();
    auto* remove = menu->addAction(tr("관심 해제"));
    remove->setObjectName("chartInstrumentRowRemove");
    connect(remove, &QAction::triggered, this, [this, valid, listId, keys] {
        if (valid()) removeBrowserSelection(listId, keys, {});
    });
    menu->popup(position);
}
} // namespace keefi

