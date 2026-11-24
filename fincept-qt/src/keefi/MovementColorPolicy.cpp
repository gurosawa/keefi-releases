#include "keefi/MovementColorPolicy.h"
#include "keefi/PublicFormatMigration.h"
#include "keefi/KeefiTheme.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
namespace keefi {
namespace {
bool validScheme(MovementScheme scheme) { return scheme==MovementScheme::GreenRed||scheme==MovementScheme::KoreanRedBlue; }
QColor foregroundFor(MovementScheme scheme,MovementDirection direction) {
    const auto& c=themeColors();
    if(direction==MovementDirection::Up)return scheme==MovementScheme::KoreanRedBlue?c.downText:c.upText;
    if(direction==MovementDirection::Down)return scheme==MovementScheme::KoreanRedBlue?c.koreanDownText:c.downText;
    return c.text3;
}
QColor fillColor(MovementScheme scheme,MovementDirection direction) {
    const auto& c=themeColors();
    if(direction==MovementDirection::Up)return scheme==MovementScheme::KoreanRedBlue?c.down:c.up;
    if(direction==MovementDirection::Down)return scheme==MovementScheme::KoreanRedBlue?c.koreanDown:c.down;
    // Flat/unknown marks must stay visible; neutral heat backgrounds remain surface.
    return c.text3;
}
QColor heatColor(MovementScheme scheme,MovementDirection direction,int band) {
    if(band<0||band>2||(direction!=MovementDirection::Up&&direction!=MovementDirection::Down))return themeColors().surface;
    static constexpr qreal amounts[]{0.12,0.28,0.52};
    return KeefiTheme::blend(fillColor(scheme,direction),themeColors().surface,amounts[band]);
}
// The format has only string pairs. Preserve decoded key identity before Qt's
// object representation can discard duplicate or escaped duplicate spellings.
bool uniqueStringKeys(const QByteArray& raw) {
    qsizetype at=0;QSet<QString> keys;
    const auto whitespace=[&](){while(at<raw.size()&&(raw[at]==' '||raw[at]=='\t'||raw[at]=='\r'||raw[at]=='\n'))++at;};
    const auto string=[&](QString* decoded){
        if(at>=raw.size()||raw[at]!='"')return false;const qsizetype start=at++;
        while(at<raw.size()){const char c=raw[at++];if(c=='\\'){if(at>=raw.size())return false;++at;continue;}if(c=='"'){
            const auto doc=QJsonDocument::fromJson(QByteArray("[")+raw.mid(start,at-start)+']');
            if(!doc.isArray()||doc.array().size()!=1||!doc.array().first().isString())return false;*decoded=doc.array().first().toString();return true;}}
        return false;
    };
    whitespace();if(at>=raw.size()||raw[at++]!='{')return false;whitespace();
    while(at<raw.size()&&raw[at]!='}'){
        QString key,value;if(!string(&key)||keys.contains(key))return false;keys.insert(key);whitespace();
        if(at>=raw.size()||raw[at++]!=':')return false;whitespace();if(!string(&value))return false;whitespace();
        if(at>=raw.size())return false;if(raw[at]=='}')break;if(raw[at++]!=',')return false;whitespace();
        if(at>=raw.size()||raw[at]=='}')return false;
    }
    if(at>=raw.size()||raw[at++]!='}')return false;whitespace();return at==raw.size();
}
}
struct MovementColorPolicy::Private { MovementScheme scheme=MovementScheme::GreenRed; };
MovementColorPolicy::MovementColorPolicy(QObject* parent) : QObject(parent), d_(std::make_unique<Private>()) { setObjectName(ObjectName); connect(KeefiTheme::instance(),&KeefiTheme::changed,this,&MovementColorPolicy::changed); }
MovementColorPolicy::~MovementColorPolicy() = default;
MovementScheme MovementColorPolicy::scheme() const { return d_->scheme; }
void MovementColorPolicy::setScheme(MovementScheme scheme) {if(!validScheme(scheme)||d_->scheme==scheme)return;d_->scheme=scheme;emit changed();}
QColor MovementColorPolicy::foreground(MovementDirection direction) const { return foregroundFor(d_->scheme,direction); }
QColor MovementColorPolicy::fill(MovementDirection direction) const { return fillColor(d_->scheme,direction); }
QColor MovementColorPolicy::heat(MovementDirection direction,int band) const { return heatColor(d_->scheme,direction,band); }
QString MovementColorPolicy::marker(MovementDirection direction) {return direction==MovementDirection::Up?QString("▲"):direction==MovementDirection::Down?QString("▼"):QString{};}
QByteArray MovementColorPolicy::publicState() const {
    return QJsonDocument(QJsonObject{{"schema","keefi-movement-colors/1"},{"scheme",d_->scheme==MovementScheme::KoreanRedBlue?"korean-red-blue":"green-red"}}).toJson(QJsonDocument::Compact);
}
bool MovementColorPolicy::restorePublicState(const QByteArray& raw,QString* error) {
    const auto reject=[&](){if(error)*error="등락 색 설정 형식이 유효하지 않습니다";return false;};
    if(raw.isEmpty()||raw.size()>MaxStateBytes||!raw.isValidUtf8()||raw.startsWith(QByteArray::fromHex("efbbbf"))||!uniqueStringKeys(raw))return reject();
    const auto document=QJsonDocument::fromJson(raw);if(!document.isObject())return reject();const auto object=document.object();
    if(object.size()!=2||canonicalPublicFormat(object.value("schema").toString())!="keefi-movement-colors/1"||!object.value("scheme").isString())return reject();
    const auto value=object.value("scheme").toString();if(value!="green-red"&&value!="korean-red-blue")return reject();
    if(error)error->clear();setScheme(value=="korean-red-blue"?MovementScheme::KoreanRedBlue:MovementScheme::GreenRed);return true;
}
MovementColorPolicy* MovementColorPolicy::forContext(const QObject* context) {
    for(auto* owner=context;owner;owner=owner->parent()){
        if(auto* itself=qobject_cast<MovementColorPolicy*>(const_cast<QObject*>(owner)))return itself;
        const auto policies=owner->findChildren<MovementColorPolicy*>(ObjectName,Qt::FindDirectChildrenOnly);
        if(policies.size()==1)return policies.first();if(policies.size()>1)return nullptr;
    }return nullptr;
}
MovementScheme MovementColorPolicy::schemeFor(const QObject* context) {const auto* policy=forContext(context);return policy?policy->scheme():MovementScheme::GreenRed;}
QColor MovementColorPolicy::colorFor(const QObject* context,MovementDirection direction) {return foregroundFor(schemeFor(context),direction);}
QColor MovementColorPolicy::fillFor(const QObject* context,MovementDirection direction) {return fillColor(schemeFor(context),direction);}
QColor MovementColorPolicy::heatFor(const QObject* context,MovementDirection direction,int band) {return heatColor(schemeFor(context),direction,band);}
}
