#include "keefi/AssetsLedger.h"
#include "keefi/LegacyStorageCodecPrivate.h"
#include "keefi/PublicFormatMigration.h"
#include "keefi/Vault.h"
#include "keefi/SolanaTokenData.h"
#include "keefi/TradingTypes.h"
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QThread>
#include <QTimer>
#include <QJsonDocument>
#include <QUuid>
#include <QJsonArray>
#include <QLockFile>
#include <QRegularExpression>
#include <QSet>
#include "StagedStorageFile.h"
#include <QUrl>
#include <QtEndian>
#include <sodium.h>
#include <algorithm>
#include <cstring>
#include <vector>
#include <stdexcept>
#include <atomic>
#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#ifdef Q_OS_MACOS
#include <sys/acl.h>
#endif
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "WindowsPrivateStorage.h"
#include <io.h>
#endif
namespace keefi {
namespace {
constexpr qsizetype HeaderBytes=40;
bool fail(QString* error,const QString& message){if(error)*error=message;return false;}


bool privatePath(const QString& path, bool directory, QString* error) {
    const auto invalid = [&] { return fail(error, QStringLiteral("금고 경로의 소유자·권한·심볼릭 링크를 확인해 주세요.")); };
    const QFileInfo info(path);
    if (info.isSymLink() || !info.exists() || info.isDir() != directory)
        return invalid();
#ifdef Q_OS_UNIX
    struct stat st{};
    const QByteArray name = QFile::encodeName(path);
    if (::lstat(name.constData(), &st) || st.st_uid != ::geteuid() || (st.st_mode & 0077) ||
        (directory ? !S_ISDIR(st.st_mode) : !S_ISREG(st.st_mode)))
        return invalid();
#ifdef Q_OS_MACOS
    // macOS extended ACL grants need not be reflected by the Unix mode bits.
    // Do not rewrite an existing ACL. Conservatively require an empty ACL.
    // acl_get_file returns NULL/ENOENT both for a missing path and for an
    // existing object without an ACL. Query the property after a successful
    // lstatx instead of treating those two cases as interchangeable.
    filesec_t security = ::filesec_init();
    if (!security) return invalid();
    struct stat aclStat{};
    int hasAcl = 0;
    bool safe = ::lstatx_np(name.constData(), &aclStat, security) == 0 &&
        aclStat.st_dev == st.st_dev && aclStat.st_ino == st.st_ino &&
        ::filesec_query_property(security, FILESEC_ACL, &hasAcl) == 0;
    if (safe && hasAcl) {
        acl_t acl = nullptr;
        safe = ::filesec_get_property(security, FILESEC_ACL, &acl) == 0 && acl;
        if (safe) {
            acl_entry_t entry = nullptr;
            safe = ::acl_get_entry(acl, ACL_FIRST_ENTRY, &entry) != 0;
        }
        if (acl) ::acl_free(acl);
    }
    ::filesec_free(security);
    if (!safe) return invalid();
#endif
#elif defined(Q_OS_WIN)
    if (!privateWindowsPath(path, false)) return invalid();
#else
    return fail(error, QStringLiteral("이 운영체제의 금고 접근 권한 검증은 지원하지 않습니다."));
#endif
    return true;
}

QByteArray lockFileIdentity(const QString& path) {
#ifdef Q_OS_UNIX
    struct stat st{};
    if (::lstat(QFile::encodeName(path).constData(), &st) || !S_ISREG(st.st_mode) || st.st_uid != ::geteuid())
        return {};
    return QByteArray::number(static_cast<qulonglong>(st.st_dev)) + ':' +
           QByteArray::number(static_cast<qulonglong>(st.st_ino));
#elif defined(Q_OS_WIN)
    HANDLE file = CreateFileW(reinterpret_cast<const wchar_t*>(path.utf16()), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    BY_HANDLE_FILE_INFORMATION info{};
    const bool valid = GetFileInformationByHandle(file, &info) &&
        !(info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY));
    CloseHandle(file);
    if (!valid) return {};
    return QByteArray::number(info.dwVolumeSerialNumber) + ':' + QByteArray::number(info.nFileIndexHigh) + ':' +
           QByteArray::number(info.nFileIndexLow);
#else
    Q_UNUSED(path)
    return {};
#endif
}

bool textValid(const QString& s,qsizetype maximum,bool empty=false,bool multiline=false){
    if(s.size()>maximum||(!empty&&s.isEmpty()))return false;
    for(qsizetype i=0;i<s.size();++i){const auto c=s[i];if(multiline&&c==u'\n')continue;if(c.isHighSurrogate()){if(++i>=s.size()||!s[i].isLowSurrogate())return false;}
        else if(c.isLowSurrogate()||c.category()==QChar::Other_Control||c.category()==QChar::Other_Format)return false;}
    return true;
}
bool decimalValue(const QString& value,bool empty=false){
    static const QRegularExpression re("\\A-?[0-9]{1,80}(?:\\.[0-9]{1,80})?\\z");return (empty&&value.isEmpty())||re.match(value).hasMatch();
}
bool stamp(qint64 value,bool empty=false){return (empty&&value==0)||(value>0&&value<=253402300799999LL);}
bool evidenceValid(const AssetEvidence& e){
    const QUrl url(e.url);
    return textValid(e.source,160)&&textValid(e.scope,512)&&textValid(e.url,2048,true)&&stamp(e.sourceTimeMs,true)&&stamp(e.receivedAtMs)&&
        (e.url.isEmpty()||(url.isValid()&&url.scheme()=="https"&&!url.host().isEmpty()&&url.userInfo().isEmpty()&&!url.hasQuery()&&!url.hasFragment()));
}
QJsonObject encode(const AssetEvidence& e){return {{"source",e.source},{"url",e.url},{"scope",e.scope},{"sourceTimeMs",QString::number(e.sourceTimeMs)},{"receivedAtMs",QString::number(e.receivedAtMs)},{"complete",e.complete}};}
bool stringFields(const QJsonObject& o,std::initializer_list<const char*> names){for(const auto* n:names)if(!o.value(n).isString())return false;return true;}
bool timeField(const QJsonObject& o,const char* name,qint64* out,bool empty=false){
    const auto v=o.value(name);if(!v.isString())return false;bool ok=false;*out=v.toString().toLongLong(&ok);
    return ok&&QString::number(*out)==v.toString()&&stamp(*out,empty);
}
bool decode(const QJsonObject& o,AssetEvidence* e){
    if(o.size()!=6||!stringFields(o,{"source","url","scope"})||!o.value("complete").isBool()||!timeField(o,"sourceTimeMs",&e->sourceTimeMs,true)||!timeField(o,"receivedAtMs",&e->receivedAtMs))return false;
    e->source=o["source"].toString();e->url=o["url"].toString();e->scope=o["scope"].toString();e->complete=o["complete"].toBool();return evidenceValid(*e);
}
QJsonObject encode(const AssetComponent& c){QJsonObject out{{"id",c.id},{"asset",c.asset},{"quantity",c.quantity},{"costBasis",c.costBasis},{"costCurrency",c.costCurrency},
    {"reportedValue",c.reportedValue},{"reportedValueCurrency",c.reportedValueCurrency},{"providerUnrealizedPnl",c.providerUnrealizedPnl},{"valuationGroup",c.valuationGroup},{"kind",int(c.kind)},{"evidence",encode(c.evidence)}};
    if(c.asset.startsWith("solana:")){
        QJsonObject display{{"symbol",c.displaySymbol},{"name",c.displayName},{"metadataIssue",c.metadataIssue},{"priceIssue",c.priceIssue}};
        if(c.metadataEvidence.complete)display["evidence"]=encode(c.metadataEvidence);
        out["display"]=display;
    }return out;}
bool decode(const QJsonObject& o,AssetComponent* c){
    const int kind=o["kind"].toInt(-1);
    if((o.size()!=11&&!(o.size()==12&&o.contains("display")))||!stringFields(o,{"id","asset","quantity","costBasis","costCurrency","reportedValue","reportedValueCurrency","providerUnrealizedPnl","valuationGroup"})||
        o["kind"]!=QJsonValue(kind)||kind<0||kind>int(AssetComponentKind::ProviderPnl)||!o["evidence"].isObject()||!decode(o["evidence"].toObject(),&c->evidence))return false;
    c->id=o["id"].toString();c->asset=o["asset"].toString();c->quantity=o["quantity"].toString();c->costBasis=o["costBasis"].toString();c->costCurrency=o["costCurrency"].toString();
    c->reportedValue=o["reportedValue"].toString();c->reportedValueCurrency=o["reportedValueCurrency"].toString();c->providerUnrealizedPnl=o["providerUnrealizedPnl"].toString();c->valuationGroup=o["valuationGroup"].toString();c->kind=AssetComponentKind(kind);
    if(o.contains("display")){
        const auto display=o.value("display").toObject();
        if(!c->asset.startsWith("solana:")||(display.size()!=4&&!(display.size()==5&&display.contains("evidence")))||
            !stringFields(display,{"symbol","name","metadataIssue","priceIssue"}))return false;
        c->displaySymbol=display["symbol"].toString();c->displayName=display["name"].toString();
        c->metadataIssue=display["metadataIssue"].toString();c->priceIssue=display["priceIssue"].toString();
        if((!c->displaySymbol.isEmpty()&&!SolanaTokenData::displayTextValid(c->displaySymbol,32))||
            (!c->displayName.isEmpty()&&!SolanaTokenData::displayTextValid(c->displayName,160))||
            !textValid(c->metadataIssue,256,true)||!textValid(c->priceIssue,256,true))return false;
        if(display.contains("evidence")&&(!display["evidence"].isObject()||!decode(display["evidence"].toObject(),&c->metadataEvidence)||
            c->metadataEvidence.scope!="public:jupiter:metadata:"+c->asset.mid(7)))return false;
        if((!c->displaySymbol.isEmpty()||!c->displayName.isEmpty())&&!c->metadataEvidence.complete)return false;
    }
    return textValid(c->id,160)&&textValid(c->asset,80)&&textValid(c->valuationGroup,80)&&decimalValue(c->quantity,true)&&decimalValue(c->costBasis,true)&&!c->costBasis.startsWith('-')&&decimalValue(c->reportedValue,true)&&decimalValue(c->providerUnrealizedPnl,true)&&
        textValid(c->costCurrency,80,c->costBasis.isEmpty())&&textValid(c->reportedValueCurrency,80,c->reportedValue.isEmpty()&&c->providerUnrealizedPnl.isEmpty());
}
QJsonObject encode(const AssetSnapshot& s){QJsonArray rows;for(const auto& c:s.components)rows.append(encode(c));QJsonObject out{{"id",s.id},{"accountScope",s.accountScope},{"provider",s.provider},{"environment",s.environment},{"market",s.market},
    {"timeMs",QString::number(s.timeMs)},{"balanceComplete",s.balanceComplete},{"reconciled",s.reconciled},{"coverage",s.coverage},{"components",rows},{"evidence",encode(s.evidence)}};if(!s.connectionId.isEmpty())out["connectionId"]=s.connectionId;if(!s.connectionBinding.isEmpty())out["connectionBinding"]=s.connectionBinding;return out;}
bool decode(const QJsonObject& o,AssetSnapshot* s){
    if(o.size()!=11+int(o.contains("connectionId"))+int(o.contains("connectionBinding")))return false;
    if(o.contains("connectionId")&&(!o.value("connectionId").isString()||!textValid(o.value("connectionId").toString(),160)))return false;
    if(o.contains("connectionBinding")&&(!o.contains("connectionId")||!o.value("connectionBinding").isString()||!QRegularExpression("\\A[a-f0-9]{64}\\z").match(o.value("connectionBinding").toString()).hasMatch()))return false;
    if(!stringFields(o,{"id","accountScope","provider","environment","market","coverage"})||!timeField(o,"timeMs",&s->timeMs)||
        !o["balanceComplete"].isBool()||!o["reconciled"].isBool()||!o["components"].isArray()||o["components"].toArray().size()>2048||!o["evidence"].isObject()||!decode(o["evidence"].toObject(),&s->evidence))return false;
    s->id=o["id"].toString();s->accountScope=o["accountScope"].toString();s->provider=o["provider"].toString();s->environment=o["environment"].toString();s->market=o["market"].toString();s->coverage=o["coverage"].toString();s->connectionId=o.value("connectionId").toString();s->connectionBinding=o.value("connectionBinding").toString();
    s->balanceComplete=o["balanceComplete"].toBool();s->reconciled=o["reconciled"].toBool();QSet<QString> ids;
    for(const auto& row:o["components"].toArray()){AssetComponent c;if(!row.isObject()||!decode(row.toObject(),&c)||ids.contains(c.id))return false;ids.insert(c.id);s->components<<c;}
    // Coverage is plain-text presentation produced by controllers and contains LF.
    // Identifiers and evidence still reject every control/format character.
    return textValid(s->id,160)&&textValid(s->accountScope,160)&&textValid(s->provider,40)&&textValid(s->environment,40)&&textValid(s->market,40)&&textValid(s->coverage,2048,true,true);
}
QJsonObject encode(const AssetEvent& e){return {{"id",e.id},{"accountScope",e.accountScope},{"asset",e.asset},{"quantity",e.quantity},{"officialId",e.officialId},{"transferReference",e.transferReference},{"peerAccountScope",e.peerAccountScope},
    {"timeMs",QString::number(e.timeMs)},{"kind",int(e.kind)},{"evidence",encode(e.evidence)}};}
bool decode(const QJsonObject& o,AssetEvent* e){
    const int kind=o["kind"].toInt(-1);
    if(o.size()!=10||!stringFields(o,{"id","accountScope","asset","quantity","officialId","transferReference","peerAccountScope"})||!timeField(o,"timeMs",&e->timeMs)||o["kind"]!=QJsonValue(kind)||
        kind<0||kind>int(AssetEventKind::Unknown)||!o["evidence"].isObject()||!decode(o["evidence"].toObject(),&e->evidence))return false;
    e->id=o["id"].toString();e->accountScope=o["accountScope"].toString();e->asset=o["asset"].toString();e->quantity=o["quantity"].toString();e->officialId=o["officialId"].toString();
    e->transferReference=o["transferReference"].toString();e->peerAccountScope=o["peerAccountScope"].toString();e->kind=AssetEventKind(kind);
    return textValid(e->id,160)&&textValid(e->accountScope,160)&&textValid(e->asset,80)&&decimalValue(e->quantity)&&textValid(e->officialId,256)&&textValid(e->transferReference,256,true)&&textValid(e->peerAccountScope,160,true);
}
QJsonObject encode(const AssetQuote& q){return {{"baseAsset",q.baseAsset},{"quoteAsset",q.quoteAsset},{"price",q.price},{"evidence",encode(q.evidence)}};}
bool decode(const QJsonObject& o,AssetQuote* q){
    if(o.size()!=4||!stringFields(o,{"baseAsset","quoteAsset","price"})||!o["evidence"].isObject()||!decode(o["evidence"].toObject(),&q->evidence))return false;
    q->baseAsset=o["baseAsset"].toString();q->quoteAsset=o["quoteAsset"].toString();q->price=o["price"].toString();return textValid(q->baseAsset,80)&&textValid(q->quoteAsset,80)&&decimalValue(q->price)&&decimal::positive(q->price);
}
QJsonObject encode(const AssetOrderObservation& r){return {{"accountScope",r.accountScope},{"provider",r.provider},{"market",r.market},{"orderId",r.orderId},{"orderFamily",r.orderFamily},{"observedAtMs",QString::number(r.observedAtMs)},
    {"side",r.side},{"type",r.type},{"status",r.status},{"price",r.price},{"quantity",r.quantity},{"executed",r.executed},{"quoteAmount",r.quoteAmount},{"quantityUnit",r.quantityUnit},{"currency",r.currency},{"evidence",encode(r.evidence)}};}
bool decode(const QJsonObject& o,AssetOrderObservation* r){
    if(o.size()!=16||!stringFields(o,{"accountScope","provider","market","orderId","orderFamily","side","type","status","price","quantity","executed","quoteAmount","quantityUnit","currency"})||
        !timeField(o,"observedAtMs",&r->observedAtMs)||!o["evidence"].isObject()||!decode(o["evidence"].toObject(),&r->evidence))return false;
    r->accountScope=o["accountScope"].toString();r->provider=o["provider"].toString();r->market=o["market"].toString();r->orderId=o["orderId"].toString();r->orderFamily=o["orderFamily"].toString();
    r->side=o["side"].toString();r->type=o["type"].toString();r->status=o["status"].toString();r->price=o["price"].toString();r->quantity=o["quantity"].toString();r->executed=o["executed"].toString();r->quoteAmount=o["quoteAmount"].toString();r->quantityUnit=o["quantityUnit"].toString();r->currency=o["currency"].toString();
    return textValid(r->accountScope,160)&&textValid(r->provider,40)&&textValid(r->market,40)&&textValid(r->orderId,160)&&QStringList{"ordinary","conditional"}.contains(r->orderFamily)&&
        textValid(r->side,40,true)&&textValid(r->type,80,true)&&textValid(r->status,80)&&decimalValue(r->price,true)&&decimalValue(r->quantity,true)&&decimalValue(r->executed,true)&&decimalValue(r->quoteAmount,true)&&
        textValid(r->quantityUnit,40,true)&&textValid(r->currency,80,true);
}
QString key(const AssetSnapshot& r){return r.accountScope+QChar(0)+r.id;}
QString key(const AssetEvent& r){return r.accountScope+QChar(0)+r.officialId+QChar(0)+QString::number(int(r.kind))+QChar(0)+r.asset;}
QString key(const AssetQuote& r){return r.baseAsset+QChar(0)+r.quoteAsset+QChar(0)+r.evidence.source+QChar(0)+r.evidence.scope+QChar(0)+QString::number(r.evidence.sourceTimeMs)+QChar(0)+QString::number(r.evidence.receivedAtMs);}
QString key(const AssetOrderObservation& r){return r.accountScope+QChar(0)+r.provider+QChar(0)+r.market+QChar(0)+r.orderFamily+QChar(0)+r.orderId+QChar(0)+QString::number(r.observedAtMs);}
struct Contents {
    QList<AssetSnapshot> snapshots;QList<AssetEvent> events;QList<AssetQuote> quotes;QList<AssetOrderObservation> orders;
    int schema=2;QList<EarnAccountState> collections;QJsonObject migration;
    QList<FlowAccountState> flows;QJsonObject coinBaseline; // Optional schema 3 members.
};
// The fixed coin-performance start: its time, KRW net assets and the exact
// connections it was valued with. It is never a valuation input by itself.
bool coinBaselineValid(const QJsonObject& o){
    static const QRegularExpression digest("\\A[a-f0-9]{64}\\z");
    if(o.size()!=6||o.value("kind")!=QJsonValue("coin-baseline-v1")||!o.value("time").isString()||!o.value("netAssets").isString()||
        !o.value("policy").isString()||!o.value("connections").isArray()||!o.value("scopes").isArray())return false;
    bool ok=false;const auto time=o.value("time").toString().toLongLong(&ok);
    if(!ok||time<=0||QString::number(time)!=o.value("time").toString()||!decimal::valid(o.value("netAssets").toString().startsWith('-')?o.value("netAssets").toString().mid(1):o.value("netAssets").toString()))return false;
    const auto connections=o.value("connections").toArray(),scopes=o.value("scopes").toArray();
    if(connections.isEmpty()||connections.size()>64||scopes.isEmpty()||scopes.size()>64)return false;
    for(const auto& v:connections){const auto c=v.toObject();bool timeOk=false;const auto at=c.value("snapshot").toString().toLongLong(&timeOk);
        if(!v.isObject()||c.size()!=3||c.value("id").toString().isEmpty()||c.value("id").toString().size()>160||!digest.match(c.value("binding").toString()).hasMatch()||!timeOk||at<=0||at>time)return false;}
    for(const auto& v:scopes)if(!digest.match(v.toString()).hasMatch())return false;
    return true;
}
bool migrationValid(const QJsonObject& o){
    static const QRegularExpression digest("\\A[a-f0-9]{64}\\z");
    static const QRegularExpression name("\\Aassets\\.keefi\\.pre-schema3-[a-f0-9]{64}\\.keefi\\z");
    static const QRegularExpression archive("\\Aassets\\.keefi\\.archive-[a-f0-9]{32}\\.keefi\\z");
    if(o.size()!=4||!digest.match(o.value("sourceDigest").toString()).hasMatch()||!name.match(o.value("backup").toString()).hasMatch()||
        !o.value("archives").isArray()||o.value("archives").toArray().size()>256)return false;
    QMap<QString,QString> originals{{o.value("backup").toString(),o.value("sourceDigest").toString()}};
    for(const auto& v:o.value("archives").toArray()){
        if(!v.isObject())return false;const auto row=v.toObject();const auto file=row.value("name").toString();
        if(row.size()!=2||!archive.match(file).hasMatch()||originals.contains(file)||!digest.match(row.value("digest").toString()).hasMatch())return false;
        originals.insert(file,row.value("digest").toString());
    }
    if(!o.value("rewriteEvidence").isObject())return false;
    const auto evidence=o.value("rewriteEvidence").toObject();
    if(evidence.size()!=2||evidence.value("version")!=QJsonValue(1)||!evidence.value("files").isArray()||evidence.value("files").toArray().size()!=originals.size())return false;
    QSet<QString> seen;
    for(const auto& value:evidence.value("files").toArray()){
        if(!value.isObject())return false;const auto row=value.toObject();const auto file=row.value("name").toString();
        if(row.size()!=3||!originals.contains(file)||seen.contains(file)||row.value("sourceDigest").toString()!=originals.value(file)
            ||!digest.match(row.value("cipherDigest").toString()).hasMatch())return false;
        seen.insert(file);
    }
    return seen.size()==originals.size();
}
template<class T> bool parseRows(const QJsonArray& array,QList<T>* result,qsizetype maximum){
    if(array.size()>maximum)return false;QSet<QString> ids;
    for(const auto& value:array){T r;if(!value.isObject()||!decode(value.toObject(),&r)||ids.contains(key(r)))return false;ids.insert(key(r));result->append(r);}return true;
}
bool unpack(const SecureBuffer& buffer,Contents* output,bool legacy=false){
    auto bytes=QByteArray(reinterpret_cast<const char*>(buffer.data()),qsizetype(buffer.size()));QJsonParseError error;const auto doc=QJsonDocument::fromJson(bytes,&error);
    const bool canonical=doc.toJson(QJsonDocument::Compact)==bytes;sodium_memzero(bytes.data(),std::size_t(bytes.size()));
    if(error.error!=QJsonParseError::NoError||!doc.isObject()||!canonical)return false;const auto o=doc.object();
    if(o["schema_version"]==QJsonValue(1))for(const auto& value:o["snapshots"].toArray())if(value.toObject().contains("connectionId")||value.toObject().contains("connectionBinding"))return false;
    const bool v3=o["schema_version"]==QJsonValue(3);
    if(v3){
        const int optional=int(o.contains("flows"))+int(o.contains("coinBaseline"));
        if(o.size()!=7+optional||!o.value("collections").isArray()||o.value("collections").toArray().size()>EarnTracking::MaxAccounts||
            !o.value("migration").isObject()||!(legacy?storage_detail::legacyAssetMigrationValid(o.value("migration").toObject()):migrationValid(o.value("migration").toObject())))return false;
        if(o.contains("flows")){
            if(!o.value("flows").isArray()||o.value("flows").toArray().size()>FundFlowTracking::MaxAccounts)return false;
            QSet<QString> flowScopes;
            for(const auto& value:o.value("flows").toArray()){
                FlowAccountState state;if(!value.isObject()||!FundFlowTracking::decode(value.toObject(),&state)||flowScopes.contains(state.accountScope))return false;
                flowScopes.insert(state.accountScope);output->flows.append(state);
            }
        }
        if(o.contains("coinBaseline")){if(!o.value("coinBaseline").isObject()||!coinBaselineValid(o.value("coinBaseline").toObject()))return false;output->coinBaseline=o.value("coinBaseline").toObject();}
        QSet<QString> scopes;
        for(const auto& value:o.value("collections").toArray()){
            EarnAccountState state;if(!value.isObject()||!EarnTracking::decode(value.toObject(),&state)||scopes.contains(state.accountScope))return false;
            scopes.insert(state.accountScope);output->collections.append(state);
        }
        output->schema=3;output->migration=o.value("migration").toObject();
    }
    return ((v3&&o.size()>=7)||(!v3&&o.size()==5&&(o["schema_version"]==QJsonValue(1)||o["schema_version"]==QJsonValue(2))))&&o["snapshots"].isArray()&&o["events"].isArray()&&o["quotes"].isArray()&&o["orders"].isArray()&&
        parseRows(o["snapshots"].toArray(),&output->snapshots,AssetsLedger::MaxSnapshots)&&parseRows(o["events"].toArray(),&output->events,AssetsLedger::MaxEvents)&&
        parseRows(o["quotes"].toArray(),&output->quotes,AssetsLedger::MaxQuotes)&&parseRows(o["orders"].toArray(),&output->orders,AssetsLedger::MaxOrders);
}
SecureBuffer pack(const Contents& c){
    const auto array=[](const auto& rows){QJsonArray out;for(const auto& r:rows)out.append(encode(r));return out;};
    QJsonObject root{{"schema_version",c.schema},{"snapshots",array(c.snapshots)},{"events",array(c.events)},{"quotes",array(c.quotes)},{"orders",array(c.orders)}};
    if(c.schema==3){QJsonArray collections;for(const auto& state:c.collections)collections.append(EarnTracking::encode(state));root["collections"]=collections;root["migration"]=c.migration;
        if(!c.flows.isEmpty()){QJsonArray flows;for(const auto& state:c.flows)flows.append(FundFlowTracking::encode(state));root["flows"]=flows;}
        if(!c.coinBaseline.isEmpty())root["coinBaseline"]=c.coinBaseline;}
    auto bytes=QJsonDocument(root).toJson(QJsonDocument::Compact);
    if(bytes.size()>AssetsLedger::MaxFileBytes-HeaderBytes-16){sodium_memzero(bytes.data(),std::size_t(bytes.size()));return {};}
    SecureBuffer result(bytes.constData(),std::size_t(bytes.size()));sodium_memzero(bytes.data(),std::size_t(bytes.size()));return result;
}
// alreadyVerified: the same rows passed this exact encode/decode check in an earlier merge call.
template<class T> bool merge(QList<T>* old,const QList<T>& added,qsizetype maximum,QString* error,bool alreadyVerified=false){
    if(added.size()>maximum)return fail(error,"자산 원장 보존 상한에 도달했습니다. 기존 기록은 삭제하지 않았습니다.");
    QMap<QString,qsizetype> indices;for(qsizetype i=0;i<old->size();++i)indices.insert(key(old->at(i)),i);
    for(const auto& r:added){T verified;if(!alreadyVerified&&!decode(encode(r),&verified))return fail(error,"자산 원장의 값·출처·범위 형식이 올바르지 않습니다.");
        const auto k=key(r);if(indices.contains(k)){if(old->at(indices.value(k))!=r)return fail(error,"같은 공식 기록 식별자에 다른 내용이 있어 덮어쓰기를 중단했습니다.");}
        else{indices.insert(k,old->size());old->append(r);}}
    return old->size()<=maximum||fail(error,"자산 원장 보존 상한에 도달했습니다. 기존 기록은 삭제하지 않았습니다.");
}
bool readEncrypted(const QString& path,QByteArray* bytes,QString* error){
    if(!privatePath(path,false,error))return false;QFile file(path);
#ifdef Q_OS_UNIX
    const int fd=::open(QFile::encodeName(path).constData(),O_RDONLY|O_NOFOLLOW|O_CLOEXEC);struct stat st{};
    if(fd<0)return fail(error,"자산 원장 파일을 열지 못했습니다.");
    if(::fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_uid!=::geteuid()||(st.st_mode&0077)){::close(fd);return fail(error,"자산 원장 접근 권한이 안전하지 않습니다.");}
    if(!file.open(fd,QIODevice::ReadOnly,QFileDevice::AutoCloseHandle)){::close(fd);return fail(error,"자산 원장을 읽지 못했습니다.");}
#else
    if(!file.open(QIODevice::ReadOnly))return fail(error,"자산 원장을 읽지 못했습니다.");
#endif
    if(file.size()<HeaderBytes+16||file.size()>AssetsLedger::MaxFileBytes)return fail(error,"자산 원장 크기 상한 또는 형식 오류입니다.");
    *bytes=file.read(AssetsLedger::MaxFileBytes+1);return (file.error()==QFile::NoError&&bytes->size()==file.size())||fail(error,"자산 원장 전체 읽기에 실패했습니다.");
}
bool decrypt(const QByteArray& bytes,const SecureBuffer& key,SecureBuffer* plain,QString* error){
    const auto* p=reinterpret_cast<const unsigned char*>(bytes.constData());
    if(key.size()!=32||bytes.size()<HeaderBytes+16||bytes.size()>AssetsLedger::MaxFileBytes||storage_detail::formatOf(bytes,storage_detail::Kind::Assets)==storage_detail::Format::Invalid||qFromLittleEndian<quint16>(p+8)!=1||qFromLittleEndian<quint16>(p+10)!=0||qFromLittleEndian<quint32>(p+36)!=bytes.size()-HeaderBytes)return fail(error,"지원하지 않거나 손상된 자산 원장 형식입니다.");
    SecureBuffer result(std::size_t(bytes.size()-HeaderBytes-16));unsigned long long size=0;
    if(crypto_aead_xchacha20poly1305_ietf_decrypt(result.data(),&size,nullptr,p+HeaderBytes,bytes.size()-HeaderBytes,p,HeaderBytes,p+12,key.data())||size!=result.size())return fail(error,"자산 원장 인증에 실패했습니다. 다른 금고의 원장일 수 있습니다.");
    *plain=std::move(result);return true;
}
QByteArray encrypt(const SecureBuffer& plain,const SecureBuffer& key){
    QByteArray out(HeaderBytes+qsizetype(plain.size())+16,'\0');auto* p=reinterpret_cast<unsigned char*>(out.data());std::memcpy(p,"KEEFIAST",8);qToLittleEndian<quint16>(1,p+8);randombytes_buf(p+12,24);qToLittleEndian<quint32>(quint32(plain.size()+16),p+36);
    unsigned long long size=0;if(key.size()!=32||crypto_aead_xchacha20poly1305_ietf_encrypt(p+HeaderBytes,&size,plain.data(),plain.size(),p,HeaderBytes,nullptr,p+12,key.data())||size!=plain.size()+16)return {};return out;
}
bool persistCipher(const QString& path,const QByteArray& bytes,const QByteArray& previous,bool creating,QByteArray* digest,QString* error,const std::atomic_bool* cancelled=nullptr){
    const auto unchanged=[&]{if(creating)return (!QFileInfo::exists(path)&&!QFileInfo(path).isSymLink())||fail(error,"자산 원장 파일이 이미 생겨 저장을 중단했습니다.");QByteArray bytes;return readEncrypted(path,&bytes,error)&&(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256)==previous||fail(error,"자산 원장이 외부에서 변경되어 저장을 중단했습니다."));};
    if(cancelled&&cancelled->load())return fail(error,"잠금 또는 종료로 기록 저장을 중단했습니다.");
    if(!unchanged()||bytes.isEmpty())return false;
    StagedStorageFile staged(QFileInfo(path).absolutePath()+"/.keefi-assets-XXXXXX");
    auto& temp = staged.file();
    if(!temp.open())return fail(error,"자산 원장 임시 파일을 만들지 못했습니다.");
#ifdef Q_OS_WIN
    if(!privateWindowsPath(temp.fileName(),true))return fail(error,"자산 원장 임시 파일을 보호하지 못했습니다.");
#else
    if(!temp.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner))return fail(error,"자산 원장 임시 파일을 보호하지 못했습니다.");
#endif
    if(!privatePath(temp.fileName(),false,error)||temp.write(bytes)!=bytes.size()||!temp.flush())return fail(error,"자산 원장 저장에 실패했습니다. 기존 파일을 보존했습니다.");
#ifdef Q_OS_UNIX
    if(::fsync(temp.handle()))return fail(error,"자산 원장을 디스크에 반영하지 못했습니다.");
#elif defined(Q_OS_WIN)
    if(!FlushFileBuffers(reinterpret_cast<HANDLE>(_get_osfhandle(temp.handle()))))return fail(error,"자산 원장을 디스크에 반영하지 못했습니다.");
#endif
    if(!temp.seek(0))return fail(error,"자산 원장 검증 읽기에 실패했습니다.");const auto readback=temp.read(AssetsLedger::MaxFileBytes+1);
    if(readback!=bytes)return fail(error,"자산 원장 저장 후 검증에 실패했습니다.");
    if(cancelled&&cancelled->load())return fail(error,"잠금 또는 종료로 교체 전 저장을 중단했습니다.");
    if(!unchanged()||!privatePath(QFileInfo(path).absolutePath(),true,error))return false;const auto name=temp.fileName();temp.close();
#ifdef Q_OS_UNIX
    const int directory=::open(QFile::encodeName(QFileInfo(path).absolutePath()).constData(),O_RDONLY|O_CLOEXEC);if(directory<0)return fail(error,"자산 원장 폴더를 열지 못했습니다.");
    const int renamed=::rename(QFile::encodeName(name).constData(),QFile::encodeName(path).constData());const int synced=renamed ? 0 : ::fsync(directory);::close(directory);
    if(renamed)return fail(error,"자산 원장 원자 교체에 실패했습니다.");if(synced)return fail(error,"교체 후 디스크 반영을 확인하지 못했습니다. 원장을 다시 열어 주세요.");
#elif defined(Q_OS_WIN)
    if(!staged.publish(path,!creating))return fail(error,"자산 원장 원자 교체에 실패했습니다.");
#else
    return fail(error,"이 운영체제의 원장 원자 교체는 지원하지 않습니다.");
#endif
    QByteArray finalBytes;if(!readEncrypted(path,&finalBytes,error)||finalBytes!=bytes)return fail(error,"교체된 원장을 확인하지 못했습니다. 다시 열어 주세요.");*digest=QCryptographicHash::hash(bytes,QCryptographicHash::Sha256);return true;
}
bool persist(const QString& path,const SecureBuffer& key,const SecureBuffer& plain,const QByteArray& previous,bool creating,QByteArray* digest,QString* error){
    const auto bytes=encrypt(plain,key);SecureBuffer check;
    if(bytes.isEmpty()||!decrypt(bytes,key,&check,error)||check.size()!=plain.size()||sodium_memcmp(check.data(),plain.data(),plain.size()))return fail(error,"자산 원장 암호화 검증에 실패했습니다.");
    return persistCipher(path,bytes,previous,creating,digest,error);
}
// Range filtering is shared by synchronous verification and deferred UI reads.
// Only owner-thread state contains decoded records or a derived ledger key.
struct WindowRows {
    qint64 fromMs=0,toMs=0,nextEligibleMs=0;
    QMap<QString,AssetSnapshot> snapshots;
    QMap<QString,AssetQuote> quotes;
    bool future(qint64 time){
        if(time<=toMs)return false;
        if(!nextEligibleMs||time<nextEligibleMs)nextEligibleMs=time;
        return true;
    }
    bool consume(const SecureBuffer& plain,QString* error,bool legacy=false){
        Contents contents;if(!unpack(plain,&contents,legacy))return fail(error,"자산 이력의 형식을 확인하지 못했습니다.");
        for(const auto& row:contents.snapshots){
            if(row.timeMs<fromMs||future(std::max({row.timeMs,row.evidence.sourceTimeMs,row.evidence.receivedAtMs})))continue;
            const auto id=key(row);const auto previous=snapshots.constFind(id);
            if(previous!=snapshots.cend()&&previous.value()!=row)return fail(error,"보관 이력에 같은 식별자의 다른 잔고 기록이 있습니다.");
            snapshots.insert(id,row);
        }
        for(const auto& row:contents.quotes){
            if(row.evidence.receivedAtMs<fromMs||future(qMax(row.evidence.receivedAtMs,row.evidence.sourceTimeMs)))continue;
            const auto id=key(row);const auto previous=quotes.constFind(id);
            if(previous!=quotes.cend()&&previous.value()!=row)return fail(error,"보관 이력에 같은 식별자의 다른 시세 기록이 있습니다.");
            quotes.insert(id,row);
        }
        return true;
    }
    AssetsLedger::HistoryWindow result()const{
        AssetsLedger::HistoryWindow out;out.snapshots=snapshots.values();out.quotes=quotes.values();out.nextEligibleMs=nextEligibleMs;
        std::stable_sort(out.snapshots.begin(),out.snapshots.end(),[](const auto& a,const auto& b){return a.timeMs<b.timeMs;});
        std::stable_sort(out.quotes.begin(),out.quotes.end(),[](const auto& a,const auto& b){return a.evidence.receivedAtMs<b.evidence.receivedAtMs;});
        return out;
    }
};
struct WindowRead {
    WindowRows rows;SecureBuffer key;QStringList paths;QString error;
    bool consumeOpen=false;AssetsLedger::HistoryWindowCallback callback;
};
struct CipherRead {
    QString path,error;QByteArray bytes;std::atomic_bool cancelled=false;bool success=false;
};
struct CipherSave {
    QString path,archivePath,message;QByteArray bytes,archiveBytes,previous,digest;
    quint64 receipt=0;
    std::atomic_bool cancelled=false;bool success=false;std::unique_ptr<QLockFile> lock;
};
} // namespace

struct AssetsLedger::Private {
    Vault* vault=nullptr;QString path;SecureBuffer key,plain;QByteArray digest,residualIdentity;std::unique_ptr<QLockFile> fileLock;bool legacy=false;
    SecureBuffer pendingPlain;std::shared_ptr<CipherSave> save;QThread* writer=nullptr;quint64 generation=0;
    std::unique_ptr<WindowRead> window;std::shared_ptr<CipherRead> cipherRead;QThread* reader=nullptr;quint64 readGeneration=0;
    ~Private(){if(save)save->cancelled=true;if(writer){writer->wait();delete writer;}if(cipherRead)cipherRead->cancelled=true;if(reader){reader->wait();delete reader;}}
    void release(){if(fileLock&&fileLock->isLocked()){const auto identity=lockFileIdentity(path+".lock");fileLock->unlock();residualIdentity=!identity.isEmpty()&&lockFileIdentity(path+".lock")==identity?identity:QByteArray{};}fileLock.reset();}
};
AssetsLedger::AssetsLedger(Vault* vault,QString path,QObject* parent):QObject(parent),d_(std::make_unique<Private>()){
    Q_ASSERT(vault);d_->vault=vault;d_->path=QFileInfo(path).absoluteFilePath();connect(vault,&Vault::aboutToLock,this,&AssetsLedger::lock);connect(vault,&QObject::destroyed,this,[this]{lock();d_->vault=nullptr;});
}
AssetsLedger::~AssetsLedger(){lock();}
bool AssetsLedger::isOpen()const{return d_->vault&&d_->vault->isUnlocked()&&d_->key.size()==32&&!d_->plain.empty();}
bool AssetsLedger::exists()const{return QFileInfo::exists(d_->path);}
QString AssetsLedger::filePath()const{return d_->path;}
void AssetsLedger::lock(){cancelWindowRead();const bool was=!d_->key.empty()||!d_->plain.empty();++d_->generation;if(d_->save)d_->save->cancelled=true;d_->pendingPlain.clear();d_->key.clear();d_->plain.clear();d_->digest.clear();d_->legacy=false;d_->release();if(was){emit changed();emit locked();}}
bool AssetsLedger::open(bool create,QString* error){
    if(isWriting())return isOpen()||fail(error,"이전 암호문 저장을 정리 중입니다. 잠시 후 다시 여세요.");
    if(error)error->clear();if(isOpen())return true;
    if(!d_->vault||!d_->vault->isUnlocked())return fail(error,"자산 원장을 열려면 금고 잠금을 해제해 주세요.");
    const bool present=exists();if(!present&&!create)return fail(error,"저장된 자산 원장이 없습니다.");
    try{
        if(!Vault::prepareProfileDirectory(QFileInfo(d_->path).absolutePath(),error)||QFileInfo(d_->path).isSymLink()||QFileInfo(d_->path+".lock").isSymLink())return fail(error,"자산 원장 경로가 안전하지 않습니다.");
        if(!d_->residualIdentity.isEmpty()){if(lockFileIdentity(d_->path+".lock")==d_->residualIdentity){QLockFile residual(d_->path+".lock");if(!residual.removeStaleLockFile())return fail(error,"이전 원장 파일 잠금을 해제하지 못했습니다.");}d_->residualIdentity.clear();}
        d_->fileLock=std::make_unique<QLockFile>(d_->path+".lock");d_->fileLock->setStaleLockTime(0);if(!d_->fileLock->tryLock(0)){lock();return fail(error,"다른 앱이 자산 원장을 사용하고 있습니다.");}
        // Existing data never causes a missing key to be invented or an older vault to be rewritten.
        QByteArray bytes;
        if(present&&!readEncrypted(d_->path,&bytes,error)){lock();return false;}
        const auto format=present?storage_detail::formatOf(bytes,storage_detail::Kind::Assets):storage_detail::Format::Current;
        d_->legacy=format==storage_detail::Format::Legacy;
        bool derived=false;
        if(!d_->vault->withAssetsLedgerKey(create&&!present,[&](const SecureBuffer& key){
            derived=storage_detail::deriveStoreKey(key,storage_detail::Kind::Assets,format,&d_->key,error);
        },error)||!derived){lock();return false;}
        if(present){Contents contents;if(!decrypt(bytes,d_->key,&d_->plain,error)||!unpack(d_->plain,&contents,d_->legacy)){lock();return fail(error,"자산 원장 인증·스키마 검증에 실패했습니다.");}d_->digest=QCryptographicHash::hash(bytes,QCryptographicHash::Sha256);}
        else{d_->plain=pack(Contents{});if(!persist(d_->path,d_->key,d_->plain,{},true,&d_->digest,error)){lock();return false;}}
        emit changed();return true;
    }catch(const std::exception&){lock();return fail(error,"자산 원장 보호 메모리 또는 암호화 처리에 실패했습니다.");}
}
bool AssetsLedger::append(const QList<AssetSnapshot>& s,const QList<AssetEvent>& e,const QList<AssetQuote>& q,QString* error){return appendBatch(s,e,q,{},error);}
bool AssetsLedger::appendBatch(const QList<AssetSnapshot>& s,const QList<AssetEvent>& e,const QList<AssetQuote>& q,const QList<AssetOrderObservation>& orders,QString* error){
    if(isWriting())return fail(error,"자산 기록을 저장 중입니다.");
    if(error)error->clear();if(!isOpen())return fail(error,"자산 원장이 잠겨 있습니다.");
    if(!storage_detail::rejectLegacyWrite(d_->legacy,error))return false;
    try{Contents next;if(!unpack(d_->plain,&next)){lock();return fail(error,"메모리 원장 검증에 실패했습니다.");}
        if(!merge(&next.snapshots,s,MaxSnapshots,error)||!merge(&next.events,e,MaxEvents,error)||!merge(&next.quotes,q,MaxQuotes,error)||!merge(&next.orders,orders,MaxOrders,error))return false;
        std::stable_sort(next.snapshots.begin(),next.snapshots.end(),[](const auto& a,const auto& b){return a.timeMs<b.timeMs;});
        std::stable_sort(next.events.begin(),next.events.end(),[](const auto& a,const auto& b){return a.timeMs<b.timeMs;});
        std::stable_sort(next.orders.begin(),next.orders.end(),[](const auto& a,const auto& b){return a.observedAtMs<b.observedAtMs;});
        auto candidate=pack(next);if(candidate.empty())return fail(error,"자산 원장 크기 상한에 도달했습니다. 기존 기록은 삭제하지 않았습니다.");
        if(candidate.size()==d_->plain.size()&&!sodium_memcmp(candidate.data(),d_->plain.data(),candidate.size()))return true;
        if(!persist(d_->path,d_->key,candidate,d_->digest,false,&d_->digest,error)){lock();return false;}
        d_->plain=std::move(candidate);emit changed();return true;
    }catch(const std::exception&){lock();return fail(error,"자산 원장 저장 실패로 원장을 잠갔습니다.");}
}
QList<AssetSnapshot> AssetsLedger::snapshots()const{Contents c;if(isOpen())unpack(d_->plain,&c,d_->legacy);return c.snapshots;}
bool AssetsLedger::replaceLatest(const QList<AssetSnapshot>& snapshots,const QList<AssetQuote>& quotes,QString* error,bool background){
    if(isWriting())return fail(error,"최신 자산을 저장 중입니다.");
    if(error)error->clear();if(!isOpen())return fail(error,"현재 자산 저장소가 잠겨 있습니다.");
    if(!storage_detail::rejectLegacyWrite(d_->legacy,error))return false;
    try{Contents next;if(!merge(&next.snapshots,snapshots,MaxSnapshots,error)||!merge(&next.quotes,quotes,MaxQuotes,error))return false;
        auto candidate=pack(next);if(candidate.empty())return fail(error,"최신 자산 저장 상한을 넘었습니다. 마지막 저장 자료를 보존했습니다.");
        if(background)return persistAsync(std::move(candidate),{},{},error);
        if(!persist(d_->path,d_->key,candidate,d_->digest,false,&d_->digest,error)){lock();return false;}
        d_->plain=std::move(candidate);emit changed();return true;
    }catch(const std::exception&){lock();return fail(error,"최신 자산을 저장하지 못했습니다.");}
}
QStringList AssetsLedger::archiveFiles()const{
    return QDir(QFileInfo(d_->path).absolutePath()).entryList(storage_detail::archivePatterns(QFileInfo(d_->path).fileName()),QDir::Files,QDir::Name);
}
bool AssetsLedger::readWindow(qint64 fromMs,qint64 toMs,HistoryWindow* output,QString* error)const{
    if(error)error->clear();if(output)*output={};
    if(!output||fromMs<=0||toMs<fromMs)return fail(error,"이력 조회 시각 범위가 올바르지 않습니다.");
    if(!d_->vault||!d_->vault->isUnlocked())return fail(error,"금고를 열면 오늘 이력을 조회할 수 있습니다.");
    try{
        const auto archives=archiveFiles();const bool opened=isOpen();
        if(QFileInfo(d_->path).isSymLink())return fail(error,"자산 원장 경로가 안전하지 않습니다.");
        if(!opened&&!exists()&&archives.isEmpty())return true; // Never invent a key or empty file.
        if(archives.size()>256)return fail(error,"자산 이력 보관 파일 수가 지원 범위를 넘었습니다.");
        if(!privatePath(QFileInfo(d_->path).absolutePath(),true,error))return false;
        SecureBuffer root;
        if(!d_->vault->withAssetsLedgerKey(false,[&](const SecureBuffer& key){root=SecureBuffer(key.data(),key.size());},error))return false;
        WindowRows rows;rows.fromMs=fromMs;rows.toMs=toMs;
        const auto consumeFile=[&](const QString& path){
            QByteArray bytes;SecureBuffer plain;
            return readEncrypted(path,&bytes,error)&&storage_detail::decodeStore(bytes,storage_detail::Kind::Assets,root,&plain,error)
                &&rows.consume(plain,error,storage_detail::formatOf(bytes,storage_detail::Kind::Assets)==storage_detail::Format::Legacy);
        };
        if(opened){if(!rows.consume(d_->plain,error,d_->legacy))return false;}
        else if(exists()&&!consumeFile(d_->path))return false;
        const QDir directory(QFileInfo(d_->path).absolutePath());
        for(const auto& name:archives)if(!consumeFile(directory.filePath(name)))return false;
        *output=rows.result();return true;
    }catch(const std::exception&){return fail(error,"보호된 자산 이력을 읽지 못했습니다.");}
}
void AssetsLedger::readWindowAsync(qint64 fromMs,qint64 toMs,HistoryWindowCallback callback){
    cancelWindowRead();
    auto request=std::make_unique<WindowRead>();request->rows.fromMs=fromMs;request->rows.toMs=toMs;request->callback=std::move(callback);
    try{
        if(fromMs<=0||toMs<fromMs)request->error=QStringLiteral("이력 조회 시각 범위가 올바르지 않습니다.");
        else if(!d_->vault||!d_->vault->isUnlocked())request->error=QStringLiteral("금고를 열면 오늘 이력을 조회할 수 있습니다.");
        else{
            const auto archives=archiveFiles();const bool opened=isOpen();const QDir directory(QFileInfo(d_->path).absolutePath());
            if(QFileInfo(d_->path).isSymLink())request->error=QStringLiteral("자산 원장 경로가 안전하지 않습니다.");
            else if(archives.size()>256)request->error=QStringLiteral("자산 이력 보관 파일 수가 지원 범위를 넘었습니다.");
            else if((opened||exists()||!archives.isEmpty())&&privatePath(directory.path(),true,&request->error)){
                request->consumeOpen=opened;
                d_->vault->withAssetsLedgerKey(false,[&](const SecureBuffer& key){request->key=SecureBuffer(key.data(),key.size());},&request->error);
                if(!opened&&exists())request->paths.append(d_->path);
                for(const auto& name:archives)request->paths.append(directory.filePath(name));
            }
        }
    }catch(const std::exception&){request->error=QStringLiteral("보호된 자산 이력을 읽지 못했습니다.");}
    d_->window=std::move(request);const auto generation=d_->readGeneration;
    QTimer::singleShot(0,this,[this,generation]{if(generation==d_->readGeneration)readWindowStep();});
}
void AssetsLedger::cancelWindowRead(){
    ++d_->readGeneration;if(d_->cipherRead)d_->cipherRead->cancelled=true;
    // This synchronously erases the owner-thread key and releases decoded rows.
    d_->window.reset();
}
void AssetsLedger::readWindowStep(){
    if(!d_->window||d_->reader)return;
    auto& request=*d_->window;
    if(!request.error.isEmpty()){finishWindowRead(false,request.error);return;}
    if(!d_->vault||!d_->vault->isUnlocked()){cancelWindowRead();return;}
    try{
        if(request.consumeOpen){
            request.consumeOpen=false;
            if(!isOpen()||!request.rows.consume(d_->plain,&request.error,d_->legacy)){finishWindowRead(false,request.error);return;}
            const auto generation=d_->readGeneration;
            QTimer::singleShot(0,this,[this,generation]{if(generation==d_->readGeneration)readWindowStep();});return;
        }
        if(request.paths.isEmpty()){finishWindowRead(true,{});return;}
        auto read=std::make_shared<CipherRead>();read->path=request.paths.takeFirst();d_->cipherRead=read;const auto generation=d_->readGeneration;
        // File I/O workers receive ciphertext only, as with persistAsync. Vault,
        // keys, authentication, decoded records and callbacks stay on this thread.
        d_->reader=QThread::create([read]{if(!read->cancelled)read->success=readEncrypted(read->path,&read->bytes,&read->error);});
        connect(d_->reader,&QThread::finished,this,[this,read,generation]{
            auto* reader=d_->reader;d_->reader=nullptr;d_->cipherRead.reset();reader->deleteLater();
            if(generation==d_->readGeneration&&d_->window&&!read->cancelled&&d_->vault&&d_->vault->isUnlocked()){
                QString error=read->error;bool ok=false;
                try{SecureBuffer plain;ok=read->success&&storage_detail::decodeStore(read->bytes,storage_detail::Kind::Assets,d_->window->key,&plain,&error)
                    &&d_->window->rows.consume(plain,&error,storage_detail::formatOf(read->bytes,storage_detail::Kind::Assets)==storage_detail::Format::Legacy);}
                catch(const std::exception&){error=QStringLiteral("보호된 자산 이력을 읽지 못했습니다.");}
                if(!ok){finishWindowRead(false,error);return;}
            }
            // Yield between files. A cancelled worker may have a newer request
            // waiting; only that request's generation may continue.
            const auto nextGeneration=d_->readGeneration;
            QTimer::singleShot(0,this,[this,nextGeneration]{if(nextGeneration==d_->readGeneration)readWindowStep();});
        });
        d_->reader->start();
    }catch(const std::exception&){finishWindowRead(false,QStringLiteral("보호된 자산 이력을 읽지 못했습니다."));}
}
void AssetsLedger::finishWindowRead(bool success,const QString& error){
    if(!d_->window)return;
    auto request=std::move(d_->window);auto callback=std::move(request->callback);
    HistoryWindow output;auto message=error;
    try{if(success)output=request->rows.result();}
    catch(const std::exception&){success=false;output={};message=QStringLiteral("보호된 자산 이력을 읽지 못했습니다.");}
    if(!success&&message.isEmpty())message=QStringLiteral("오늘 기록을 읽지 못했습니다.");
    request.reset(); // No derived key remains while client code handles the result.
    if(callback)callback(success,std::move(output),message);
}
bool AssetsLedger::appendWithArchive(const QList<AssetSnapshot>& s,const QList<AssetEvent>& e,const QList<AssetQuote>& q,const QList<AssetOrderObservation>& orders,QString* error,bool background){
    if(isWriting())return fail(error,"자산 이력을 저장 중입니다.");
    if(error)error->clear();if(!isOpen())return fail(error,"자산 원장이 잠겨 있습니다.");
    if(!storage_detail::rejectLegacyWrite(d_->legacy,error))return false;
    try{
        Contents added,combined;
        if(!merge(&added.snapshots,s,MaxSnapshots,error)||!merge(&added.events,e,MaxEvents,error)||!merge(&added.quotes,q,MaxQuotes,error)||!merge(&added.orders,orders,MaxOrders,error))return false;
        if(!unpack(d_->plain,&combined)){lock();return fail(error,"자산 원장 형식을 확인하지 못했습니다.");}
        // The added rows were verified just above; only duplicate/conflict checks remain.
        if(!merge(&combined.snapshots,s,2*MaxSnapshots,error,true)||!merge(&combined.events,e,2*MaxEvents,error,true)||!merge(&combined.quotes,q,2*MaxQuotes,error,true)||!merge(&combined.orders,orders,2*MaxOrders,error,true))return false;
        std::stable_sort(combined.snapshots.begin(),combined.snapshots.end(),[](const auto& a,const auto& b){return a.timeMs<b.timeMs;});
        std::stable_sort(combined.events.begin(),combined.events.end(),[](const auto& a,const auto& b){return a.timeMs<b.timeMs;});
        std::stable_sort(combined.orders.begin(),combined.orders.end(),[](const auto& a,const auto& b){return a.observedAtMs<b.observedAtMs;});
        auto candidate=pack(combined);const bool full=candidate.empty()||combined.snapshots.size()>MaxSnapshots||combined.events.size()>MaxEvents||combined.quotes.size()>MaxQuotes||combined.orders.size()>MaxOrders;
        QString archive;QByteArray archiveBytes;
        if(full){
            // Cursor/pending/identity state survives history rollover. Merely
            // packing this append's new rows would lose the committed collectors.
            added.schema=combined.schema;added.collections=combined.collections;added.migration=combined.migration;
            added.flows=combined.flows;added.coinBaseline=combined.coinBaseline;
            candidate=pack(added);if(candidate.empty())return fail(error,"이번 기록 자체가 저장 상한을 넘었습니다. 기존 기록을 보존했습니다.");
            if(archiveFiles().size()>=256)return fail(error,"암호화 기록 보관 파일 256개에 도달했습니다. 기존 기록은 유지하며 보관 공간 정리가 필요합니다.");
            archive=d_->path+".archive-"+QUuid::createUuid().toString(QUuid::Id128)+".keefi";
            if(background){archiveBytes=encrypt(d_->plain,d_->key);if(archiveBytes.isEmpty())return fail(error,"과거 기록의 암호화를 준비하지 못했습니다.");}
            else{QByteArray before;if(!readEncrypted(d_->path,&before,error)||QCryptographicHash::hash(before,QCryptographicHash::Sha256)!=d_->digest){lock();return fail(error,"보관 전 원장 변경을 감지했습니다. 저장을 중단했습니다.");}
                QByteArray archiveDigest;if(!persist(archive,d_->key,d_->plain,{},true,&archiveDigest,error))return false;}
        }
        if(candidate.size()==d_->plain.size()&&!sodium_memcmp(candidate.data(),d_->plain.data(),candidate.size()))return true;
        if(background)return persistAsync(std::move(candidate),archive,archiveBytes,error);
        if(!persist(d_->path,d_->key,candidate,d_->digest,false,&d_->digest,error)){lock();return false;}
        d_->plain=std::move(candidate);emit changed();return true;
    }catch(const std::exception&){lock();return fail(error,"기록 보관·저장에 실패했습니다. 기존 파일을 확인하세요.");}
}
bool AssetsLedger::isWriting()const{return d_->writer!=nullptr;}
bool AssetsLedger::persistAsync(SecureBuffer&& candidate,const QString& archivePath,const QByteArray& archiveBytes,QString* error,quint64 receipt){
    if(isWriting()||!isOpen()||!d_->fileLock)return fail(error,"암호문 저장을 시작할 수 없습니다.");
    if(!storage_detail::rejectLegacyWrite(d_->legacy,error))return false;
    auto save=std::make_shared<CipherSave>();save->path=d_->path;save->previous=d_->digest;save->bytes=encrypt(candidate,d_->key);
    save->receipt=receipt;
    save->archivePath=archivePath;save->archiveBytes=archiveBytes;SecureBuffer checked;
    if(save->bytes.isEmpty()||!decrypt(save->bytes,d_->key,&checked,error)||checked.size()!=candidate.size()||sodium_memcmp(checked.data(),candidate.data(),candidate.size()))return fail(error,"저장 전 암호문 검증에 실패했습니다.");
    d_->pendingPlain=std::move(candidate);save->lock=std::move(d_->fileLock);d_->save=save;const auto generation=d_->generation;
    // The worker receives ciphertext only. Keys/plaintext/Vault stay on the owning thread.
    d_->writer=QThread::create([save]{QByteArray ignored;
        bool archived=true;
        if(!save->archivePath.isEmpty())archived=persistCipher(save->archivePath,save->archiveBytes,{},true,&ignored,&save->message,&save->cancelled);
        if(archived)save->success=persistCipher(save->path,save->bytes,save->previous,false,&save->digest,&save->message,&save->cancelled);
        if(save->cancelled)save->lock.reset();
    });
    connect(d_->writer,&QThread::finished,this,[this,save,generation]{auto* writer=d_->writer;d_->writer=nullptr;d_->save.reset();
        if(generation==d_->generation&&isOpen()&&!save->cancelled){d_->fileLock=std::move(save->lock);
            if(save->success){d_->plain=std::move(d_->pendingPlain);d_->digest=save->digest;emit changed();}
            else lock();
            emit writeFinished(save->success,save->success?QStringLiteral("암호화 기록 저장 완료"):save->message);
        }else{save->lock.reset();d_->pendingPlain.clear();emit writeFinished(false,QStringLiteral("잠금 또는 종료로 저장 완료 상태를 폐기했습니다."));}
        if(save->receipt)emit collectionWriteFinished(save->receipt,save->success&&generation==d_->generation&&isOpen()&&!save->cancelled,
            save->success&&generation==d_->generation&&isOpen()&&!save->cancelled?QString{}:QStringLiteral("수집 기록의 저장 완료를 확인하지 못했습니다."));
        writer->deleteLater();
    });
    d_->writer->start();return true;
}
QList<AssetEvent> AssetsLedger::events()const{Contents c;if(isOpen())unpack(d_->plain,&c,d_->legacy);return c.events;}
QList<AssetQuote> AssetsLedger::quotes()const{Contents c;if(isOpen())unpack(d_->plain,&c,d_->legacy);return c.quotes;}
QList<AssetOrderObservation> AssetsLedger::orders()const{Contents c;if(isOpen())unpack(d_->plain,&c,d_->legacy);return c.orders;}
bool AssetsLedger::prepareSchema3(QJsonObject* migration,QString* error){
    const auto failed=[&](const QString& message){if(error)*error=message;return false;};
    // Preserve the exact pre-migration ciphertext. Existing archives are
    // immutable and remain in place; their authenticated set is recorded
    // inside the new encrypted ledger for a bounded rollback procedure.
    QByteArray bytes;
    if(!readEncrypted(d_->path,&bytes,error)||QCryptographicHash::hash(bytes,QCryptographicHash::Sha256)!=d_->digest)return failed("전환 전 원장 변경을 확인했습니다. 기존 파일은 유지합니다.");
    const auto hash=QString::fromLatin1(d_->digest.toHex());
    const auto backupName=QString("assets.keefi.pre-schema3-%1.keefi").arg(hash);
    const QDir directory=QFileInfo(d_->path).dir();const auto backupPath=directory.filePath(backupName);
    QJsonArray archives,evidence;
    evidence.append(QJsonObject{{"name",backupName},{"sourceDigest",hash},{"cipherDigest",hash}});
    for(const auto& file:archiveFiles()){
        QByteArray original;SecureBuffer plain;Contents contents;
        if(!readEncrypted(directory.filePath(file),&original,error)||!decrypt(original,d_->key,&plain,error)||!unpack(plain,&contents)||contents.schema>=3)
            return failed("기존 보관 파일 집합을 검증하지 못해 schema 전환을 중단했습니다.");
        const auto archiveHash=QString::fromLatin1(QCryptographicHash::hash(original,QCryptographicHash::Sha256).toHex());
        archives.append(QJsonObject{{"name",file},{"digest",archiveHash}});
        evidence.append(QJsonObject{{"name",file},{"sourceDigest",archiveHash},{"cipherDigest",archiveHash}});
    }
    if(QFileInfo::exists(backupPath)||QFileInfo(backupPath).isSymLink()){
        QByteArray backup;if(!readEncrypted(backupPath,&backup,error)||backup!=bytes)return failed("전환 백업의 내용 또는 권한을 확인하지 못했습니다.");
    }else{QByteArray digest;if(!persistCipher(backupPath,bytes,{},true,&digest,error))return false;}
    *migration={{"backup",backupName},{"sourceDigest",hash},{"archives",archives},{"rewriteEvidence",QJsonObject{{"version",1},{"files",evidence}}}};
    return migrationValid(*migration)||failed("전환 백업 목록의 형식이 올바르지 않습니다.");
}
QList<FlowAccountState> AssetsLedger::flowStates()const{Contents c;if(isOpen())unpack(d_->plain,&c,d_->legacy);return c.flows;}
QJsonObject AssetsLedger::coinBaseline()const{Contents c;if(isOpen())unpack(d_->plain,&c,d_->legacy);return c.coinBaseline;}
AssetsLedger::CoinInputs AssetsLedger::coinInputs(QString* error)const{
    if(error)error->clear();
    const auto failed=[&](const QString& message){fail(error,message);return CoinInputs{};};
    if(!d_->vault||!d_->vault->isUnlocked())return failed("금고를 열면 코인 투자 기록을 조회할 수 있습니다.");
    try{
        SecureBuffer filePlain,root;const SecureBuffer* plain=&d_->plain;bool legacy=d_->legacy;
        if(!isOpen()){
            if(QFileInfo(d_->path).isSymLink())return failed("자산 원장 경로가 안전하지 않습니다.");
            if(!exists())return {}; // Absence is not permission to create a key or file.
            if(!privatePath(QFileInfo(d_->path).absolutePath(),true,error))return {};
            if(!d_->vault->withAssetsLedgerKey(false,[&](const SecureBuffer& key){root=SecureBuffer(key.data(),key.size());},error))return {};
            QByteArray bytes;
            if(!readEncrypted(d_->path,&bytes,error)||!storage_detail::decodeStore(bytes,storage_detail::Kind::Assets,root,&filePlain,error))return {};
            legacy=storage_detail::formatOf(bytes,storage_detail::Kind::Assets)==storage_detail::Format::Legacy;
            plain=&filePlain;
        }
        Contents c;if(!unpack(*plain,&c,legacy))return failed("자산 원장 인증·스키마 검증에 실패했습니다.");
        return {std::move(c.snapshots),std::move(c.quotes),std::move(c.collections),std::move(c.flows),std::move(c.coinBaseline)};
    }catch(const std::exception&){return failed("보호된 코인 투자 기록을 읽지 못했습니다.");}
}
AssetsLedger::CollectionWrite AssetsLedger::saveFlowState(const FlowAccountState& state,quint64 expectedRevision,quint64 receipt,QString* error){
    const auto failed=[&](const QString& message){if(error)*error=message;return CollectionWrite::Failed;};
    if(error)error->clear();
    if(!receipt||!isOpen()||isWriting()||d_->legacy||QFileInfo(d_->path).fileName()!="assets.keefi")return failed("수집 기록은 열린 원장의 단일 저장 큐에서 저장해야 합니다.");
    try{
        FlowAccountState verified;Contents next;
        if(!FundFlowTracking::decode(FundFlowTracking::encode(state),&verified)||verified!=state||!unpack(d_->plain,&next))return failed("입출금 수집 기록의 형식을 확인하지 못했습니다.");
        auto existing=std::find_if(next.flows.begin(),next.flows.end(),[&](const auto& row){return row.accountScope==state.accountScope;});
        const quint64 revision=existing==next.flows.end()?0:existing->revision;
        if(revision!=expectedRevision)return failed("입출금 수집 기록의 저장 세대가 바뀌었습니다. 최신 기록에서 다시 합쳐야 합니다.");
        if(existing!=next.flows.end()&&*existing==state)return CollectionWrite::Unchanged;
        if(state.revision<=revision)return failed("입출금 수집 기록의 새 세대가 올바르지 않습니다.");
        if(existing!=next.flows.end()&&(existing->binding!=state.binding||existing->connectionId!=state.connectionId||existing->provider!=state.provider||existing->market!=state.market))
            return failed("연결 귀속이 바뀌어 기존 입출금 수집 기록을 덮어쓰지 않습니다.");
        if(existing==next.flows.end()){
            if(next.flows.size()>=FundFlowTracking::MaxAccounts)return failed("수집 계좌 보존 상한에 도달했습니다.");
            next.flows.append(state);
        }else *existing=state;
        if(next.schema<3){if(!prepareSchema3(&next.migration,error))return CollectionWrite::Failed;next.schema=3;}
        auto candidate=pack(next);if(candidate.empty())return failed("입출금 수집 기록이 원장 크기 상한을 넘었습니다. 기존 기록은 보존합니다.");
        return persistAsync(std::move(candidate),{},{},error,receipt)?CollectionWrite::Pending:CollectionWrite::Failed;
    }catch(const std::exception&){return failed("암호화 입출금 수집 기록 저장에 실패했습니다. 기존 기록을 확인해 주세요.");}
}
AssetsLedger::CollectionWrite AssetsLedger::saveCoinBaseline(const QJsonObject& baseline,quint64 receipt,QString* error){
    const auto failed=[&](const QString& message){if(error)*error=message;return CollectionWrite::Failed;};
    if(error)error->clear();
    if(!receipt||!isOpen()||isWriting()||d_->legacy||QFileInfo(d_->path).fileName()!="assets.keefi")return failed("기록 시작 기준은 열린 원장의 단일 저장 큐에서 저장해야 합니다.");
    if(!coinBaselineValid(baseline))return failed("기록 시작 기준의 형식을 확인하지 못했습니다.");
    try{
        Contents next;if(!unpack(d_->plain,&next))return failed("자산 원장 형식을 확인하지 못했습니다.");
        if(next.coinBaseline==baseline)return CollectionWrite::Unchanged;
        next.coinBaseline=baseline;
        if(next.schema<3){if(!prepareSchema3(&next.migration,error))return CollectionWrite::Failed;next.schema=3;}
        auto candidate=pack(next);if(candidate.empty())return failed("기록 시작 기준을 저장할 공간이 없습니다. 기존 기록은 보존합니다.");
        return persistAsync(std::move(candidate),{},{},error,receipt)?CollectionWrite::Pending:CollectionWrite::Failed;
    }catch(const std::exception&){return failed("기록 시작 기준 저장에 실패했습니다. 기존 기록을 확인해 주세요.");}
}
QList<EarnAccountState> AssetsLedger::earnStates()const{Contents c;if(isOpen())unpack(d_->plain,&c,d_->legacy);return c.collections;}
AssetsLedger::CollectionWrite AssetsLedger::saveEarnState(const EarnAccountState& state,quint64 expectedRevision,quint64 receipt,QString* error){
    const auto failed=[&](const QString& message){if(error)*error=message;return CollectionWrite::Failed;};
    if(error)error->clear();
    if(!receipt||!isOpen()||isWriting()||d_->legacy||QFileInfo(d_->path).fileName()!="assets.keefi")return failed("수집 기록은 열린 원장의 단일 저장 큐에서 저장해야 합니다.");
    try{
        EarnAccountState verified;Contents next;
        if(!EarnTracking::decode(EarnTracking::encode(state),&verified)||verified!=state||!unpack(d_->plain,&next))return failed("수집 기록의 형식을 확인하지 못했습니다.");
        auto existing=std::find_if(next.collections.begin(),next.collections.end(),[&](const auto& row){return row.accountScope==state.accountScope;});
        const quint64 revision=existing==next.collections.end()?0:existing->revision;
        if(revision!=expectedRevision)return failed("수집 기록의 저장 세대가 바뀌었습니다. 최신 기록에서 다시 합쳐야 합니다.");
        if(existing!=next.collections.end()&&*existing==state)return CollectionWrite::Unchanged;
        if(state.revision<=revision)return failed("수집 기록의 새 세대가 올바르지 않습니다.");
        if(existing!=next.collections.end()&&(existing->binding!=state.binding||existing->connectionId!=state.connectionId))return failed("연결 귀속이 바뀌어 기존 수집 기록을 덮어쓰지 않습니다.");
        if(existing==next.collections.end()){
            if(next.collections.size()>=EarnTracking::MaxAccounts)return failed("수집 계좌 보존 상한에 도달했습니다.");
            next.collections.append(state);
        }else *existing=state;
        if(next.schema<3){if(!prepareSchema3(&next.migration,error))return CollectionWrite::Failed;next.schema=3;}
        auto candidate=pack(next);if(candidate.empty())return failed("수집 기록이 원장 크기 상한을 넘었습니다. 기존 기록은 보존합니다.");
        return persistAsync(std::move(candidate),{},{},error,receipt)?CollectionWrite::Pending:CollectionWrite::Failed;
    }catch(const std::exception&){return failed("암호화 수집 기록 저장에 실패했습니다. 기존 기록을 확인해 주세요.");}
}
namespace storage_detail {
bool validateAssetsPayload(const SecureBuffer& plain,bool legacy,QString* error){
    Contents contents;return unpack(plain,&contents,legacy)||fail(error,QStringLiteral("자산 원장의 인증된 JSON 계약이 올바르지 않습니다."));
}
bool rewriteAssetsPayload(const SecureBuffer& plain,const QJsonObject& rewriteFiles,SecureBuffer* result,QString* error){
    if(!result||!validateAssetsPayload(plain,true,error))return false;
    auto bytes=QByteArray(reinterpret_cast<const char*>(plain.data()),qsizetype(plain.size()));
    auto root=QJsonDocument::fromJson(bytes).object();sodium_memzero(bytes.data(),std::size_t(bytes.size()));
    if(root.value("coinBaseline").isObject()){
        auto baseline=root.value("coinBaseline").toObject();
        const auto policy=baseline.value("policy").toString();
        const auto currentPolicy=canonicalPublicFormat(policy);
        if(currentPolicy==QStringLiteral("keefi-valuation-v2")&&currentPolicy!=policy){baseline["policy"]=currentPolicy;root["coinBaseline"]=baseline;}
    }
    if(root.value("schema_version")==QJsonValue(3)){
        auto migration=root.value("migration").toObject();QJsonArray archives,evidence;
        const auto add=[&](const QString& name,const QString& digest,QString* replacement){
            const auto row=rewriteFiles.value(name).toObject();
            if(row.size()!=3||row.value("name").toString()!=rewrittenFileName(name)||row.value("sourceDigest").toString()!=digest)return false;
            *replacement=row.value("name").toString();evidence.append(row);return true;
        };
        QString backup;
        if(!add(migration.value("backup").toString(),migration.value("sourceDigest").toString(),&backup))
            return fail(error,QStringLiteral("원장의 원본 백업과 새 암호문 증거가 일치하지 않습니다."));
        for(const auto& value:migration.value("archives").toArray()){
            const auto old=value.toObject();QString name;
            if(!add(old.value("name").toString(),old.value("digest").toString(),&name))
                return fail(error,QStringLiteral("원장의 원본 보관 파일과 새 암호문 증거가 일치하지 않습니다."));
            archives.append(QJsonObject{{"name",name},{"digest",old.value("digest")}});
        }
        migration["backup"]=backup;migration["archives"]=archives;
        migration["rewriteEvidence"]=QJsonObject{{"version",1},{"files",evidence}};
        root["migration"]=migration;
    }
    bytes=QJsonDocument(root).toJson(QJsonDocument::Compact);
    if(bytes.size()>AssetsLedger::MaxFileBytes-HeaderBytes-16){sodium_memzero(bytes.data(),std::size_t(bytes.size()));return fail(error,QStringLiteral("새 원장 증거가 저장 크기 상한을 넘었습니다."));}
    SecureBuffer next(bytes.constData(),std::size_t(bytes.size()));sodium_memzero(bytes.data(),std::size_t(bytes.size()));
    if(!validateAssetsPayload(next,false,error))return false;
    *result=std::move(next);return true;
}
} // namespace storage_detail
} // namespace keefi
