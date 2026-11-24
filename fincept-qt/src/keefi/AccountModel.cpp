#include "keefi/AccountModel.h"
#include <QHash>
#include "keefi/Vault.h"
#include <QSet>
#include <QUuid>

namespace keefi {
namespace {
bool fail(QString* error, const QString& message) {
    if (error)
        *error = message;
    return false;
}
bool plainText(const QString& value, int max) {
    if (value.isEmpty() || value.size() > max || value.trimmed().isEmpty())
        return false;
    for (const QChar c : value)
        if (c.isNull() || c.category() == QChar::Other_Control)
            return false;
    return true;
}
} // namespace

const QList<ProviderSpec>& AccountModel::providers() {
    const auto guide = [](const char* id) {
        return QStringLiteral("https://victorious-ground-064b41900.6.azurestaticapps.net/guides/%1/").arg(QLatin1String(id));
    };
    static const QList<ProviderSpec> catalog{
        {"toss", "토스증권", {{"client_id", "Client ID"}, {"client_secret", "Client Secret"}},
         guide("toss"), {"domestic_equity"}, {"KRW"}, {"live"}, false,
         "https://www.tossinvest.com/open-api/landing"},
        {"kis", "한국투자증권", {{"app_key", "App Key"}, {"app_secret", "App Secret"},
         {"account_number", "계좌번호"}, {"product_code", "상품코드"}, {"hts_id", "HTS ID", false}},
         "https://apiportal.koreainvestment.com", {"domestic_equity"}, {"KRW"}, {"live"}},
        {"kiwoom", "키움 REST", {{"app_key", "App Key"}, {"app_secret", "App Secret"},
         {"account_number", "계좌번호", false}},
         guide("kiwoom"), {"domestic_equity"}, {"KRW"}, {"live"}, false,
         "https://openapi.kiwoom.com/mgmt/VOpenApiRegView"},
        {"ls", "LS증권", {{"app_key", "APP_KEY"}, {"app_secret", "APP_SECRET"},
         {"account_number", "계좌번호", false}},
         "https://openapi.ls-sec.co.kr", {"domestic_equity"}, {"KRW"}, {"live"}},
        {"binance", "Binance", {{"api_key", "API Key"}, {"secret", "Secret"}},
         guide("binance"), {"spot", "usdm_futures"}, {"USDT", "USDC", "BTC", "ETH"},
         {"live"}, false, "https://www.binance.com/en/my/settings/api-management"},
        {"okx", "OKX", {{"api_key", "API Key"}, {"secret", "Secret"}, {"passphrase", "Passphrase"}},
         guide("okx"), {"spot", "okx_futures"}, {"USDT", "USDC", "BTC", "ETH"}, {"live"}, false,
         "https://www.okx.com/account/my-api"},
        {"upbit", "Upbit", {{"access_key", "Access Key"}, {"secret", "Secret"}},
         guide("upbit"), {"spot"}, {"KRW", "BTC", "USDT"}, {"live"}, false,
         "https://upbit.com/mypage/open_api_management"},
        {"bithumb", "빗썸", {{"api_key", "API Key"}, {"secret", "Secret"}},
         guide("bithumb"), {"spot"}, {"KRW", "BTC"}, {"live"}, false,
         "https://www.bithumb.com/react/api-support/management-api"},
        {"jupiter", "Jupiter", {{"api_key", "API Key"}},
         "https://developers.jup.ag/docs/portal/setup", {"solana"}, {"SOL", "USDC"}, {"mainnet"}},
        {"phantom", "Phantom", {{"address", "조회용 Solana 주소", false, false}}, guide("phantom"), {"solana", "hyperliquid_perps"}, {"SOL", "USDC"},
         {"mainnet"}, true}
    };
    return catalog;
}

const ProviderSpec* AccountModel::provider(const QString& id) {
    for (const auto& entry : providers())
        if (entry.id == id)
            return &entry;
    return nullptr;
}

bool AccountModel::isProviderSuspended(const QString& id) {
    return id == "kis" || id == "ls";
}

bool AccountModel::allowsNewConnection(const QString& id) {
    const auto* spec = provider(id);
    return spec && !isProviderSuspended(id) && (!spec->markets.contains("domestic_equity") || id == "toss" || id == "kiwoom");
}

bool AccountModel::retiredEnvironment(const AccountRecord& record) {
    // Paper, demo and testnet support was removed on 2026-10-21.
    static const QHash<QString, QStringList> retired{{"kis", {"paper"}}, {"kiwoom", {"paper"}}, {"ls", {"paper"}},
                                                     {"binance", {"testnet", "demo"}}, {"okx", {"demo"}}};
    return retired.value(record.provider).contains(record.environment);
}

bool AccountModel::validate(const AccountRecord& record, QString* error, bool acceptRetired) {
    const auto* spec = provider(record.provider);
    const QUuid id(record.id);
    if (!spec || id.isNull() || id.toString(QUuid::WithoutBraces) != record.id ||
        !plainText(record.alias, 80) || !spec->markets.contains(record.market) ||
        !spec->currencies.contains(record.currency) ||
        !(spec->environments.contains(record.environment) || (acceptRetired && retiredEnvironment(record))))
        return fail(error, QStringLiteral("계좌 설정 형식이나 지원 범위를 확인해 주세요."));
    if (!record.credentialRef.isEmpty() && (QUuid(record.credentialRef).isNull() || QUuid(record.credentialRef).toString(QUuid::WithoutBraces) != record.credentialRef))
        return fail(error, QStringLiteral("인증정보 참조 형식이 올바르지 않습니다."));
    if ((record.provider == "okx" && !record.region.isEmpty() && !QStringList{"global", "us", "eea"}.contains(record.region)) ||
        (record.provider != "okx" && !record.region.isEmpty()))
        return fail(error, QStringLiteral("OKX 연결 지역을 확인해 주세요."));
    if (!record.readOnly || record.connectionState != "not_checked" || record.permissionState != "unverified")
        return fail(error, QStringLiteral("G1에서는 조회 모드와 API 권한 미검증 상태만 사용할 수 있습니다."));
    if (record.storageMode != StorageMode::EncryptedVault && record.storageMode != StorageMode::SessionOnly)
        return fail(error, QStringLiteral("지원하지 않는 보관 방식입니다."));
    return true;
}

QJsonObject AccountModel::toJson(const AccountRecord& r) {
    QJsonObject result{{"id", r.id}, {"provider", r.provider}, {"alias", r.alias}, {"environment", r.environment},
            {"market", r.market}, {"currency", r.currency},
            {"storage_mode", r.storageMode == StorageMode::EncryptedVault ? "vault" : "session"},
            {"connection_state", r.connectionState}, {"permission_state", r.permissionState},
            {"read_only", r.readOnly}};
    if (r.provider == "okx" && !r.region.isEmpty()) result["region"] = r.region;
    if (!r.credentialRef.isEmpty()) result["credentialRef"] = r.credentialRef;
    return result;
}

bool AccountModel::fromJson(const QJsonObject& obj, AccountRecord* record, QString* error) {
    static const QSet<QString> strings{"id", "provider", "alias", "environment", "market", "currency",
                                       "storage_mode", "connection_state", "permission_state"};
    const bool withRegion = obj.contains("region"), withRef = obj.contains("credentialRef");
    if (!record || obj.size() != strings.size() + 1 + (withRegion ? 1 : 0) + (withRef ? 1 : 0) ||
        (withRef && (!obj.value("credentialRef").isString() || obj.value("credentialRef").toString().isEmpty())) || !obj.value("read_only").isBool() ||
        (withRegion && (obj.value("provider") != QJsonValue("okx") || !obj.value("region").isString() ||
            !QStringList{"global", "us", "eea"}.contains(obj.value("region").toString()))))
        return fail(error, QStringLiteral("계좌 JSON 스키마가 올바르지 않습니다."));
    for (const auto& field : strings)
        if (!obj.value(field).isString())
            return fail(error, QStringLiteral("계좌 JSON 필드가 올바르지 않습니다."));
    const QString mode = obj.value("storage_mode").toString();
    if (mode != "vault" && mode != "session")
        return fail(error, QStringLiteral("계좌 보관 방식이 올바르지 않습니다."));
    AccountRecord r;
    r.id = obj.value("id").toString();
    r.provider = obj.value("provider").toString();
    r.region = obj.value("region").toString();
    r.credentialRef = obj.value("credentialRef").toString();
    r.alias = obj.value("alias").toString();
    r.environment = obj.value("environment").toString();
    r.market = obj.value("market").toString();
    r.currency = obj.value("currency").toString();
    r.storageMode = mode == "vault" ? StorageMode::EncryptedVault : StorageMode::SessionOnly;
    r.connectionState = obj.value("connection_state").toString();
    r.permissionState = obj.value("permission_state").toString();
    r.readOnly = obj.value("read_only").toBool();
    if (!validate(r, error, true))
        return false;
    *record = std::move(r);
    return true;
}

AccountModel::AccountModel(Vault* vault, QObject* parent) : QAbstractListModel(parent), vault_(vault) {
    Q_ASSERT(vault_);
    connect(vault_, &Vault::accountsChanged, this, &AccountModel::refresh);
    refresh();
}

void AccountModel::refresh() {
    beginResetModel();
    records_ = vault_->accounts();
    endResetModel();
}

int AccountModel::rowCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : records_.size(); }

AccountRecord AccountModel::recordAt(int row) const {
    return row >= 0 && row < records_.size() ? records_.at(row) : AccountRecord{};
}

QVariant AccountModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= records_.size() || index.column() != 0)
        return {};
    const auto& r = records_.at(index.row());
    switch (role) {
    case Qt::DisplayRole: case AliasRole: return r.alias;
    case IdRole: return r.id;
    case ProviderRole: return r.provider;
    case EnvironmentRole: return r.environment;
    case MarketRole: return r.market;
    case CurrencyRole: return r.currency;
    case StorageModeRole: return r.storageMode == StorageMode::EncryptedVault ? "vault" : "session";
    case ConnectionStateRole: return r.connectionState;
    case PermissionStateRole: return r.permissionState;
    case ReadOnlyRole: return r.readOnly;
    default: return {};
    }
}

QHash<int, QByteArray> AccountModel::roleNames() const {
    return {{IdRole, "id"}, {ProviderRole, "provider"}, {AliasRole, "alias"},
            {EnvironmentRole, "environment"}, {MarketRole, "market"}, {CurrencyRole, "currency"},
            {StorageModeRole, "storageMode"}, {ConnectionStateRole, "connectionState"},
            {PermissionStateRole, "permissionState"}, {ReadOnlyRole, "readOnly"}};
}

} // namespace keefi
