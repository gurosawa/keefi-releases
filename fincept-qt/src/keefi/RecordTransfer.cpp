#include "keefi/RecordTransfer.h"
#include "keefi/IdentityCompatibility.h"
#include "keefi/LegacyStorageCodecPrivate.h"
#include "keefi/Vault.h"
#include "keefi/AssetsLedger.h"
#include "keefi/SwapJournal.h"
#include "keefi/SolanaTransactionCodec.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QRegularExpression>
#include <QSet>
#include "StagedStorageFile.h"
#include <QThread>
#include <QUuid>
#include <QUrl>
#include <QtEndian>
#include <sodium.h>
#include <cstring>
#include <vector>
#include <stdexcept>
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
#include <aclapi.h>
#include <io.h>
#include <fcntl.h>
#endif
namespace keefi {
namespace {
bool fail(QString* error,const QString& text){if(error)*error=text;return false;}
bool stopped(const RecordTransferCancellation& c){return c&&c->load();}
#ifdef Q_OS_WIN
bool privateWindowsPath(const QString& path, bool setAcl) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<unsigned char> user(size);
    const bool gotUser = GetTokenInformation(token, TokenUser, user.data(), size, &size);
    CloseHandle(token);
    if (!gotUser) return false;
    const auto sid = reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid;
    auto* name = const_cast<wchar_t*>(reinterpret_cast<const wchar_t*>(path.utf16()));
    if (setAcl) {
        EXPLICIT_ACCESSW access{};
        access.grfAccessPermissions = FILE_ALL_ACCESS;
        access.grfAccessMode = SET_ACCESS;
        access.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
        access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        access.Trustee.TrusteeType = TRUSTEE_IS_USER;
        access.Trustee.ptstrName = static_cast<LPWSTR>(sid);
        PACL acl = nullptr;
        if (SetEntriesInAclW(1, &access, nullptr, &acl) != ERROR_SUCCESS) return false;
        const DWORD status = SetNamedSecurityInfoW(name, SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, acl, nullptr);
        LocalFree(acl);
        if (status != ERROR_SUCCESS) return false;
    }
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PSID owner = nullptr;
    PACL acl = nullptr;
    if (GetNamedSecurityInfoW(name, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                             &owner, nullptr, &acl, nullptr, &descriptor) != ERROR_SUCCESS) return false;
    bool valid = owner && EqualSid(owner, sid) && acl;
    for (DWORD i = 0; valid && i < acl->AceCount; ++i) {
        void* ace = nullptr;
        if (!GetAce(acl, i, &ace)) { valid = false; break; }
        const auto* head = static_cast<ACE_HEADER*>(ace);
        if (head->AceType == ACCESS_ALLOWED_ACE_TYPE) {
            const auto* allowed = static_cast<ACCESS_ALLOWED_ACE*>(ace);
            valid = EqualSid(const_cast<DWORD*>(&allowed->SidStart), sid);
        } else if (head->AceType != ACCESS_DENIED_ACE_TYPE) {
            valid = false;
        }
    }
    LocalFree(descriptor);
    return valid;
}
#endif

bool privatePath(const QString& path, bool directory, QString* error) {
    const auto invalid = [&] { return fail(error, QStringLiteral("금고 경로의 소유자·권한·심볼릭 링크를 확인해 주세요.")); };
    const QFileInfo info(path);
    if (info.isSymLink() || !info.exists() || info.isDir() != directory)
        return invalid();
#ifdef Q_OS_UNIX
    struct stat st{};
    const QByteArray name = QFile::encodeName(path);
    if (::lstat(name.constData(), &st) || st.st_uid != ::geteuid() || (st.st_mode & 0077) || (!directory && st.st_nlink != 1) ||
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
    HANDLE handle = CreateFileW(reinterpret_cast<const wchar_t*>(path.utf16()), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | (directory ? FILE_FLAG_BACKUP_SEMANTICS : 0), nullptr);
    if (handle == INVALID_HANDLE_VALUE) return invalid();
    BY_HANDLE_FILE_INFORMATION attributes{};
    const bool safe = GetFileInformationByHandle(handle, &attributes) &&
        !(attributes.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && (directory || attributes.nNumberOfLinks == 1);
    CloseHandle(handle);
    if (!safe) return invalid();
#else
    return fail(error, QStringLiteral("이 운영체제의 금고 접근 권한 검증은 지원하지 않습니다."));
#endif
    return true;
}

QString hash(const QByteArray& input){return QString::fromLatin1(QCryptographicHash::hash(input,QCryptographicHash::Sha256).toHex());}
QString portable(const QString& origin,const QString& value){return hash((identityDomain(IdentityDomain::PortableRecordScope)+char(0))+origin.toUtf8()+'\0'+value.toUtf8());}
bool uuid(const QString& value){const QUuid id(value);return !id.isNull()&&id.toString(QUuid::WithoutBraces)==value;}
bool hex(const QString& value){static const QRegularExpression re("\\A[a-f0-9]{64}\\z");return re.match(value).hasMatch();}
bool text(const QString& value,bool empty=false){
    if((value.isEmpty()&&!empty)||value.size()>4096)return false;
    for(qsizetype i=0;i<value.size();++i){const auto c=value[i];if(c.isHighSurrogate()){if(++i>=value.size()||!value[i].isLowSurrogate())return false;}
        else if(c.isLowSurrogate()||c.category()==QChar::Other_Control||c.category()==QChar::Other_Format)return false;}
    return true;
}
bool fields(const QJsonObject& o,const QStringList& required,const QStringList& optional={}){
    for(const auto& name:required)if(!o.contains(name))return false;
    for(auto it=o.begin();it!=o.end();++it)if(!required.contains(it.key())&&!optional.contains(it.key()))return false;
    return true;
}
bool strings(const QJsonObject& o,const QStringList& names){for(const auto& name:names)if(!o[name].isString()||!text(o[name].toString(),true))return false;return true;}
bool time(const QJsonValue& value,bool zero=false){
    if(!value.isString())return false;bool ok=false;const auto v=value.toString().toLongLong(&ok);
    return ok&&QString::number(v)==value.toString()&&v>=(zero?0:1)&&v<=253402300799999LL;
}
bool decimalString(const QJsonValue& value){static const QRegularExpression re("\\A-?[0-9]{1,80}(?:\\.[0-9]{1,80})?\\z");return value.isString()&&(value.toString().isEmpty()||re.match(value.toString()).hasMatch());}
bool evidenceValid(const QJsonObject& o){
    if(!fields(o,{"source","url","scope","sourceTimeMs","receivedAtMs","complete"})||!strings(o,{"source","url","scope"})||!hex(o["scope"].toString())||
       !time(o["sourceTimeMs"],true)||!time(o["receivedAtMs"])||!o["complete"].isBool())return false;
    const QUrl url(o["url"].toString());return o["url"].toString().isEmpty()||(url.isValid()&&url.scheme()=="https"&&!url.host().isEmpty()&&url.userInfo().isEmpty()&&!url.hasQuery()&&!url.hasFragment());
}
QJsonObject evidence(const AssetEvidence& e,const QString& scope){return {{"source",e.source},{"url",e.url},{"scope",scope},{"sourceTimeMs",QString::number(e.sourceTimeMs)},{"receivedAtMs",QString::number(e.receivedAtMs)},{"complete",e.complete}};}
QJsonObject component(const AssetComponent& c,const QString& origin,const QString& scope){return {{"id",portable(origin,c.id)},{"asset",c.asset},{"quantity",c.quantity},{"costBasis",c.costBasis},{"costCurrency",c.costCurrency},{"reportedValue",c.reportedValue},{"reportedValueCurrency",c.reportedValueCurrency},{"providerUnrealizedPnl",c.providerUnrealizedPnl},{"valuationGroup",c.valuationGroup},{"kind",int(c.kind)},{"evidence",evidence(c.evidence,scope)}};}
QJsonObject snapshot(const AssetSnapshot& s,const QString& origin){const auto scope=portable(origin,s.accountScope);QJsonArray parts;for(const auto& c:s.components)parts.append(component(c,origin,scope));
    return {{"id",portable(origin,s.id)},{"accountScope",scope},{"provider",s.provider},{"environment",s.environment},{"market",s.market},{"timeMs",QString::number(s.timeMs)},
        {"balanceComplete",s.balanceComplete},{"reconciled",s.reconciled},{"components",parts},{"evidence",evidence(s.evidence,scope)}};}
QJsonObject portableEvent(const AssetEvent& e,const QString& origin){const auto scope=portable(origin,e.accountScope);return {{"id",portable(origin,e.id)},{"accountScope",scope},{"asset",e.asset},{"quantity",e.quantity},{"officialId",e.officialId},{"transferReference",e.transferReference},
    {"peerAccountScope",e.peerAccountScope.isEmpty()?QString{}:portable(origin,e.peerAccountScope)},{"timeMs",QString::number(e.timeMs)},{"kind",int(e.kind)},{"evidence",evidence(e.evidence,scope)}};}
QJsonObject order(const AssetOrderObservation& r,const QString& origin,const QString& environment){const auto scope=portable(origin,r.accountScope);return {{"accountScope",scope},{"provider",r.provider},{"environment",environment},{"market",r.market},{"orderId",r.orderId},{"orderFamily",r.orderFamily},{"observedAtMs",QString::number(r.observedAtMs)},
    {"side",r.side},{"type",r.type},{"status",r.status},{"price",r.price},{"quantity",r.quantity},{"executed",r.executed},{"quoteAmount",r.quoteAmount},{"quantityUnit",r.quantityUnit},{"currency",r.currency},{"evidence",evidence(r.evidence,scope)}};}
QJsonObject swap(const SwapPendingRecord& r,const QString& origin){return {{"id",r.id},{"accountScope",portable(origin,r.wallet)},{"provider","jupiter"},{"environment","mainnet"},{"market","solana"},{"chain",r.chain},{"wallet",r.wallet},{"inputMint",r.inputMint},{"outputMint",r.outputMint},
    {"inputAtomic",r.inputAtomic},{"minimumRouteOutput",r.minimumRouteOutput},{"slippageBps",r.slippageBps},{"requestId",r.requestId},{"transactionId",r.transactionId},{"messageHash",r.messageHash},{"lastValidBlockHeight",r.lastValidBlockHeight},{"createdAt",r.createdAt},{"state",r.state},
    {"confirmedSlot",r.confirmedSlot},{"actualFeeLamports",r.actualFeeLamports},{"actualInputAtomic",r.actualInputAtomic},{"actualOutputAtomic",r.actualOutputAtomic}};}
const QStringList PendingRequired={"id","accountScope","provider","environment","market","symbol","operation","clientId","orderId","replacementClientId","state","createdAt","quantity","price","quoteAmount","side","type"};
const QStringList PendingOptional={"operationOrderId","orderDate","exchange","baseOriginalOrderId","baseOriginalProvenance","quantityMode","orderBranch","parentOrderId","amendRequestId","quantityUnit","ackState","beforePrice","beforeQuantity","beforeExecuted","beforeUpdatedAt","region","orderFamily","algoId","clientAlgoId","actualOrderId","orderListId","replaceMode","replacementState","brokerConditional","futures","before"};
QJsonObject pending(const QJsonObject& source,const QString& origin){QJsonObject out;for(const auto& name:PendingRequired)if(source.contains(name))out[name]=source[name];for(const auto& name:PendingOptional)if(source.contains(name))out[name]=source[name];
    out["accountScope"]=portable(origin,source.value("accountScope").toString().isEmpty()?source["accountId"].toString():source["accountScope"].toString());
    out["market"]=source.contains("market")?source["market"]:QJsonValue("spot");out["operation"]=source["kind"];return out;}
QStringList observationFields(const QString& kind,const QJsonObject& p){
    if(kind=="swap")return {"state","confirmedSlot","actualFeeLamports","actualInputAtomic","actualOutputAtomic"};
    if(kind=="pending"){
        QStringList names={"state","operationOrderId","baseOriginalOrderId","baseOriginalProvenance","ackState","replacementState"};
        if(p["operation"]=="place")names<<"orderId"<<"algoId"<<"actualOrderId";return names;
    }return {};
}
QJsonObject immutableIntent(const QString& kind,QJsonObject p){for(const auto& name:observationFields(kind,p))p.remove(name);return p;}
QString identity(const QString& kind,const QJsonObject& p){QJsonArray keys;
    if(kind=="snapshot")keys={p["accountScope"],p["id"]};
    else if(kind=="event")keys={p["accountScope"],p["officialId"],p["kind"],p["asset"]};
    else if(kind=="order")keys={p["accountScope"],p["provider"],p["environment"],p["market"],p["orderFamily"],p["orderId"],p["observedAtMs"]};
    else{QJsonObject observation;for(const auto& name:observationFields(kind,p))if(p.contains(name))observation[name]=p[name];keys={p["id"],hash(QJsonDocument(observation).toJson(QJsonDocument::Compact))};}
    return hash(QJsonDocument(keys).toJson(QJsonDocument::Compact));
}
bool compatibleObservations(const QString& kind,const QJsonObject& a,const QJsonObject& b){
    if(immutableIntent(kind,a)!=immutableIntent(kind,b))return false;
    const QStringList known=kind=="swap"?QStringList{"confirmedSlot","actualFeeLamports","actualInputAtomic","actualOutputAtomic"}:QStringList{"operationOrderId","baseOriginalOrderId","baseOriginalProvenance","orderId","algoId","actualOrderId"};
    for(const auto& name:known)if(a.contains(name)&&b.contains(name)&&!a[name].toString().isEmpty()&&!b[name].toString().isEmpty()&&a[name]!=b[name])return false;
    if(kind=="swap"){
        const auto first=a["state"].toString(),second=b["state"].toString();
        if((first=="failed_onchain"&&(second=="confirmed"||second=="finalized"))||(second=="failed_onchain"&&(first=="confirmed"||first=="finalized")))return false;
    }return true;
}

QJsonObject wrap(const QString& origin,const QString& kind,const QJsonObject& payload){return {{"origin",origin},{"kind",kind},{"identity",identity(kind,payload)},{"payload",payload}};}
bool componentValid(const QJsonObject& p){
    if(!fields(p,{"id","asset","quantity","costBasis","costCurrency","reportedValue","reportedValueCurrency","providerUnrealizedPnl","valuationGroup","kind","evidence"})||
       !strings(p,{"id","asset","costCurrency","reportedValueCurrency","valuationGroup"})||!hex(p["id"].toString())||!p["kind"].isDouble()||p["kind"].toInt(-1)<0||p["kind"].toInt()>int(AssetComponentKind::ProviderPnl)||p["kind"]!=QJsonValue(p["kind"].toInt())||!p["evidence"].isObject()||!evidenceValid(p["evidence"].toObject()))return false;
    for(const auto* name:{"quantity","costBasis","reportedValue","providerUnrealizedPnl"})if(!decimalString(p[name]))return false;
    return true;
}
bool payloadValid(const QString& kind,const QJsonObject& p){
    if(kind=="snapshot"){
        if(!fields(p,{"id","accountScope","provider","environment","market","timeMs","balanceComplete","reconciled","components","evidence"})||!strings(p,{"id","accountScope","provider","environment","market"})||
           !hex(p["id"].toString())||!hex(p["accountScope"].toString())||!time(p["timeMs"])||!p["balanceComplete"].isBool()||!p["reconciled"].isBool()||!p["components"].isArray()||p["components"].toArray().size()>2048||!p["evidence"].isObject()||!evidenceValid(p["evidence"].toObject()))return false;
        QSet<QString> ids;for(const auto& value:p["components"].toArray()){if(!value.isObject()||!componentValid(value.toObject())||ids.contains(value.toObject()["id"].toString()))return false;ids.insert(value.toObject()["id"].toString());}return true;
    }
    if(kind=="event")return fields(p,{"id","accountScope","asset","quantity","officialId","transferReference","peerAccountScope","timeMs","kind","evidence"})&&strings(p,{"id","accountScope","asset","officialId","transferReference","peerAccountScope"})&&hex(p["id"].toString())&&hex(p["accountScope"].toString())&&(p["peerAccountScope"].toString().isEmpty()||hex(p["peerAccountScope"].toString()))&&decimalString(p["quantity"])&&time(p["timeMs"])&&p["kind"].isDouble()&&p["kind"]==QJsonValue(p["kind"].toInt())&&p["kind"].toInt(-1)>=0&&p["kind"].toInt()<=int(AssetEventKind::Unknown)&&p["evidence"].isObject()&&evidenceValid(p["evidence"].toObject());
    if(kind=="order"){
        if(!fields(p,{"accountScope","provider","environment","market","orderId","orderFamily","observedAtMs","side","type","status","price","quantity","executed","quoteAmount","quantityUnit","currency","evidence"})||!strings(p,{"accountScope","provider","environment","market","orderId","orderFamily","side","type","status","quantityUnit","currency"})||!hex(p["accountScope"].toString())||!time(p["observedAtMs"])||!QStringList{"ordinary","conditional"}.contains(p["orderFamily"].toString())||!p["evidence"].isObject()||!evidenceValid(p["evidence"].toObject()))return false;
        for(const auto* name:{"price","quantity","executed","quoteAmount"})if(!decimalString(p[name]))return false;return true;
    }
    if(kind=="swap"){
        const QStringList names={"id","accountScope","provider","environment","market","chain","wallet","inputMint","outputMint","inputAtomic","minimumRouteOutput","slippageBps","requestId","transactionId","messageHash","lastValidBlockHeight","createdAt","state","confirmedSlot","actualFeeLamports","actualInputAtomic","actualOutputAtomic"};
        if(!fields(p,names)||!strings(p,names)||!uuid(p["id"].toString())||!hex(p["accountScope"].toString())||p["provider"]!="jupiter"||p["environment"]!="mainnet"||p["market"]!="solana"||p["chain"]!="solana-mainnet"||!time(p["createdAt"])||!hex(p["messageHash"].toString())||!QStringList{"signed_not_submitted","submitted_unknown","confirmed","finalized","failed_onchain"}.contains(p["state"].toString()))return false;
        for(const auto* name:{"wallet","inputMint","outputMint"})if(!SolanaTransactionCodec::publicKeyValid(p[name].toString()))return false;
        if(SolanaTransactionCodec::decodeBase58(p["transactionId"].toString(),64).size()!=64)return false;
        for(const auto* name:{"inputAtomic","minimumRouteOutput","slippageBps","lastValidBlockHeight","confirmedSlot","actualFeeLamports","actualInputAtomic","actualOutputAtomic"}){const auto value=p[name].toString();bool ok=false;const auto number=value.toULongLong(&ok);Q_UNUSED(number)if(!value.isEmpty()&&(!ok||!QRegularExpression("\\A[0-9]{1,20}\\z").match(value).hasMatch()))return false;}return true;
    }
    if(kind=="pending"){
        if(!fields(p,PendingRequired,PendingOptional)||!strings(p,PendingRequired)||!hex(p["accountScope"].toString())||!time(p["createdAt"])||!QStringList{"place","cancel","amend"}.contains(p["operation"].toString())||!QStringList{"pending","unknown"}.contains(p["state"].toString()))return false;
        for(auto it=p.begin();it!=p.end();++it){if(it.key()=="futures"||it.key()=="before"){
                if(!it->isObject())return false;const auto nested=it->toObject();
                const QStringList allowed=it.key()=="futures"?QStringList{"positionSide","exposure","reduceOnly","closePosition","timeInForce","workingType","triggerPrice","activationPrice","callbackRate","modifyId","priceProtect"}:QStringList{"price","quantity","executed","updatedAt"};
                if(!fields(nested,it.key()=="before"?allowed:QStringList{"positionSide","exposure","reduceOnly","closePosition","timeInForce","workingType","triggerPrice","activationPrice","callbackRate","modifyId"},it.key()=="before"?QStringList{}:QStringList{"priceProtect"}))return false;
                for(auto n=nested.begin();n!=nested.end();++n){const bool boolean=QStringList{"reduceOnly","closePosition","priceProtect"}.contains(n.key());if(boolean?!n->isBool():(!n->isString()||!text(n->toString(),true)))return false;}
            }else if(!it->isString()||!text(it->toString(),true))return false;}
        return true;
    }
    return false;
}
bool validate(const RecordTransferBundle& data,QString* error){
    if(data.records.size()>RecordTransfer::MaxRecords)return fail(error,"이관 기록 수 상한을 넘었습니다.");QSet<QString> ids;QMap<QString,QJsonObject> requests;qsizetype totalBytes=1024;
    for(const auto& value:data.records){if(!value.isObject())return fail(error,"이관 기록 형식이 올바르지 않습니다.");const auto o=value.toObject();const auto p=o["payload"].toObject();const auto kind=o["kind"].toString();
        if(!fields(o,{"origin","kind","identity","payload"})||!uuid(o["origin"].toString())||!o["kind"].isString()||!o["identity"].isString()||!o["payload"].isObject()||!payloadValid(kind,p)||identity(kind,p)!=o["identity"].toString() )return fail(error,"이관 기록의 허용 필드·식별자·자료형 검증에 실패했습니다.");
        if(kind=="swap"||kind=="pending"){
            const auto request=o["origin"].toString()+":"+kind+":"+p["id"].toString();
            if(requests.contains(request)){
                auto aggregate=requests[request];if(!compatibleObservations(kind,aggregate,p))return fail(error,"같은 원본 요청의 불변 내용 또는 확인된 결과가 충돌합니다.");
                for(const auto& name:observationFields(kind,p))if(name!="state"&&aggregate[name].toString().isEmpty()&&!p[name].toString().isEmpty())aggregate[name]=p[name];
                if(kind=="swap"){const auto rank=[](const QString& state){return state=="failed_onchain"?4:state=="finalized"?3:state=="confirmed"?2:state=="submitted_unknown"?1:0;};if(rank(p["state"].toString())>rank(aggregate["state"].toString()))aggregate["state"]=p["state"];}
                requests[request]=aggregate;
            }else requests[request]=p;
        }
        const auto recordBytes=QJsonDocument(o).toJson(QJsonDocument::Compact).size();if(recordBytes>(kind=="snapshot"?RecordTransfer::MaxSnapshotBytes:RecordTransfer::MaxRecordBytes)||(totalBytes+=recordBytes)>RecordTransfer::MaxFileBytes-1024)return fail(error,"이관 기록별 또는 전체 크기 상한을 넘었습니다.");
        const auto id=o["origin"].toString()+":"+kind+":"+o["identity"].toString();if(ids.contains(id))return fail(error,"패키지 안에 중복된 이관 기록 식별자가 있습니다.");ids.insert(id);
    }return true;
}
SecureBuffer serialize(const RecordTransferBundle& data,const QString& origin={}){
    QJsonObject object{{"schema_version",1},{"records",data.records}};if(!origin.isEmpty())object["origin"]=origin;
    auto bytes=QJsonDocument(object).toJson(QJsonDocument::Compact);if(bytes.size()>RecordTransfer::MaxFileBytes-88){sodium_memzero(bytes.data(),std::size_t(bytes.size()));return {};}
    SecureBuffer out(bytes.constData(),std::size_t(bytes.size()));sodium_memzero(bytes.data(),std::size_t(bytes.size()));return out;
}
bool deserialize(const SecureBuffer& plain,RecordTransferBundle* out,QString* origin,QString* error){
    if(plain.empty()||plain.size()>std::size_t(RecordTransfer::MaxFileBytes-56))return fail(error,"이관 평문 크기가 허용 범위를 벗어났습니다.");
    auto bytes=QByteArray(reinterpret_cast<const char*>(plain.data()),qsizetype(plain.size()));QJsonParseError parse;const auto doc=QJsonDocument::fromJson(bytes,&parse);const bool canonical=doc.toJson(QJsonDocument::Compact)==bytes;sodium_memzero(bytes.data(),std::size_t(bytes.size()));
    if(parse.error!=QJsonParseError::NoError||!canonical||!doc.isObject())return fail(error,"이관 JSON 형식 검증에 실패했습니다.");const auto o=doc.object();
    if(!fields(o,origin?QStringList{"schema_version","records","origin"}:QStringList{"schema_version","records"})||o["schema_version"]!=QJsonValue(1)||!o["records"].isArray()||(origin&&(!o["origin"].isString()||!uuid(o["origin"].toString()))))return fail(error,"지원하지 않는 이관 schema입니다.");
    RecordTransferBundle next;next.records=o["records"].toArray();if(!validate(next,error))return false;if(origin)*origin=o["origin"].toString();*out=std::move(next);return true;
}

// Encrypted portable packages may be copied between PCs and owned by another OS
// user. Validate file kind and links, while leaving its directory ACL/mode intact.
bool portablePath(const QString& path,bool directory,QString* error){
    const QFileInfo info(path);if(info.isSymLink()||!info.exists()||info.isDir()!=directory)return fail(error,"이동 패키지 경로의 파일 종류·링크가 안전하지 않습니다.");
#ifdef Q_OS_UNIX
    struct stat st{};if(::lstat(QFile::encodeName(path).constData(),&st)||(directory?!S_ISDIR(st.st_mode):!S_ISREG(st.st_mode)||st.st_nlink!=1))return fail(error,"이동 패키지 경로의 파일 종류·하드 링크가 안전하지 않습니다.");
#elif defined(Q_OS_WIN)
    HANDLE handle=CreateFileW(reinterpret_cast<const wchar_t*>(path.utf16()),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|(directory?FILE_FLAG_BACKUP_SEMANTICS:0),nullptr);
    if(handle==INVALID_HANDLE_VALUE)return fail(error,"이동 패키지 경로를 확인하지 못했습니다.");BY_HANDLE_FILE_INFORMATION attributes{};
    const bool valid=GetFileInformationByHandle(handle,&attributes)&&!(attributes.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)&&(directory||attributes.nNumberOfLinks==1);CloseHandle(handle);if(!valid)return fail(error,"이동 패키지 경로의 링크가 안전하지 않습니다.");
#else
    return fail(error,"이 운영체제의 이동 패키지 파일 검증은 지원하지 않습니다.");
#endif
    return true;
}

bool readCipher(const QString& path,QByteArray* out,QString* error,bool strict=true){
    if(!out||!(strict?privatePath(path,false,error):portablePath(path,false,error)))return false;QFile file(path);
#ifdef Q_OS_UNIX
    const int fd=::open(QFile::encodeName(path).constData(),O_RDONLY|O_NOFOLLOW|O_CLOEXEC);struct stat st{};
    if(fd<0)return fail(error,"암호화 기록 파일을 열지 못했습니다.");
    if(::fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_nlink!=1||(strict&&(st.st_uid!=::geteuid()||(st.st_mode&0077)))){::close(fd);return fail(error,"암호화 기록 파일 권한·링크가 안전하지 않습니다.");}
    if(!file.open(fd,QIODevice::ReadOnly,QFileDevice::AutoCloseHandle)){::close(fd);return fail(error,"암호화 기록 파일을 읽지 못했습니다.");}
#elif defined(Q_OS_WIN)
    HANDLE handle=CreateFileW(reinterpret_cast<const wchar_t*>(path.utf16()),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(handle==INVALID_HANDLE_VALUE)return fail(error,"암호화 기록 파일을 열지 못했습니다.");BY_HANDLE_FILE_INFORMATION info{};
    if(!GetFileInformationByHandle(handle,&info)||(info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))||info.nNumberOfLinks!=1){CloseHandle(handle);return fail(error,"암호화 기록 파일의 링크가 안전하지 않습니다.");}
    const int fd=_open_osfhandle(reinterpret_cast<intptr_t>(handle),_O_RDONLY|_O_BINARY);
    if(fd<0){CloseHandle(handle);return fail(error,"암호화 기록 파일 handle을 열지 못했습니다.");}
    if(!file.open(fd,QIODevice::ReadOnly,QFileDevice::AutoCloseHandle)){_close(fd);return fail(error,"암호화 기록 파일을 읽지 못했습니다.");}
#else
    return fail(error,"이 운영체제의 암호화 기록 읽기를 지원하지 않습니다.");
#endif
    if(file.size()<56||file.size()>RecordTransfer::MaxFileBytes)return fail(error,"암호화 기록 파일 크기 상한 또는 형식 오류입니다.");
    auto bytes=file.read(RecordTransfer::MaxFileBytes+1);if(file.error()!=QFile::NoError||bytes.size()!=file.size())return fail(error,"암호화 기록 파일 전체 읽기에 실패했습니다.");*out=std::move(bytes);return true;
}
QByteArray storeEncrypt(const SecureBuffer& plain,const SecureBuffer& key){
    if(key.size()!=32||plain.empty()||plain.size()>std::size_t(RecordTransfer::MaxFileBytes-56))return {};
    QByteArray bytes(40+qsizetype(plain.size())+16,'\0');auto* p=reinterpret_cast<unsigned char*>(bytes.data());std::memcpy(p,"KEEFIIMP",8);qToLittleEndian<quint16>(1,p+8);randombytes_buf(p+12,24);qToLittleEndian<quint32>(quint32(plain.size()+16),p+36);
    unsigned long long size=0;if(crypto_aead_xchacha20poly1305_ietf_encrypt(p+40,&size,plain.data(),plain.size(),p,40,nullptr,p+12,key.data())||size!=plain.size()+16)return {};return bytes;
}
bool storeDecrypt(const QByteArray& bytes,const SecureBuffer& key,SecureBuffer* plain,QString* error){
    if(key.size()!=32||bytes.size()<56||bytes.size()>RecordTransfer::MaxFileBytes)return fail(error,"이관 저장소 크기가 올바르지 않습니다.");const auto* p=reinterpret_cast<const unsigned char*>(bytes.constData());
    if(storage_detail::formatOf(bytes,storage_detail::Kind::Imported)==storage_detail::Format::Invalid||qFromLittleEndian<quint16>(p+8)!=1||qFromLittleEndian<quint16>(p+10)!=0||qFromLittleEndian<quint32>(p+36)!=bytes.size()-40)return fail(error,"지원하지 않는 이관 저장소 형식입니다.");
    SecureBuffer next(std::size_t(bytes.size()-56));unsigned long long size=0;
    if(crypto_aead_xchacha20poly1305_ietf_decrypt(next.data(),&size,nullptr,p+40,bytes.size()-40,p,40,p+12,key.data())||size!=next.size())return fail(error,"이관 저장소 인증에 실패했습니다.");*plain=std::move(next);return true;
}
// Called after complete crypto/schema preparation. Guard is checked twice so a
// cancellation or owner generation change at a persistence seam cannot commit.
bool atomicCipher(const QString& path,const QByteArray& bytes,const QByteArray& previous,bool creating,QString* error,
                  const RecordTransferCancellation& cancel={},const std::function<bool()>& guard={},const std::function<bool()>& precommitCheck={},bool strict=true){
    const auto ready=[&]{return !stopped(cancel)&&(!guard||guard());};
    const auto unchanged=[&]{if(creating)return !QFileInfo::exists(path)&&!QFileInfo(path).isSymLink();QByteArray old;return readCipher(path,&old,error,strict)&&QCryptographicHash::hash(old,QCryptographicHash::Sha256)==previous;};
    if(!ready())return fail(error,"잠금 또는 취소로 저장을 중단했습니다.");if(bytes.isEmpty()||bytes.size()>RecordTransfer::MaxFileBytes||!unchanged())return fail(error,"기존 이관 기록의 변경 또는 저장 대상 충돌을 감지했습니다.");
    if(!(strict?privatePath(QFileInfo(path).absolutePath(),true,error):portablePath(QFileInfo(path).absolutePath(),true,error)))return false;
    StagedStorageFile staged(QFileInfo(path).absolutePath()+"/.keefi-record-transfer-XXXXXX");
    auto& temp = staged.file();
    if(!temp.open())return fail(error,"이관 임시 파일을 만들지 못했습니다.");
#ifdef Q_OS_WIN
    if(!privateWindowsPath(temp.fileName(),true))return fail(error,"이관 임시 파일을 보호하지 못했습니다.");
#else
    if(!temp.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner))return fail(error,"이관 임시 파일을 보호하지 못했습니다.");
#endif
    if(!privatePath(temp.fileName(),false,error)||temp.write(bytes)!=bytes.size()||!temp.flush())return fail(error,"이관 저장에 실패했습니다. 기존 기록을 보존했습니다.");
#ifdef Q_OS_UNIX
    if(::fsync(temp.handle()))return fail(error,"이관 임시 파일의 디스크 반영에 실패했습니다.");
#elif defined(Q_OS_WIN)
    if(!FlushFileBuffers(reinterpret_cast<HANDLE>(_get_osfhandle(temp.handle()))))return fail(error,"이관 임시 파일의 디스크 반영에 실패했습니다.");
#endif
    if(!temp.seek(0)||temp.read(RecordTransfer::MaxFileBytes+1)!=bytes)return fail(error,"이관 임시 암호문 readback에 실패했습니다.");
    if((precommitCheck&&!precommitCheck())||!ready())return fail(error,"잠금·취소 또는 최종 저장 조건 변경으로 교체를 중단했습니다.");
    if(!unchanged()||!(strict?privatePath(QFileInfo(path).absolutePath(),true,error):portablePath(QFileInfo(path).absolutePath(),true,error)))return fail(error,"교체 전 이관 파일 변경을 감지했습니다.");const auto name=temp.fileName();temp.close();
#ifdef Q_OS_UNIX
    const int directory=::open(QFile::encodeName(QFileInfo(path).absolutePath()).constData(),O_RDONLY|O_CLOEXEC);if(directory<0)return fail(error,"이관 저장 폴더를 열지 못했습니다.");
    // Creating uses link+unlink (no replace) to close a destination creation race.
    const auto from=QFile::encodeName(name),to=QFile::encodeName(path);
    if(!ready()){::close(directory);return fail(error,"잠금 또는 취소로 원자 교체 직전 저장을 중단했습니다.");}
    const int changed=creating ? ::link(from.constData(),to.constData()) : ::rename(from.constData(),to.constData());
    if(!changed&&creating)::unlink(from.constData());const int synced=changed ? 0 : ::fsync(directory);::close(directory);
    if(changed)return fail(error,"이관 파일 원자 교체에 실패했습니다.");if(synced)return fail(error,"교체 후 디스크 반영을 확인하지 못했습니다. 다시 여세요.");
#elif defined(Q_OS_WIN)
    if(!ready())return fail(error,"잠금 또는 취소로 원자 교체 직전 저장을 중단했습니다.");
    if(!staged.publish(path,!creating))return fail(error,"이관 파일 원자 교체에 실패했습니다.");
#else
    return fail(error,"이 운영체제의 원자 저장은 지원하지 않습니다.");
#endif
    QByteArray finalBytes;if(!readCipher(path,&finalBytes,error,strict)||finalBytes!=bytes)return fail(error,"교체된 이관 암호문의 readback에 실패했습니다. 다시 여세요.");return true;
}
bool packageHeader(const QByteArray& bytes,quint32* ops,quint64* mem,QString* error){
    if(bytes.size()<88||bytes.size()>RecordTransfer::MaxFileBytes)return fail(error,"이동 패키지 크기가 허용 범위를 벗어났습니다.");const auto* p=reinterpret_cast<const unsigned char*>(bytes.constData());
    if(storage_detail::formatOf(bytes,storage_detail::Kind::Package)==storage_detail::Format::Invalid||qFromLittleEndian<quint16>(p+8)!=1||qFromLittleEndian<quint16>(p+10)!=1||qFromLittleEndian<quint16>(p+12)!=1||qFromLittleEndian<quint16>(p+14)!=0)return fail(error,"지원하지 않는 이동 패키지 형식입니다.");
    *ops=qFromLittleEndian<quint32>(p+16);*mem=qFromLittleEndian<quint64>(p+20);
    return (*ops>=3&&*ops<=6&&*mem>=64ULL*1024*1024&&*mem<=256ULL*1024*1024&&qFromLittleEndian<quint32>(p+68)==bytes.size()-72)||fail(error,"이동 패키지 KDF 비용 또는 길이가 허용 범위를 벗어났습니다.");
}
bool derive(const SecureBuffer& password,const unsigned char* salt,quint32 ops,quint64 mem,SecureBuffer* key,QString* error){
    if(password.empty()||password.size()>std::size_t(Vault::MaxPasswordBytes))return fail(error,"이동 암호 길이가 허용 범위를 벗어났습니다.");SecureBuffer next(32);
    if(crypto_pwhash(next.data(),next.size(),reinterpret_cast<const char*>(password.data()),password.size(),salt,ops,std::size_t(mem),crypto_pwhash_ALG_ARGON2ID13))return fail(error,"이동 암호 키 유도에 실패했습니다.");*key=std::move(next);return true;
}
} // namespace

struct RecordTransfer::Private {
    RecordTransfer* owner;Vault& vault;QString path,origin;SecureBuffer key,plain;QByteArray digest;std::unique_ptr<QLockFile> fileLock;quint64 generation=0;bool legacy=false;
    Private(RecordTransfer* q,Vault& v):owner(q),vault(v){
        const auto directory=QFileInfo(v.filePath()).dir();path=directory.filePath(storage_detail::readableFileName(directory.path(),storage_detail::Kind::Imported));
    }
    bool thread()const{return QThread::currentThread()==owner->thread();}
    bool borrow(bool create,storage_detail::Format format,QString* error){
        bool derived=false;
        return vault.withAssetsLedgerKey(create,[&](const SecureBuffer& base){derived=storage_detail::deriveStoreKey(base,storage_detail::Kind::Imported,format,&key,error);},error)&&derived;
    }
    bool acquire(QString* error){
        if(!Vault::prepareProfileDirectory(QFileInfo(path).absolutePath(),error)||QFileInfo(path).isSymLink()||QFileInfo(path+".lock").isSymLink())return fail(error,"이관 저장소 경로가 안전하지 않습니다.");
        if(QFileInfo::exists(path+".lock")){
            // QLockFile contains PID/host metadata, not records. Qt may create it
            // with mode 0644; a dead child's lock must remain reacquirable.
            if(!portablePath(path+".lock",false,error))return false;
#ifdef Q_OS_UNIX
            struct stat st{};if(::lstat(QFile::encodeName(path+".lock").constData(),&st)||st.st_uid!=::geteuid())return fail(error,"다른 소유자의 기록 잠금 파일을 거부했습니다.");
#elif defined(Q_OS_WIN)
            if(!privateWindowsPath(path+".lock",false))return fail(error,"기록 잠금 파일 권한이 안전하지 않습니다.");
#endif
        }
        fileLock=std::make_unique<QLockFile>(path+".lock");fileLock->setStaleLockTime(0);return fileLock->tryLock(0)||fail(error,"다른 작업이 이관 저장소를 사용하고 있습니다.");
    }
};
RecordTransfer::RecordTransfer(Vault& vault,QObject* parent):QObject(parent),d_(std::make_unique<Private>(this,vault)){connect(&vault,&Vault::aboutToLock,this,&RecordTransfer::lock);}
RecordTransfer::~RecordTransfer(){lock();}
bool RecordTransfer::exists()const{return QFileInfo::exists(d_->path);}
bool RecordTransfer::isOpen()const{return d_->thread()&&d_->vault.isUnlocked()&&d_->key.size()==32&&!d_->origin.isEmpty()&&d_->fileLock&&d_->fileLock->isLocked();}
QString RecordTransfer::filePath()const{return d_->path;}
quint64 RecordTransfer::generation()const{return d_->generation;}
void RecordTransfer::lock(){++d_->generation;d_->plain.clear();d_->origin.clear();d_->key.clear();d_->digest.clear();d_->legacy=false;if(d_->fileLock)d_->fileLock->unlock();d_->fileLock.reset();emit locked();}
QJsonArray RecordTransfer::records()const{RecordTransferBundle data;QString origin;if(isOpen()&&deserialize(d_->plain,&data,&origin,nullptr))return data.records;return {};}
bool RecordTransfer::initialize(QString* error){
    if(!d_->thread()||!d_->vault.isUnlocked())return fail(error,"기록 이동 준비에는 금고 잠금 해제가 필요합니다.");if(isOpen())return true;if(exists())return open(error);
    try{
        if(!d_->acquire(error)||!d_->borrow(true,storage_detail::Format::Current,error)){lock();return false;}const auto origin=QUuid::createUuid().toString(QUuid::WithoutBraces);auto plain=serialize({},origin);const auto cipher=storeEncrypt(plain,d_->key);SecureBuffer checked;RecordTransferBundle verified;QString checkedOrigin;
        if(cipher.isEmpty()||!storeDecrypt(cipher,d_->key,&checked,error)||!deserialize(checked,&verified,&checkedOrigin,error)||!atomicCipher(d_->path,cipher,{},true,error)){lock();return false;}
        d_->origin=origin;d_->plain=std::move(plain);d_->digest=QCryptographicHash::hash(cipher,QCryptographicHash::Sha256);emit changed();return true;
    }catch(const std::exception&){lock();return fail(error,"기록 이동 초기 준비에 실패했습니다. 기존 기록은 덮어쓰지 않았습니다.");}
}
bool RecordTransfer::open(QString* error){
    if(!d_->thread()||!d_->vault.isUnlocked())return fail(error,"기록 이동 저장소를 열려면 금고를 열어 주세요.");if(isOpen())return true;
    try{if(!d_->acquire(error)){lock();return false;}QByteArray bytes;SecureBuffer plain;RecordTransferBundle data;QString origin;
        if(!readCipher(d_->path,&bytes,error)){lock();return false;}
        const auto format=storage_detail::formatOf(bytes,storage_detail::Kind::Imported);d_->legacy=format==storage_detail::Format::Legacy;
        if(!d_->borrow(false,format,error)||!storeDecrypt(bytes,d_->key,&plain,error)||!deserialize(plain,&data,&origin,error)){lock();return false;}
        d_->origin=origin;d_->plain=std::move(plain);d_->digest=QCryptographicHash::hash(bytes,QCryptographicHash::Sha256);emit changed();return true;
    }catch(const std::exception&){lock();return fail(error,"이관 저장소 보호 메모리 또는 인증에 실패했습니다.");}
}
bool RecordTransfer::collect(const RecordTransferSelection& selection,AssetsLedger* sharedHistory,SwapJournal* sharedSwap,RecordTransferBundle* output,QString* error,AssetsLedger* sharedLatest){
    if(!isOpen()||!output)return fail(error,"기록 이동 준비와 금고 잠금 해제가 필요합니다.");
    try{
        const auto folder=QFileInfo(d_->path).dir();RecordTransferBundle next;QMap<QString,QJsonObject> gathered;QMap<QString,AssetSnapshot> scopes;
        QList<AssetSnapshot> snapshots;QList<AssetEvent> events;QList<AssetOrderObservation> orders;
        const auto take=[&](AssetsLedger& ledger,bool latest){
            if(ledger.isWriting())return fail(error,"자산 저장 checkpoint가 아직 완료되지 않았습니다.");
            snapshots.append(ledger.snapshots());if(!latest){events.append(ledger.events());orders.append(ledger.orders());}
            return snapshots.size()+events.size()+orders.size()<=MaxRecords||fail(error,"원본 자산 기록 수가 이동 상한을 넘었습니다. 생략하지 않았습니다.");
        };
        const auto load=[&](const QString& name,AssetsLedger* shared,bool latest){
            const auto path=folder.filePath(name);if(shared&&QFileInfo(shared->filePath()).absoluteFilePath()!=QFileInfo(path).absoluteFilePath())return fail(error,"공유 원장의 출처 경로가 일치하지 않습니다.");
            if(shared&&shared->isOpen())return take(*shared,latest);
            if(!QFileInfo::exists(path)){if(QFileInfo(path).isSymLink())return fail(error,"원본 원장 경로가 안전하지 않습니다.");return true;}
            if(shared){if(!shared->open(false,error))return false;return take(*shared,latest);}
            AssetsLedger temporary(&d_->vault,path);if(!temporary.open(false,error))return false;return take(temporary,latest);
        };
        const bool assets=selection.snapshots||selection.events||selection.orders;
        if(assets){
            const auto history=storage_detail::readableFileName(folder.path(),storage_detail::Kind::Assets);
            if(!load(history,sharedHistory,false))return false;
            const auto patterns=storage_detail::archivePatterns(history);
            QStringList archives=folder.entryList(patterns,QDir::Files|QDir::System,QDir::Name);
            if(archives.size()>256)return fail(error,"과거 보관 원장 수가 상한을 넘었습니다. 생략하지 않았습니다.");
            for(const auto& name:archives){AssetsLedger archive(&d_->vault,folder.filePath(name));if(!archive.open(false,error)||!take(archive,false))return false;}
            if(folder.entryList(patterns,QDir::Files|QDir::System,QDir::Name)!=archives)return fail(error,"수집 중 과거 원장 목록이 바뀌었습니다. 다시 checkpoint 후 내보내세요.");
            if(!load(storage_detail::readableLatestAssetsFileName(folder.path()),sharedLatest,true))return false;
            next.sourceSummary<<QString("자산 이력·과거 보관 %1개와 마지막 자산 자료 · 스냅샷 %2 / 사건 %3 / 주문 관찰 %4개").arg(archives.size()).arg(snapshots.size()).arg(events.size()).arg(orders.size());
        }
        for(const auto& s:snapshots){if(scopes.contains(s.accountScope)&&(scopes[s.accountScope].provider!=s.provider||scopes[s.accountScope].environment!=s.environment||scopes[s.accountScope].market!=s.market))return fail(error,"같은 원본 계좌 범위에 서로 다른 환경·시장이 있습니다.");scopes[s.accountScope]=s;}
        const auto add=[&](const QJsonObject& record){const auto key=record["origin"].toString()+":"+record["kind"].toString()+":"+record["identity"].toString();
            if(gathered.contains(key)&&gathered[key]!=record)return fail(error,"원본 기록 식별자 충돌로 내보내기를 중단했습니다.");gathered[key]=record;return gathered.size()<=MaxRecords||fail(error,"이동 기록 수 상한을 넘었습니다. 생략하지 않았습니다.");};
        if(selection.snapshots)for(const auto& s:snapshots)if(!add(wrap(d_->origin,"snapshot",snapshot(s,d_->origin))))return false;
        if(selection.events)for(const auto& e:events)if(!add(wrap(d_->origin,"event",portableEvent(e,d_->origin))))return false;
        if(selection.orders)for(const auto& o:orders)if(!add(wrap(d_->origin,"order",order(o,d_->origin,scopes.contains(o.accountScope)?scopes[o.accountScope].environment:"unknown"))))return false;
        if(selection.swaps){
            const auto swapPath=folder.filePath(storage_detail::readableFileName(folder.path(),storage_detail::Kind::Swap));
            QList<SwapPendingRecord> rows;if(sharedSwap&&sharedSwap->isOpen())rows=sharedSwap->records();
            else if(QFileInfo::exists(swapPath)||QFileInfo(swapPath).isSymLink()){
                if(sharedSwap){if(!sharedSwap->open(false,error))return false;rows=sharedSwap->records();}
                else{SwapJournal journal(&d_->vault,swapPath);if(!journal.open(false,error))return false;rows=journal.records();}
            }
            for(const auto& r:rows)if(!add(wrap(d_->origin,"swap",swap(r,d_->origin))))return false;
            next.sourceSummary<<QString("스왑 관찰 %1개").arg(rows.size());
        }
        if(selection.pending){const auto rows=d_->vault.pendingOperations();for(const auto& value:rows)if(!add(wrap(d_->origin,"pending",pending(value.toObject(),d_->origin))))return false;next.sourceSummary<<QString("미확정 요청 비실행 관찰 %1개").arg(rows.size());}
        int imported=0;for(const auto& value:records()){const auto kind=value.toObject()["kind"].toString();const bool selected=(kind=="snapshot"&&selection.snapshots)||(kind=="event"&&selection.events)||(kind=="order"&&selection.orders)||(kind=="swap"&&selection.swaps)||(kind=="pending"&&selection.pending);if(selected){if(!add(value.toObject()))return false;++imported;}}
        for(const auto& record:gathered)next.records.append(record);next.sourceSummary<<QString("이전에 가져온 기록 %1개 · 원래 출처 유지").arg(imported)<<"제외: 금융 인증정보·금고·내부 연결 식별자·서명 원문·시세 캐시";
        if(!validate(next,error))return false;*output=std::move(next);return true;
    }catch(const std::exception&){return fail(error,"기록 수집 보호 메모리 처리에 실패했습니다.");}
}
bool RecordTransfer::commit(const RecordTransferBundle& incoming,quint64 expectedGeneration,RecordTransferResult* result,QString* error,const RecordTransferCancellation& cancel,const std::function<bool()>& precommitCheck){
    if(!isOpen()||expectedGeneration!=d_->generation||stopped(cancel))return fail(error,"금고 잠금 또는 취소로 가져오기 결과를 반영하지 않았습니다.");
    try{
        if(!validate(incoming,error))return false;QMap<QString,QJsonObject> rows;const auto id=[](const QJsonObject& o){return o["origin"].toString()+":"+o["kind"].toString()+":"+o["identity"].toString();};
        for(const auto& value:records())rows[id(value.toObject())]=value.toObject();RecordTransferResult receipt;
        for(const auto& value:incoming.records){const auto o=value.toObject();const auto k=id(o);if(rows.contains(k)){if(rows[k]!=o)return fail(error,"같은 출처·기록 식별자에 다른 내용이 있어 전체 가져오기를 거부했습니다.");++receipt.duplicates;}else{rows[k]=o;++receipt.added;}}
        if(rows.size()>MaxRecords)return fail(error,"가져온 뒤 기록 수가 상한을 넘습니다. 기존 기록을 보존했습니다.");receipt.total=rows.size();
        RecordTransferBundle merged;for(const auto& row:rows)merged.records.append(row);if(!validate(merged,error))return false;
        QByteArray current;if(!readCipher(d_->path,&current,error)||QCryptographicHash::hash(current,QCryptographicHash::Sha256)!=d_->digest){lock();return fail(error,"이관 저장소의 외부 변경을 감지했습니다. 기존 파일을 보존했습니다.");}
        if(!receipt.added){if(result)*result=receipt;return true;}
        if(!storage_detail::rejectLegacyWrite(d_->legacy,error))return false;
        RecordTransferBundle next;for(const auto& value:rows)next.records.append(value);auto plain=serialize(next,d_->origin);const auto cipher=storeEncrypt(plain,d_->key);SecureBuffer checked;RecordTransferBundle verified;QString origin;
        if(cipher.isEmpty()||!storeDecrypt(cipher,d_->key,&checked,error)||!deserialize(checked,&verified,&origin,error))return fail(error,"가져오기 암호문·크기 검증에 실패했습니다.");
        if(!atomicCipher(d_->path,cipher,d_->digest,false,error,cancel,[&]{return isOpen()&&expectedGeneration==d_->generation;},precommitCheck)){lock();return false;}
        d_->plain=std::move(plain);d_->digest=QCryptographicHash::hash(cipher,QCryptographicHash::Sha256);if(result)*result=receipt;emit changed();return true;
    }catch(const std::exception&){return fail(error,"가져오기 보호 메모리 처리에 실패했습니다. 기존 기록을 보존했습니다.");}
}
bool RecordTransfer::encodePackage(const RecordTransferBundle& data,const SecureBuffer& password,QByteArray* output,QString* error,const RecordTransferCancellation& cancel){
    if(!output||stopped(cancel))return fail(error,"이동 패키지 생성을 취소했습니다.");
    try{
        if(!validate(data,error)||password.empty()||password.size()>std::size_t(Vault::MaxPasswordBytes))return fail(error,"이동 기록 또는 암호 길이가 올바르지 않습니다.");
        QString check=QString::fromUtf8(reinterpret_cast<const char*>(password.data()),qsizetype(password.size()));const bool policy=Vault::validateNewPassword(check,error);check.fill(QChar(0));check.clear();if(!policy)return false;
        auto plain=serialize(data);if(plain.empty())return fail(error,"패키지 크기 상한을 넘었습니다. 기록은 생략하지 않았습니다.");
        QByteArray cipher(72+qsizetype(plain.size())+16,'\0');auto* p=reinterpret_cast<unsigned char*>(cipher.data());std::memcpy(p,"KEEFIRTX",8);qToLittleEndian<quint16>(1,p+8);qToLittleEndian<quint16>(1,p+10);qToLittleEndian<quint16>(1,p+12);qToLittleEndian<quint32>(3,p+16);qToLittleEndian<quint64>(64ULL*1024*1024,p+20);randombytes_buf(p+28,16);randombytes_buf(p+44,24);qToLittleEndian<quint32>(quint32(plain.size()+16),p+68);
        SecureBuffer key;if(!derive(password,p+28,3,64ULL*1024*1024,&key,error)||stopped(cancel))return fail(error,"이동 패키지 생성을 취소했거나 키 유도에 실패했습니다.");unsigned long long size=0;
        if(crypto_aead_xchacha20poly1305_ietf_encrypt(p+72,&size,plain.data(),plain.size(),p,72,nullptr,p+44,key.data())||size!=plain.size()+16||stopped(cancel))return fail(error,"이동 패키지 암호화에 실패했거나 취소했습니다.");*output=std::move(cipher);return true;
    }catch(const std::exception&){return fail(error,"이동 패키지 보호 메모리 또는 암호화에 실패했습니다.");}
}
bool RecordTransfer::decodePackage(const QByteArray& bytes,const SecureBuffer& password,RecordTransferBundle* output,QString* error,const RecordTransferCancellation& cancel){
    if(!output||stopped(cancel))return fail(error,"가져오기를 취소했습니다.");
    try{
        quint32 ops=0;quint64 mem=0;if(!packageHeader(bytes,&ops,&mem,error))return false;const auto* p=reinterpret_cast<const unsigned char*>(bytes.constData());SecureBuffer key;
        if(!derive(password,p+28,ops,mem,&key,error)||stopped(cancel))return fail(error,"가져오기를 취소했거나 키 유도에 실패했습니다.");SecureBuffer plain(std::size_t(bytes.size()-88));unsigned long long size=0;
        if(crypto_aead_xchacha20poly1305_ietf_decrypt(plain.data(),&size,nullptr,p+72,bytes.size()-72,p,72,p+44,key.data())||size!=plain.size())return fail(error,"이동 암호가 다르거나 패키지가 변조되었습니다.");
        RecordTransferBundle next;if(!deserialize(plain,&next,nullptr,error)||stopped(cancel))return fail(error,"가져오기 자료 검증에 실패했거나 취소했습니다.");next.sourceSummary<<QString("암호화 패키지 관찰 기록 %1개").arg(next.records.size());*output=std::move(next);return true;
    }catch(const std::exception&){return fail(error,"가져오기 보호 메모리 또는 복호화에 실패했습니다.");}
}
bool RecordTransfer::readPackageFile(const QString& path,QByteArray* output,QString* error,const RecordTransferCancellation& cancel){
    if(!output||stopped(cancel))return fail(error,"가져오기 읽기를 취소했습니다.");QByteArray bytes;if(!readCipher(path,&bytes,error,false)||stopped(cancel))return false;quint32 ops=0;quint64 mem=0;if(!packageHeader(bytes,&ops,&mem,error))return false;*output=std::move(bytes);return true;
}
bool RecordTransfer::writePackageFile(const QString& path,const QByteArray& bytes,QString* error,const RecordTransferCancellation& cancel){
    if(storage_detail::formatOf(bytes,storage_detail::Kind::Package)!=storage_detail::Format::Current)return fail(error,QStringLiteral("새 이동 패키지는 현재 저장 형식으로 내보내야 합니다."));
    quint32 ops=0;quint64 mem=0;if(!packageHeader(bytes,&ops,&mem,error))return false;return atomicCipher(QFileInfo(path).absoluteFilePath(),bytes,{},true,error,cancel,{},{},false);
}
namespace storage_detail {
bool validateImportedPayload(const SecureBuffer& plain,QString* error){
    RecordTransferBundle records;QString origin;
    return deserialize(plain,&records,&origin,error);
}
} // namespace storage_detail
} // namespace keefi
