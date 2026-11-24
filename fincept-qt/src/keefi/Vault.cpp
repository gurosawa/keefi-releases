#include "keefi/Vault.h"
#include "keefi/ProductIdentity.h"
#include "keefi/IdentityCompatibility.h"
#include "keefi/LegacyStorageCodecPrivate.h"
#include "keefi/ExchangeIdentifiers.h"
#include "keefi/BrokerOrderReferences.h"
#include "keefi/OkxProtocol.h"
#include "keefi/SecretInput.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QSet>
#include <QRegularExpression>
#include "StagedStorageFile.h"
#include <QUuid>
#include <QtEndian>
#include <sodium.h>
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <vector>

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
constexpr int MaxFields = 16;
constexpr int MaxSecretBytes = 8192;
constexpr int MaxMetadataBytes = 128 * 1024;
constexpr quint64 MaxMemLimit = 256ULL * 1024 * 1024;
constexpr quint32 MaxOpsLimit = 6;
constexpr char Magic[] = "KEEFIVLT";
constexpr char PayloadMagic[] = "FVP1";
static_assert(crypto_pwhash_SALTBYTES == 16);
static_assert(crypto_aead_xchacha20poly1305_ietf_KEYBYTES == 32);
static_assert(crypto_aead_xchacha20poly1305_ietf_NPUBBYTES == 24);

bool fail(QString* error, const QString& message) {
    if (error)
        *error = message;
    return false;
}
void clearError(QString* error) { if (error) error->clear(); }

bool validUnicode(const QString& text, int* characters = nullptr, int* utf8Bytes = nullptr) {
    int count = 0, bytes = 0;
    for (qsizetype i = 0; i < text.size(); ++i, ++count) {
        const QChar c = text.at(i);
        if (c.isHighSurrogate()) {
            if (++i >= text.size() || !text.at(i).isLowSurrogate())
                return false;
            bytes += 4;
        } else if (c.isLowSurrogate()) {
            return false;
        } else {
            bytes += c.unicode() < 0x80 ? 1 : c.unicode() < 0x800 ? 2 : 3;
        }
    }
    if (characters)
        *characters = count;
    if (utf8Bytes)
        *utf8Bytes = bytes;
    return true;
}

// Validate without constructing an ordinary QString containing a secret.
bool validCredentialUtf8(const SecureBuffer& bytes) {
    const auto* p = bytes.data();
    for (std::size_t i = 0; i < bytes.size();) {
        const unsigned c = p[i++];
        if (c < 0x20 || c == 0x7f)
            return false;
        if (c < 0x80)
            continue;
        unsigned count = 0, cp = 0, minimum = 0;
        if (c >= 0xc2 && c <= 0xdf) { count = 1; cp = c & 0x1f; minimum = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { count = 2; cp = c & 0x0f; minimum = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { count = 3; cp = c & 7; minimum = 0x10000; }
        else return false;
        if (count > bytes.size() - i)
            return false;
        while (count--) {
            const unsigned next = p[i++];
            if ((next & 0xc0) != 0x80)
                return false;
            cp = (cp << 6) | (next & 0x3f);
        }
        if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
            return false;
    }
    return true;
}

struct CredentialStorage {
    QString id, provider, environment, region;
    StorageMode mode = StorageMode::EncryptedVault;
    CredentialSet values;
};
struct Entry {
    AccountRecord record;
    std::shared_ptr<const CredentialStorage> credentials;
};
bool canonicalUuid(const QString& value) {
    const QUuid id(value); return !id.isNull() && id.toString(QUuid::WithoutBraces)==value;
}
std::vector<Entry> cloneEntries(const std::vector<Entry>& source) { return source; }
bool credentialMatches(const Entry& entry) {
    const auto& r=entry.record; const auto& c=entry.credentials;
    return c && canonicalUuid(c->id) && r.credentialRef==c->id && r.provider==c->provider &&
        r.environment==c->environment && r.region==c->region && r.storageMode==c->mode;
}
bool validCredentialReferences(const std::vector<Entry>& entries) {
    QMap<QString,const CredentialStorage*> references;
    QMap<QString,QSet<QString>> markets;
    for(const auto& entry:entries){
        if(!credentialMatches(entry))return false;
        const auto& ref=entry.record.credentialRef;
        if(references.contains(ref)){
            if(references.value(ref)!=entry.credentials.get() || entry.record.provider!="binance" ||
                entry.record.environment!="live" || !entry.record.region.isEmpty() ||
                !QStringList{"spot","usdm_futures"}.contains(entry.record.market) || markets[ref].contains(entry.record.market))return false;
        }
        references[ref]=entry.credentials.get();markets[ref].insert(entry.record.market);
    }
    return references.size()<=Vault::MaxAccounts;
}
bool validNewMarketEnvironment(const AccountRecord& r) {
    return r.provider!="binance" || ((r.market=="spot" || r.market=="usdm_futures") && r.environment=="live");
}
std::shared_ptr<CredentialStorage> storageFor(const AccountRecord& r) {
    auto c=std::make_shared<CredentialStorage>(); c->id=r.credentialRef;c->provider=r.provider;
    c->environment=r.environment;c->region=r.region;c->mode=r.storageMode;return c;
}

bool validateCredentials(const AccountRecord& record, const CredentialSet& values, QString* error) {
    const auto* provider = AccountModel::provider(record.provider);
    if (!provider || values.size() > MaxFields)
        return fail(error, QStringLiteral("인증정보 스키마가 올바르지 않습니다."));
    for (const auto& field : values) {
        auto spec = std::find_if(provider->fields.cbegin(), provider->fields.cend(),
                                 [&](const CredentialField& f) { return f.key == field.first; });
        if (spec == provider->fields.cend() || field.second.empty() || field.second.size() > MaxSecretBytes ||
            !validCredentialUtf8(field.second))
            return fail(error, QStringLiteral("허용되지 않는 인증 항목·길이·문자 형식입니다."));
    }
    for (const auto& field : provider->fields)
        if (field.required && values.find(field.key) == values.end())
            return fail(error, QStringLiteral("필수 API 인증 항목을 입력해 주세요."));
    return true;
}

bool inputCredentials(const AccountRecord& record, const QMap<QString, QString>& input,
                      CredentialSet* output, QString* error) {
    if (input.size() > MaxFields)
        return fail(error, QStringLiteral("인증 항목 수가 상한을 초과했습니다."));
    const auto* provider = AccountModel::provider(record.provider);
    if (!provider)
        return fail(error, QStringLiteral("지원하지 않는 금융 서비스입니다."));
    for (auto it = input.cbegin(); it != input.cend(); ++it) {
        const auto spec = std::find_if(provider->fields.cbegin(), provider->fields.cend(),
                                       [&](const CredentialField& f) { return f.key == it.key(); });
        if (spec == provider->fields.cend() || it.value().size() > MaxSecretBytes || !validUnicode(it.value()))
            return fail(error, QStringLiteral("허용되지 않는 인증 항목·길이·문자 형식입니다."));
        if (!it.value().isEmpty())
            output->emplace(it.key(), SecureBuffer::fromUtf8(it.value()));
    }
    return validateCredentials(record, *output, error);
}

struct Cursor {
    const unsigned char* data;
    std::size_t remaining;
    const unsigned char* take(std::size_t size) {
        if (size > remaining)
            return nullptr;
        const auto* value = data;
        data += size;
        remaining -= size;
        return value;
    }
    bool u32(quint32& value) {
        const auto* p = take(4);
        if (!p) return false;
        value = qFromLittleEndian<quint32>(p);
        return true;
    }
};

// Local schema checks do not depend on the trading service or rewrite decimals.
bool journalDecimal(const QString& text, bool positive = false) {
    static const QRegularExpression re("\\A[0-9]{1,80}(?:\\.[0-9]{1,80})?\\z");
    return re.match(text).hasMatch() && (!positive || QRegularExpression("[1-9]").match(text).hasMatch());
}
int journalDecimalCompare(QString left, QString right) {
    const int ls = left.contains('.') ? left.size() - left.indexOf('.') - 1 : 0;
    const int rs = right.contains('.') ? right.size() - right.indexOf('.') - 1 : 0;
    left.remove('.'); right.remove('.'); left += QString(qMax(ls, rs) - ls, '0'); right += QString(qMax(ls, rs) - rs, '0');
    while (left.size() > 1 && left.startsWith('0')) left.remove(0, 1);
    while (right.size() > 1 && right.startsWith('0')) right.remove(0, 1);
    if (left.size() != right.size()) return left.size() < right.size() ? -1 : 1;
    return QString::compare(left, right);
}
bool journalMillis(const QString& text) {
    static const QRegularExpression re("\\A[0-9]{1,19}\\z");
    bool ok = false; const auto value = text.toLongLong(&ok);
    return re.match(text).hasMatch() && ok && value > 0;
}
bool validOkxPending(const QJsonObject& o) {
    const QSet<QString> added{"market", "region", "accountScope", "amendRequestId", "quantityUnit", "ackState",
        "beforePrice", "beforeQuantity", "beforeExecuted", "beforeUpdatedAt"};
    if (o.size() != 27) return false;
    for (const auto& name : added) if (!o.value(name).isString()) return false;
    const auto v = [&](const char* name) { return o.value(name).toString(); };
    static const QRegularExpression hash("\\A[a-f0-9]{64}\\z"), fresh("\\A[a-f0-9]{32}\\z");
    if (v("market") != "spot" || !okxRegionValid(v("region")) || !okxEnvironmentValid(v("environment")) ||
        !hash.match(v("accountScope")).hasMatch() || !hash.match(v("fingerprint")).hasMatch() ||
        !okxInstrumentValid(v("symbol")) || !v("replacementClientId").isEmpty() || !journalMillis(v("createdAt")) ||
        !QStringList{"not_received", "accepted", "rejected", "unknown"}.contains(v("ackState")) ||
        !QStringList{"base", "quote"}.contains(v("quantityUnit")) || !QStringList{"buy", "sell"}.contains(v("side")) ||
        (!v("orderId").isEmpty() && !okxOrderIdValid(v("orderId"))) || !okxClientIdValid(v("clientId"))) return false;
    const bool amend = v("kind") == "amend", cancel = v("kind") == "cancel", place = v("kind") == "place";
    if ((!amend && !cancel && !place) || ((amend || cancel) && v("orderId").isEmpty()) ||
        (place && !fresh.match(v("clientId")).hasMatch()) ||
        (amend ? !fresh.match(v("amendRequestId")).hasMatch() : !v("amendRequestId").isEmpty())) return false;
    if (amend) {
        if (v("type") != "limit" || v("quantityUnit") != "base" ||
            !journalDecimal(v("beforePrice"), true) || !journalDecimal(v("beforeQuantity"), true) ||
            !journalDecimal(v("beforeExecuted")) || !journalMillis(v("beforeUpdatedAt")) ||
            journalDecimalCompare(v("beforeExecuted"), v("beforeQuantity")) >= 0 ||
            !journalDecimal(v("quantity"), true) || journalDecimalCompare(v("quantity"), v("beforeExecuted")) <= 0) return false;
    } else for (const auto* name : {"beforePrice", "beforeQuantity", "beforeExecuted", "beforeUpdatedAt"})
        if (!v(name).isEmpty()) return false;
    if (v("type") == "market") {
        if (!v("price").isEmpty()) return false;
        if (!cancel && v("quantityUnit") != (v("side") == "buy" ? "quote" : "base")) return false;
        return v("quantityUnit") == "quote" ? journalDecimal(v("quoteAmount"), true) && v("quantity").isEmpty() :
            journalDecimal(v("quantity"), true) && v("quoteAmount").isEmpty();
    }
    if (!(cancel ? QStringList{"limit", "post_only", "ioc", "fok"} : QStringList{"limit"}).contains(v("type"))) return false;
    return v("quantityUnit") == "base" && journalDecimal(v("price"), true) &&
        journalDecimal(v("quantity"), true) && v("quoteAmount").isEmpty();
}

bool positiveInt64(const QString& text) {
    bool ok=false;const auto value=text.toLongLong(&ok);
    return QRegularExpression("\\A[0-9]{1,19}\\z").match(text).hasMatch()&&ok&&value>0;
}
bool validFuturesPending(const QJsonObject& o,int schemaVersion) {
    if(o.size()!=25)return false;
    for(const auto* key:{"market","credentialRef","orderFamily","algoId","clientAlgoId","actualOrderId"})if(!o.value(key).isString())return false;
    if(!o.value("futures").isObject()||!o.value("before").isObject())return false;
    const auto f=o.value("futures").toObject(),before=o.value("before").toObject();
    if((f.size()!=10&&f.size()!=11)||before.size()!=4)return false;
    if(f.contains("priceProtect")&&(schemaVersion<9||!f.value("priceProtect").isBool()))return false;
    if(f.size()==11&&!f.contains("priceProtect"))return false;
    for(const auto* key:{"positionSide","exposure","timeInForce","workingType","triggerPrice","activationPrice","callbackRate","modifyId"})if(!f.value(key).isString())return false;
    for(const auto* key:{"reduceOnly","closePosition"})if(!f.value(key).isBool())return false;
    for(const auto* key:{"price","quantity","executed","updatedAt"})if(!before.value(key).isString())return false;
    const auto v=[&](const char* key){return o.value(key).toString();};
    const auto t=[&](const char* key){return f.value(key).toString();};
    const auto b=[&](const char* key){return before.value(key).toString();};
    const bool conditional=v("orderFamily")=="conditional",amend=v("kind")=="amend",place=v("kind")=="place",cancel=v("kind")=="cancel";
    const auto client=[](const QString& value,bool required){return (!required&&value.isEmpty())||QRegularExpression("\\A[.A-Za-z0-9_:/-]{1,36}\\z").match(value).hasMatch();};
    if(v("provider")!="binance"||v("market")!="usdm_futures"||!QStringList{"live","demo"}.contains(v("environment"))||
        !canonicalUuid(v("credentialRef"))||!QStringList{"ordinary","conditional"}.contains(v("orderFamily"))||
        !QStringList{"BOTH","LONG","SHORT"}.contains(t("positionSide"))||!QStringList{"unspecified","open","reduce"}.contains(t("exposure"))||
        !QStringList{"buy","sell"}.contains(v("side"))||!journalMillis(v("createdAt"))||!v("quoteAmount").isEmpty()||!v("replacementClientId").isEmpty()||
        !QStringList{"","GTC","IOC","FOK","GTX","GTD"}.contains(t("timeInForce"))||
        (!t("workingType").isEmpty()&&!QStringList{"CONTRACT_PRICE","MARK_PRICE"}.contains(t("workingType")))||
        (f.value("reduceOnly").toBool()&&t("positionSide")!="BOTH")||
        (f.value("closePosition").toBool()&&f.value("reduceOnly").toBool()))return false;
    if(conditional){
        if((!cancel&&!place)||!v("orderId").isEmpty()||!v("clientId").isEmpty()||
            (cancel?!positiveInt64(v("algoId")):(!v("algoId").isEmpty()&&!positiveInt64(v("algoId"))))||!client(v("clientAlgoId"),place)||
            (!v("actualOrderId").isEmpty()&&!positiveInt64(v("actualOrderId")))||
            !QStringList{"stop","stop_market","take_profit","take_profit_market","trailing_stop_market"}.contains(v("type")))return false;
        if(place&&(schemaVersion<9||!QStringList{"stop","take_profit"}.contains(v("type"))||t("timeInForce")!="GTC"||
            f.value("closePosition").toBool()||f.value("priceProtect")!=QJsonValue(false)||!v("actualOrderId").isEmpty()||
            !QStringList{"CONTRACT_PRICE","MARK_PRICE"}.contains(t("workingType"))||t("exposure")=="unspecified"||
            (t("exposure")=="open"&&f.value("reduceOnly").toBool())||
            (t("exposure")=="reduce"&&t("positionSide")=="BOTH"&&!f.value("reduceOnly").toBool())))return false;
        if(f.value("closePosition").toBool()){
            if(!QStringList{"stop_market","take_profit_market"}.contains(v("type"))||(!v("quantity").isEmpty()&&(!journalDecimal(v("quantity"))||journalDecimalCompare(v("quantity"),"0")!=0)))return false;
        }else if(!journalDecimal(v("quantity"),true))return false;
        if(v("type")=="stop"||v("type")=="take_profit"){if(!journalDecimal(v("price"),true))return false;}
        else if(!v("price").isEmpty()&&(!journalDecimal(v("price"))||journalDecimalCompare(v("price"),"0")!=0))return false;
        if(v("type")=="trailing_stop_market"){
            if((!t("triggerPrice").isEmpty()&&(!journalDecimal(t("triggerPrice"))||journalDecimalCompare(t("triggerPrice"),"0")!=0))||
                !journalDecimal(t("callbackRate"),true)||journalDecimalCompare(t("callbackRate"),"0.1")<0||journalDecimalCompare(t("callbackRate"),"10")>0||
                (!t("activationPrice").isEmpty()&&!journalDecimal(t("activationPrice"),true)))return false;
        }else if(!journalDecimal(t("triggerPrice"),true)||!t("activationPrice").isEmpty()||!t("callbackRate").isEmpty())return false;
    }else{
        if(!v("algoId").isEmpty()||!v("clientAlgoId").isEmpty()||!v("actualOrderId").isEmpty()||!client(v("clientId"),place)||
            (!v("orderId").isEmpty()&&!positiveInt64(v("orderId")))||((cancel||amend)&&v("orderId").isEmpty())||
            !QStringList{"limit","market"}.contains(v("type"))||!journalDecimal(v("quantity"),true)||f.value("closePosition").toBool()||f.contains("priceProtect")||
            !t("workingType").isEmpty()||!t("triggerPrice").isEmpty()||!t("activationPrice").isEmpty()||!t("callbackRate").isEmpty()||
            (place&&(t("exposure")=="unspecified"||(t("exposure")=="open"&&f.value("reduceOnly").toBool())||
                (t("exposure")=="reduce"&&t("positionSide")=="BOTH"&&!f.value("reduceOnly").toBool()))))return false;
        if(v("type")=="limit"?!journalDecimal(v("price"),true):!v("price").isEmpty())return false;
    }
    if(amend){
        if(conditional||v("type")!="limit"||!positiveInt64(t("modifyId"))||!journalDecimal(b("price"),true)||!journalDecimal(b("quantity"),true)||
            !journalDecimal(b("executed"))||journalDecimalCompare(b("executed"),b("quantity"))>=0||
            journalDecimalCompare(v("quantity"),b("executed"))<=0||!journalMillis(b("updatedAt")))return false;
    }else{
        if(!t("modifyId").isEmpty())return false;
        for(const auto* key:{"price","quantity","executed","updatedAt"})if(!b(key).isEmpty())return false;
    }
    return true;
}

bool validPending(const QJsonArray& operations, int schemaVersion = 9) {
    if (operations.size() > 128) return false;
    const QSet<QString> fields{"id", "accountId", "fingerprint", "provider", "environment", "symbol",
        "kind", "clientId", "orderId", "replacementClientId", "state", "createdAt",
        "quantity", "price", "quoteAmount", "side", "type"};
    const QRegularExpression identifier("^[A-Za-z0-9_.:/-]{0,128}$");
    const QRegularExpression fingerprint("^[a-f0-9]{64}$");
    const QRegularExpression accountScope("\\A[a-f0-9]{64}\\z");
    const QRegularExpression operationOrderId("\\A[A-Za-z0-9_-]{0,128}\\z");
    QSet<QString> ids;
    for (const auto& value : operations) {
        if (!value.isObject()) return false;
        const auto object = value.toObject();
        const bool futures = object.value("market") == QJsonValue("usdm_futures");
        const bool okx = object.value("provider") == QJsonValue("okx");
        const bool bithumb = object.value("provider") == QJsonValue("bithumb");
        const bool toss = object.value("provider").toString() == "toss";
        const bool kis = object.value("provider").toString() == "kis";
        const bool kiwoom = object.value("provider").toString() == "kiwoom";
        const bool ls = object.value("provider").toString() == "ls";
        const bool tripleReference = kiwoom || ls;
        const bool broker = toss || kis || tripleReference;
        const auto environment = object.value("environment").toString();
        if (futures) {
            if(schemaVersion<6||!validFuturesPending(object,schemaVersion))return false;
        } else if (okx) {
            if (schemaVersion < 5 || !validOkxPending(object)) return false;
        } else if (bithumb) {
            const auto v=[&](const char* key){return object.value(key).toString();};
            if(object.size()!=17 || environment!="live" || !QRegularExpression("\\A(?:KRW|BTC)-[A-Z0-9]{1,24}\\z").match(v("symbol")).hasMatch() ||
                !QStringList{"place","cancel"}.contains(v("kind")) || !QStringList{"buy","sell"}.contains(v("side")) ||
                !QStringList{"limit","market"}.contains(v("type")) || !v("replacementClientId").isEmpty() ||
                !QRegularExpression("\\A[A-Za-z0-9_-]{0,36}\\z").match(v("clientId")).hasMatch() ||
                (v("kind")=="place" && v("clientId").isEmpty()) || (v("kind")=="cancel" && v("orderId").isEmpty()))return false;
            if(v("type")=="limit"){
                if(!journalDecimal(v("price"),true)||!journalDecimal(v("quantity"),true)||!v("quoteAmount").isEmpty())return false;
            }else if(!v("price").isEmpty() || (v("side")=="buy" ? !v("quantity").isEmpty()||!journalDecimal(v("quoteAmount"),true) :
                !v("quoteAmount").isEmpty()||!journalDecimal(v("quantity"),true)))return false;
        } else if (broker) {
            if (schemaVersion < 3 || (tripleReference && schemaVersion < 4) || object.size() != fields.size() + (tripleReference ? 6 : 3) + (object.contains("brokerConditional")?1:0) ||
                (toss ? environment != "live" : (environment != "live" && environment != "paper")) ||
                object.value("market") != QJsonValue("domestic_equity") ||
                !object.value("accountScope").isString() ||
                !accountScope.match(object.value("accountScope").toString()).hasMatch() ||
                !object.value("operationOrderId").isString() ||
                !operationOrderId.match(object.value("operationOrderId").toString()).hasMatch()) return false;
            if(object.contains("brokerConditional")&&(schemaVersion<8||!toss||object.value("brokerConditional")!="true"||object.value("kind")!="cancel"||object.value("orderId").toString().isEmpty()||!object.value("operationOrderId").toString().isEmpty()))return false;
            if (tripleReference) {
                for (const auto* field : {"baseOriginalOrderId", "baseOriginalProvenance", "quantityMode"})
                    if (!object.value(field).isString()) return false;
                const auto base = object.value("baseOriginalOrderId").toString();
                const auto provenance = object.value("baseOriginalProvenance").toString();
                const auto mode = object.value("quantityMode").toString();
                const auto kind = object.value("kind").toString();
                const auto target = object.value("orderId").toString();
                const auto receipt = object.value("operationOrderId").toString();
                const auto validReference = [ls](const QString& ref) { return ls ? lsOrderReferenceValid(ref) : kiwoomOrderReferenceValid(ref); };
                if (!(ls ? mode == "exact" : QStringList{"exact", "all_remaining"}.contains(mode)) ||
                    (!target.isEmpty() && !validReference(target)) ||
                    (!receipt.isEmpty() && !validReference(receipt)) ||
                    (!base.isEmpty() && !validReference(base)) ||
                    (base.isEmpty() ? !provenance.isEmpty() : (provenance != "broker_receipt" || receipt.isEmpty())) ||
                    (kind == "place" && (mode != "exact" || !base.isEmpty() || !provenance.isEmpty())) ||
                    ((kind == "cancel" || kind == "amend") && target.isEmpty())) return false;
                if (mode == "all_remaining" || ls) {
                    const auto quantity = object.value("quantity").toString();
                    const QRegularExpression quantityFormat(ls ? QStringLiteral("\\A[0-9]{1,16}\\z") : QStringLiteral("\\A[0-9]{1,30}\\z"));
                    if (!quantityFormat.match(quantity).hasMatch() ||
                        !QRegularExpression("[1-9]").match(quantity).hasMatch()) return false;
                }
            }
        } else if(object.contains("orderListId")){
            const auto list=object.value("orderListId").toString();
            if(schemaVersion<8||object.size()!=fields.size()+1||!object.value("orderListId").isString()||(!positiveInt64(list)&&list!="0")||object.value("provider")!="binance"||object.value("kind")!="cancel"||!positiveInt64(object.value("orderId").toString()))return false;
        } else if(object.contains("replaceMode")){
            if(schemaVersion<8||object.size()!=fields.size()+2||object.value("provider")!="binance"||object.value("kind")!="amend"||object.value("type")!="limit"||object.value("replaceMode")!="cancel_replace"||
                !object.value("replacementState").isString()||!QStringList{"unknown","SUCCESS","FAILURE","NOT_ATTEMPTED"}.contains(object.value("replacementState").toString())||
                !positiveInt64(object.value("orderId").toString())||object.value("replacementClientId").toString().isEmpty()||!journalDecimal(object.value("price").toString(),true)||!journalDecimal(object.value("quantity").toString(),true)||!object.value("quoteAmount").toString().isEmpty())return false;
        } else if (object.size() != fields.size()) return false;
        for (const auto& key : fields) {
            if (!object.value(key).isString()) return false;
            const QString text = object.value(key).toString();
            const bool binanceSymbol = key == "symbol" && object.value("provider").toString() == "binance";
            const auto match = identifier.match(text);
            if (okx && (key == "symbol" || key == "quantity" || key == "price" || key == "quoteAmount")) continue;
            if (binanceSymbol ? !binanceIdentifierValid(text, 2) :
                (!match.hasMatch() || ((broker || okx || bithumb || futures) && match.capturedLength() != text.size()))) return false;
        }
        const QString id = object.value("id").toString();
        if (id.isEmpty() || ids.contains(id) || !fingerprint.match(object.value("fingerprint").toString()).hasMatch() ||
            (!broker && !okx && !QStringList{"upbit", "binance", "bithumb"}.contains(object.value("provider").toString())) ||
            !(okx || futures ? QStringList{"live", "demo"} : (kis || tripleReference) ? QStringList{"live", "paper"} : QStringList{"live", "testnet"}).contains(environment) ||
            !QStringList{"place", "cancel", "amend"}.contains(object.value("kind").toString()) ||
            !QStringList{"pending", "unknown"}.contains(object.value("state").toString()) ||
            object.value("symbol").toString().isEmpty()) return false;
        ids.insert(id);
    }
    return true;
}

bool encodePayload(const std::vector<Entry>& entries, const QJsonArray& pending, const SecureBuffer& ledgerKey, SecureBuffer* payload, QString* error) {
    if((!ledgerKey.empty()&&ledgerKey.size()!=32)||!validPending(pending) || !validCredentialReferences(entries))return fail(error,QStringLiteral("금고 참조·주문 기록 형식이 올바르지 않습니다."));
    QJsonArray metadata, sets; QList<std::shared_ptr<const CredentialStorage>> saved; QSet<QString> seen;
    std::size_t total=16+ledgerKey.size();
    for(const auto& entry:entries){
        if(entry.record.storageMode!=StorageMode::EncryptedVault)continue;
        // A retired legacy record must not stop the vault from saving other changes.
        if(!AccountModel::validate(entry.record,error,true)||!validateCredentials(entry.record,entry.credentials->values,error))return false;
        metadata.append(AccountModel::toJson(entry.record));
        const auto& c=entry.credentials;
        if(seen.contains(c->id))continue;
        seen.insert(c->id);saved.append(c);
        sets.append(QJsonObject{{"id",c->id},{"provider",c->provider},{"environment",c->environment},{"region",c->region},{"storage_mode","vault"}});
        total+=4;
        for(const auto& field:c->values)total+=8+std::size_t(field.first.toUtf8().size())+field.second.size();
    }
    const int schema=validPending(pending,7)?7:validPending(pending,8)?8:9; // Use the minimum compatible schema for typed journals.
    const auto json=QJsonDocument(QJsonObject{{"schema_version",schema},{"accounts",metadata},{"credential_sets",sets},{"pending_operations",pending}}).toJson(QJsonDocument::Compact);
    total+=std::size_t(json.size());
    if(metadata.size()>Vault::MaxAccounts||sets.size()>Vault::MaxAccounts||json.size()>MaxMetadataBytes||total>std::size_t(Vault::MaxFileBytes-Vault::HeaderBytes-16))
        return fail(error,QStringLiteral("금고 크기가 허용 상한을 초과했습니다."));
    SecureBuffer buffer(total);auto* write=buffer.data();
    const auto bytes=[&](const void* p,std::size_t n){std::memcpy(write,p,n);write+=n;};
    const auto u32=[&](quint32 n){qToLittleEndian(n,write);write+=4;};
    bytes(PayloadMagic,4);u32(quint32(json.size()));bytes(json.constData(),std::size_t(json.size()));u32(quint32(saved.size()));
    for(const auto& c:saved){
        u32(quint32(c->values.size()));
        for(const auto& field:c->values){const auto name=field.first.toUtf8();u32(quint32(name.size()));bytes(name.constData(),name.size());u32(quint32(field.second.size()));bytes(field.second.data(),field.second.size());}
    }
    u32(quint32(ledgerKey.size()));if(!ledgerKey.empty())bytes(ledgerKey.data(),ledgerKey.size());
    *payload=std::move(buffer);return true;
}

bool decodePayload(const SecureBuffer& payload, std::vector<Entry>* entries, QJsonArray* pending, SecureBuffer* ledgerKey, QString* error) {
    const auto invalid=[&]{return fail(error,QStringLiteral("복호화된 금고의 스키마가 올바르지 않습니다."));};
    Cursor cursor{payload.data(),payload.size()};const auto* magic=cursor.take(4);quint32 jsonSize=0,count=0;
    if(!magic||std::memcmp(magic,PayloadMagic,4)||!cursor.u32(jsonSize)||jsonSize>MaxMetadataBytes)return invalid();
    const auto* data=cursor.take(jsonSize);if(!data)return invalid();
    const QByteArray json(reinterpret_cast<const char*>(data),jsonSize);QJsonParseError parseError;
    const auto doc=QJsonDocument::fromJson(json,&parseError);
    if(parseError.error!=QJsonParseError::NoError||!doc.isObject()||doc.toJson(QJsonDocument::Compact)!=json)return invalid();
    const auto root=doc.object();const int version=root.value("schema_version").toInt(-1);
    if(root.value("schema_version")!=QJsonValue(version)||version<1||version>9||root.size()!=(version==1?2:version>=6?4:3)||
        !root.value("accounts").isArray()||root.value("accounts").toArray().size()>Vault::MaxAccounts||
        (version>=2&&(!root.value("pending_operations").isArray()||!validPending(root.value("pending_operations").toArray(),version)))||
        (version>=6&&!root.value("credential_sets").isArray())||!cursor.u32(count)||count>Vault::MaxAccounts)return invalid();
    const auto accounts=root.value("accounts").toArray();const auto sets=root.value("credential_sets").toArray();
    if(count!=quint32(version>=6?sets.size():accounts.size()))return invalid();
    *pending=version>=2?root.value("pending_operations").toArray():QJsonArray{};
    std::vector<AccountRecord> records;QSet<QString> ids;
    for(const auto& item:accounts){
        AccountRecord r;
        if(!item.isObject()||(version<5&&item.toObject().contains("region"))||
            (version<6&&item.toObject().contains("credentialRef"))||(version>=6&&!item.toObject().contains("credentialRef"))||
            !AccountModel::fromJson(item.toObject(),&r,error)||r.storageMode!=StorageMode::EncryptedVault||ids.contains(r.id))return invalid();
        ids.insert(r.id);if(version<6)r.credentialRef=r.id;records.push_back(r);
    }
    QMap<QString,std::shared_ptr<CredentialStorage>> stores;
    for(quint32 index=0;index<count;++index){
        std::shared_ptr<CredentialStorage> c;
        if(version>=6){
            if(!sets[int(index)].isObject())return invalid();const auto m=sets[int(index)].toObject();
            if(m.size()!=5)return invalid();for(const auto* name:{"id","provider","environment","region","storage_mode"})if(!m.value(name).isString())return invalid();
            if(!canonicalUuid(m.value("id").toString())||m.value("storage_mode")!="vault"||!AccountModel::provider(m.value("provider").toString()))return invalid();
            c=std::make_shared<CredentialStorage>();c->id=m.value("id").toString();c->provider=m.value("provider").toString();c->environment=m.value("environment").toString();c->region=m.value("region").toString();
        }else c=storageFor(records[index]);
        if(stores.contains(c->id))return invalid();
        quint32 fields=0;if(!cursor.u32(fields)||fields>MaxFields)return invalid();
        for(quint32 n=0;n<fields;++n){
            quint32 nameSize=0,valueSize=0;if(!cursor.u32(nameSize)||nameSize==0||nameSize>64)return invalid();
            const auto* nameData=cursor.take(nameSize);if(!nameData||!cursor.u32(valueSize)||valueSize==0||valueSize>MaxSecretBytes)return invalid();
            for(quint32 i=0;i<nameSize;++i)if(!((nameData[i]>='a'&&nameData[i]<='z')||nameData[i]=='_'))return invalid();
            const auto name=QString::fromLatin1(reinterpret_cast<const char*>(nameData),nameSize);const auto* value=cursor.take(valueSize);
            if(!value||c->values.find(name)!=c->values.end())return invalid();c->values.emplace(name,SecureBuffer(value,valueSize));
        }
        stores.insert(c->id,c);
    }
    QSet<QString> used;
    for(const auto& r:records){
        if(!stores.contains(r.credentialRef))return invalid();Entry entry{r,stores.value(r.credentialRef)};
        if(!credentialMatches(entry)||!validateCredentials(r,entry.credentials->values,error))return invalid();
        used.insert(r.credentialRef);entries->push_back(std::move(entry));
    }
    if(version>=7){
        quint32 size=0;if(!cursor.u32(size)||(size!=0&&size!=32))return invalid();
        const auto* key=cursor.take(size);if(!key)return invalid();if(size)*ledgerKey=SecureBuffer(key,size);
    }
    return (cursor.remaining==0&&used.size()==stores.size()&&validCredentialReferences(*entries))||invalid();
}



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
    if (info.isJunction()) return invalid();
    if (!privateWindowsPath(path, false, error)) return false;
#else
    return fail(error, QStringLiteral("이 운영체제의 금고 접근 권한 검증은 지원하지 않습니다."));
#endif
    return true;
}

bool ensureDirectory(const QString& path, bool create, QString* error) {
    const QString dir = QFileInfo(path).absolutePath();
    if (QFileInfo(dir).isSymLink())
        return fail(error, QStringLiteral("금고 폴더의 심볼릭 링크를 거부했습니다."));
    if (!QFileInfo::exists(dir)) {
        if (!create) return fail(error, QStringLiteral("금고 폴더를 열 수 없습니다."));
#ifdef Q_OS_WIN
        if (!createPrivateWindowsDirectory(dir, error)) return false;
#else
        if (!QDir().mkpath(dir)) return fail(error, QStringLiteral("금고 폴더를 만들 수 없습니다."));
        if (!QFile::setPermissions(dir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner))
            return fail(error, QStringLiteral("금고 폴더를 현재 사용자 전용으로 보호하지 못했습니다."));
#endif
    }
    return privatePath(dir, true, error);
}

bool readFile(const QString& path, QByteArray* result, QString* error) {
    if (!privatePath(path, false, error)) return false;
    QFile file(path);
#ifdef Q_OS_UNIX
    const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    struct stat st{};
    if (fd < 0)
        return fail(error, QStringLiteral("금고 파일을 열 수 없습니다."));
    if (::fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != ::geteuid() || (st.st_mode & 0077)) {
        ::close(fd);
        return fail(error, QStringLiteral("금고 파일 접근 권한이 안전하지 않습니다."));
    }
    if (!file.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
        ::close(fd);
        return fail(error, QStringLiteral("금고 파일을 읽을 수 없습니다."));
    }
#else
    if (!file.open(QIODevice::ReadOnly))
        return fail(error, QStringLiteral("금고 파일을 읽을 수 없습니다."));
#endif
    if (file.size() < Vault::HeaderBytes + 16 || file.size() > Vault::MaxFileBytes)
        return fail(error, QStringLiteral("금고 파일 크기가 올바르지 않습니다."));
    *result = file.read(Vault::MaxFileBytes + 1);
    if (file.error() != QFileDevice::NoError || result->size() != file.size() || result->size() > Vault::MaxFileBytes)
        return fail(error, QStringLiteral("금고 파일 전체를 안전하게 읽지 못했습니다."));
    return true;
}

struct Header {
    quint32 ops = Vault::DefaultOpsLimit;
    quint64 mem = Vault::DefaultMemLimit;
    QByteArray salt;
    QByteArray nonce;
    bool legacy = false;
    quint16 identityCode = 1; // Authenticated v2 field: 1=keefi-v1, 2=legacy-v1.
};
QString headerIdentity(const Header& header) {
    return header.identityCode == 2 ? QStringLiteral("legacy-v1") : QStringLiteral("keefi-v1");
}
bool validIdentity(const QString& identity) {
    return identity == QStringLiteral("keefi-v1") || identity == QStringLiteral("legacy-v1");
}
bool identityMatches(const QString& expected, const Header& header, QString* error) {
    return expected.isEmpty() || expected == headerIdentity(header)
        || fail(error, QStringLiteral("인증된 금고의 식별자 형식이 현재 프로필과 다릅니다. 비활성 복원 사본을 준비한 뒤 다시 시작해 주세요."));
}

bool parseHeader(const QByteArray& file, Header* header, QString* error) {
    const auto* p = reinterpret_cast<const unsigned char*>(file.constData());
    const auto format = storage_detail::formatOf(file, storage_detail::Kind::Vault);
    if (file.size() < Vault::HeaderBytes + 16 || file.size() > Vault::MaxFileBytes ||
        format == storage_detail::Format::Invalid ||
        qFromLittleEndian<quint16>(p + 10) != 1 || qFromLittleEndian<quint16>(p + 12) != 1)
        return fail(error, QStringLiteral("지원하지 않거나 손상된 금고 형식입니다."));
    const auto version = qFromLittleEndian<quint16>(p + 8);
    const auto code = qFromLittleEndian<quint16>(p + 14);
    header->legacy = format == storage_detail::Format::Legacy;
    if (header->legacy ? version != 1 || code != 0 : version != 2 || (code != 1 && code != 2))
        return fail(error, QStringLiteral("금고 버전 또는 식별자 형식을 확인할 수 없습니다."));
    header->identityCode = header->legacy ? 2 : code;
    header->ops = qFromLittleEndian<quint32>(p + 16);
    header->mem = qFromLittleEndian<quint64>(p + 20);
    if (header->ops < Vault::DefaultOpsLimit || header->ops > MaxOpsLimit ||
        header->mem < Vault::DefaultMemLimit || header->mem > MaxMemLimit ||
        qFromLittleEndian<quint32>(p + 68) != file.size() - Vault::HeaderBytes)
        return fail(error, QStringLiteral("금고 KDF 비용이나 길이가 허용 범위를 벗어났습니다."));
    header->salt = file.mid(28, 16);
    header->nonce = file.mid(44, 24);
    return true;
}

bool derive(const QString& password, const Header& header, SecureBuffer* key, QString* error) {
    if (password.isEmpty() || password.size() > Vault::MaxPasswordBytes || !validUnicode(password))
        return fail(error, QStringLiteral("금고 암호의 문자 형식이나 길이를 확인해 주세요."));
    SecureBuffer input = SecureBuffer::fromUtf8(password);
    if (input.size() > static_cast<std::size_t>(Vault::MaxPasswordBytes))
        return fail(error, QStringLiteral("금고 암호는 UTF-8 1024바이트 이하여야 합니다."));
    SecureBuffer output(32);
    const int rc = crypto_pwhash(output.data(), output.size(), reinterpret_cast<const char*>(input.data()),
        input.size(), reinterpret_cast<const unsigned char*>(header.salt.constData()), header.ops,
        static_cast<std::size_t>(header.mem), crypto_pwhash_ALG_ARGON2ID13);
    input.clear();
    if (rc != 0)
        return fail(error, QStringLiteral("키 유도에 필요한 메모리를 확보하지 못했습니다. 암호화 수준을 낮추지 않았습니다."));
    *key = std::move(output);
    return true;
}

bool decrypt(const QByteArray& file, const SecureBuffer& key, SecureBuffer* payload, QString* error) {
    Header header;
    if (!parseHeader(file, &header, error) || key.size() != 32) return false;
    SecureBuffer plain(static_cast<std::size_t>(file.size() - Vault::HeaderBytes - 16));
    unsigned long long length = 0;
    const auto* data = reinterpret_cast<const unsigned char*>(file.constData());
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(plain.data(), &length, nullptr,
        data + Vault::HeaderBytes, file.size() - Vault::HeaderBytes, data, Vault::HeaderBytes,
        reinterpret_cast<const unsigned char*>(header.nonce.constData()), key.data()) != 0 || length != plain.size())
        return fail(error, QStringLiteral("암호가 올바르지 않거나 금고 인증에 실패했습니다."));
    *payload = std::move(plain);
    return true;
}

QByteArray encrypt(const SecureBuffer& payload, const SecureBuffer& key, const Header& parameters) {
    QByteArray result(Vault::HeaderBytes + static_cast<qsizetype>(payload.size()) + 16, '\0');
    auto* p = reinterpret_cast<unsigned char*>(result.data());
    std::memcpy(p, Magic, 8);
    qToLittleEndian<quint16>(2, p + 8);
    qToLittleEndian<quint16>(1, p + 10);
    qToLittleEndian<quint16>(1, p + 12);
    if (parameters.identityCode != 1 && parameters.identityCode != 2)
        throw std::runtime_error("VAULT_IDENTITY_SCHEME_INVALID");
    qToLittleEndian<quint16>(parameters.identityCode, p + 14);
    qToLittleEndian<quint32>(parameters.ops, p + 16);
    qToLittleEndian<quint64>(parameters.mem, p + 20);
    std::memcpy(p + 28, parameters.salt.constData(), 16);
    randombytes_buf(p + 44, 24);
    qToLittleEndian<quint32>(static_cast<quint32>(payload.size() + 16), p + 68);
    unsigned long long length = 0;
    if (crypto_aead_xchacha20poly1305_ietf_encrypt(p + Vault::HeaderBytes, &length,
        payload.data(), payload.size(), p, Vault::HeaderBytes, nullptr, p + 44, key.data()) != 0 ||
        length != payload.size() + 16)
        throw std::runtime_error("VAULT_ENCRYPTION_FAILED");
    return result;
}

bool atomicWrite(const QString& path, const QByteArray& bytes, const SecureBuffer& key,
                 const SecureBuffer& expectedPlain, const QByteArray& previousDigest, bool creating, QString* error) {
    const auto unchanged = [&] {
        if (creating) {
            const QFileInfo info(path);
            return (!info.exists() && !info.isSymLink()) ||
                   fail(error, QStringLiteral("다른 금고 파일이 생겨 덮어쓰기를 중단했습니다."));
        }
        QByteArray current;
        return readFile(path, &current, error) &&
            (QCryptographicHash::hash(current, QCryptographicHash::Sha256) == previousDigest ||
             fail(error, QStringLiteral("금고 파일이 외부에서 변경되었습니다. 잠금 후 다시 열어 주세요.")));
    };
    if (!unchanged()) return false;
    StagedStorageFile staged(QFileInfo(path).absolutePath() + QStringLiteral("/.keefi-vault-XXXXXX"));
    auto& temp = staged.file();
    if (!temp.open())
        return fail(error, QStringLiteral("암호화 금고 임시 파일을 만들 수 없습니다."));
#ifdef Q_OS_WIN
    if (!privateWindowsPath(temp.fileName(), true, error)) return false;
#else
    if (!temp.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner))
        return fail(error, QStringLiteral("임시 금고 접근 권한을 제한하지 못했습니다."));
#endif
    if (!privatePath(temp.fileName(), false, error)) return false;
    if (temp.write(bytes) != bytes.size() || !temp.flush())
        return fail(error, QStringLiteral("금고 저장에 실패했습니다. 기존 파일을 보존했습니다."));
#ifdef Q_OS_UNIX
    if (::fsync(temp.handle()) != 0)
        return fail(error, QStringLiteral("금고 파일을 디스크에 반영하지 못했습니다. 기존 파일을 보존했습니다."));
#elif defined(Q_OS_WIN)
    if (!FlushFileBuffers(reinterpret_cast<HANDLE>(_get_osfhandle(temp.handle()))))
        return fail(error, QStringLiteral("금고 파일을 디스크에 반영하지 못했습니다. 기존 파일을 보존했습니다."));
#endif
    if (!temp.seek(0)) return fail(error, QStringLiteral("저장한 금고를 검증하지 못했습니다."));
    const QByteArray readback = temp.read(Vault::MaxFileBytes + 1);
    SecureBuffer check;
    if (readback != bytes || !decrypt(readback, key, &check, error) || check.size() != expectedPlain.size() ||
        sodium_memcmp(check.data(), expectedPlain.data(), check.size()) != 0)
        return fail(error, QStringLiteral("저장 후 읽기 검증에 실패했습니다. 기존 파일을 보존했습니다."));
    check.clear();
    if (!unchanged() || !privatePath(QFileInfo(path).absolutePath(), true, error)) return false;
    const QString tempName = temp.fileName();
    temp.close();
#ifdef Q_OS_UNIX
    const int directory = ::open(QFile::encodeName(QFileInfo(path).absolutePath()).constData(), O_RDONLY | O_CLOEXEC);
    if (directory < 0)
        return fail(error, QStringLiteral("금고 폴더를 동기화할 수 없어 교체를 중단했습니다."));
    if (::rename(QFile::encodeName(tempName).constData(), QFile::encodeName(path).constData()) != 0) {
        ::close(directory);
        return fail(error, QStringLiteral("금고 원자적 교체에 실패했습니다. 기존 파일을 보존했습니다."));
    }
    const int synced = ::fsync(directory);
    ::close(directory);
    if (synced != 0)
        return fail(error, QStringLiteral("금고 교체 후 디스크 반영을 확인하지 못했습니다. 다시 열어 확인해 주세요."));
#elif defined(Q_OS_WIN)
    if (!staged.publish(path, !creating))
        return fail(error, QStringLiteral("금고 원자적 교체에 실패했습니다. 기존 파일을 보존했습니다."));
#else
    return fail(error, QStringLiteral("이 운영체제의 원자적 교체는 지원하지 않습니다."));
#endif
    QByteArray finalReadback;
    if (!readFile(path, &finalReadback, error) || finalReadback != bytes)
        return fail(error, QStringLiteral("교체된 금고를 확인하지 못했습니다. 다시 열어 확인해 주세요."));
    return true;
}

// Backup restore is create-only. Do not reuse atomicWrite(creating=true): its
// check followed by a replacing rename permits a file created in between to be
// overwritten. This publishes an already authenticated ciphertext without any
// replacing operation, and never removes/chmods a destination on failure.
bool installBackup(const QString& path, const QByteArray& bytes, QString* error) {
    const QFileInfo destination(path);
    if (destination.exists() || destination.isSymLink())
        return fail(error, QStringLiteral("기존 금고를 덮어쓸 수 없습니다. 잠금을 해제해 주세요."));
    if (!privatePath(destination.absolutePath(), true, error)) return false;
    StagedStorageFile staged(destination.absolutePath() + "/.keefi-restore-XXXXXX");
    auto& temp = staged.file();
    if (!temp.open()) return fail(error, QStringLiteral("복원할 임시 금고를 만들지 못했습니다."));
#ifdef Q_OS_WIN
    if (!privateWindowsPath(temp.fileName(), true, error)) return false;
#else
    if (!temp.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner))
        return fail(error, QStringLiteral("임시 금고 접근 권한을 제한하지 못했습니다."));
#endif
    if (!privatePath(temp.fileName(), false, error)) return false;
    if (temp.write(bytes) != bytes.size() || !temp.flush())
        return fail(error, QStringLiteral("백업 복원에 실패했습니다. 원본 파일을 보존했습니다."));
#ifdef Q_OS_UNIX
    if (::fsync(temp.handle()) != 0)
        return fail(error, QStringLiteral("복원할 금고를 디스크에 반영하지 못했습니다."));
#elif defined(Q_OS_WIN)
    if (!FlushFileBuffers(reinterpret_cast<HANDLE>(_get_osfhandle(temp.handle()))))
        return fail(error, QStringLiteral("복원할 금고를 디스크에 반영하지 못했습니다."));
#endif
    if (!temp.seek(0) || temp.read(Vault::MaxFileBytes + 1) != bytes)
        return fail(error, QStringLiteral("복원할 금고의 읽기 검증에 실패했습니다."));
    if (!privatePath(destination.absolutePath(), true, error)) return false;
    const QString tempName = temp.fileName();
    temp.close();
#ifdef Q_OS_UNIX
    const int directory = ::open(QFile::encodeName(destination.absolutePath()).constData(), O_RDONLY | O_CLOEXEC);
    if (directory < 0)
        return fail(error, QStringLiteral("금고 폴더를 동기화할 수 없어 복원을 중단했습니다."));
    // link() fails with EEXIST even for a dangling symlink. Both paths are on
    // the same filesystem because the temporary file is in this directory.
    if (::link(QFile::encodeName(tempName).constData(), QFile::encodeName(path).constData()) != 0) {
        ::close(directory);
        return fail(error, QStringLiteral("새 금고를 만들지 못했습니다. 기존 대상 파일을 보존했습니다."));
    }
    // Only our temporary name is removed. The complete installed file stays
    // intact even if cleanup/durability verification fails after publication.
    const bool removed = temp.remove();
    const int synced = ::fsync(directory);
    ::close(directory);
    if (!removed || synced != 0)
        return fail(error, QStringLiteral("복원된 금고의 디스크 반영을 확인하지 못했습니다. 다시 열어 확인해 주세요."));
#elif defined(Q_OS_WIN)
    if (!staged.publish(path, false))
        return fail(error, QStringLiteral("새 금고를 만들지 못했습니다. 기존 대상 파일을 보존했습니다."));
#else
    return fail(error, QStringLiteral("이 운영체제의 안전한 백업 복원은 지원하지 않습니다."));
#endif
    QByteArray finalReadback;
    if (!readFile(path, &finalReadback, error) || finalReadback != bytes)
        return fail(error, QStringLiteral("복원된 금고를 확인하지 못했습니다. 다시 열어 확인해 주세요."));
    return true;
}

// Remember the physical file, not merely QLockFile's PID: another Vault in this
// process has the same PID. This is only used to retry removal of our own lock
// after Qt has closed its native lock but failed to unlink the directory entry.
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

} // namespace

struct Vault::Private {
    QString path;
    QString expectedIdentityScheme;
    SecureBuffer key, assetsLedgerKey;
    Header parameters;
    QByteArray digest;
    std::unique_ptr<QLockFile> fileLock;
    QByteArray residualLockIdentity;
    std::vector<Entry> entries;
    QJsonArray pending;
    bool locking = false;

    void release() {
        if (fileLock && fileLock->isLocked()) {
            const QByteArray identity = lockFileIdentity(path + ".lock");
            fileLock->unlock();
            residualLockIdentity = !identity.isEmpty() && lockFileIdentity(path + ".lock") == identity
                                       ? identity : QByteArray{};
        }
        fileLock.reset();
    }

    bool acquire(bool create, QString* error) {
        if (!ensureDirectory(path, create, error)) return false;
        if (QFileInfo(path + ".lock").isSymLink())
            return fail(error, QStringLiteral("금고 잠금 파일의 심볼릭 링크를 거부했습니다."));
        if (!residualLockIdentity.isEmpty()) {
            if (lockFileIdentity(path + ".lock") == residualLockIdentity) {
                QLockFile residual(path + ".lock");
                // Qt also acquires the native OS lock before removing a stale
                // file, so a new live owner is never removed on PID alone.
                if (!residual.removeStaleLockFile())
                    return fail(error, QStringLiteral("이전 금고 잠금을 정리하지 못했습니다. 폴더 접근 권한을 확인해 주세요."));
            }
            residualLockIdentity.clear();
        }
        fileLock = std::make_unique<QLockFile>(path + ".lock");
        fileLock->setStaleLockTime(0);
        if (!fileLock->tryLock(0)) {
            fileLock.reset();
            return fail(error, QStringLiteral("다른 앱 인스턴스가 이 금고를 사용 중입니다."));
        }
        return true;
    }

    bool persist(const std::vector<Entry>& candidate, const SecureBuffer& encryptionKey,
                 const Header& header, bool creating, QString* error, const QJsonArray* replacementPending = nullptr, const SecureBuffer* replacementLedgerKey = nullptr) {
        if (!creating && !storage_detail::rejectLegacyWrite(parameters.legacy, error)) return false;
        if (!fileLock || !fileLock->isLocked())
            return fail(error, QStringLiteral("금고 파일 잠금이 없어 저장을 중단했습니다."));
        SecureBuffer payload;
        if (!encodePayload(candidate, replacementPending ? *replacementPending : pending, replacementLedgerKey ? *replacementLedgerKey : assetsLedgerKey, &payload, error)) return false;
        const QByteArray encoded = encrypt(payload, encryptionKey, header);
        if (!atomicWrite(path, encoded, encryptionKey, payload, digest, creating, error)) return false;
        digest = QCryptographicHash::hash(encoded, QCryptographicHash::Sha256);
        return true;
    }
};

Vault::Vault(QString filePath, QObject* parent) : QObject(parent), d_(std::make_unique<Private>()) {
    d_->path = QFileInfo(filePath).absoluteFilePath();
}
Vault::Vault(QString filePath, const QString& expectedIdentityScheme, QObject* parent)
    : Vault(std::move(filePath), parent) {
    d_->expectedIdentityScheme = expectedIdentityScheme;
}

Vault::~Vault() { lock(); }
QStringList Vault::erasedFileNames(const QString& vaultPath) {
    const QFileInfo vaultInfo(vaultPath);
    const QDir folder = vaultInfo.dir();
    QStringList names;
    if (vaultInfo.exists() || vaultInfo.isSymLink()) names.append(vaultInfo.fileName());
    // Both explicit compatibility and current store names remain covered by erase.
    names.append(storage_detail::relatedFileNames(folder.path()));
    return names;
}

bool Vault::erase(QString* error) {
    clearError(error);
    lock();
    const QDir folder = QFileInfo(d_->path).dir();
    QStringList failed;
    auto names = erasedFileNames(d_->path);
    // The vault goes first: if it cannot be removed, nothing else is touched.
    if (names.removeOne(QFileInfo(d_->path).fileName()) && !QFile::remove(d_->path))
        return fail(error, QStringLiteral("금고 파일을 지우지 못했습니다. 다른 %1 창이나 권한을 확인해 주세요.").arg(productName()));
    for (const auto& name : names)
        if (!QFile::remove(folder.filePath(name))) failed.append(name);
    emit stateChanged();
    emit accountsChanged();
    if (!failed.isEmpty())
        return fail(error, QStringLiteral("금고는 지웠지만 일부 암호화 기록을 지우지 못했습니다: %1").arg(failed.join(", ")));
    return true;
}

bool Vault::exists() const { const QFileInfo info(d_->path); return info.exists() || info.isSymLink(); }
bool Vault::isUnlocked() const { return !d_->locking && !d_->key.empty(); }
QString Vault::filePath() const { return d_->path; }

QList<AccountRecord> Vault::accounts() const {
    QList<AccountRecord> result;
    for (const auto& entry : d_->entries)
        result.append(entry.record);
    return result;
}

bool Vault::validateNewPassword(const QString& password, QString* error) {
    clearError(error);
    int characters = 0, bytes = 0;
    if (password.size() > MaxPasswordBytes)
        return fail(error, QStringLiteral("금고 암호는 UTF-8 1024바이트 이하여야 합니다."));
    if (!validUnicode(password, &characters, &bytes))
        return fail(error, QStringLiteral("금고 암호에 올바르지 않은 Unicode 문자가 있습니다."));
    if (bytes > MaxPasswordBytes)
        return fail(error, QStringLiteral("금고 암호는 UTF-8 1024바이트 이하여야 합니다."));
    if (!isNewVaultPasswordText(password))
        return fail(error, newVaultPasswordAsciiMessage());
    bool uppercase = false, lowercase = false, digit = false, punctuation = false;
    for (const QChar character : password) {
        const auto value = character.unicode();
        uppercase = uppercase || (value >= 'A' && value <= 'Z');
        lowercase = lowercase || (value >= 'a' && value <= 'z');
        digit = digit || (value >= '0' && value <= '9');
        // Printable ASCII punctuation only. Space, controls and Unicode
        // symbols remain exact input but do not satisfy this requirement.
        punctuation = punctuation || (value >= 0x21 && value <= 0x2f) ||
            (value >= 0x3a && value <= 0x40) || (value >= 0x5b && value <= 0x60) ||
            (value >= 0x7b && value <= 0x7e);
    }
    QStringList missing;
    if (characters < MinPasswordCharacters) missing.append(QStringLiteral("12자 이상"));
    if (!uppercase) missing.append(QStringLiteral("영문 대문자(A-Z)"));
    if (!lowercase) missing.append(QStringLiteral("영문 소문자(a-z)"));
    if (!digit) missing.append(QStringLiteral("숫자(0-9)"));
    if (!punctuation) missing.append(QStringLiteral("특수문자(!, @, # 등 · 공백 제외)"));
    if (!missing.isEmpty())
        return fail(error, QStringLiteral("새 암호 조건 미충족: ") + missing.join(QStringLiteral(" · ")));
    return true;
}

bool Vault::prepareProfileDirectory(const QString& directory, QString* error) {
    clearError(error);
    if (directory.isEmpty() || !QDir::isAbsolutePath(directory))
        return fail(error, QStringLiteral("프로필 폴더는 절대 경로여야 합니다."));
    return ensureDirectory(QDir(directory).filePath(QStringLiteral("vault.bin")), true, error);
}

bool Vault::create(const QString& password, QString* error) {
    clearError(error);
    if (exists()) return fail(error, QStringLiteral("기존 금고를 덮어쓸 수 없습니다. 잠금을 해제해 주세요."));
    if (!validateNewPassword(password, error)) return false;
    const auto identity = d_->expectedIdentityScheme.isEmpty() ? identitySchemeName() : d_->expectedIdentityScheme;
    if (!validIdentity(identity)) return fail(error, QStringLiteral("새 금고의 프로필 식별자 형식이 올바르지 않습니다."));
    try {
        if (sodium_init() < 0)
            return fail(error, QStringLiteral("암호화 라이브러리를 초기화하지 못했습니다."));
        if (!d_->acquire(true, error)) return false;
        Header header;
        header.identityCode = identity == QStringLiteral("legacy-v1") ? 2 : 1;
        header.salt.resize(16);
        randombytes_buf(header.salt.data(), 16);
        SecureBuffer key;
        if (!derive(password, header, &key, error) || !d_->persist(d_->entries, key, header, true, error)) {
            lock();
            return false;
        }
        d_->parameters = header;
        d_->key = std::move(key);
        emit stateChanged();
        emit accountsChanged();
        return true;
    } catch (const std::exception&) {
        lock();
        return fail(error, QStringLiteral("보호 메모리 또는 암호화 처리에 실패하여 금고를 잠갔습니다."));
    }
}

bool Vault::restoreBackup(const QString& sourcePath, const QString& password, QString* error) {
    clearError(error);
    if (isUnlocked()) return fail(error, QStringLiteral("이미 열린 금고입니다."));
    if (exists()) return fail(error, QStringLiteral("기존 금고를 덮어쓸 수 없습니다. 잠금을 해제해 주세요."));
    try {
        if (sodium_init() < 0)
            return fail(error, QStringLiteral("암호화 라이브러리를 초기화하지 못했습니다."));
        QByteArray file;
        Header header;
        SecureBuffer key, payload, ledgerKey;
        std::vector<Entry> entries;
        QJsonArray pending;
        // Authenticate the original bytes before creating a destination or its
        // lock. Existing-password rules intentionally match unlock(), including
        // older weak/Unicode passwords; validateNewPassword() does not apply.
        if (!readFile(sourcePath, &file, error) || !parseHeader(file, &header, error) ||
            !derive(password, header, &key, error) || !decrypt(file, key, &payload, error) ||
            !decodePayload(payload, &entries, &pending, &ledgerKey, error)) return false;
        if (!identityMatches(d_->expectedIdentityScheme, header, error)) return false;
        const auto restored = header.legacy ? encrypt(payload, key, header) : file;
        SecureBuffer verified;
        if (!decrypt(restored, key, &verified, error) || verified.size() != payload.size()
            || sodium_memcmp(verified.data(), payload.data(), payload.size())) return false;
        header.legacy = false;
        if (!d_->acquire(true, error)) return false;
        if (!installBackup(d_->path, restored, error)) {
            lock();
            return false;
        }
        d_->entries = std::move(entries);
        d_->pending = pending;
        d_->assetsLedgerKey = std::move(ledgerKey);
        d_->parameters = header;
        d_->key = std::move(key);
        d_->digest = QCryptographicHash::hash(restored, QCryptographicHash::Sha256);
        emit stateChanged();
        emit accountsChanged();
        return true;
    } catch (const std::exception&) {
        lock();
        return fail(error, QStringLiteral("보호 메모리 또는 백업 복원에 실패하여 잠금을 유지합니다."));
    }
}

bool Vault::authenticatedBackupIdentity(const QString& sourcePath, const QString& password,
                                       QString* identityScheme, QString* error) {
    clearError(error);
    if (identityScheme) identityScheme->clear();
    if (!identityScheme) return fail(error, QStringLiteral("인증된 백업 식별자 결과가 없습니다."));
    try {
        QByteArray bytes;storage_detail::VaultMaterial material;
        if (!readFile(sourcePath, &bytes, error) || !storage_detail::authenticateVault(bytes, password, &material, error)) return false;
        *identityScheme = material.identityScheme;
        return true;
    } catch (const std::exception&) {
        return fail(error, QStringLiteral("백업의 인증된 식별자 형식을 확인하지 못했습니다."));
    }
}

bool Vault::unlock(const QString& password, QString* error) {
    clearError(error);
    if (isUnlocked()) return fail(error, QStringLiteral("이미 열린 금고입니다."));
    try {
        if (!d_->acquire(false, error)) return false;
        QByteArray file;
        Header header;
        SecureBuffer key, payload, ledgerKey;
        std::vector<Entry> entries;
        QJsonArray pending;
        if (!readFile(d_->path, &file, error) || !parseHeader(file, &header, error) ||
            !derive(password, header, &key, error) || !decrypt(file, key, &payload, error) ||
            !decodePayload(payload, &entries, &pending, &ledgerKey, error)) {
            lock();
            return false;
        }
        if (!identityMatches(d_->expectedIdentityScheme, header, error)) {
            d_->release();
            return false;
        }
        d_->entries = std::move(entries);
        d_->pending = pending;
        d_->assetsLedgerKey = std::move(ledgerKey);
        d_->parameters = header;
        d_->key = std::move(key);
        d_->digest = QCryptographicHash::hash(file, QCryptographicHash::Sha256);
        emit stateChanged();
        emit accountsChanged();
        return true;
    } catch (const std::exception&) {
        lock();
        return fail(error, QStringLiteral("보호 메모리 또는 금고 읽기에 실패하여 잠금을 유지합니다."));
    }
}

void Vault::lock() {
    if (d_->locking) return;
    d_->locking = true;
    emit aboutToLock();
    d_->entries.clear();
    d_->pending = {};
    d_->key.clear();
    d_->assetsLedgerKey.clear();
    d_->digest.clear();
    d_->parameters = Header{};
    d_->release();
    d_->locking = false;
    emit accountsChanged();
    emit stateChanged();
    emit locked();
}

bool Vault::changePassword(const QString& currentPassword, const QString& newPassword, QString* error) {
    clearError(error);
    if (!isUnlocked()) return fail(error, QStringLiteral("먼저 금고 잠금을 해제해 주세요."));
    if (!validateNewPassword(newPassword, error)) return false;
    try {
        SecureBuffer oldKey;
        if (!derive(currentPassword, d_->parameters, &oldKey, error) ||
            sodium_memcmp(oldKey.data(), d_->key.data(), 32) != 0) {
            lock();
            return fail(error, QStringLiteral("현재 암호 확인에 실패하여 금고를 잠갔습니다."));
        }
        oldKey.clear();
        Header header = d_->parameters;
        header.salt.resize(16);
        randombytes_buf(header.salt.data(), 16);
        SecureBuffer newKey;
        if (!derive(newPassword, header, &newKey, error) || !d_->persist(d_->entries, newKey, header, false, error)) {
            lock();
            return false;
        }
        d_->key = std::move(newKey);
        d_->parameters = header;
        emit stateChanged();
        return true;
    } catch (const std::exception&) {
        lock();
        return fail(error, QStringLiteral("암호 변경에 실패하여 금고를 잠갔습니다."));
    }
}

bool Vault::addAccount(AccountRecord record, const QMap<QString, QString>& credentials,
                       StorageMode mode, QString* error) {
    clearError(error);
    if (mode != StorageMode::EncryptedVault)
        return fail(error, QStringLiteral("연결은 암호화 금고에만 저장합니다."));
    if (!isUnlocked())
        return fail(error, QStringLiteral("저장하려면 먼저 금고를 만들거나 잠금을 해제해 주세요."));
    if (d_->entries.size() >= MaxAccounts)
        return fail(error, QStringLiteral("연결 수 상한은 64개입니다."));
    if (AccountModel::isProviderSuspended(record.provider))
        return fail(error, QStringLiteral("이 증권사의 연결은 비활성화되어 있습니다."));
    if(!record.credentialRef.isEmpty())return fail(error,QStringLiteral("새 연결의 인증정보 참조를 직접 지정할 수 없습니다."));
    record.storageMode = mode;
    record.credentialRef=QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (record.id.isEmpty()) record.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!AccountModel::validate(record, error) || !validNewMarketEnvironment(record)) return fail(error,QStringLiteral("시장에 맞는 연결 환경을 확인해 주세요."));
    if (record.provider == "okx" && !okxRegionValid(record.region))
        return fail(error, QStringLiteral("새 OKX 연결의 지역을 직접 선택해 주세요."));
    for (const auto& entry : d_->entries)
        if (entry.record.id == record.id)
            return fail(error, QStringLiteral("이미 등록된 연결 식별자입니다."));
    try {
        Entry entry;
        entry.record = std::move(record);
        auto storage=storageFor(entry.record);
        if (!inputCredentials(entry.record, credentials, &storage->values, error)) return false;
        entry.credentials=std::move(storage);
        auto next = cloneEntries(d_->entries);
        next.push_back(std::move(entry));
        if (!d_->persist(next, d_->key, d_->parameters, false, error)) {
            lock();
            return false;
        }
        d_->entries = std::move(next);
        emit accountsChanged();
        emit stateChanged();
        return true;
    } catch (const std::exception&) {
        lock();
        return fail(error, QStringLiteral("인증정보 보호 메모리를 확보하지 못해 금고를 잠갔습니다."));
    }
}

bool Vault::hasAssetsLedgerKey() const { return isUnlocked() && d_->assetsLedgerKey.size() == 32; }
bool Vault::withAssetsLedgerKey(bool createIfMissing, const std::function<void(const SecureBuffer&)>& visitor, QString* error) {
    clearError(error);
    if(!isUnlocked()||d_->locking||!visitor)return fail(error,QStringLiteral("자산 원장을 열려면 금고 잠금을 해제해 주세요."));
    try {
        if(d_->assetsLedgerKey.empty()){
            if(!createIfMissing)return fail(error,QStringLiteral("이 금고에 자산 원장 키가 아직 없습니다."));
            SecureBuffer candidate(32);randombytes_buf(candidate.data(),candidate.size());
            if(!d_->persist(d_->entries,d_->key,d_->parameters,false,error,nullptr,&candidate)){lock();return false;}
            d_->assetsLedgerKey=std::move(candidate);
        }
        visitor(d_->assetsLedgerKey);return true;
    }catch(const std::exception&){lock();return fail(error,QStringLiteral("자산 원장 키 보호에 실패하여 금고를 잠갔습니다."));}
}

QList<AccountRecord> Vault::credentialUsers(const QString& accountId) const {
    QString ref;for(const auto& entry:d_->entries)if(entry.record.id==accountId){ref=entry.record.credentialRef;break;}
    QList<AccountRecord> result;if(ref.isEmpty())return result;
    for(const auto& entry:d_->entries)if(entry.record.credentialRef==ref)result.append(entry.record);
    return result;
}
bool Vault::addAccountUsingCredentials(AccountRecord record,const QString& sourceAccountId,QString* error) {
    clearError(error);
    if(d_->entries.size()>=MaxAccounts)return fail(error,QStringLiteral("연결 수 상한은 64개입니다."));
    const auto source=std::find_if(d_->entries.cbegin(),d_->entries.cend(),[&](const Entry& e){return e.record.id==sourceAccountId;});
    if(source==d_->entries.cend()||!credentialMatches(*source)||!record.credentialRef.isEmpty())return fail(error,QStringLiteral("공유할 인증정보 연결을 확인해 주세요."));
    const auto& r=source->record;
    if(r.provider!="binance"||record.provider!="binance"||r.environment!="live"||record.environment!="live"||
        !r.region.isEmpty()||!record.region.isEmpty()||record.storageMode!=r.storageMode||
        !QStringList{"spot","usdm_futures"}.contains(r.market)||!QStringList{"spot","usdm_futures"}.contains(record.market)||record.market==r.market||
        (r.storageMode==StorageMode::EncryptedVault&&!isUnlocked()))return fail(error,QStringLiteral("동일 보관 방식의 Binance 실전 현물·선물만 인증정보를 공유할 수 있습니다."));
    if(record.id.isEmpty())record.id=QUuid::createUuid().toString(QUuid::WithoutBraces);
    record.credentialRef=r.credentialRef;
    if(!AccountModel::validate(record,error))return false;
    for(const auto& e:d_->entries)if(e.record.id==record.id||(e.record.credentialRef==record.credentialRef&&e.record.market==record.market))
        return fail(error,QStringLiteral("이미 해당 시장에 연결된 인증정보입니다."));
    try{
        auto next=cloneEntries(d_->entries);next.push_back(Entry{record,source->credentials});
        if(record.storageMode==StorageMode::EncryptedVault&&!d_->persist(next,d_->key,d_->parameters,false,error)){lock();return false;}
        d_->entries=std::move(next);emit accountsChanged();emit stateChanged();return true;
    }catch(const std::exception&){lock();return fail(error,QStringLiteral("공유 인증정보 연결에 실패하여 금고를 잠갔습니다."));}
}

bool Vault::removeAccount(const QString& id, QString* error) {
    clearError(error);
    try {
        const auto found = std::find_if(d_->entries.cbegin(), d_->entries.cend(),
                                       [&](const Entry& e) { return e.record.id == id; });
        if (found == d_->entries.cend()) return fail(error, QStringLiteral("연결을 찾을 수 없습니다."));
        const bool persistent = found->record.storageMode == StorageMode::EncryptedVault;
        auto next = cloneEntries(d_->entries);
        next.erase(next.begin() + std::distance(d_->entries.cbegin(), found));
        if (persistent && (!isUnlocked() || !d_->persist(next, d_->key, d_->parameters, false, error))) {
            lock();
            return false;
        }
        d_->entries = std::move(next);
        emit accountsChanged();
        emit stateChanged();
        return true;
    } catch (const std::exception&) {
        lock();
        return fail(error, QStringLiteral("연결 삭제 처리에 실패하여 금고를 잠갔습니다."));
    }
}

bool Vault::replaceCredentials(const QString& id, const QMap<QString, QString>& credentials, QString* error) {
    const auto found = std::find_if(d_->entries.cbegin(), d_->entries.cend(), [&](const Entry& e) { return e.record.id == id; });
    if (found == d_->entries.cend()) return fail(error, QStringLiteral("연결을 찾을 수 없습니다."));
    return replaceCredentialsImpl(id, credentials, found->record.region, true, error);
}
bool Vault::replaceCredentials(const QString& id, const QMap<QString, QString>& credentials, const QString& region, QString* error) {
    return replaceCredentialsImpl(id, credentials, region, false, error);
}
bool Vault::replaceCredentialsImpl(const QString& id, const QMap<QString, QString>& credentials, const QString& region, bool preserveLegacy, QString* error) {
    clearError(error);
    try {
        const auto found = std::find_if(d_->entries.cbegin(), d_->entries.cend(),
                                       [&](const Entry& e) { return e.record.id == id; });
        if (found == d_->entries.cend()) return fail(error, QStringLiteral("연결을 찾을 수 없습니다."));
        if (AccountModel::isProviderSuspended(found->record.provider))
            return fail(error, QStringLiteral("비활성 증권사의 인증정보는 변경할 수 없습니다."));
        AccountRecord updated = found->record;
        // Empty is preserved only for an existing legacy record, never used as a region reset.
        if ((updated.provider == "okx" && !okxRegionValid(region) && !(preserveLegacy && region.isEmpty() && updated.region.isEmpty())) ||
            (updated.provider != "okx" && !region.isEmpty())) return fail(error, QStringLiteral("OKX 연결 지역을 확인해 주세요."));
        updated.region = region;
        if (!AccountModel::validate(updated, error)) return false;
        auto replacement=storageFor(updated);
        if (!inputCredentials(updated, credentials, &replacement->values, error)) return false;
        const bool persistent = found->record.storageMode == StorageMode::EncryptedVault;
        auto next = cloneEntries(d_->entries);
        const auto index = static_cast<std::size_t>(std::distance(d_->entries.cbegin(), found));
        next[index].record = updated;
        for(auto& entry:next)if(entry.record.credentialRef==updated.credentialRef)entry.credentials=replacement;
        if (persistent && (!isUnlocked() || !d_->persist(next, d_->key, d_->parameters, false, error))) {
            lock();
            return false;
        }
        d_->entries = std::move(next);
        emit accountsChanged();
        return true;
    } catch (const std::exception&) {
        lock();
        return fail(error, QStringLiteral("인증정보 교체에 실패하여 금고를 잠갔습니다."));
    }
}

QJsonArray Vault::pendingOperations() const { return isUnlocked() ? d_->pending : QJsonArray{}; }

bool Vault::setPendingOperations(const QJsonArray& operations, QString* error) {
    clearError(error);
    if (!isUnlocked()) return fail(error, QStringLiteral("주문 대사 기록을 저장하려면 금고 잠금을 해제해 주세요."));
    if (!validPending(operations)) return fail(error, QStringLiteral("주문 대사 기록 형식이 올바르지 않습니다."));
    try {
        if (!d_->persist(d_->entries, d_->key, d_->parameters, false, error, &operations)) {
            lock();
            return false;
        }
        d_->pending = operations;
        return true;
    } catch (const std::exception&) {
        lock();
        return fail(error, QStringLiteral("주문 기록 저장에 실패하여 금고를 잠갔습니다."));
    }
}

bool Vault::withCredentials(const QString& id, const std::function<void(const CredentialSet&)>& visitor,
                            QString* error) const {
    clearError(error);
    if (d_->locking) return fail(error, QStringLiteral("금고 잠금 중에는 인증정보를 사용할 수 없습니다."));
    if (!visitor) return fail(error, QStringLiteral("인증정보 사용 경로가 없습니다."));
    for (const auto& entry : d_->entries) {
        if (entry.record.id == id) {
            if (AccountModel::isProviderSuspended(entry.record.provider))
                return fail(error, QStringLiteral("비활성 증권사의 인증정보는 사용할 수 없습니다."));
            if (entry.record.storageMode == StorageMode::EncryptedVault && !isUnlocked())
                return fail(error, QStringLiteral("금고가 잠겨 있습니다."));
            if(!credentialMatches(entry))return fail(error,QStringLiteral("인증정보 참조가 일치하지 않습니다."));
            visitor(entry.credentials->values);
            return true;
        }
    }
    return fail(error, QStringLiteral("잠겼거나 등록되지 않은 연결입니다."));
}

namespace storage_detail {
bool privateStoragePath(const QString& path, bool directory, QString* error) {
    return privatePath(path, directory, error);
}
bool authenticateVault(const QByteArray& bytes, const QString& password,
                       VaultMaterial* result, QString* error) {
    if (!result) return fail(error, QStringLiteral("금고 전환 결과가 없습니다."));
    *result = {};
    Header header;
    SecureBuffer key, payload, ledgerRoot;
    std::vector<Entry> entries;
    QJsonArray pending;
    if (!parseHeader(bytes, &header, error) || !derive(password, header, &key, error)
        || !decrypt(bytes, key, &payload, error)
        || !decodePayload(payload, &entries, &pending, &ledgerRoot, error)) return false;
    result->ops = header.ops;
    result->mem = header.mem;
    result->salt = header.salt;
    result->format = header.legacy ? Format::Legacy : Format::Current;
    result->identityScheme = headerIdentity(header);
    result->key = std::move(key);
    result->payload = std::move(payload);
    result->ledgerRoot = std::move(ledgerRoot);
    return true;
}
QByteArray encodeCurrentVault(const VaultMaterial& material, QString* error) {
    Header header;
    header.ops = material.ops;
    header.mem = material.mem;
    header.salt = material.salt;
    if (material.key.size() != 32 || material.payload.empty() || header.salt.size() != 16 || !validIdentity(material.identityScheme)) {
        fail(error, QStringLiteral("인증된 금고 전환 재료가 없습니다."));
        return {};
    }
    header.identityCode = material.identityScheme == QStringLiteral("legacy-v1") ? 2 : 1;
    const auto bytes = encrypt(material.payload, material.key, header);
    SecureBuffer checked;
    if (!decrypt(bytes, material.key, &checked, error)
        || checked.size() != material.payload.size()
        || sodium_memcmp(checked.data(), material.payload.data(), checked.size())) return {};
    return bytes;
}
} // namespace storage_detail
} // namespace keefi
