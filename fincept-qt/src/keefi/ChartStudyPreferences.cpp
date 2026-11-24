#include "keefi/ChartStudyPreferences.h"
#include "keefi/PublicFormatMigration.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <cmath>
namespace keefi {
namespace {
const QStringList legacyIds{"pullback","whales","similarity","cloud","volume","stock_flow"};
const QStringList newIds{"ma","bb","rsi","macd","levels","cvd","oi","contract","sentiment","absorption","sweep","squeeze","adx","breakout","low_volume","strong","trend","relative_strength"};
QStringList defaultOrder(){ return QStringList{"cloud","pullback","similarity","whales","stock_flow","volume"}+newIds; }
QMap<QString,ChartStudyPreference> defaults(){
    QMap<QString,ChartStudyPreference> values;
    for(const auto& id:legacyIds)values.insert(id,{});
    // Dev86 new profiles show the three core studies. Restore always overwrites
    // each enabled/visible bit, including saved false values of unknown origin.
    values["similarity"]={true,true};
    for(const auto& id:newIds)values.insert(id,{false,true});
    return values;
}
}
ChartStudyPreferences::ChartStudyPreferences(QObject* p):QObject(p),values_(defaults()),order_(defaultOrder()){}
QStringList ChartStudyPreferences::supportedIds(const QString& market)const{
    if(market!="coin"&&market!="stock")return {};
    QStringList ids{"pullback",market=="coin"?"whales":"stock_flow","similarity","cloud","volume"};
    ids+=newIds;
    if(market=="stock")for(const auto* id:{"cvd","oi","contract","sentiment","strong","relative_strength"})ids.removeAll(id);
    return ids;
}
bool ChartStudyPreferences::enabled(const QString& id)const{return values_.contains(id)&&values_.value(id).enabled;}
bool ChartStudyPreferences::visible(const QString& id)const{return values_.contains(id)&&values_.value(id).visible;}
bool ChartStudyPreferences::favorite(const QString& id)const{return values_.contains(id)&&favorites_.contains(id);}
bool ChartStudyPreferences::setFavorite(const QString& id,bool value){
    if(!values_.contains(id))return false;if(favorite(id)==value)return true;
    if(value)favorites_.append(id);else favorites_.removeAll(id);emit changed();return true;
}
bool ChartStudyPreferences::isPane(const QString& id){return id=="rsi"||id=="macd"||id=="cvd"||id=="oi";}
QStringList ChartStudyPreferences::enabledOrder(const QString& market)const{
    QStringList result;const auto supported=supportedIds(market);
    for(const auto& id:order_)if(supported.contains(id)&&enabled(id))result.append(id);
    return result;
}
QStringList ChartStudyPreferences::visiblePanes(const QString& market) const {
    QStringList panes;
    for (const auto& id : supportedIds(market))
        if (isPane(id) && enabled(id) && visible(id)) panes.append(id);
    return panes;
}
QList<ExtendedMaSpec> ChartStudyPreferences::movingAverages()const{return averages_;}
QStringList ChartStudyPreferences::colorKeys(const QString& study) {
    static const QMap<QString, QStringList> keys{
        {"ma", {"ma1","ma2","ma3","ma4"}}, {"bb", {"bbUpper","bbMiddle","bbLower"}},
        {"rsi", {"rsi"}}, {"macd", {"macd","macdSignal","macdHistogramUp","macdHistogramDown"}},
        {"cvd", {"cvd"}}, {"oi", {"oi"}}, {"low_volume", {"low_volume"}}, {"levels", {"levels"}},
        {"volume", {"volumeUp","volumeDown"}}, {"cloud", {"cloudUp","cloudDown"}},
        {"whales", {"whalesUp","whalesDown"}}, {"pullback", {"pullback"}},
        {"similarity", {"similarity","similarity1","similarity2","similarity3"}}
    };
    return keys.value(study);
}
namespace {
bool validSettings(const ChartStudySettings& value) {
    const auto validPeriod = [](int p) { return p >= 2 && p <= 120; };
    if (!validPeriod(value.rsiPeriod) || !validPeriod(value.macdFast) || !validPeriod(value.macdSlow) ||
        !validPeriod(value.macdSignal) || value.macdFast >= value.macdSlow || value.macdSlow + value.macdSignal > 201 ||
        value.displayBandPeriod < 2 || value.displayBandPeriod > ChartDisplayBandPeriod ||
        !std::isfinite(value.displayBandDeviations) || value.displayBandDeviations < 0.1 || value.displayBandDeviations > 10) return false;
    static const auto known = []{
        QSet<QString> all;
        for (const auto& study : defaultOrder()) {
            const auto keys = ChartStudyPreferences::colorKeys(study);
            all.unite(QSet<QString>(keys.begin(), keys.end()));
        }
        return all;
    }();
    for (auto i=value.colors.cbegin(); i!=value.colors.cend(); ++i)
        if (!known.contains(i.key()) || !i.value().isValid() || i.value().alpha()!=255) return false;
    return true;
}
QJsonObject encodeSettings(const ChartStudySettings& s) {
    QJsonObject colors;
    for (auto i=s.colors.cbegin(); i!=s.colors.cend(); ++i) colors.insert(i.key(),i.value().name());
    return {{"rsi",s.rsiPeriod},{"fast",s.macdFast},{"slow",s.macdSlow},{"signal",s.macdSignal},
            {"bandPeriod",s.displayBandPeriod},{"deviations",s.displayBandDeviations},{"colors",colors}};
}
bool decodeSettings(const QJsonObject& o, ChartStudySettings& s) {
    if (o.size()!=7 || !o.value("colors").isObject()) return false;
    s.rsiPeriod=o.value("rsi").toInt(-1); s.macdFast=o.value("fast").toInt(-1);
    s.macdSlow=o.value("slow").toInt(-1); s.macdSignal=o.value("signal").toInt(-1);
    s.displayBandPeriod=o.value("bandPeriod").toInt(-1); s.displayBandDeviations=o.value("deviations").toDouble(-1);
    const auto colors=o.value("colors").toObject();
    for (auto i=colors.begin();i!=colors.end();++i) {
        const auto text=i.value().toString();
        const QColor color(text);
        if (text.size()!=7 || color.name()!=text.toLower() || !color.isValid()) return false;
        s.colors.insert(i.key(),color);
    }
    return validSettings(s);
}
}
bool ChartStudyPreferences::setMovingAverages(const QList<ExtendedMaSpec>& input){
    return setStudySettings(settings_, input);
}
bool ChartStudyPreferences::setStudySettings(const ChartStudySettings& settings, const QList<ExtendedMaSpec>& input) {
    if (!validSettings(settings)) return false;
    if(input.isEmpty()||input.size()>ChartMovingAverageMaximumLines)return false;QList<ExtendedMaSpec> next;
    for(int i=0;i<input.size();++i){const auto& item=input[i];if(item.period<2||item.period>ChartMovingAverageMaximumPeriod||(item.kind!=ExtendedMaKind::Simple&&item.kind!=ExtendedMaKind::Exponential&&item.kind!=ExtendedMaKind::Weighted))return false;next.append({"ma"+QString::number(i+1),item.period,item.kind,item.enabled});}
    bool same=next.size()==averages_.size();for(int i=0;same&&i<next.size();++i)same=next[i].period==averages_[i].period&&next[i].kind==averages_[i].kind&&next[i].enabled==averages_[i].enabled;
    if(!same || settings_!=settings){averages_=next;settings_=settings;emit changed();}return true;
}
bool ChartStudyPreferences::setEnabled(const QString& id, bool value) {
    auto study = values_.find(id);
    if (study == values_.end()) return false;
    if (study->enabled == value) return true;
    study->enabled = value;
    if (value) {
        if (isPane(id)) study->visible = true;
        order_.removeAll(id);
        order_.prepend(id);
    }
    emit changed();
    return true;
}
bool ChartStudyPreferences::setVisible(const QString& id, bool value) {
    auto study = values_.find(id);
    if (study == values_.end()) return false;
    if (study->visible != value) {
        study->visible = value;
        emit changed();
    }
    return true;
}
bool ChartStudyPreferences::introSeen()const{return introSeen_;}
void ChartStudyPreferences::markIntroSeen(){if(!introSeen_){introSeen_=true;emit changed();}}
QString ChartStudyPreferences::chartStyle()const{return chartStyle_;}
bool ChartStudyPreferences::isChartStyle(const QString& style){return style=="candles"||style=="bars"||style=="line";}
bool ChartStudyPreferences::setChartStyle(const QString& style){
    if(!isChartStyle(style))return false;if(chartStyle_!=style){chartStyle_=style;emit changed();}return true;
}
QByteArray ChartStudyPreferences::publicState()const{
    QJsonObject studies;
    for(auto i=values_.cbegin();i!=values_.cend();++i)studies.insert(i.key(),QJsonObject{{"enabled",i->enabled},{"visible",i->visible}});
    QJsonArray averages;for(const auto& a:averages_)averages.append(QJsonObject{{"period",a.period},{"kind",a.kind==ExtendedMaKind::Simple?"sma":a.kind==ExtendedMaKind::Exponential?"ema":"wma"},{"enabled",a.enabled}});
    QJsonObject root{{"averages",averages},{"format","keefi-chart-studies/5"},{"studies",studies},{"introSeen",introSeen_},{"order",QJsonArray::fromStringList(order_)}};
    // Optional public presentation choices remain omitted at their defaults.
    if (settings_!=ChartStudySettings{}) root.insert("settings",encodeSettings(settings_));
    if(chartStyle_!="candles")root.insert("chartStyle",chartStyle_);
    if(!favorites_.isEmpty())root.insert("favorites",QJsonArray::fromStringList(favorites_));
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}
bool ChartStudyPreferences::restorePublicState(const QByteArray& bytes,QString* reason){
    const auto fail=[&](){if(reason)*reason=QStringLiteral("지표 표시 설정 형식이 올바르지 않습니다.");return false;};
    if(bytes.isEmpty()||bytes.size()>16384)return fail();
    QJsonParseError error;const auto doc=QJsonDocument::fromJson(bytes,&error);
    if(error.error!=QJsonParseError::NoError||!doc.isObject())return fail();
    const auto o=doc.object();const auto version=canonicalPublicFormat(o.value("format").toString());const bool legacy=version=="keefi-chart-studies/1";
    const bool multiPane=version=="keefi-chart-studies/5";
    const bool fourLines=multiPane||version=="keefi-chart-studies/4";
    const bool current=fourLines||version=="keefi-chart-studies/3";
    // Version 3 introduced optional style/favorites; version 4 adds line enable
    // bits and the wider MA bounds. Every supported version retains saved bits.
    const bool hasStyle=current&&o.contains("chartStyle"),hasFavorites=current&&o.contains("favorites");
    const bool hasSettings=version=="keefi-chart-studies/5" && o.contains("settings");
    ChartStudySettings nextSettings;
    if (hasSettings && (!o.value("settings").isObject() || !decodeSettings(o.value("settings").toObject(),nextSettings))) return fail();
    const int extra=int(hasStyle)+int(hasFavorites)+int(hasSettings);
    if(hasStyle&&(!o.value("chartStyle").isString()||!isChartStyle(o.value("chartStyle").toString())))return fail();
    const auto nextStyle=hasStyle?o.value("chartStyle").toString():QStringLiteral("candles");
    QStringList nextFavorites;
    if(hasFavorites){
        if(!o.value("favorites").isArray()||o.value("favorites").toArray().size()>values_.size())return fail();
        for(const auto& value:o.value("favorites").toArray()){
            if(!value.isString()||!values_.contains(value.toString())||nextFavorites.contains(value.toString()))return fail();
            nextFavorites.append(value.toString());
        }
    }
    if((!legacy&&!current&&version!="keefi-chart-studies/2")||(legacy?o.size()!=3:multiPane?o.size()-extra!=5:(o.size()-extra!=5&&o.size()-extra!=6))||!o.value("introSeen").isBool()||!o.value("studies").isObject())return fail();
    const auto studies=o.value("studies").toObject();if(studies.size()!=(legacy?legacyIds.size():values_.size()))return fail();
    auto next=defaults();
    for(auto i=studies.begin();i!=studies.end();++i){
        if(!next.contains(i.key())||(legacy&&!legacyIds.contains(i.key()))||!i.value().isObject())return fail();
        const auto state=i.value().toObject();
        if(state.size()!=2||!state.value("enabled").isBool()||!state.value("visible").isBool())return fail();
        next[i.key()]={state.value("enabled").toBool(),state.value("visible").toBool()};
    }
    auto nextOrder=defaultOrder();
    if(!legacy){
        if(!o.value("order").isArray()||(!multiPane&&!o.value("pane").isString()))return fail();
        nextOrder.clear();QSet<QString> seenIds;
        for(const auto& value:o.value("order").toArray()){
            if(!value.isString()||!next.contains(value.toString())||seenIds.contains(value.toString()))return fail();
            seenIds.insert(value.toString());nextOrder.append(value.toString());
        }
        if(nextOrder.size()!=next.size())return fail();
        if (!multiPane) {
            const QString oldPane = o.value("pane").toString();
            if (!oldPane.isEmpty() && (!isPane(oldPane) || !next.value(oldPane).enabled || !next.value(oldPane).visible)) return fail();
            for (auto it = next.cbegin(); it != next.cend(); ++it)
                if (isPane(it.key()) && it->enabled && it->visible && it.key() != oldPane) return fail();
        }
    }
    // A saved false value can be an explicit user choice in any schema. New
    // profile defaults must never turn an existing off/hidden study back on.
    QList<ExtendedMaSpec> averages{{"ma1",20,ExtendedMaKind::Simple},{"ma2",60,ExtendedMaKind::Simple}};
    if(fourLines&&!o.contains("averages"))return fail();
    if(!legacy&&o.contains("averages")){
        if(!o.value("averages").isArray())return fail();const auto array=o.value("averages").toArray();if(array.isEmpty()||array.size()>(fourLines?ChartMovingAverageMaximumLines:3))return fail();averages.clear();
        for(const auto& value:array){if(!value.isObject())return fail();const auto item=value.toObject();const auto n=item.value("period").toDouble(-1);const auto kind=item.value("kind").toString();
            if(item.size()!=(fourLines?3:2)||n<2||n>(fourLines?ChartMovingAverageMaximumPeriod:120)||n!=int(n)||(kind!="sma"&&kind!="ema"&&kind!="wma")||(fourLines&&!item.value("enabled").isBool()))return fail();averages.append({"ma"+QString::number(averages.size()+1),int(n),kind=="sma"?ExtendedMaKind::Simple:kind=="ema"?ExtendedMaKind::Exponential:ExtendedMaKind::Weighted,fourLines?item.value("enabled").toBool():true});}
    }else if(!legacy&&o.size()-extra!=5)return fail();
    bool averageChanged=averages.size()!=averages_.size();for(int i=0;!averageChanged&&i<averages.size();++i)averageChanged=averages[i].period!=averages_[i].period||averages[i].kind!=averages_[i].kind||averages[i].enabled!=averages_[i].enabled;
    const bool seen=o.value("introSeen").toBool();
    if(settings_!=nextSettings||values_!=next||introSeen_!=seen||order_!=nextOrder||averageChanged||chartStyle_!=nextStyle||favorites_!=nextFavorites){settings_=nextSettings;favorites_=nextFavorites;values_=next;introSeen_=seen;order_=nextOrder;averages_=averages;chartStyle_=nextStyle;emit changed();}
    if(reason)reason->clear();return true;
}
}
