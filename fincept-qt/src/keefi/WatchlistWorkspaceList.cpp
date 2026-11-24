#include "keefi/WatchlistWorkspaceList.h"
#include "keefi/PublicFormatMigration.h"
#include "keefi/ExchangeIdentifiers.h"
#include "keefi/PublicQuoteStream.h"
#include "keefi/PublicMarketCatalog.h"
#include "keefi/TradingTypes.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QUuid>

namespace keefi {
WatchlistRowState projectRetainedBinanceWatchlistQuote(const WatchlistIdentity& identity,const PublicTick& tick,
    const QList<MarketInstrument>& currentInstruments,qint64 asOfMs,bool disconnected,quint64 currentStreamEpoch){
    auto row=projectBinanceWatchlistQuote(identity,tick,asOfMs,disconnected,currentStreamEpoch);
    if(row.price.isEmpty())return row;
    if(currentInstruments.isEmpty()){
        if(row.quoteState!=WatchlistQuoteState::Disconnected)row.quoteState=WatchlistQuoteState::Delayed;
        return row;
    }
    int matches=0;bool supported=false;
    for(const auto& item:currentInstruments)if(item.symbol==identity.code){
        ++matches;supported=item.market=="spot"&&item.status=="TRADING"&&item.tradable&&
            item.baseAsset==tick.quote.baseAsset&&item.quoteAsset=="USDT"&&item.symbol==item.baseAsset+item.quoteAsset;
    }
    if(matches!=1||!supported){row={};row.quoteState=WatchlistQuoteState::Unsupported;}
    return row;
}
WatchlistRowState projectBinanceWatchlistQuote(const WatchlistIdentity& identity, const PublicTick& tick,
    qint64 asOfMs, bool disconnected, quint64 currentStreamEpoch) {
    constexpr qint64 freshnessMs=90000;
    const auto& quote=tick.quote;const auto& evidence=quote.evidence;
    static const QRegularExpression amount(QStringLiteral("\\A[0-9]{1,80}(?:\\.[0-9]{1,80})?\\z"));
    // Only a formerly fresh public USDT observation can survive later expiry.
    // A late first observation, another symbol/provider/currency, or clock
    // reversal cannot become a last-known quote through this display path.
    if(identity.market!="coin"||identity.source!="binance"||identity.code!=tick.market||
       !WatchlistWorkspaceList::validIdentity(identity)||!identity.code.endsWith("USDT")||
       quote.baseAsset!=identity.code.chopped(4)||quote.quoteAsset!="USDT"||
       evidence.scope!="public:binance:"+identity.code||!evidence.complete||
       !amount.match(quote.price).hasMatch()||!decimal::positive(quote.price)||
       evidence.receivedAtMs<=0||evidence.receivedAtMs>asOfMs||
       evidence.sourceTimeMs<=0||evidence.sourceTimeMs>evidence.receivedAtMs+2000||
       evidence.receivedAtMs-evidence.sourceTimeMs>freshnessMs||
       evidence.sourceTimeMs>asOfMs+2000)return {};
    WatchlistRowState row;
    row.price=quote.price;row.change=changePercentText(tick.signedChangeRate);row.evidence=evidence.url;row.quoteCurrency=quote.quoteAsset;
    const bool delayed=asOfMs-evidence.receivedAtMs>freshnessMs||asOfMs-evidence.sourceTimeMs>freshnessMs||
        (currentStreamEpoch>0&&tick.streamEpoch!=currentStreamEpoch);
    row.quoteState=disconnected?WatchlistQuoteState::Disconnected:
        delayed?WatchlistQuoteState::Delayed:WatchlistQuoteState::Receiving;
    return row;
}
namespace {
constexpr int MaxLists = 32, MaxSections = 64, MaxEntries = 2000, MaxRecent = 8;
constexpr int MaxTotalEntries = 10000;
constexpr qsizetype MaxStateBytes = 4 * 1024 * 1024;
constexpr qsizetype MaxCsvBytes = 1024 * 1024;
bool publicText(const QString& value, int limit, bool allowEmpty = false) {
    if (value.size() > limit || (!allowEmpty && value.trimmed().isEmpty())) return false;
    for (const auto c : value)
        if ((c.category() == QChar::Other_Control && c != '\n') || c.category() == QChar::Other_Format) return false;
    return true;
}
bool flagValid(const QString& flag) {
    return QStringList{{}, "red", "orange", "yellow", "green", "blue", "purple"}.contains(flag);
}
bool idValid(const QString& id) {
    static const QRegularExpression pattern(QStringLiteral("\\A[A-Za-z0-9_-]{1,64}\\z"));
    return id != "holdings" && pattern.match(id).hasMatch();
}
QString newId() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
WatchlistUserList* listAt(QList<WatchlistUserList>& lists, const QString& id) {
    for (auto& list : lists) if (list.id == id) return &list;
    return nullptr;
}
bool sectionExists(const WatchlistUserList& list, const QString& id) {
    if (id.isEmpty()) return true;
    for (const auto& section : list.sections) if (section.id == id) return true;
    return false;
}
bool exactKeys(const QJsonObject& object, const QStringList& keys) {
    if (object.size() != keys.size()) return false;
    for (const auto& key : keys) if (!object.contains(key)) return false;
    return true;
}
bool textFields(const QJsonObject& object, const QStringList& keys) {
    for (const auto& key : keys) if (!object.value(key).isString()) return false;
    return true;
}
int entryCount(const QList<WatchlistUserList>& lists) {
    int count = 0; for (const auto& list : lists) count += list.entries.size(); return count;
}
bool rowValid(const WatchlistRowState& row) {
    static const QRegularExpression currency(QStringLiteral("\\A[A-Z]{2,10}\\z"));
    if(!row.quoteCurrency.isEmpty()&&!currency.match(row.quoteCurrency).hasMatch())return false;
    if (row.quoteState != WatchlistQuoteState::Receiving && row.quoteState != WatchlistQuoteState::Disconnected &&
        row.quoteState != WatchlistQuoteState::Unsupported && row.quoteState != WatchlistQuoteState::ConnectionRequired &&
        row.quoteState != WatchlistQuoteState::Delayed) return false;
    if (!publicText(row.price, 256, true) || !publicText(row.change, 64, true) ||
        !publicText(row.evidence, 2048, true) || row.completedAtMs < 0 || row.badges.size() > 16 || row.badgePresentations.size()>16) return false;
    for (const auto& badge : row.badges) if (!publicText(badge, 160)) return false;
    for(const auto& badge:row.badgePresentations){
        if(!publicText(badge.text,160)||QString::fromUtf8(badge.text.toUtf8())!=badge.text)return false;
        if(badge.direction!=MovementDirection::Unknown&&badge.direction!=MovementDirection::Flat&&badge.direction!=MovementDirection::Up&&badge.direction!=MovementDirection::Down)return false;
    }
    return true;
}
QString csvText(const QString& value) { return value.isEmpty() ? QString{} : "'" + value; }
QString csvOriginal(const QString& value) { return value.startsWith('\'') ? value.mid(1) : value; }
void appendCsvRow(QString* output, const QStringList& cells) {
    QStringList quoted;
    for (auto cell : cells) { cell.replace('"', "\"\""); quoted.append('"' + cell + '"'); }
    *output += quoted.join(',') + "\r\n";
}
bool readCsv(const QByteArray& bytes, QList<QStringList>* rows) {
    if (bytes.isEmpty() || bytes.size() > MaxCsvBytes) return false;
    auto text = QString::fromUtf8(bytes);
    if (text.toUtf8() != bytes) return false;
    if (text.startsWith(QChar(0xfeff))) text.remove(0, 1);
    QStringList row; QString cell; bool quoted = false, afterQuote = false, endedRecord = false;
    const auto finishRow = [&] {
        row.append(cell); cell.clear(); rows->append(row); row.clear();
        afterQuote = false; endedRecord = true;
        return rows->last().size() == 7 && rows->size() <= MaxEntries + MaxSections + 2;
    };
    for (qsizetype i = 0; i < text.size(); ++i) {
        const auto c = text[i];
        if (quoted) {
            if (c == '"') {
                if (i + 1 < text.size() && text[i + 1] == '"') { cell += '"'; ++i; }
                else { quoted = false; afterQuote = true; }
            } else cell += c;
            endedRecord = false;
        } else if (c == '"') {
            if (!cell.isEmpty() || afterQuote) return false;
            quoted = true; endedRecord = false;
        } else if (c == ',') {
            row.append(cell); cell.clear(); afterQuote = false; endedRecord = false;
            if (row.size() >= 7) return false;
        } else if (c == '\r' || c == '\n') {
            if (c == '\r') { if (i + 1 >= text.size() || text[++i] != '\n') return false; }
            if (!finishRow()) return false;
        } else {
            if (afterQuote) return false;
            cell += c; endedRecord = false;
        }
        if (cell.size() > 1024) return false;
    }
    if (quoted) return false;
    if (!endedRecord && !finishRow()) return false;
    return !rows->isEmpty();
}
}
QString WatchlistIdentity::key() const { return market + ':' + source + ':' + code; }
WatchlistWorkspaceList::WatchlistWorkspaceList(QObject* parent) : QObject(parent) {}
bool WatchlistWorkspaceList::validIdentity(const WatchlistIdentity& identity) {
    if (identity.market == "coin") {
        if (identity.source == "upbit" || identity.source == "bithumb") {
            static const QRegularExpression pattern(QStringLiteral("\\A(?:KRW|BTC|USDT)-[A-Z0-9]{1,20}\\z"));
            return pattern.match(identity.code).hasMatch();
        }
        if (identity.source == "binance") return binanceIdentifierValid(identity.code,2);
        return false;
    }
    if (identity.market != "stock") return false;
    if (identity.source == "KR") {
        static const QRegularExpression pattern(QStringLiteral("\\A[A-Z0-9]{6}\\z"));
        return pattern.match(identity.code).hasMatch();
    }
    if (identity.source == "US") {
        static const QRegularExpression pattern(QStringLiteral("\\A[A-Z]{1,8}(?:[.-][A-Z])?\\z"));
        return pattern.match(identity.code).hasMatch();
    }
    return false;
}
WatchlistIdentity WatchlistWorkspaceList::identityFromKey(const QString& key) {
    const auto parts = key.split(':');
    if (parts.size() != 3) return {};
    const WatchlistIdentity result{parts[0], parts[1], parts[2]};
    return validIdentity(result) ? result : WatchlistIdentity{};
}
void WatchlistWorkspaceList::setVerifiedInstruments(const QList<WatchlistIdentity>& input) {
    QList<WatchlistIdentity> next; QSet<QString> before, after;
    for (const auto& identity : verified_) before.insert(identity.key());
    for (const auto& identity : input) if (validIdentity(identity) && !after.contains(identity.key())) {
        after.insert(identity.key()); next.append(identity);
    }
    if (before == after) return;
    verified_ = next; emit changed();
}
bool WatchlistWorkspaceList::isCurrentlyVerified(const WatchlistIdentity& identity) const {
    return validIdentity(identity) && (identity.market != "stock" || verified_.contains(identity));
}
void WatchlistWorkspaceList::setPublicStockInstruments(const QList<WatchlistIdentity>& input) {
    QList<WatchlistIdentity> next; QSet<QString> before, after;
    for (const auto& identity : publicStocks_) before.insert(identity.key());
    for (const auto& identity : input) if (identity.market == "stock" && validIdentity(identity) && !after.contains(identity.key())) {
        after.insert(identity.key()); next.append(identity);
    }
    if (before == after) return;
    publicStocks_ = next; emit changed();
}
QList<WatchlistUserList> WatchlistWorkspaceList::userLists() const { return lists_; }
WatchlistUserList WatchlistWorkspaceList::userList(const QString& id) const {
    for (const auto& list : lists_) if (list.id == id) return list;
    return {};
}
QString WatchlistWorkspaceList::createList(const QString& name, const QString& flag) {
    if (!publicText(name, 100) || !flagValid(flag) || lists_.size() >= MaxLists) return {};
    const auto id = newId(); lists_.append({id, name, flag, {}, {}}); emit changed(); return id;
}
bool WatchlistWorkspaceList::renameList(const QString& id, const QString& name) {
    auto* list = listAt(lists_, id); if (!list || !publicText(name, 100)) return false;
    if (list->name != name) { list->name = name; emit changed(); } return true;
}
bool WatchlistWorkspaceList::setFlag(const QString& id, const QString& flag) {
    auto* list = listAt(lists_, id); if (!list || !flagValid(flag)) return false;
    if (list->flag != flag) { list->flag = flag; emit changed(); } return true;
}
bool WatchlistWorkspaceList::removeList(const QString& id) {
    for (int i = 0; i < lists_.size(); ++i) if (lists_[i].id == id) {
        lists_.removeAt(i); recent_.removeAll(id); emit changed(); return true;
    }
    return false;
}
bool WatchlistWorkspaceList::moveList(const QString& id, int index) {
    if (index < 0 || index >= lists_.size()) return false;
    for (int i = 0; i < lists_.size(); ++i) if (lists_[i].id == id) {
        if (i != index) { lists_.move(i, index); emit changed(); } return true;
    }
    return false;
}
QString WatchlistWorkspaceList::addSection(const QString& listId, const QString& name) {
    auto* list = listAt(lists_, listId);
    if (!list || !publicText(name, 100) || list->sections.size() >= MaxSections) return {};
    for (const auto& section : list->sections) if (section.name == name) return {};
    const auto id = newId(); list->sections.append({id, name}); emit changed(); return id;
}
bool WatchlistWorkspaceList::renameSection(const QString& listId, const QString& sectionId, const QString& name) {
    auto* list = listAt(lists_, listId); if (!list || !publicText(name, 100)) return false;
    for (const auto& section : list->sections) if (section.id != sectionId && section.name == name) return false;
    for (auto& section : list->sections) if (section.id == sectionId) {
        if (section.name != name) { section.name = name; emit changed(); } return true;
    }
    return false;
}
bool WatchlistWorkspaceList::removeSection(const QString& listId, const QString& sectionId) {
    auto* list = listAt(lists_, listId); if (!list) return false;
    for (int i = 0; i < list->sections.size(); ++i) if (list->sections[i].id == sectionId) {
        list->sections.removeAt(i);
        for (auto& entry : list->entries) if (entry.sectionId == sectionId) entry.sectionId.clear();
        emit changed(); return true;
    }
    return false;
}
bool WatchlistWorkspaceList::moveSection(const QString& listId, const QString& sectionId, int index) {
    auto* list = listAt(lists_, listId); if (!list || index < 0 || index >= list->sections.size()) return false;
    for (int i = 0; i < list->sections.size(); ++i) if (list->sections[i].id == sectionId) {
        if (i != index) { list->sections.move(i, index); emit changed(); } return true;
    }
    return false;
}
bool WatchlistWorkspaceList::addIdentity(const QString& listId, const WatchlistIdentity& identity, const QString& sectionId) {
    auto* list = listAt(lists_, listId);
    if (!list || (!isCurrentlyVerified(identity) && !publicStocks_.contains(identity)) || !sectionExists(*list, sectionId) || list->entries.size() >= MaxEntries ||
        entryCount(lists_) >= MaxTotalEntries) return false;
    for (const auto& entry : list->entries) if (entry.identity == identity) return false;
    list->entries.append({identity, sectionId}); emit changed(); return true;
}
bool WatchlistWorkspaceList::removeIdentity(const QString& listId, const WatchlistIdentity& identity) {
    auto* list = listAt(lists_, listId); if (!list) return false;
    for (int i = 0; i < list->entries.size(); ++i) if (list->entries[i].identity == identity) {
        list->entries.removeAt(i); emit changed(); return true;
    }
    return false;
}
bool WatchlistWorkspaceList::replaceIdentity(const QString& listId, const WatchlistIdentity& oldIdentity, const WatchlistIdentity& newIdentity) {
    auto* list = listAt(lists_, listId);
    if (!list || (!isCurrentlyVerified(newIdentity) && !publicStocks_.contains(newIdentity))) return false;
    int previous = -1, existing = -1;
    for (int i = 0; i < list->entries.size(); ++i) {
        if (list->entries[i].identity == oldIdentity) previous = i;
        if (list->entries[i].identity == newIdentity) existing = i;
    }
    if (previous < 0) return false;
    if (oldIdentity == newIdentity) return true;
    if (existing >= 0) list->entries.removeAt(previous);
    else list->entries[previous].identity = newIdentity;
    emit changed(); return true;
}
bool WatchlistWorkspaceList::moveIdentity(const QString& listId, const WatchlistIdentity& identity, int index, const QString& sectionId) {
    auto* list = listAt(lists_, listId);
    if (!list || index < 0 || index >= list->entries.size() || !sectionExists(*list, sectionId)) return false;
    for (int i = 0; i < list->entries.size(); ++i) if (list->entries[i].identity == identity) {
        if (i != index || list->entries[i].sectionId != sectionId) {
            list->entries[i].sectionId = sectionId; list->entries.move(i, index); emit changed();
        }
        return true;
    }
    return false;
}
void WatchlistWorkspaceList::useList(const QString& id) {
    if (!listAt(lists_, id) || recent_.value(0) == id) return;
    recent_.removeAll(id); recent_.prepend(id); while (recent_.size() > MaxRecent) recent_.removeLast(); emit changed();
}
QStringList WatchlistWorkspaceList::recentLists() const { return recent_; }
void WatchlistWorkspaceList::setHoldings(const QList<WatchlistIdentity>& input) {
    QList<WatchlistIdentity> next;
    for (const auto& identity : input) if (isCurrentlyVerified(identity) && !next.contains(identity) && next.size() < MaxEntries) next.append(identity);
    if (next == holdings_) return;
    for (const auto& previous : holdings_) if (!next.contains(previous)) rows_.remove(previous.key());
    holdings_ = next; emit changed();
}
QList<WatchlistIdentity> WatchlistWorkspaceList::holdings() const { return holdings_; }
void WatchlistWorkspaceList::setRowState(const WatchlistIdentity& identity, const WatchlistRowState& row) {
    if (!validIdentity(identity) || !rowValid(row) || (!rows_.contains(identity.key()) && rows_.size() >= MaxTotalEntries)) return;
    if (rows_.value(identity.key()) == row) return;
    if (row == WatchlistRowState{}) rows_.remove(identity.key()); else rows_.insert(identity.key(), row);
    emit changed();
}
void WatchlistWorkspaceList::setRowStates(const QHash<QString, WatchlistRowState>& rows) {
    if (rows.size() > MaxTotalEntries) return;
    int additions = 0;
    for (auto it = rows.constBegin(); it != rows.constEnd(); ++it) {
        if (!validIdentity(identityFromKey(it.key())) || !rowValid(it.value())) return;
        if (!rows_.contains(it.key()) && it.value() != WatchlistRowState{}) ++additions;
    }
    if (rows_.size() + additions > MaxTotalEntries) return;
    bool altered = false;
    for (auto it = rows.constBegin(); it != rows.constEnd(); ++it) if (rows_.value(it.key()) != it.value()) {
        if (it.value() == WatchlistRowState{}) rows_.remove(it.key()); else rows_.insert(it.key(), it.value());
        altered = true;
    }
    if (altered) emit changed();
}
WatchlistRowState WatchlistWorkspaceList::rowState(const WatchlistIdentity& identity) const { return rows_.value(identity.key()); }
void WatchlistWorkspaceList::clearPrivate() {
    bool removed = !holdings_.isEmpty(); QSet<QString> holdings;
    for (const auto& identity : holdings_) holdings.insert(identity.key());
    for (auto it = rows_.begin(); it != rows_.end();) {
        if (it.value().privateData || holdings.contains(it.key())) { it = rows_.erase(it); removed = true; }
        else ++it;
    }
    holdings_.clear(); if (removed) emit changed();
}
QByteArray WatchlistWorkspaceList::publicState() const {
    QJsonArray lists, recent, migrated;
    for (const auto& list : lists_) {
        QJsonArray sections, entries;
        for (const auto& section : list.sections) sections.append(QJsonObject{{"id", section.id}, {"name", section.name}});
        for (const auto& entry : list.entries) entries.append(QJsonObject{{"market", entry.identity.market},
            {"source", entry.identity.source}, {"code", entry.identity.code}, {"section", entry.sectionId}});
        lists.append(QJsonObject{{"id", list.id}, {"name", list.name}, {"flag", list.flag}, {"sections", sections}, {"entries", entries}});
    }
    for (const auto& id : recent_) recent.append(id);
    for (const auto& marker : migrated_) migrated.append(marker);
    return QJsonDocument(QJsonObject{{"schema", "keefi-watchlist-workspace/2"}, {"lists", lists},
        {"recent", recent}, {"migrated", migrated}}).toJson(QJsonDocument::Compact);
}
bool WatchlistWorkspaceList::restorePublicState(const QByteArray& bytes, QString* error) {
    if (error) error->clear();
    const auto reject = [&] { if (error) *error = tr("공개 관심 리스트 형식 또는 한도를 확인해 주세요."); return false; };
    if (bytes.isEmpty() || bytes.size() > MaxStateBytes) return reject();
    const auto document = QJsonDocument::fromJson(bytes);
    if (!document.isObject()) return reject();
    const auto root = document.object();
    if (!exactKeys(root, {"schema", "lists", "recent", "migrated"}) || canonicalPublicFormat(root.value("schema").toString()) != "keefi-watchlist-workspace/2" ||
        !root.value("lists").isArray() || !root.value("recent").isArray() || !root.value("migrated").isArray() ||
        root.value("lists").toArray().size() > MaxLists || root.value("recent").toArray().size() > MaxRecent ||
        root.value("migrated").toArray().size() > 3 * MaxEntries) return reject();
    QList<WatchlistUserList> next; int totalEntries = 0;
    QSet<QString> listIds;
    for (const auto& value : root.value("lists").toArray()) {
        if (!value.isObject()) return reject();
        const auto object = value.toObject();
        if (!exactKeys(object, {"id", "name", "flag", "sections", "entries"}) || !textFields(object, {"id", "name", "flag"}) ||
            !object.value("sections").isArray() || !object.value("entries").isArray()) return reject();
        WatchlistUserList list{object.value("id").toString(), object.value("name").toString(), object.value("flag").toString(), {}, {}};
        if (!idValid(list.id) || listIds.contains(list.id) || !publicText(list.name, 100) || !flagValid(list.flag) ||
            object.value("sections").toArray().size() > MaxSections || object.value("entries").toArray().size() > MaxEntries) return reject();
        listIds.insert(list.id); QSet<QString> sectionIds, sectionNames, identities;
        totalEntries += object.value("entries").toArray().size(); if (totalEntries > MaxTotalEntries) return reject();
        for (const auto& item : object.value("sections").toArray()) {
            if (!item.isObject()) return reject();
            const auto section = item.toObject();
            if (!exactKeys(section, {"id", "name"}) || !textFields(section, {"id", "name"})) return reject();
            const auto id = section.value("id").toString(), name = section.value("name").toString();
            if (!idValid(id) || sectionIds.contains(id) || !publicText(name, 100) || sectionNames.contains(name)) return reject();
            sectionIds.insert(id); sectionNames.insert(name); list.sections.append({id, name});
        }
        for (const auto& item : object.value("entries").toArray()) {
            if (!item.isObject()) return reject();
            const auto entry = item.toObject();
            if (!exactKeys(entry, {"market", "source", "code", "section"}) || !textFields(entry, {"market", "source", "code", "section"})) return reject();
            WatchlistIdentity identity{entry.value("market").toString(), entry.value("source").toString(), entry.value("code").toString()};
            const auto section = entry.value("section").toString();
            if (!validIdentity(identity) || identities.contains(identity.key()) || !sectionExists(list, section)) return reject();
            identities.insert(identity.key()); list.entries.append({identity, section});
        }
        next.append(list);
    }
    QStringList recent, migrated;
    for (const auto& item : root.value("recent").toArray()) {
        if (!item.isString() || !listIds.contains(item.toString()) || recent.contains(item.toString())) return reject();
        recent.append(item.toString());
    }
    for (const auto& item : root.value("migrated").toArray()) {
        static const QRegularExpression pattern(QStringLiteral("\\A(?:coin|kr|us):[A-Za-z0-9._-]{1,80}\\z"));
        if (!item.isString() || !pattern.match(item.toString()).hasMatch() || migrated.contains(item.toString())) return reject();
        migrated.append(item.toString());
    }
    if (next == lists_ && recent == recent_ && migrated == migrated_) return true;
    lists_ = next; recent_ = recent; migrated_ = migrated; emit changed(); return true;
}
QByteArray WatchlistWorkspaceList::exportCsv(const QString& listId) const {
    const auto list = userList(listId); if (list.id.isEmpty()) return {};
    QString result;
    appendCsvRow(&result, {"kind", "list_name", "flag", "section", "market", "source", "code"});
    // Text is always prefixed, including literal leading apostrophes. Removing
    // exactly one on import makes escaping injective and spreadsheet-safe.
    appendCsvRow(&result, {"list", csvText(list.name), csvText(list.flag), {}, {}, {}, {}});
    QHash<QString, QString> sections;
    for (const auto& section : list.sections) {
        sections.insert(section.id, section.name);
        appendCsvRow(&result, {"section", {}, {}, csvText(section.name), {}, {}, {}});
    }
    for (const auto& entry : list.entries) appendCsvRow(&result, {"entry", {}, {}, csvText(sections.value(entry.sectionId)),
        entry.identity.market, entry.identity.source, entry.identity.code});
    return result.toUtf8();
}
QString WatchlistWorkspaceList::importCsv(QIODevice& input, QString* error) {
    QByteArray bytes;
    while (bytes.size() <= MaxCsvBytes) {
        const auto chunk = input.read(qMin<qint64>(64 * 1024, MaxCsvBytes + 1 - bytes.size()));
        if (chunk.isEmpty()) break;
        bytes += chunk;
    }
    return importCsv(bytes, error);
}
QString WatchlistWorkspaceList::importCsv(const QByteArray& bytes, QString* error) {
    if (error) error->clear();
    const auto reject = [&] { if (error) *error = tr("CSV 형식·공개 종목 identity·시장 메타데이터를 확인해 주세요."); return QString{}; };
    QList<QStringList> rows;
    if (!readCsv(bytes, &rows) || rows.size() < 2 || lists_.size() >= MaxLists ||
        rows.first() != QStringList{"kind", "list_name", "flag", "section", "market", "source", "code"}) return reject();
    const auto metadata = rows[1];
    if (metadata[0] != "list" || !metadata[3].isEmpty() || !metadata[4].isEmpty() || !metadata[5].isEmpty() || !metadata[6].isEmpty()) return reject();
    WatchlistUserList list{newId(), csvOriginal(metadata[1]), csvOriginal(metadata[2]), {}, {}};
    if (!publicText(list.name, 100) || !flagValid(list.flag)) return reject();
    QHash<QString, QString> sections; QSet<QString> identities; bool entriesStarted = false;
    for (int i = 2; i < rows.size(); ++i) {
        const auto row = rows[i];
        if (!row[1].isEmpty() || !row[2].isEmpty()) return reject();
        const auto name = csvOriginal(row[3]);
        if (row[0] == "section") {
            if (entriesStarted || !row[4].isEmpty() || !row[5].isEmpty() || !row[6].isEmpty() ||
                !publicText(name, 100) || sections.contains(name) || list.sections.size() >= MaxSections) return reject();
            const auto id = newId(); sections.insert(name, id); list.sections.append({id, name});
        } else if (row[0] == "entry") {
            entriesStarted = true;
            const WatchlistIdentity identity{row[4], row[5], row[6]};
            if (!isCurrentlyVerified(identity) || identities.contains(identity.key()) ||
                (!name.isEmpty() && !sections.contains(name)) || list.entries.size() >= MaxEntries) return reject();
            identities.insert(identity.key()); list.entries.append({identity, sections.value(name)});
        } else return reject();
    }
    if (entryCount(lists_) + list.entries.size() > MaxTotalEntries) return reject();
    lists_.append(list); emit changed(); return list.id;
}
int WatchlistWorkspaceList::migrateLegacy(const QMap<QString, QStringList>& original) {
    auto next = lists_; auto migrated = migrated_; int pending = 0;
    const QMap<QString, QString> names{{"coin", "CRYPTO"}, {"kr", "KOSPI"}, {"us", "NASDAQ"}};
    for (const auto& section : {QStringLiteral("coin"), QStringLiteral("kr"), QStringLiteral("us")}) {
        QSet<QString> seen;
        for (const auto& code : original.value(section)) {
            if (seen.contains(code)) continue; seen.insert(code);
            const auto marker = section + ':' + code;
            if (migrated.contains(marker)) continue;
            const WatchlistIdentity identity = section == "coin" ? WatchlistIdentity{"coin", "upbit", "KRW-" + code} :
                WatchlistIdentity{"stock", section == "kr" ? QStringLiteral("KR") : QStringLiteral("US"), code};
            if (!isCurrentlyVerified(identity) || migrated.size() >= 3 * MaxEntries) { ++pending; continue; }
            WatchlistUserList* list = nullptr;
            for (auto& candidate : next) if (candidate.name == names.value(section)) { list = &candidate; break; }
            if (!list) {
                if (next.size() >= MaxLists) { ++pending; continue; }
                next.append({newId(), names.value(section), {}, {}, {}}); list = &next.last();
            }
            bool exists = false; for (const auto& entry : list->entries) if (entry.identity == identity) { exists = true; break; }
            if (!exists) {
                if (list->entries.size() >= MaxEntries || entryCount(next) >= MaxTotalEntries) { ++pending; continue; }
                list->entries.append({identity, {}});
            }
            migrated.append(marker);
        }
    }
    if (next != lists_ || migrated != migrated_) { lists_ = next; migrated_ = migrated; emit changed(); }
    return pending;
}
QList<WatchlistIdentity> WatchlistWorkspaceList::rankRealtime(const QList<WatchlistIdentity>& visible,
    const QList<WatchlistIdentity>& selected, const QList<WatchlistIdentity>& holdings, const QList<WatchlistIdentity>& rest, int cap) {
    QList<WatchlistIdentity> result; QSet<QString> seen;
    if (cap <= 0) return result;
    const int limit = qMin(cap, MaxTotalEntries);
    for (const auto& group : {visible, selected, holdings, rest}) for (const auto& identity : group) {
        if (!validIdentity(identity) || seen.contains(identity.key())) continue;
        seen.insert(identity.key()); result.append(identity);
        if (result.size() == limit) return result;
    }
    return result;
}
}
