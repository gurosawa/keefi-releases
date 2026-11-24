#include "keefi/HoldingMarketResolver.h"
#include "keefi/ExchangeIdentifiers.h"
#include "keefi/SwapTypes.h"
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>

namespace keefi {
HoldingChartRow resolveHoldingMarket(const AssetKey& key,const QStringList& providers,
    const HoldingMarketCatalog& catalog,const HoldingMarketPreferences& preferences){
    HoldingChartRow row;row.assetKey=key;
    row.chartReason=QObject::tr("보유 중 · 차트 지원 없음 (공개 시장 연결 없음)");
    const auto add=[&](const WatchlistIdentity& identity){if(WatchlistWorkspaceList::validIdentity(identity)&&!row.candidates.contains(identity))row.candidates.append(identity);};
    if(key.startsWith("stock:KR:")||key.startsWith("stock:US:")){
        const auto identity=WatchlistWorkspaceList::identityFromKey(key);row.symbol=key.section(':',2);
        if(providers.isEmpty()){
            if(catalog.verifiedStocks.contains(identity))add(identity);
        }else{
            // Legacy domestic_equity grouping has no country field. Require a
            // unique broker-verified instrument; never infer country from a code.
            QList<WatchlistIdentity> matches;
            for(const auto& candidate:catalog.verifiedStocks)if(candidate.market=="stock"&&candidate.code==row.symbol&&
                WatchlistWorkspaceList::validIdentity(candidate)&&!matches.contains(candidate))matches.append(candidate);
            if(matches.size()==1)add(matches.first());
        }
    }else if(key=="solana:native"||key.startsWith("exchange:")){
        const auto asset=key=="solana:native"?QStringLiteral("SOL"):key.mid(9);row.symbol=asset;
        if(binanceIdentifierValid(asset)){
            // Only a catalogued instrument with implemented history is selectable.
            // Unknown custody is allowed solely for an explicitly public request.
            const auto mayUse=[&](const QString& provider){return key=="solana:native"||providers.isEmpty()||providers.contains(provider);};
            if(mayUse("upbit")&&catalog.upbitAvailable){int matches=0;
                for(const auto& item:catalog.upbitRows)if(item.base==asset&&item.market=="KRW-"+asset)++matches;
                if(matches==1)add({"coin","upbit","KRW-"+asset});
            }
            if(mayUse("binance")){int matches=0;MarketInstrument selected;
                for(const auto& item:catalog.binance)if(item.baseAsset==asset&&item.quoteAsset=="USDT"){++matches;selected=item;}
                if(matches==1&&selected.market=="spot"&&selected.status=="TRADING"&&selected.tradable&&
                   binanceIdentifierValid(selected.baseAsset)&&binanceIdentifierValid(selected.quoteAsset)&&
                   selected.symbol==selected.baseAsset+selected.quoteAsset)add({"coin","binance",selected.symbol});
            }
        }
    }else if(key.startsWith("solana:")){
        row.symbol=key.mid(7).left(6)+QStringLiteral("…")+key.right(6);
        row.chartReason=key=="solana:"+SolMint?QObject::tr("wSOL · 래핑 토큰 · SOL과 합치지 않아요 · 차트 지원 없음"):
            QObject::tr("별도 토큰 · 차트 지원 없음 (mint 기준 공개 시장 연결 없음)");
    }
    if(row.candidates.isEmpty()){
        if(providers.contains("bithumb")&&key.startsWith("exchange:"))row.chartReason=QObject::tr("보유 중 · 차트 지원 없음 (빗썸 차트 이력 미지원)");
        else if((key=="solana:native"||key.startsWith("exchange:"))&&!catalog.upbitAvailable&&catalog.binance.isEmpty())
            row.chartReason=QObject::tr("보유 중 · 차트 확인 중 (검증된 공개 종목 목록 없음)");
        return row;
    }
    const auto choose=[&](const WatchlistIdentity& identity,int rule){row.chart=identity;row.selectionRule=rule;row.chartReason.clear();};
    const auto session=preferences.sessionSelections.value(key);
    if(row.candidates.contains(session)){choose(session,0);return row;}
    for(const auto& identity:preferences.publicPriority)if(row.candidates.contains(identity)){choose(identity,1);return row;}
    for(const auto& identity:row.candidates)if(providers.contains(identity.source)){choose(identity,2);return row;}
    for(const auto& identity:row.candidates)if((preferences.displayCurrency=="KRW"&&identity.source=="upbit")||
        (preferences.displayCurrency=="USD"&&identity.source=="binance")){choose(identity,3);return row;}
    choose(row.candidates.first(),4);return row;
}
QList<HoldingChartRow> resolveHoldingMarkets(const QList<AssetSnapshot>& snapshots,
    const QList<PortfolioConnection>& connections,const QList<AssetValueRow>& values,
    const HoldingMarketCatalog& catalog,const HoldingMarketPreferences& preferences,qint64 now){
    struct Group {HoldingChartRow row;QStringList providers,valuationMarkets;qint64 freshAt=0;bool fresh=false;};
    QMap<AssetKey,Group> groups;QSet<QString> seen;
    const auto current=[now](qint64 at){return at>0&&at<=now&&now-at<=DisplayFreshnessMs;};
    const auto custodyName=[](const QString& provider){
        static const QMap<QString,QString> names{{"phantom","Phantom"},{"upbit","업비트"},{"binance","Binance"},{"bithumb","빗썸"},
            {"okx","OKX"},{"toss","토스"},{"kiwoom","키움"},{"hyperliquid","Hyperliquid"}};
        return names.value(provider,QObject::tr("기타 보관처"));
    };
    const auto valuationLabel=[](const AssetValueRow& value){
        const auto& evidence=value.priceEvidence;const QUrl url(evidence.url,QUrl::StrictMode);
        if(!value.included||value.value.isEmpty()||!evidence.complete||!url.isValid()||!url.userInfo().isEmpty()||url.hasFragment())return QString{};
        if(evidence.scope=="public:upbit:KRW-"+value.asset&&
           ((url.scheme()=="https"&&url.host()=="api.upbit.com"&&url.path()=="/v1/ticker")||
            (url.scheme()=="wss"&&url.host()=="api.upbit.com"&&url.path()=="/websocket/v1")))
            return QObject::tr("업비트 KRW-%1").arg(value.asset);
        // Arbitrary evidence/source text can contain account labels. Unknown
        // origins remain absent instead of guessing an evaluator route.
        return QString{};
    };
    using ScopeKey = QPair<QString, QString>;
    QHash<ScopeKey, QList<const AssetValueRow*>> valuesByComponent;
    for (const auto& value : values)
        valuesByComponent[{value.accountScope, value.componentId}].append(&value);
    QHash<ScopeKey, const PortfolioConnection*> connectionsByScope;
    for (const auto& connection : connections) {
        const ScopeKey key{connection.account.id, connection.accountScope};
        if (!connection.shared && !connectionsByScope.contains(key))
            connectionsByScope.insert(key, &connection);
    }
    static const QSet<QString> failedStates{"failed","authentication_required","account_required","unsupported","stopped"};
    for(const auto& snapshot:snapshots)for(const auto& component:snapshot.components){
        if(component.kind!=AssetComponentKind::Holding||!decimal::positive(component.quantity))continue;
        const auto unique=snapshot.accountScope+QChar(0)+component.id;
        if(seen.contains(unique))continue;seen.insert(unique);
        const auto key=assetIdentity(snapshot,component);if(key.isEmpty()||key.startsWith("cash:"))continue;
        auto& group=groups[key];group.row.assetKey=key;
        if(group.row.symbol.isEmpty()){
            group.row.symbol=key=="solana:native"?QStringLiteral("SOL"):component.asset;
            if(key.startsWith("solana:")&&key!="solana:native"){
                const auto mint=key.mid(7);const auto shortMint=mint.left(6)+QStringLiteral("…")+mint.right(6);
                group.row.symbol=component.displaySymbol.isEmpty()?shortMint:component.displaySymbol+" · "+shortMint;
            }
            group.row.displayName=component.displayName.isEmpty()?group.row.symbol:component.displayName;
        }
        if(!group.providers.contains(snapshot.provider))group.providers.append(snapshot.provider);
        const auto* connection = connectionsByScope.value({snapshot.connectionId, snapshot.accountScope});
        const auto& evidence=component.evidence==AssetEvidence{}?snapshot.evidence:component.evidence;
        const bool fresh=connection&&connection->state=="complete"&&current(connection->receivedAtMs)&&snapshot.balanceComplete&&
            current(snapshot.timeMs)&&evidence.complete&&current(evidence.receivedAtMs)&&(evidence.sourceTimeMs==0||current(evidence.sourceTimeMs));
        const bool failed=!connection||failedStates.contains(connection->state);
        const auto state=fresh?HoldingCustodyState::Fresh:failed?HoldingCustodyState::Failed:HoldingCustodyState::Delayed;
        const auto at=snapshot.timeMs>0&&snapshot.timeMs<=now?snapshot.timeMs:0;
        bool merged=false;for(auto& custody:group.row.custodies)if(custody.provider==snapshot.provider){
            if(int(state)>int(custody.state))custody.state=state;
            if(at>0)custody.observedAtMs=custody.observedAtMs?qMin(custody.observedAtMs,at):at;merged=true;break;
        }
        if(!merged)group.row.custodies.append({snapshot.provider,custodyName(snapshot.provider),state,at});
        if(at>0)group.row.observedAtMs=group.row.observedAtMs?qMin(group.row.observedAtMs,at):at;
        if(fresh){group.fresh=true;const auto observed=qMin(snapshot.timeMs,connection->receivedAtMs);group.freshAt=group.freshAt?qMin(group.freshAt,observed):observed;}
        const auto matchingValues = valuesByComponent.constFind({snapshot.accountScope, component.id});
        if (matchingValues != valuesByComponent.cend()) for (const auto* value : matchingValues.value()) {
            const auto label=valuationLabel(*value);if(!label.isEmpty()&&!group.valuationMarkets.contains(label))group.valuationMarkets.append(label);
        }
    }
    QList<HoldingChartRow> rows;
    for(auto it=groups.begin();it!=groups.end();++it){auto& group=it.value();const auto route=resolveHoldingMarket(it.key(),group.providers,catalog,preferences);
        group.row.chart=route.chart;group.row.candidates=route.candidates;group.row.chartReason=route.chartReason;group.row.selectionRule=route.selectionRule;
        group.row.alertsEligible=group.fresh&&route.chart.has_value();if(group.row.alertsEligible)group.row.observedAtMs=group.freshAt;
        group.row.valuationMarket=group.valuationMarkets.join(QStringLiteral(" · "));rows.append(group.row);
    }
    return rows;
}
}
