#include "keefi/ChartDrawings.h"
#include "keefi/PublicFormatMigration.h"
#include "keefi/ExchangeIdentifiers.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <limits>

namespace keefi {
namespace {
struct Decimal { QString digits="0";int scale=0; };
Decimal compact(QString digits,int scale) {
    int first=0;while(first+1<digits.size()&&digits[first]=='0')++first;
    if(first)digits.remove(0,first);if(digits=="0")return {};
    while(scale>0&&digits.endsWith('0')){digits.chop(1);--scale;}return {digits,scale};
}
Decimal exact(const QString& text) {
    QString digits=text;const int point=int(digits.indexOf('.'));const int scale=point<0?0:int(digits.size())-point-1;
    if(point>=0)digits.remove(point,1);return compact(digits,scale);
}
int compare(const Decimal& a,const Decimal& b) {
    const int scale=std::max(a.scale,b.scale);
    const QString left=a.digits=="0"?QString("0"):a.digits+QString(scale-a.scale,'0');
    const QString right=b.digits=="0"?QString("0"):b.digits+QString(scale-b.scale,'0');
    if(left.size()!=right.size())return left.size()<right.size()?-1:1;
    const int value=QString::compare(left,right);return value<0?-1:value>0?1:0;
}
Decimal difference(const Decimal& greater,const Decimal& smaller) {
    const int scale=std::max(greater.scale,smaller.scale);QString left=greater.digits+QString(scale-greater.scale,'0'),right=smaller.digits+QString(scale-smaller.scale,'0');
    const int size=int(std::max(left.size(),right.size()));left=left.rightJustified(size,'0');right=right.rightJustified(size,'0');int borrow=0;
    for(int i=size-1;i>=0;--i){int value=left[i].digitValue()-right[i].digitValue()-borrow;borrow=value<0;if(borrow)value+=10;left[i]=QChar('0'+value);}return compact(left,scale);
}
long double displayValue(const Decimal& value) {
    if(value.digits=="0")return 0;const auto prefix=value.digits.left(18);const int exponent=int(value.digits.size())-value.scale-int(prefix.size());
    return static_cast<long double>(prefix.toULongLong())*std::pow(10.0L,exponent);
}
bool parseDecimal(const QString& text,bool price,Decimal* output=nullptr) {
    static const QRegularExpression grammar("\\A(?:0|[1-9][0-9]*)(?:\\.[0-9]+)?\\z");
    if(text.isEmpty()||text.size()>ChartDrawings::MaxPriceChars||!grammar.match(text).hasMatch())return false;
    const auto value=exact(text);static const Decimal maximum{QString("1")+QString(100,'0'),0},minimum{"1",100};
    if(compare(value,maximum)>0||(price&&compare(value,minimum)<0))return false;if(output)*output=value;return true;
}
bool uuid(const QString& id) {static const QRegularExpression grammar("\\A[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\\z");return grammar.match(id).hasMatch();}
bool listId(const QString& id) {static const QRegularExpression grammar("\\A[A-Za-z0-9_-]{1,64}\\z");return id!="holdings"&&grammar.match(id).hasMatch();}
bool validContext(const ChartDrawingContext& context) {
    if(!WatchlistWorkspaceList::validIdentity(context.identity))return false;const auto& identity=context.identity;
    if(identity.market=="stock")return context.currency==(identity.source=="KR"?"KRW":"USD");
    if(identity.source=="upbit"||identity.source=="bithumb")return context.currency==identity.code.section('-',0,0);
    return binanceIdentifierValid(context.currency);
}
QString key(const ChartDrawingContext& context,const QString& bucket) {return context.identity.key()+'|'+context.currency+'|'+bucket;}
bool pointValid(const ChartDrawingPoint& point) {return point.atMs>0&&parseDecimal(point.price,true);}
bool kindValid(ChartDrawingKind kind) {
    return kind==ChartDrawingKind::Trend||kind==ChartDrawingKind::Horizontal||kind==ChartDrawingKind::Measure
        ||kind==ChartDrawingKind::FibRetracement||kind==ChartDrawingKind::FibExtension||kind==ChartDrawingKind::Zone;
}
bool geometryValid(ChartDrawingKind kind,const QList<ChartDrawingPoint>& points) {
    if(!kindValid(kind))return false;
    if(kind==ChartDrawingKind::Horizontal){if(points.size()!=1)return false;}
    else if(kind==ChartDrawingKind::FibExtension){if(points.size()!=2&&points.size()!=3)return false;}
    else if(points.size()!=2)return false;
    for(const auto& point:points)if(!pointValid(point))return false;
    if(kind==ChartDrawingKind::Trend)return points[0]!=points[1];
    if(kind==ChartDrawingKind::FibRetracement||kind==ChartDrawingKind::FibExtension)
        return compare(exact(points[0].price),exact(points[1].price))!=0;
    if(kind==ChartDrawingKind::Zone)
        return points[0].atMs!=points[1].atMs&&compare(exact(points[0].price),exact(points[1].price))!=0;
    return true;
}
int exactBar(const ChartDrawingFrame& frame,qint64 at) {for(int i=0;i<frame.bars.size();++i)if(frame.bars[i].candle.startsAtMs==at)return i;return -1;}
int containingBar(const ChartDrawingFrame& frame,qint64 at) {
    const int direct=exactBar(frame,at);if(direct>=0)return direct;int result=-1;
    for(int i=0;i<frame.bars.size();++i){const auto& bar=frame.bars[i];if(at>bar.candle.startsAtMs&&at<bar.completesAtMs){if(result>=0)return -1;result=i;}}return result;
}
QString kindText(ChartDrawingKind kind) {
    switch(kind){
    case ChartDrawingKind::Trend:return "trend";
    case ChartDrawingKind::Horizontal:return "horizontal";
    case ChartDrawingKind::Measure:return "measure";
    case ChartDrawingKind::FibRetracement:return "fib-retracement";
    case ChartDrawingKind::FibExtension:return "fib-extension";
    case ChartDrawingKind::Zone:return "zone";
    }
    return "horizontal";
}
std::optional<ChartDrawingKind> parseKind(const QString& text,bool allowV2) {
    if(text=="trend")return ChartDrawingKind::Trend;
    if(text=="horizontal")return ChartDrawingKind::Horizontal;
    if(text=="measure")return ChartDrawingKind::Measure;
    if(allowV2){
        if(text=="fib-retracement")return ChartDrawingKind::FibRetracement;
        if(text=="fib-extension")return ChartDrawingKind::FibExtension;
        if(text=="zone")return ChartDrawingKind::Zone;
    }
    return {};
}
QJsonObject drawingObject(const ChartDrawing& drawing) {
    QJsonArray points;for(const auto& point:drawing.anchors)points.append(QJsonObject{{"atMs",QString::number(point.atMs)},{"price",point.price}});
    return {{"id",drawing.id},{"kind",kindText(drawing.kind)},{"anchors",points},
            {"alert",QJsonObject{{"enabled",drawing.alert.enabled},{"listId",drawing.alert.listId},{"intervalMs",drawing.alert.intervalMs}}},
            {"visible",drawing.visible},{"locked",drawing.locked},{"autoBatchId",drawing.autoBatchId}};
}
// What an alert line's revision covers: its id, kind, anchors and alert, the same fields as the
// v1 shape object. Hiding or locking a line is not an alert change and must not reset its state.
QJsonObject alertProofObject(const ChartDrawing& drawing) {
    auto object=drawingObject(drawing);object.remove("visible");object.remove("locked");object.remove("autoBatchId");return object;
}
bool exactKeys(const QJsonObject& object,const QStringList& keys) {if(object.size()!=keys.size())return false;for(const auto& key:keys)if(!object.contains(key))return false;return true;}
bool boundedJson(const QByteArray& bytes) {
    if(bytes.isEmpty()||bytes.size()>ChartDrawings::MaxJsonBytes||!bytes.isValidUtf8())return false;int depth=0;bool string=false,escaped=false;
    for(char byte:bytes){if(string){if(escaped)escaped=false;else if(byte=='\\')escaped=true;else if(byte=='"')string=false;continue;}
        if(byte=='"')string=true;else if(byte=='{'||byte=='['){if(++depth>ChartDrawings::MaxJsonDepth)return false;}else if(byte=='}'||byte==']'){if(--depth<0)return false;}}
    return depth==0&&!string;
}
bool timestamp(const QJsonValue& value,qint64* result) {
    if(!value.isString())return false;const auto text=value.toString();static const QRegularExpression grammar("\\A[1-9][0-9]{0,18}\\z");
    if(!grammar.match(text).hasMatch())return false;bool ok=false;const qint64 at=text.toLongLong(&ok);if(!ok||at<=0)return false;*result=at;return true;
}
bool alertValid(const ChartDrawingAlert& alert,ChartDrawingKind kind,const QString& bucket) {
    if(!alert.enabled)return alert.listId.isEmpty()&&alert.intervalMs==0;
    return kind==ChartDrawingKind::Horizontal&&listId(alert.listId)&&ChartDrawings::bucketForInterval(alert.intervalMs)==bucket;
}
QByteArray linesKey(const QList<ChartHorizontalLineRule>& lines) {QByteArray result;for(const auto& line:lines){result+=line.drawingId.toUtf8();result+='|';result+=line.revision.toUtf8();result+='\n';}return result;}
} // namespace
struct ChartDrawings::Private {
    struct State {ChartDrawingContext context;QString bucket;bool visible=true,locked=false;QList<ChartDrawing> drawings;};
    ChartDrawings* q=nullptr;QMap<QString,State> publicStates,transientStates;ChartDrawingContext selected;
    qint64 interval=0;quint64 generation=1;bool active=false,emptyVisible=true,emptyLocked=false,magnet=false;
    QSet<QString> verified;QMap<QString,QSet<QString>> lists;
    void bump(){if(++generation==0)++generation;}
    QString selectedKey()const{return key(selected,ChartDrawings::bucketForInterval(interval));}
    QMap<QString,State>& selectedMap(){return selected.publicPersistenceAllowed?publicStates:transientStates;}
    const QMap<QString,State>& selectedMap()const{return selected.publicPersistenceAllowed?publicStates:transientStates;}
    State* current(){if(!active)return nullptr;auto found=selectedMap().find(selectedKey());return found==selectedMap().end()?nullptr:&found.value();}
    const State* current()const{if(!active)return nullptr;auto found=selectedMap().constFind(selectedKey());return found==selectedMap().cend()?nullptr:&found.value();}
    bool editable()const{return active&&(selected.identity.market!="stock"||verified.contains(selected.identity.key()));}
    int total()const{int result=0;for(const auto& state:publicStates)result+=int(state.drawings.size());for(const auto& state:transientStates)result+=int(state.drawings.size());return result;}
    bool frameMatches(const ChartDrawingFrame& frame)const{return active&&frame.contextGeneration==generation&&frame.identity==selected.identity&&frame.currency==selected.currency&&frame.intervalMs==interval&&ChartDrawings::validFrame(frame);}
    bool member(const State& state,const QString& list)const{return lists.value(list).contains(state.context.identity.key());}
    bool idUsed(const QString& id)const{for(const auto* map:{&publicStates,&transientStates})for(const auto& state:*map)for(const auto& drawing:state.drawings)if(drawing.id==id)return true;return false;}
    bool withinPublicByteBudget()const{return q->publicState().size()<=ChartDrawings::MaxJsonBytes;}
    // Per-context shape history, keyed like the stored state and split by public/transient map.
    struct History {QList<QList<ChartDrawing>> undo,redo;};
    QMap<QString,History> history;
    QString historyKey()const{return (selected.publicPersistenceAllowed?QStringLiteral("p|"):QStringLiteral("t|"))+selectedKey();}
    void record(const QList<ChartDrawing>& before){
        auto& entry=history[historyKey()];entry.undo.append(before);entry.redo.clear();
        while(entry.undo.size()>ChartDrawings::MaxUndoSteps)entry.undo.removeFirst();
        // Keep history for a bounded number of charts; the current chart's history always stays.
        const auto keep=historyKey();
        while(history.size()>32){auto it=history.begin();if(it.key()==keep)++it;if(it==history.end())break;history.erase(it);}
    }
    // Replace the current context's shapes with an earlier list. Alerts, visibility and locks of
    // shapes that still exist keep their current setting; any step removing or moving a currently
    // locked shape is rejected without touching history or limits.
    bool restoreShapes(QList<ChartDrawing> target){
        auto* state=current();
        if(state){
            for(const auto& now:state->drawings){
                if(!now.locked)continue;
                const auto found=std::find_if(target.cbegin(),target.cend(),[&](const ChartDrawing& shape){return shape.id==now.id;});
                if(found==target.cend()||found->anchors!=now.anchors||found->kind!=now.kind)return false;
            }
            for(auto& shape:target)for(const auto& now:state->drawings)if(now.id==shape.id){
                shape.alert=now.alert;shape.visible=now.visible;shape.locked=now.locked;break;
            }
        }
        if(target.size()>ChartDrawings::MaxPerContext)return false;
        const int others=total()-int(state?state->drawings.size():0);if(others+int(target.size())>ChartDrawings::MaxTotal)return false;
        auto& states=selectedMap();const auto key=selectedKey();const std::optional<State> previous=state?std::optional<State>(*state):std::nullopt;
        const bool oldVisible=emptyVisible,oldLocked=emptyLocked;
        if(target.isEmpty()){if(state){emptyVisible=state->visible;emptyLocked=state->locked;states.remove(key);}}
        else{
            if(!state){if(states.size()>=ChartDrawings::MaxContexts)return false;State next;next.context=selected;next.bucket=ChartDrawings::bucketForInterval(interval);next.visible=emptyVisible;next.locked=emptyLocked;states.insert(key,next);}
            current()->drawings=target;
        }
        if(!withinPublicByteBudget()){if(previous)states.insert(key,*previous);else states.remove(key);emptyVisible=oldVisible;emptyLocked=oldLocked;return false;}
        return true;
    }
    bool setDrawingFlag(const QString& id,bool ChartDrawing::*field,bool value){
        auto* state=current();if(!state||!editable()||q->locked())return false;
        for(auto& drawing:state->drawings)if(drawing.id==id){
            if(drawing.*field==value)return true;
            const auto before=q->publicState(),lines=linesKey(q->horizontalLines());
            const bool previous=drawing.*field;drawing.*field=value;
            if(!withinPublicByteBudget()){drawing.*field=previous;return false;}
            notify(before,lines);return true;
        }
        return false;
    }
    void notify(const QByteArray& before,const QByteArray& beforeLines){const bool publicChanged=q->publicState()!=before,lineChanged=linesKey(q->horizontalLines())!=beforeLines;emit q->changed();if(publicChanged)emit q->publicStateChanged();if(lineChanged)emit q->horizontalLinesChanged();}
};
ChartDrawings::ChartDrawings(QObject* parent):QObject(parent),d_(std::make_unique<Private>()){d_->q=this;}
ChartDrawings::~ChartDrawings()=default;
QString ChartDrawings::bucketForInterval(qint64 interval){
    if(interval==60000||interval==300000||interval==900000||interval==3600000||interval==14400000)return "intraday";
    return interval==86400000?QString("daily"):QString{};
}
bool ChartDrawings::setContext(const ChartDrawingContext& context,qint64 interval){
    if(!validContext(context)||bucketForInterval(interval).isEmpty())return false;
    if(d_->active&&d_->selected==context&&d_->interval==interval)return true;
    d_->selected=context;d_->interval=interval;d_->active=true;d_->emptyVisible=true;d_->emptyLocked=false;d_->bump();emit changed();return true;
}
void ChartDrawings::clearContext(){if(!d_->active)return;d_->selected={};d_->interval=0;d_->active=false;d_->emptyVisible=true;d_->emptyLocked=false;d_->bump();emit changed();}
ChartDrawingContext ChartDrawings::context()const{return d_->selected;}
qint64 ChartDrawings::intervalMs()const{return d_->interval;}
quint64 ChartDrawings::contextGeneration()const{return d_->generation;}
QList<ChartDrawing> ChartDrawings::drawings()const{const auto* current=d_->current();return current?current->drawings:QList<ChartDrawing>{};}
void ChartDrawings::setVerifiedInstruments(const QList<WatchlistIdentity>& values){
    QSet<QString> accepted;if(values.size()<=10000)for(const auto& identity:values)if(WatchlistWorkspaceList::validIdentity(identity))accepted.insert(identity.key());
    if(accepted==d_->verified)return;const auto before=publicState(),lines=linesKey(horizontalLines());d_->verified=accepted;d_->notify(before,lines);
}
void ChartDrawings::setPublicLists(const QMap<QString,QList<WatchlistIdentity>>& values){
    QMap<QString,QSet<QString>> accepted;int total=0;bool valid=values.size()<=32;
    for(auto it=values.cbegin();valid&&it!=values.cend();++it){if(!listId(it.key())||it.value().size()>2000){valid=false;break;}QSet<QString> members;
        for(const auto& identity:it.value()){if(!WatchlistWorkspaceList::validIdentity(identity)){valid=false;break;}members.insert(identity.key());}total+=int(members.size());if(total>10000)valid=false;accepted.insert(it.key(),members);}
    if(!valid)accepted.clear();if(accepted==d_->lists)return;const auto before=publicState(),lines=linesKey(horizontalLines());d_->lists=accepted;d_->notify(before,lines);
}
QString ChartDrawings::create(ChartDrawingKind kind,const QList<ChartDrawingPoint>& points,const ChartDrawingFrame& frame,QString* error){
    const auto reject=[&](const QString& reason){if(error)*error=reason;return QString{};};
    if(!d_->editable()||locked())return reject("현재 문맥이 잠겼거나 공개 종목 검증이 없습니다");
    if(!d_->frameMatches(frame)||!geometryValid(kind,points))return reject("도형·가격·실제 완료 봉 문맥이 유효하지 않습니다");
    for(const auto& point:points)if(exactBar(frame,point.atMs)<0)return reject("새 좌표는 실제 완료 봉 시작점이어야 합니다");
    const auto* old=d_->current();auto& states=d_->selectedMap();
    if((old&&old->drawings.size()>=MaxPerContext)||(!old&&states.size()>=MaxContexts)||d_->total()>=MaxTotal)return reject("그리기 저장 한도를 넘었습니다");
    const auto before=publicState(),lines=linesKey(horizontalLines());QString id;do{id=QUuid::createUuid().toString(QUuid::WithoutBraces);}while(d_->idUsed(id));
    if(!old){Private::State state;state.context=d_->selected;state.bucket=bucketForInterval(d_->interval);state.visible=d_->emptyVisible;state.locked=d_->emptyLocked;states.insert(d_->selectedKey(),state);}
    const auto previous=old?old->drawings:QList<ChartDrawing>{};
    d_->current()->drawings.append({id,kind,points,{},true,false,{}});
    if(!d_->withinPublicByteBudget()){d_->current()->drawings.removeLast();if(!old)states.remove(d_->selectedKey());return reject("그리기 공개 설정의 저장 크기 한도를 넘었습니다");}
    d_->record(previous);if(error)error->clear();d_->notify(before,lines);return id;
}
bool ChartDrawings::applyBatch(const QList<ChartDrawing>& shapes,const ChartDrawingFrame& frame,QString* error){
    const auto reject=[&](const QString& reason){if(error)*error=reason;return false;};
    if(!d_->editable()||locked())return reject("현재 문맥이 잠겼거나 공개 종목 검증이 없습니다");
    if(!d_->frameMatches(frame))return reject("도형·가격·실제 완료 봉 문맥이 유효하지 않습니다");
    for(const auto& shape:shapes){
        if(!geometryValid(shape.kind,shape.anchors))return reject("도형·가격·실제 완료 봉 문맥이 유효하지 않습니다");
        for(const auto& point:shape.anchors)if(exactBar(frame,point.atMs)<0)return reject("새 좌표는 실제 완료 봉 시작점이어야 합니다");
    }
    const auto* old=d_->current();auto& states=d_->selectedMap();
    const auto previous=old?old->drawings:QList<ChartDrawing>{};
    QList<ChartDrawing> next;
    for(const auto& existing:previous)if(existing.autoBatchId.isEmpty()||existing.locked)next.append(existing);
    QSet<QString> batchIds;
    if(!shapes.isEmpty()){
        const QString batchId=QUuid::createUuid().toString(QUuid::WithoutBraces);
        for(const auto& shape:shapes){
            QString id;
            do{id=QUuid::createUuid().toString(QUuid::WithoutBraces);}while(id==batchId||d_->idUsed(id)||batchIds.contains(id));
            batchIds.insert(id);
            next.append({id,shape.kind,shape.anchors,{},true,false,batchId});
        }
    }
    if(next==previous){if(error)error->clear();return true;}
    if(next.size()>MaxPerContext||(!old&&!next.isEmpty()&&states.size()>=MaxContexts)||(d_->total()-int(previous.size())+int(next.size()))>MaxTotal)
        return reject("그리기 저장 한도를 넘었습니다");
    const auto before=publicState(),lines=linesKey(horizontalLines());
    const auto selectedKey=d_->selectedKey();const std::optional<Private::State> previousState=old?std::optional<Private::State>(*old):std::nullopt;
    const bool oldVisible=d_->emptyVisible,oldLocked=d_->emptyLocked;
    if(next.isEmpty()){if(old){d_->emptyVisible=old->visible;d_->emptyLocked=old->locked;states.remove(selectedKey);}}
    else{
        if(!old){Private::State state;state.context=d_->selected;state.bucket=bucketForInterval(d_->interval);state.visible=d_->emptyVisible;state.locked=d_->emptyLocked;states.insert(selectedKey,state);}
        d_->current()->drawings=next;
    }
    if(!d_->withinPublicByteBudget()){if(previousState)states.insert(selectedKey,*previousState);else states.remove(selectedKey);d_->emptyVisible=oldVisible;d_->emptyLocked=oldLocked;return reject("그리기 공개 설정의 저장 크기 한도를 넘었습니다");}
    d_->record(previous);if(error)error->clear();d_->notify(before,lines);return true;
}
bool ChartDrawings::moveAnchor(const QString& id,int index,const ChartDrawingPoint& point,const ChartDrawingFrame& frame,QString* error){
    const auto reject=[&](const QString& reason){if(error)*error=reason;return false;};auto* state=d_->current();
    if(!state||!d_->editable()||locked()||!d_->frameMatches(frame)||!pointValid(point)||exactBar(frame,point.atMs)<0)return reject("현재 도형 편집 문맥이 유효하지 않습니다");
    for(auto& drawing:state->drawings)if(drawing.id==id){
        if(drawing.locked)return reject("잠긴 도형이에요");
        if(index<0||index>=drawing.anchors.size())return reject("편집할 좌표가 없습니다");auto points=drawing.anchors;points[index]=point;
        if(!geometryValid(drawing.kind,points))return reject("길이가 없는 추세선은 저장할 수 없습니다");if(points==drawing.anchors){if(error)error->clear();return true;}
        const auto before=publicState(),lines=linesKey(horizontalLines());auto previous=state->drawings;previous.detach(); // `drawing` refers into the list; keep an unshared copy.
        const auto oldAnchors=drawing.anchors;drawing.anchors=points;
        if(!d_->withinPublicByteBudget()){drawing.anchors=oldAnchors;return reject("그리기 공개 설정의 저장 크기 한도를 넘었습니다");}
        d_->record(previous);if(error)error->clear();d_->notify(before,lines);return true;}return reject("도형을 찾을 수 없습니다");
}
bool ChartDrawings::moveDrawing(const QString& id,const QList<ChartDrawingPoint>& points,const ChartDrawingFrame& frame,QString* error){
    const auto reject=[&](const QString& reason){if(error)*error=reason;return false;};auto* state=d_->current();
    if(!state||!d_->editable()||locked()||!d_->frameMatches(frame))return reject("현재 도형 편집 문맥이 유효하지 않습니다");
    for(auto& drawing:state->drawings)if(drawing.id==id){
        if(drawing.locked)return reject("잠긴 도형이에요");
        if(points.size()!=drawing.anchors.size())return reject("편집할 좌표가 없습니다");
        // A price-only move keeps an already stored anchor time; a new time must be an actual closed bar.
        for(int i=0;i<points.size();++i)if(!pointValid(points[i])||(points[i].atMs!=drawing.anchors[i].atMs&&exactBar(frame,points[i].atMs)<0))
            return reject("실제로 완료된 봉 위로만 옮길 수 있어요");
        if(!geometryValid(drawing.kind,points))return reject("길이가 없는 추세선은 저장할 수 없습니다");if(points==drawing.anchors){if(error)error->clear();return true;}
        const auto before=publicState(),lines=linesKey(horizontalLines());auto previous=state->drawings;previous.detach(); // `drawing` refers into the list; keep an unshared copy.
        const auto oldAnchors=drawing.anchors;drawing.anchors=points;
        if(!d_->withinPublicByteBudget()){drawing.anchors=oldAnchors;return reject("그리기 공개 설정의 저장 크기 한도를 넘었습니다");}
        d_->record(previous);if(error)error->clear();d_->notify(before,lines);return true;}return reject("도형을 찾을 수 없습니다");
}
bool ChartDrawings::remove(const QString& id,QString* error){
    const auto reject=[&](const QString& reason){if(error)*error=reason;return false;};
    auto* state=d_->current();if(!state||!d_->editable()||locked())return reject("현재 도형 편집 문맥이 유효하지 않습니다");
    for(int index=0;index<state->drawings.size();++index)if(state->drawings[index].id==id){
        if(state->drawings[index].locked)return reject("잠긴 도형이에요");
        const auto before=publicState(),lines=linesKey(horizontalLines());d_->record(state->drawings);state->drawings.removeAt(index);
        if(state->drawings.isEmpty()){d_->emptyVisible=state->visible;d_->emptyLocked=state->locked;d_->selectedMap().remove(d_->selectedKey());}
        if(error)error->clear();d_->notify(before,lines);return true;}return reject("도형을 찾을 수 없습니다");
}
bool ChartDrawings::clear(){
    auto* state=d_->current();if(!state||!d_->editable()||locked())return false;
    QList<ChartDrawing> kept;for(const auto& drawing:state->drawings)if(drawing.locked)kept.append(drawing);
    if(kept.size()==state->drawings.size())return false;
    const auto before=publicState(),lines=linesKey(horizontalLines());d_->record(state->drawings);
    if(kept.isEmpty()){d_->emptyVisible=state->visible;d_->emptyLocked=state->locked;d_->selectedMap().remove(d_->selectedKey());}
    else state->drawings=kept;
    d_->notify(before,lines);return true;
}
bool ChartDrawings::setDrawingVisible(const QString& id,bool value){return d_->setDrawingFlag(id,&ChartDrawing::visible,value);}
bool ChartDrawings::setDrawingLocked(const QString& id,bool value){return d_->setDrawingFlag(id,&ChartDrawing::locked,value);}
bool ChartDrawings::canUndo()const{return d_->editable()&&!locked()&&!d_->history.value(d_->historyKey()).undo.isEmpty();}
bool ChartDrawings::canRedo()const{return d_->editable()&&!locked()&&!d_->history.value(d_->historyKey()).redo.isEmpty();}
bool ChartDrawings::undo(){
    if(!canUndo())return false;auto& entry=d_->history[d_->historyKey()];
    const auto before=publicState(),lines=linesKey(horizontalLines());const auto now=drawings();
    if(!d_->restoreShapes(entry.undo.last()))return false;
    entry.undo.removeLast();entry.redo.append(now);d_->notify(before,lines);return true;
}
bool ChartDrawings::redo(){
    if(!canRedo())return false;auto& entry=d_->history[d_->historyKey()];
    const auto before=publicState(),lines=linesKey(horizontalLines());const auto now=drawings();
    if(!d_->restoreShapes(entry.redo.last()))return false;
    entry.redo.removeLast();entry.undo.append(now);d_->notify(before,lines);return true;
}
bool ChartDrawings::visible()const{const auto* state=d_->current();return state?state->visible:d_->emptyVisible;}
void ChartDrawings::setVisible(bool value){if(!d_->active||visible()==value)return;const bool previous=visible();const auto before=publicState(),lines=linesKey(horizontalLines());auto* state=d_->current();if(state)state->visible=value;else d_->emptyVisible=value;
    if(!d_->withinPublicByteBudget()){if(state)state->visible=previous;else d_->emptyVisible=previous;return;}d_->notify(before,lines);}
bool ChartDrawings::locked()const{const auto* state=d_->current();return state?state->locked:d_->emptyLocked;}
void ChartDrawings::setLocked(bool value){if(!d_->active||locked()==value)return;const bool previous=locked();const auto before=publicState(),lines=linesKey(horizontalLines());auto* state=d_->current();if(state)state->locked=value;else d_->emptyLocked=value;
    if(!d_->withinPublicByteBudget()){if(state)state->locked=previous;else d_->emptyLocked=previous;return;}d_->notify(before,lines);}
bool ChartDrawings::magnetEnabled()const{return d_->magnet;}
void ChartDrawings::setMagnetEnabled(bool value){if(d_->magnet==value)return;const bool previous=d_->magnet;const auto before=publicState(),lines=linesKey(horizontalLines());d_->magnet=value;
    if(!d_->withinPublicByteBudget()){d_->magnet=previous;return;}d_->notify(before,lines);}
bool ChartDrawings::setAlert(const QString& id,const ChartDrawingAlert& requested,QString* error){
    const auto reject=[&](const QString& reason){if(error)*error=reason;return false;};auto* state=d_->current();
    if(!state||!d_->selected.publicPersistenceAllowed||!d_->editable()||locked())return reject("잠기지 않은 공개 사용자 문맥에만 수평선 알림을 저장할 수 있습니다");const ChartDrawingAlert alert=requested.enabled?requested:ChartDrawingAlert{};
    for(auto& drawing:state->drawings)if(drawing.id==id){if(!alertValid(alert,drawing.kind,state->bucket)||(alert.enabled&&!d_->member(*state,alert.listId)))return reject("공개 리스트 소속·시간봉·수평선 알림을 확인해 주세요");
        if(drawing.alert==alert){if(error)error->clear();return true;}const auto before=publicState(),lines=linesKey(horizontalLines());const auto oldAlert=drawing.alert;drawing.alert=alert;
        if(!d_->withinPublicByteBudget()){drawing.alert=oldAlert;return reject("그리기 공개 설정의 저장 크기 한도를 넘었습니다");}
        if(error)error->clear();d_->notify(before,lines);return true;}return reject("수평선을 찾을 수 없습니다");
}
QList<ChartHorizontalLineRule> ChartDrawings::horizontalLines()const{
    QList<ChartHorizontalLineRule> result;for(const auto& state:d_->publicStates){if(state.context.identity.market=="stock"&&!d_->verified.contains(state.context.identity.key()))continue;
        for(const auto& drawing:state.drawings){if(drawing.kind!=ChartDrawingKind::Horizontal||!drawing.alert.enabled||!d_->member(state,drawing.alert.listId))continue;
            QJsonObject proof{{"identity",state.context.identity.key()},{"currency",state.context.currency},{"bucket",state.bucket},{"drawing",alertProofObject(drawing)}};
            const auto revision=QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(proof).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());
            result.append({drawing.id,drawing.alert.listId,revision,state.context.identity,state.context.currency,drawing.anchors.first().price,drawing.alert.intervalMs,true});}}return result;
}
void ChartDrawings::clearPrivate(){
    for(auto it=d_->history.begin();it!=d_->history.end();)if(it.key().startsWith(QStringLiteral("t|")))it=d_->history.erase(it);else ++it;
    const bool clearSelection=d_->active&&!d_->selected.publicPersistenceAllowed;if(d_->transientStates.isEmpty()&&!clearSelection)return;d_->transientStates.clear();if(clearSelection){d_->selected={};d_->interval=0;d_->active=false;d_->emptyVisible=true;d_->emptyLocked=false;}d_->bump();emit changed();}
QByteArray ChartDrawings::publicState()const{
    QJsonArray contexts;for(const auto& state:d_->publicStates){if(state.drawings.isEmpty())continue;QJsonArray drawings;for(const auto& drawing:state.drawings)drawings.append(drawingObject(drawing));
        contexts.append(QJsonObject{{"identity",QJsonObject{{"market",state.context.identity.market},{"source",state.context.identity.source},{"code",state.context.identity.code}}},
            {"currency",state.context.currency},{"bucket",state.bucket},{"visible",state.visible},{"locked",state.locked},{"drawings",drawings}});}
    return QJsonDocument(QJsonObject{{"schema","keefi-chart-drawings/2"},{"contexts",contexts},{"magnet",d_->magnet}}).toJson(QJsonDocument::Compact);
}
bool ChartDrawings::restorePublicState(const QByteArray& bytes,QString* error){
    const auto reject=[&](){if(error)*error="그리기 공개 설정 형식·범위가 유효하지 않습니다";return false;};
    if(!boundedJson(bytes))return reject();const auto document=QJsonDocument::fromJson(bytes);if(!document.isObject())return reject();const auto root=document.object();
    const auto schema=canonicalPublicFormat(root["schema"].toString());
    if(!exactKeys(root,{"schema","contexts","magnet"})||(schema!="keefi-chart-drawings/1"&&schema!="keefi-chart-drawings/2")
        ||!root["contexts"].isArray()||!root["magnet"].isBool()||root["contexts"].toArray().size()>MaxContexts)return reject();
    const bool v2=(schema=="keefi-chart-drawings/2");
    const QStringList expectedDrawingKeys=v2
        ?QStringList{"id","kind","anchors","alert","visible","locked","autoBatchId"}
        :QStringList{"id","kind","anchors","alert"};
    QMap<QString,Private::State> parsed;QSet<QString> ids;int total=0,transientTotal=0;
    for(const auto& state:d_->transientStates)for(const auto& drawing:state.drawings){ids.insert(drawing.id);++transientTotal;}
    for(const auto& raw:root["contexts"].toArray()){
        if(!raw.isObject())return reject();const auto object=raw.toObject();
        if(!exactKeys(object,{"identity","currency","bucket","visible","locked","drawings"})||!object["identity"].isObject()||!object["currency"].isString()||!object["bucket"].isString()
            ||!object["visible"].isBool()||!object["locked"].isBool()||!object["drawings"].isArray())return reject();
        const auto identity=object["identity"].toObject();if(!exactKeys(identity,{"market","source","code"})||!identity["market"].isString()||!identity["source"].isString()||!identity["code"].isString())return reject();
        Private::State state;state.context={{identity["market"].toString(),identity["source"].toString(),identity["code"].toString()},object["currency"].toString(),true};
        state.bucket=object["bucket"].toString();state.visible=object["visible"].toBool();state.locked=object["locked"].toBool();
        if(!validContext(state.context)||(state.bucket!="intraday"&&state.bucket!="daily")||parsed.contains(key(state.context,state.bucket)))return reject();
        const auto drawings=object["drawings"].toArray();if(drawings.isEmpty()||drawings.size()>MaxPerContext)return reject();
        for(const auto& drawingRaw:drawings){if(!drawingRaw.isObject()||++total+transientTotal>MaxTotal)return reject();const auto object=drawingRaw.toObject();
            if(!exactKeys(object,expectedDrawingKeys)||!object["id"].isString()||!object["kind"].isString()||!object["anchors"].isArray()||!object["alert"].isObject())return reject();
            if(v2&&(!object["visible"].isBool()||!object["locked"].isBool()||!object["autoBatchId"].isString()))return reject();
            ChartDrawing drawing;drawing.id=object["id"].toString();
            const auto parsedKind=parseKind(object["kind"].toString(),v2);
            if(!uuid(drawing.id)||ids.contains(drawing.id)||!parsedKind)return reject();ids.insert(drawing.id);
            drawing.kind=*parsedKind;
            if(v2){
                drawing.visible=object["visible"].toBool();drawing.locked=object["locked"].toBool();drawing.autoBatchId=object["autoBatchId"].toString();
                if(!drawing.autoBatchId.isEmpty()&&!uuid(drawing.autoBatchId))return reject();
            }
            const auto anchors=object["anchors"].toArray();
            const bool countValid=drawing.kind==ChartDrawingKind::Horizontal?anchors.size()==1
                :drawing.kind==ChartDrawingKind::FibExtension?(anchors.size()==2||anchors.size()==3):anchors.size()==2;
            if(!countValid)return reject();
            for(const auto& pointRaw:anchors){if(!pointRaw.isObject())return reject();const auto point=pointRaw.toObject();ChartDrawingPoint anchor;
                if(!exactKeys(point,{"atMs","price"})||!point["price"].isString()||!timestamp(point["atMs"],&anchor.atMs))return reject();anchor.price=point["price"].toString();drawing.anchors.append(anchor);}
            if(!geometryValid(drawing.kind,drawing.anchors))return reject();const auto alert=object["alert"].toObject();
            if(!exactKeys(alert,{"enabled","listId","intervalMs"})||!alert["enabled"].isBool()||!alert["listId"].isString()||!alert["intervalMs"].isDouble())return reject();
            const double interval=alert["intervalMs"].toDouble();if(!std::isfinite(interval)||interval<0||interval>86400000||std::floor(interval)!=interval)return reject();
            drawing.alert={alert["enabled"].toBool(),alert["listId"].toString(),qint64(interval)};
            if(!alertValid(drawing.alert,drawing.kind,state.bucket))return reject();state.drawings.append(drawing);
        }
        parsed.insert(key(state.context,state.bucket),state);
    }
    const auto before=publicState(),lines=linesKey(horizontalLines());const bool magnet=root["magnet"].toBool();
    // A valid formatting-only import does not dirty settings or cancel gestures.
    auto old=std::move(d_->publicStates);const bool oldMagnet=d_->magnet;d_->publicStates=parsed;d_->magnet=magnet;
    if(!d_->withinPublicByteBudget()){d_->publicStates=std::move(old);d_->magnet=oldMagnet;return reject();}
    if(publicState()==before){d_->publicStates=std::move(old);d_->magnet=oldMagnet;if(error)error->clear();return true;}
    for(auto it=d_->history.begin();it!=d_->history.end();)if(it.key().startsWith(QStringLiteral("p|")))it=d_->history.erase(it);else ++it;
    d_->bump();if(error)error->clear();d_->notify(before,lines);return true;
}
bool ChartDrawings::validFrame(const ChartDrawingFrame& frame){
    if(!validContext({frame.identity,frame.currency,false})||bucketForInterval(frame.intervalMs).isEmpty()
        ||frame.asOfMs<=0||frame.bars.isEmpty()||frame.bars.size()>MaxFrameBars)return false;
    qint64 previous=0;for(const auto& bar:frame.bars){const auto& candle=bar.candle;Decimal open,high,low,close;
        if(!bar.sourceCompleted||candle.startsAtMs<=previous||bar.completesAtMs<=candle.startsAtMs||bar.completesAtMs>frame.asOfMs
            ||candle.lastStoredTickMs<0||candle.lastStoredTickMs>frame.asOfMs||!parseDecimal(candle.open,true,&open)||!parseDecimal(candle.high,true,&high)
            ||!parseDecimal(candle.low,true,&low)||!parseDecimal(candle.close,true,&close)||!parseDecimal(candle.volume,false)
            ||compare(low,high)>0||compare(low,open)>0||compare(low,close)>0||compare(high,open)<0||compare(high,close)<0)return false;
        if(frame.identity.market=="coin"&&(candle.startsAtMs>std::numeric_limits<qint64>::max()-frame.intervalMs||bar.completesAtMs!=candle.startsAtMs+frame.intervalMs))return false;
        previous=candle.startsAtMs;}return true;
}
QList<ChartFibLevel> ChartDrawings::fibLevels(const ChartDrawing& drawing){
    if(!geometryValid(drawing.kind,drawing.anchors))return {};
    if(drawing.kind!=ChartDrawingKind::FibRetracement&&drawing.kind!=ChartDrawingKind::FibExtension)return {};
    const long double a=displayValue(exact(drawing.anchors[0].price));
    const long double b=displayValue(exact(drawing.anchors[1].price));
    const long double span=b-a;
    QList<ChartFibLevel> levels;
    if(drawing.kind==ChartDrawingKind::FibRetracement){
        static constexpr double ratios[]={0.0,0.236,0.382,0.5,0.618,0.786,1.0};
        for(const double ratio:ratios){
            const double price=static_cast<double>(b-static_cast<long double>(ratio)*span);
            if(std::isfinite(price)&&price>0.0)levels.append({ratio,price});
        }
    }else{
        static constexpr double ratios[]={1.272,1.414,1.618,2.0,2.618};
        const long double base=drawing.anchors.size()==3?displayValue(exact(drawing.anchors[2].price)):a;
        for(const double ratio:ratios){
            const double price=static_cast<double>(base+static_cast<long double>(ratio)*span);
            if(std::isfinite(price)&&price>0.0)levels.append({ratio,price});
        }
    }
    return levels;
}
std::optional<MarketCandle> ChartDrawings::readOhlc(const ChartDrawingFrame& frame,qint64 at){if(!validFrame(frame))return {};const int index=exactBar(frame,at);return index<0?std::optional<MarketCandle>{}:std::optional<MarketCandle>(frame.bars[index].candle);}
ChartDrawingSnap ChartDrawings::snapOhlc(const ChartDrawingFrame& frame,const ChartDrawingPoint& point,double pixelsPerUnit,double radius){
    ChartDrawingSnap result;result.reason="관측된 완료 봉·가격·자석 반경을 확인해 주세요";
    if(!pointValid(point)||!std::isfinite(pixelsPerUnit)||pixelsPerUnit<=0||!std::isfinite(radius)||radius<0)return result;
    const auto candle=readOhlc(frame,point.atMs);if(!candle)return result;const auto target=exact(point.price);Decimal nearest;QString chosen,field;bool found=false;
    const QList<QPair<QString,QString>> candidates{{"close",candle->close},{"open",candle->open},{"high",candle->high},{"low",candle->low}};
    for(const auto& candidate:candidates){const auto price=exact(candidate.second);const auto delta=compare(price,target)>=0?difference(price,target):difference(target,price);
        const long double distance=displayValue(delta)*static_cast<long double>(pixelsPerUnit);
        if(distance<=static_cast<long double>(radius)&&(!found||compare(delta,nearest)<0)){found=true;nearest=delta;chosen=candidate.second;field=candidate.first;}}
    result.available=true;result.reason.clear();result.point=point;result.snapped=found;if(found){result.point.price=chosen;result.field=field;}return result;
}
ChartDrawingMeasurement ChartDrawings::measure(const QList<ChartDrawingPoint>& points,const ChartDrawingFrame& frame){
    ChartDrawingMeasurement result;if(points.size()!=2||!pointValid(points[0])||!pointValid(points[1])){result.reason="측정할 실제 시각과 양수 가격 2개가 필요합니다";return result;}
    const auto first=exact(points[0].price),second=exact(points[1].price);const int direction=compare(second,first);const auto delta=direction>=0?difference(second,first):difference(first,second);
    long double change=100*displayValue(delta)/displayValue(first);if(direction<0)change=-change;const double value=static_cast<double>(change);
    if(!std::isfinite(value)||(direction!=0&&value==0)){result.reason="가격 변화를 유한한 표시 값으로 나타낼 수 없습니다";return result;}
    result.available=true;result.returnPercent=value;
    if(!validFrame(frame)){result.reason="봉 간격을 계산할 완료 관측 범위가 없습니다";return result;}
    const int a=containingBar(frame,points[0].atMs),b=containingBar(frame,points[1].atMs);
    if(a<0||b<0){result.reason="측정 좌표가 실제 관측 범위 밖이거나 모호합니다";return result;}
    const int begin=std::min(a,b),end=std::max(a,b);
    for(int index=begin+1;index<=end;++index)if(!frame.bars[index].connectsPrevious||(frame.identity.market=="coin"&&frame.bars[index].candle.startsAtMs-frame.bars[index-1].candle.startsAtMs!=frame.intervalMs)){
        result.reason="장중 누락 구간의 봉 수를 추정하지 않습니다";return result;}
    result.barIntervals=end-begin;return result;
}
} // namespace keefi
