#include "keefi/ChartInstrumentPresentation.h"
#include "keefi/ChartInstrumentCatalog.h"
#include "keefi/PublicMarketCatalog.h"
#include "keefi/PublicEquityBoard.h"

namespace keefi {
QString ChartInstrumentLabel::text() const {
    if(code.isEmpty())return QObject::tr("코인 선택");
    return (name.isEmpty()?QString{}:name+" ")+code+(market.isEmpty()?QString{}:" · "+market);
}
QString ChartInstrumentLabel::titleHtml(const QString& suffix,FontRole role) const {
    const auto font=KeefiFonts::font(role);
    // Names, including all-uppercase English, always retain the body face.
    const auto nameHtml=name.isEmpty()?QString{}:QString("<span style=\"font-family:'%1';font-size:%2px;font-weight:%3;\">%4 </span>")
        .arg(font.family()).arg(font.pixelSize()).arg(int(font.weight())).arg(name.toHtmlEscaped());
    auto rest=*this;rest.name.clear();
    return nameHtml+KeefiFonts::mixedTextHtml(rest.text()+suffix,role);
}
ChartInstrumentLabel ChartInstrumentPresentation::fallback(const WatchlistIdentity& identity) {
    if(!WatchlistWorkspaceList::validIdentity(identity))return {};
    ChartInstrumentLabel result;result.code=identity.code;
    if(identity.market=="stock")result.market=identity.source=="KR"?tr("국내 주식"):tr("미국 주식");
    else if(identity.source=="upbit"||identity.source=="bithumb"){
        result.market=identity.source=="upbit"?tr("업비트"):tr("빗썸");
        const auto parts=identity.code.split('-');if(parts.size()==2)result.code=parts[1]+"/"+parts[0];
    }else if(identity.source=="binance")result.market=tr("바이낸스");
    else result.market=tr("공개 시장");
    return result;
}
ChartInstrumentLabel ChartInstrumentPresentation::label(const WatchlistIdentity& identity) const {
    auto result=fallback(identity);
    if(result.code.isEmpty())return result;
    if(identity.market=="coin"&&identity.source=="upbit"&&markets_){
        for(const auto& row:markets_->rows())if(row.market==identity.code){
            result.name=row.koreanName.trimmed().isEmpty()?row.englishName.trimmed():row.koreanName.trimmed();break;
        }
    }else if(identity.market=="stock"&&board_){
        const auto item=board_->knownItem(identity.source=="KR"?"kr":"us",identity.code);
        if(item.symbol==identity.code)result.name=item.name.trimmed();
    }else if(identity.market=="coin"&&identity.source=="binance"&&instruments_){
        for(const auto& item:instruments_->instruments("binance"))if(item.symbol==identity.code&&item.market=="spot"&&
            !item.baseAsset.isEmpty()&&!item.quoteAsset.isEmpty()){
            result.code=item.baseAsset+"/"+item.quoteAsset;break;
        }
    }
    return result;
}
QHash<QString,ChartInstrumentLabel> ChartInstrumentPresentation::labels(const QList<WatchlistIdentity>& identities) const {
    bool needsUpbit=false,needsBinance=false;
    for(const auto& identity:identities)if(identity.market=="coin"){
        needsUpbit=needsUpbit||identity.source=="upbit";needsBinance=needsBinance||identity.source=="binance";
    }
    QHash<QString,QString> names,codes;
    if(needsUpbit&&markets_)for(const auto& row:markets_->rows())if(!names.contains(row.market))
        names.insert(row.market,row.koreanName.trimmed().isEmpty()?row.englishName.trimmed():row.koreanName.trimmed());
    if(needsBinance&&instruments_)for(const auto& item:instruments_->instruments("binance"))
        if(item.market=="spot"&&!item.baseAsset.isEmpty()&&!item.quoteAsset.isEmpty()&&!codes.contains(item.symbol))
            codes.insert(item.symbol,item.baseAsset+"/"+item.quoteAsset);
    QHash<QString,ChartInstrumentLabel> result;result.reserve(identities.size());
    for(const auto& identity:identities){
        auto value=fallback(identity);
        if(!value.code.isEmpty()){
            if(identity.market=="coin"&&identity.source=="upbit")value.name=names.value(identity.code);
            else if(identity.market=="stock"&&board_){const auto item=board_->knownItem(identity.source=="KR"?"kr":"us",identity.code);if(item.symbol==identity.code)value.name=item.name.trimmed();}
            else if(identity.market=="coin"&&identity.source=="binance"&&codes.contains(identity.code))value.code=codes.value(identity.code);
        }
        result.insert(identity.key(),value);
    }
    return result;
}
void ChartInstrumentPresentation::setSources(PublicMarketCatalog* markets,PublicEquityBoard* board,ChartInstrumentCatalog* instruments){
    if(markets_==markets&&board_==board&&instruments_==instruments)return;
    for(const auto& connection:connections_)disconnect(connection);connections_.clear();
    markets_=markets;board_=board;instruments_=instruments;
    if(markets)connections_.append(connect(markets,&PublicMarketCatalog::changed,this,&ChartInstrumentPresentation::changed));
    if(board)connections_.append(connect(board,&PublicEquityBoard::changed,this,&ChartInstrumentPresentation::changed));
    if(instruments)connections_.append(connect(instruments,&ChartInstrumentCatalog::changed,this,&ChartInstrumentPresentation::changed));
    for(QObject* source:{static_cast<QObject*>(markets),static_cast<QObject*>(board),static_cast<QObject*>(instruments)})
        if(source)connections_.append(connect(source,&QObject::destroyed,this,[this]{emit changed();}));
    emit changed();
}
}
