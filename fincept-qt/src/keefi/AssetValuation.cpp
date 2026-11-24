#include "keefi/AssetValuation.h"
#include "keefi/SolanaTokenData.h"
#include "keefi/SolanaTransactionCodec.h"
#include "keefi/TradingTypes.h"
#include <QHash>
#include <QMap>
#include <QDateTime>
#include <QRegularExpression>
#include <QSet>
#include <QTimeZone>
#include <algorithm>
#include <cmath>

namespace keefi {
namespace {
bool amount(const QString& s) {
    static const QRegularExpression re("\\A-?[0-9]{1,80}(?:\\.[0-9]{1,80})?\\z");
    return re.match(s).hasMatch();
}
QString magnitude(QString s) { if(s.startsWith('-'))s.remove(0,1);return s; }
QString normal(const QString& s) {
    if(!amount(s))return {};const auto n=decimal::normalize(magnitude(s));return s.startsWith('-')&&n!="0"?"-"+n:n;
}
QString negate(const QString& s) { const auto n=normal(s);return n.isEmpty()||n=="0"?n:n.startsWith('-')?n.mid(1):"-"+n; }
QString sum(const QString& a,const QString& b) {
    if(!amount(a)||!amount(b))return {};
    const bool an=a.startsWith('-'),bn=b.startsWith('-');const auto x=magnitude(a),y=magnitude(b);
    QString r;
    if(an==bn){r=decimal::add(x,y);if(an&&r!="0")r.prepend('-');}
    else if(decimal::compare(x,y)>=0){r=decimal::subtract(x,y);if(an&&r!="0")r.prepend('-');}
    else{r=decimal::subtract(y,x);if(bn&&r!="0")r.prepend('-');}
    return amount(r)?r:QString{};
}
bool fresh(const AssetEvidence& e,qint64 at,qint64 age) {
    return e.complete&&!e.source.isEmpty()&&!e.scope.isEmpty()&&e.receivedAtMs>0&&e.receivedAtMs<=at&&at-e.receivedAtMs<=age&&
        (e.sourceTimeMs==0||(e.sourceTimeMs>0&&e.sourceTimeMs<=at&&at-e.sourceTimeMs<=age));
}
qint64 priceTime(const AssetEvidence& e) { return e.sourceTimeMs > 0 ? e.sourceTimeMs : e.receivedAtMs; }
bool publicClockOrder(const AssetEvidence& e) {
    // Preserve both clocks. Admission permits a source lead of at most 2 s;
    // publicPrice/fresh still reject any source later than the valuation time.
    if(e.sourceTimeMs<0||e.receivedAtMs<=0)return false;
    return e.sourceTimeMs <= e.receivedAtMs || e.sourceTimeMs - e.receivedAtMs <= 2000;
}
bool publicPrice(const AssetQuote& q, qint64 at, bool historical = false) {
    const auto& e=q.evidence;
    return decimal::positive(q.price)&&e.complete&&!e.source.isEmpty()&&!e.url.isEmpty()&&e.scope.startsWith("public:")&&
        e.sourceTimeMs>=0&&e.sourceTimeMs<=at&&e.receivedAtMs>0&&(historical||e.receivedAtMs<=at)&&
        publicClockOrder(e);
}
bool referenceAge(const AssetEvidence& e,qint64 at) {
    return priceTime(e)>0&&priceTime(e)<=at&&at-priceTime(e)<=ReferenceMaxAgeMs&&
        e.receivedAtMs>0&&e.receivedAtMs<=at&&at-e.receivedAtMs<=ReferenceMaxAgeMs;
}
enum class QuoteSet { Current, References, ReferencesAndCurrent };

// Buckets hold pointers into the caller's immutable input lists for one valuation.
// Keep every candidate, in source order, so equal-time conflicts and evidence ties
// are resolved by the same rules for current, historical and display-only quotes.
class QuoteIndex {
  public:
    explicit QuoteIndex(const QList<AssetQuote>& current, const QList<AssetQuote>* references = nullptr)
        : current_(current), references_(references) {}

    template<class Accept>
    const AssetQuote* latest(const QString& base, const QString& counter, QuoteSet set, Accept accept) {
        if (!indexed_) {
            for (const auto& quote : current_)
                byPair_[{quote.baseAsset, quote.quoteAsset}].current.append(&quote);
            if (references_)
                for (const auto& quote : *references_)
                    byPair_[{quote.baseAsset, quote.quoteAsset}].references.append(&quote);
            indexed_ = true;
        }
        const auto pair = byPair_.constFind({base, counter});
        if (pair == byPair_.cend()) return nullptr;
        const AssetQuote* selected = nullptr;
        bool conflict = false;
        const auto consider = [&](const QList<const AssetQuote*>& candidates) {
            for (const auto* quote : candidates) {
                if (!accept(*quote)) continue;
                const auto time = priceTime(quote->evidence);
                if (!selected || time > priceTime(selected->evidence)) {
                    selected = quote;
                    conflict = false;
                } else if (time == priceTime(selected->evidence)
                           && decimal::normalize(quote->price) != decimal::normalize(selected->price)) {
                    conflict = true;
                }
            }
        };
        // The reference fallback historically searched saved quotes before current ones.
        if (set != QuoteSet::Current) consider(pair->references);
        if (set != QuoteSet::References) consider(pair->current);
        return conflict ? nullptr : selected;
    }

    const AssetQuote* publicLeg(const QString& base, const QString& counter, const QString& scope,
                                qint64 at, QuoteSet set) {
        return latest(base, counter, set, [&](const AssetQuote& quote) {
            return quote.evidence.scope == scope && quote.evidence.sourceTimeMs > 0 && publicPrice(quote, at);
        });
    }

  private:
    struct Candidates { QList<const AssetQuote*> current, references; };
    const QList<AssetQuote>& current_;
    const QList<AssetQuote>* references_;
    QHash<QPair<QString, QString>, Candidates> byPair_;
    bool indexed_ = false; // Cash-only and zero balances need no price lookup.
};

struct CrossPrice { AssetQuote quote; QList<AssetEvidence> legs; };
CrossPrice crossPrice(const QString& asset, const QString& currency, QuoteIndex& quotes, qint64 at, QuoteSet set);
CrossPrice solanaCrossPrice(const QString& asset, QuoteIndex& quotes, qint64 at, QuoteSet set) {
    const auto mint=asset.mid(7);
    const auto* token = quotes.publicLeg(asset, "USD", "public:jupiter:price:" + mint, at, set);
    const auto* usdc = quotes.publicLeg("solana:" + UsdcMint, "USD", "public:jupiter:price:" + UsdcMint, at, set);
    if(!token||!usdc)return {};
    // Upbit KRW-USDC trades thinly. When Binance USDC/USDT × Upbit KRW-USDT has
    // a later original time, that fresher USDC→KRW leg is used instead.
    const auto* upbit = quotes.publicLeg("USDC", "KRW", "public:upbit:KRW-USDC", at, set);
    const auto binance = crossPrice("USDC", "KRW", quotes, at, set);
    AssetQuote krw;QList<AssetEvidence> krwLegs;
    if(upbit&&(binance.quote.price.isEmpty()||upbit->evidence.sourceTimeMs>=binance.quote.evidence.sourceTimeMs)){krw=*upbit;krwLegs={upbit->evidence};}
    else if(!binance.quote.price.isEmpty()){krw=binance.quote;krwLegs=binance.legs;}
    else return {};
    const auto price=solanaPriceRatio(decimal::multiply(token->price,krw.price),usdc->price);
    if(!decimal::positive(price))return {};
    CrossPrice result;result.legs={token->evidence,usdc->evidence};result.legs.append(krwLegs);
    result.quote={asset,"KRW",price,{"Jupiter USD ÷ USDC USD × "+(krwLegs.size()==1?QString("업비트 KRW-USDC"):krw.evidence.source),
        token->evidence.url+" × "+usdc->evidence.url+" × "+krw.evidence.url,
        "derived:solana-usdc:"+mint,
        qMin(token->evidence.sourceTimeMs,qMin(usdc->evidence.sourceTimeMs,krw.evidence.sourceTimeMs)),
        qMin(token->evidence.receivedAtMs,qMin(usdc->evidence.receivedAtMs,krw.evidence.receivedAtMs)),true}};
    return result;
}
CrossPrice crossPrice(const QString& asset, const QString& currency, QuoteIndex& quotes, qint64 at, QuoteSet set) {
    if(currency!="KRW"||asset=="KRW"||asset=="USDT")return {};
    if(asset.startsWith("solana:"))return solanaCrossPrice(asset,quotes,at,set);
    const auto* coin = quotes.publicLeg(asset, "USDT", "public:binance:" + asset + "USDT", at, set);
    const auto* fx = quotes.publicLeg("USDT", "KRW", "public:upbit:KRW-USDT", at, set);
    if (!coin || !fx) return {};
    CrossPrice result;result.legs={coin->evidence,fx->evidence};
    result.quote={asset,"KRW",decimal::multiply(coin->price,fx->price),
        {"Binance USDT × 업비트 KRW-USDT",coin->evidence.url+" × "+fx->evidence.url,"derived:binance-usdt:"+asset,
        qMin(coin->evidence.sourceTimeMs,fx->evidence.sourceTimeMs),qMin(coin->evidence.receivedAtMs,fx->evidence.receivedAtMs),true}};
    return result;
}
QString convert(const QString& quantity, const QString& asset, const QString& currency, QuoteIndex& quotes,
                qint64 at, qint64 age, bool historical = false, AssetEvidence* used = nullptr,
                QList<AssetEvidence>* legs = nullptr) {
    if(!amount(quantity)||asset.isEmpty()||currency.isEmpty())return {};
    if(asset==currency||normal(quantity)=="0")return normal(quantity);
    const auto* selected = quotes.latest(asset, currency, QuoteSet::Current, [&](const AssetQuote& quote) {
        const auto& e = quote.evidence;
        const bool validTime = historical ? e.sourceTimeMs > 0 && e.sourceTimeMs <= at && at - e.sourceTimeMs <= age
                                         : fresh(e, at, age);
        return validTime && publicPrice(quote, at, historical);
    });
    CrossPrice cross;
    if (!selected && !historical) {
        cross = crossPrice(asset, currency, quotes, at, QuoteSet::Current);
        if (!cross.quote.price.isEmpty() && fresh(cross.quote.evidence, at, age)) {
            selected = &cross.quote;
            if (legs) *legs = cross.legs;
        }
    }
    if(!selected)return {};if(used)*used=selected->evidence;auto value=decimal::multiply(magnitude(quantity),selected->price);
    if(quantity.startsWith('-')&&value!="0")value.prepend('-');return amount(value)?value:QString{};
}
QString referenceValue(const QString& quantity, const QString& asset, const QString& currency, QuoteIndex& quotes,
                       qint64 at, qint64 age, AssetEvidence* used, QList<AssetEvidence>* legs) {
    if(!amount(quantity)||asset.isEmpty()||currency.isEmpty())return {};
    const auto* selected = quotes.latest(asset, currency, QuoteSet::References, [&](const AssetQuote& quote) {
        return publicPrice(quote, at) && referenceAge(quote.evidence, at) && !fresh(quote.evidence, at, age);
    });
    CrossPrice cross;
    if (!selected) {
        cross = crossPrice(asset, currency, quotes, at, QuoteSet::ReferencesAndCurrent);
        if (!cross.quote.price.isEmpty() && referenceAge(cross.quote.evidence, at) && !fresh(cross.quote.evidence, at, age)) {
            selected = &cross.quote;
            *legs = cross.legs;
        }
    }
    if(!selected)return {};
    auto value=decimal::multiply(magnitude(quantity),selected->price);if(!amount(value))return {};
    if(quantity.startsWith('-')&&value!="0")value.prepend('-');*used=selected->evidence;return value;
}
bool sameScopes(QStringList a,QStringList b){a.removeDuplicates();b.removeDuplicates();a.sort();b.sort();return !a.isEmpty()&&a==b;}
AssetRateResult missing(const QString& why){AssetRateResult r;r.reason=why;return r;}
long double number(const QString& value,bool* ok=nullptr){bool valid=false;const double n=value.toDouble(&valid);if(ok)*ok=valid&&std::isfinite(n);return n;}
}

QString sumAssetAmounts(const QStringList& amounts) {
    QStringList positive, negative;
    for (const auto& value : amounts) {
        const auto normalized = normal(value);
        if (normalized.isEmpty()) return {};
        if (normalized == "0") continue;
        if (normalized.startsWith('-')) negative.append(normalized.mid(1));
        else positive.append(normalized);
    }
    // Opposing terms are subtracted first: their difference cannot grow past
    // either valid operand. Only the remaining single-sign sum can overflow.
    while (!positive.isEmpty() && !negative.isEmpty()) {
        const auto plus = positive.takeLast(), minus = negative.takeLast();
        const int comparison = decimal::compare(plus, minus);
        if (comparison > 0) positive.append(decimal::subtract(plus, minus));
        else if (comparison < 0) negative.append(decimal::subtract(minus, plus));
    }
    const bool belowZero = !negative.isEmpty();
    const auto& remainder = belowZero ? negative : positive;
    QString total = "0";
    for (const auto& value : remainder) {
        total = decimal::add(total, value);
        if (!amount(total)) return {};
    }
    return belowZero && total != "0" ? "-" + total : total;
}

AssetValuationResult AssetValuation::value(const QList<AssetSnapshot>& snapshots,const QList<AssetQuote>& quotes,const QString& currency,qint64 at,qint64 age,const QList<AssetQuote>& displayOnlyReferences,const QSet<QString>& excludedAssets) {
    AssetValuationResult r;r.currency=currency;r.timeMs=at;r.knownSubtotal="0";r.complete=!snapshots.isEmpty()&&!currency.isEmpty()&&at>0&&age>=0;r.reconciled=r.complete;
    QuoteIndex indexedQuotes(quotes, &displayOnlyReferences);
    QStringList freshValues;
    QSet<QString> seen;
    for(const auto& s:snapshots){
        if(s.accountScope.isEmpty()||seen.contains(s.accountScope)){r.complete=false;r.reconciled=false;r.issues<<"계좌 범위 중복 또는 누락";continue;}
        seen.insert(s.accountScope);r.accountScopes<<s.accountScope;
        if(!s.balanceComplete||!fresh(s.evidence,at,age)||s.timeMs<=0||s.timeMs>at||at-s.timeMs>age){r.complete=false;r.issues<<"잔고 조회 범위 또는 평가 시각 미완";}
        r.reconciled=r.reconciled&&s.reconciled;
        QSet<QString> equityGroups;for(const auto& c:s.components)if(c.kind==AssetComponentKind::Equity)equityGroups.insert(c.valuationGroup);
        QSet<QString> ids,equityAssets;
        for(const auto& c:s.components){
            AssetValueRow row;row.accountScope=s.accountScope;row.componentId=c.id;row.asset=c.asset;row.quantity=c.quantity;row.currency=currency;
            row.priceEvidence=c.evidence;row.providerUnrealizedPnl=c.providerUnrealizedPnl;row.providerPnlCurrency=c.reportedValueCurrency;
            const bool equity=equityGroups.contains(c.valuationGroup);
            const auto equityKey=c.valuationGroup+QChar(0)+c.asset;
            const bool base=c.kind==AssetComponentKind::Holding||c.kind==AssetComponentKind::Cash||c.kind==AssetComponentKind::Liability;
            row.included=c.kind==AssetComponentKind::Equity||(base&&!equity);
            if(c.id.isEmpty()||ids.contains(c.id)){r.complete=false;row.reason="중복 자산 행";row.included=false;}
            else if(!row.included)row.reason=equity&&base?"계좌 equity에 포함되어 별도 가산하지 않음":"순자산과 다른 의미의 표시 항목";
            else if(c.valuationGroup.isEmpty()){r.complete=false;row.reason="자산 평가 범위 미확인";}
            else if(c.kind==AssetComponentKind::Equity&&equityAssets.contains(equityKey)){r.complete=false;row.reason="동일 범위·통화 equity 중복";}
            else if(c.asset.startsWith("solana:")&&SolanaTransactionCodec::publicKeyValid(c.asset.mid(7))&&excludedAssets.contains(c.asset)){
                row.included=false;row.excludedByUser=true;row.reason="사용자 평가 대상 제외";
            }
            else if((c.reportedValue.isEmpty()?!amount(c.quantity):!amount(c.reportedValue))||!fresh(c.evidence,at,age)){r.complete=false;row.reason="수량·공식 평가액 또는 출처·시각 미확인";}
            else{
                auto quantity=c.kind==AssetComponentKind::Liability?negate(magnitude(c.quantity)):c.quantity;
                if(!c.reportedValue.isEmpty()){
                    const auto reported=c.kind==AssetComponentKind::Liability?negate(magnitude(c.reportedValue)):c.reportedValue;
                    row.value=convert(reported,c.reportedValueCurrency,currency,indexedQuotes,at,age,false,&row.priceEvidence,&row.conversionEvidence);
                }else row.value=convert(quantity,c.asset,currency,indexedQuotes,at,age,false,&row.priceEvidence,&row.conversionEvidence);
                if(row.value.isEmpty()){
                    r.complete=false;row.reason="평가가격·직접 환율 또는 유효 시각 자료 부족";
                    const auto base=c.reportedValue.isEmpty()?quantity:(c.kind==AssetComponentKind::Liability?negate(magnitude(c.reportedValue)):c.reportedValue);
                    row.referenceValue=referenceValue(base,c.reportedValue.isEmpty()?c.asset:c.reportedValueCurrency,currency,indexedQuotes,at,age,&row.priceEvidence,&row.conversionEvidence);
                    if(!row.referenceValue.isEmpty())row.reason=QString("참고 평가 · 가격 갱신 %1 KST · 합계 제외").arg(
                        QDateTime::fromMSecsSinceEpoch(priceTime(row.priceEvidence)).toTimeZone(QTimeZone("Asia/Seoul")).toString("yyyy-MM-dd HH:mm:ss"));
                }
                else{
                    freshValues.append(row.value);
                    if(c.kind==AssetComponentKind::Holding&&!c.costBasis.isEmpty()&&!c.costBasis.startsWith('-')){
                        const auto cost=convert(c.costBasis,c.costCurrency,currency,indexedQuotes,at,age);
                        if(!cost.isEmpty())row.unrealizedPnl=sum(row.value,negate(cost));
                    }
                }
            }
            ids.insert(c.id);if(c.kind==AssetComponentKind::Equity)equityAssets.insert(equityKey);r.rows.append(row);
        }
    }
    r.knownSubtotal=sumAssetAmounts(freshValues);
    if(r.knownSubtotal.isEmpty()){r.complete=false;r.issues<<"금액 정밀도 상한 초과";}
    if(r.complete&&!r.knownSubtotal.isEmpty())r.netAssets=r.knownSubtotal;else r.complete=false;
    r.issues.removeDuplicates();return r;
}

AssetFlowResult AssetValuation::flows(const QList<AssetEvent>& events,const QStringList& scopes,const QList<AssetQuote>& quotes,const QString& currency,qint64 from,qint64 to,bool coverage,qint64 age) {
    AssetFlowResult r;r.currency=currency;r.netExternal="0";r.accountScopes=scopes;r.fromMs=from;r.toMs=to;r.complete=coverage&&!scopes.isEmpty()&&from>0&&to>from&&age>=0;
    QuoteIndex indexedQuotes(quotes);
    QSet<QString> ids,paired;QList<AssetEvent> selected;
    for(const auto& e:events)if(scopes.contains(e.accountScope)&&e.timeMs>from&&e.timeMs<=to){
        const auto key=e.accountScope+QChar(0)+e.id;
        if(ids.contains(key)){r.complete=false;r.issues<<"중복 이벤트";continue;}ids.insert(key);selected<<e;
    }
    for(const auto& e:selected){
        const auto key=e.accountScope+QChar(0)+e.id;
        if(!amount(e.quantity)||e.officialId.isEmpty()||!e.evidence.complete||e.evidence.source.isEmpty()||e.evidence.receivedAtMs<=0){r.complete=false;r.issues<<"공식 이벤트 근거 미완";continue;}
        if(e.kind==AssetEventKind::Unknown){r.complete=false;r.issues<<"성격 미확인 이벤트";continue;}
        if(e.kind==AssetEventKind::TransferIn||e.kind==AssetEventKind::TransferOut){
            if(paired.contains(key))continue;
            QList<const AssetEvent*> matches;
            if(!e.transferReference.isEmpty()&&!e.peerAccountScope.isEmpty()&&e.peerAccountScope!=e.accountScope&&scopes.contains(e.peerAccountScope))
                for(const auto& other:selected)if(other.accountScope==e.peerAccountScope&&other.peerAccountScope==e.accountScope&&other.transferReference==e.transferReference&&
                    other.asset==e.asset&&normal(other.quantity)==normal(e.quantity)&&!normal(e.quantity).startsWith('-')&&
                    ((e.kind==AssetEventKind::TransferIn&&other.kind==AssetEventKind::TransferOut)||(e.kind==AssetEventKind::TransferOut&&other.kind==AssetEventKind::TransferIn))&&
                    std::abs(other.timeMs-e.timeMs)<=3600000&&other.evidence.complete&&!other.officialId.isEmpty()&&!other.evidence.source.isEmpty()&&other.evidence.receivedAtMs>0)
                    matches<<&other;
            if(matches.size()!=1||paired.contains(matches.first()->accountScope+QChar(0)+matches.first()->id)){r.complete=false;r.issues<<"양쪽 이체 근거 불일치·누락";continue;}
            const auto& other=*matches.first();paired.insert(key);paired.insert(other.accountScope+QChar(0)+other.id);r.internalEventIds<<e.id<<other.id;continue;
        }
        // Fees, funding, dividends and fills are already reflected in reconciled equity;
        // keep them in the ledger, never deduct/add them again as external capital.
        if(e.kind!=AssetEventKind::ExternalIn&&e.kind!=AssetEventKind::ExternalOut)continue;
        if(e.quantity.startsWith('-')){r.complete=false;r.issues<<"입출금 방향·수량 충돌";continue;}
        auto converted=convert(e.quantity,e.asset,currency,indexedQuotes,e.timeMs,age,true);
        if(converted.isEmpty()){r.complete=false;r.issues<<"입출금 당시 평가가격·환율 부족";continue;}
        if(e.kind==AssetEventKind::ExternalOut)converted=negate(converted);
        r.external.append({e.id,converted,e.timeMs});r.netExternal=sum(r.netExternal,converted);if(r.netExternal.isEmpty())r.complete=false;
    }
    std::sort(r.external.begin(),r.external.end(),[](const auto& a,const auto& b){return a.timeMs<b.timeMs;});
    if(!r.complete)r.netExternal.clear();r.issues.removeDuplicates();return r;
}

AssetPerformanceResult AssetValuation::performance(const AssetValuationResult& opening,const AssetValuationResult& closing,const AssetFlowResult& flows,const AssetPerformanceCoverage& coverage) {
    AssetPerformanceResult r;r.currency=opening.currency;r.openingNetAssets=opening.netAssets;r.closingNetAssets=closing.netAssets;
    r.complete=opening.complete&&closing.complete&&opening.reconciled&&closing.reconciled&&flows.complete&&coverage.externalFlowsComplete&&coverage.balancesReconciled&&!coverage.hasGaps&&
        coverage.fromMs==opening.timeMs&&coverage.toMs==closing.timeMs&&coverage.toMs>coverage.fromMs&&opening.currency==closing.currency&&opening.currency==flows.currency&&
        sameScopes(opening.accountScopes,closing.accountScopes)&&sameScopes(opening.accountScopes,coverage.accountScopes)&&
        sameScopes(flows.accountScopes,coverage.accountScopes)&&flows.fromMs==coverage.fromMs&&flows.toMs==coverage.toMs;
    QString flowSum="0";for(const auto& f:flows.external){if(f.timeMs<=coverage.fromMs||f.timeMs>coverage.toMs||!amount(f.amount))r.complete=false;flowSum=sum(flowSum,f.amount);}
    if(flowSum.isEmpty()||flowSum!=normal(flows.netExternal))r.complete=false;
    if(r.complete){r.externalNet=flows.netExternal;r.investmentPnl=sum(sum(closing.netAssets,negate(opening.netAssets)),negate(flows.netExternal));r.complete=!r.investmentPnl.isEmpty();}
    if(!r.complete){r.externalNet.clear();r.investmentPnl.clear();r.issues<<"기초·기말 잔고 대사, 동일 계좌 범위 또는 외부 흐름 자료 부족";return r;}
    QList<AssetFlowResult::Flow> cash{{"opening",negate(opening.netAssets),opening.timeMs}};
    for(const auto& f:flows.external)cash.append({f.eventId,negate(f.amount),f.timeMs});
    cash.append({"closing",closing.netAssets,closing.timeMs});r.mwr=mwr(cash,true);return r;
}

AssetRateResult AssetValuation::twr(const QList<AssetTwrPoint>& points,const AssetFlowResult& flows,const AssetPerformanceCoverage& coverage) {
    if(points.size()<2||!flows.complete||!coverage.externalFlowsComplete||!coverage.balancesReconciled||coverage.hasGaps||points.first().timeMs!=coverage.fromMs||points.last().timeMs!=coverage.toMs||
        flows.fromMs!=coverage.fromMs||flows.toMs!=coverage.toMs||!sameScopes(flows.accountScopes,coverage.accountScopes))
        return missing("구간 경계·외부 흐름·중간 평가 자료 부족");
    QMap<qint64,QString> changes;for(const auto& f:flows.external){if(f.timeMs<=coverage.fromMs||f.timeMs>coverage.toMs||!amount(f.amount))return missing("외부 흐름 범위 오류");changes[f.timeMs]=sum(changes.value(f.timeMs,"0"),f.amount);}
    long double product=1;QSet<qint64> observed;
    for(qsizetype i=0;i<points.size();++i){const auto& p=points[i];
        if(!p.reconciled||!amount(p.beforeFlowValue)||!amount(p.afterFlowValue)||(i&&p.timeMs<=points[i-1].timeMs)||
            sum(p.afterFlowValue,negate(p.beforeFlowValue))!=normal(changes.value(p.timeMs,"0")))return missing("외부 흐름 시점의 전후 평가가 맞지 않음");
        observed.insert(p.timeMs);if(!i)continue;
        bool a=false,b=false;const auto before=number(p.beforeFlowValue,&a),after=number(points[i-1].afterFlowValue,&b);
        if(!a||!b||before<0||after<=0)return missing("0 이하 기초 평가 또는 비율 계산 범위 초과");product*=before/after;
    }
    for(auto it=changes.cbegin();it!=changes.cend();++it)if(!observed.contains(it.key()))return missing("외부 흐름 시점의 중간 평가 누락");
    if(!std::isfinite(product))return missing("비율 계산 범위 초과");
    AssetRateResult r;r.state=AssetRateState::Available;r.rate=QString::number(double(product-1),'g',12);r.method="정확한 외부 흐름 경계의 기하 연결, 기간 수익률·비연율화; 비율 12유효숫자 반올림";return r;
}

AssetRateResult AssetValuation::mwr(const QList<AssetFlowResult::Flow>& input,bool complete) {
    if(!complete||input.size()<2||input.size()>20002)return missing("MWR 현금 흐름 자료 부족");
    QMap<qint64,QString> grouped;
    for(const auto& f:input){if(f.timeMs<=0||!amount(f.amount))return missing("MWR 날짜·금액 오류");grouped[f.timeMs]=sum(grouped.value(f.timeMs,"0"),f.amount);}
    QList<QPair<long double,long double>> cash;int last=0,changes=0;const qint64 first=grouped.firstKey();
    for(auto it=grouped.cbegin();it!=grouped.cend();++it){bool ok=false;const auto n=number(it.value(),&ok);if(!ok)return missing("MWR 수치 범위 초과");if(n==0)continue;const int sign=n>0?1:-1;if(last&&last!=sign)++changes;last=sign;cash.append({static_cast<long double>(it.key()-first)/(365.0L*86400000.0L),n});}
    AssetRateResult r;r.annualized=true;r.method="ACT/365 XIRR, 연율화·12유효숫자; 단일 부호 전환만 전역 유일성 확인";
    if(cash.isEmpty()){r.state=AssetRateState::UniquenessUnverified;r.reason="모든 현금 흐름이 상쇄되어 수익률을 정할 수 없음";return r;}
    if(changes==0||cash.size()<2||cash.first().first==cash.last().first){r.state=AssetRateState::NoSolution;r.reason="서로 다른 시점의 양수·음수 현금 흐름이 필요함";return r;}
    const auto npv=[&](long double x){long double largest=-1e30L;for(const auto& f:cash)largest=std::max(largest,-x*f.first);long double total=0;for(const auto& f:cash)total+=f.second*std::exp(-x*f.first-largest);return total;};
    const auto root=[&](long double a,long double b){const auto left=npv(a);for(int i=0;i<200;++i){const auto m=(a+b)/2,v=npv(m);if(v==0||b-a<1e-14L)return m;if((v<0)==(left<0))a=m;else b=m;}return (a+b)/2;};
    QList<long double> roots;long double previous=-40,prev=npv(previous);
    // Multiple roots found in this finite range prove non-uniqueness. A scan finding
    // zero/one root never proves global absence/uniqueness for nonconventional flows.
    for(int i=1;i<=8000;++i){const long double x=-40+i*0.01L,v=npv(x);if(v==0||(v<0)!=(prev<0)){const auto found=v==0?x:root(previous,x);if(roots.isEmpty()||std::abs(found-roots.last())>1e-8L)roots.append(found);}previous=x;prev=v;}
    if(roots.size()>1){r.state=AssetRateState::MultipleSolutions;r.reason="복수 MWR 해를 확인하여 단일 수익률을 표시하지 않음";return r;}
    if(changes!=1||roots.size()!=1){r.state=AssetRateState::UniquenessUnverified;r.reason="해의 존재·전역 유일성 또는 탐색 범위 미확인";return r;}
    const auto rate=std::expm1(roots.first());if(!std::isfinite(rate)||!std::isfinite(double(rate))){r.state=AssetRateState::UniquenessUnverified;r.reason="수익률 수치 범위 초과";return r;}
    r.state=AssetRateState::Available;r.rate=QString::number(double(rate),'g',12);return r;
}
} // namespace keefi
