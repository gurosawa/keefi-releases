#include "ChartInstrumentPanelTestSupport.h"
#include "keefi/KeefiTheme.h"
#include "keefi/MovementColorPolicy.h"
#include "keefi/MarketMap.h"
#include "keefi/Spotlight.h"
#include "keefi/ExploreWatchlist.h"
#include "keefi/KeefiVisuals.h"
#include "keefi/WatchlistWorkspaceList.h"
#include "keefi/ChartWorkspace.h"
#include "keefi/ChartStudyPreferences.h"
#include <QComboBox>
#include <QDateTime>
#include <QImage>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardItemModel>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>
using namespace keefi;
namespace {
int pixels(const QImage& image,const QColor& color){int count=0;for(int y=0;y<image.height();++y)for(int x=0;x<image.width();++x)count+=image.pixelColor(x,y)==color;return count;}
QColor tileInk(MarketMapWidget& map,const QString& id){for(const auto& tile:map.tileRects())if(tile.first=="coin:"+id){const auto image=map.grab().toImage();const auto at=QPointF(tile.second.center().x(),tile.second.top()+12)*image.devicePixelRatio();return image.pixelColor(at.toPoint());}return {};}
}
class MovementPresentationsTest final:public QObject {
    Q_OBJECT
    QTemporaryDir settings_;
  private slots:
    void initTestCase();
    void init(){KeefiTheme::instance()->setSettings(nullptr);KeefiTheme::instance()->setMode(ThemeMode::Dark);}
    void mapHeatAndLegendUseTheOwningWindowWithoutChangingData();
    void spotlightRepaintsCachedDirectionalValuesInBothWindows();
    void legacyWatchlistRecolorsExistingCellsAndFoldsMissingHistory();
    void rowDelegateUsesTypedDirectionWithTheCurrentWindowPolicy();
    void typedBadgesRemainTransientAndPrivateRowsClear();
    void invalidTypedBadgeCannotReplaceAnAcceptedObservation_data();
    void invalidTypedBadgeCannotReplaceAnAcceptedObservation();
    void invalidTypedBadgeBatchRejectsAtomically();
    void workspaceRendersEachTypedBadgeWithoutParsingItsText();
};
void MovementPresentationsTest::initTestCase(){QVERIFY(settings_.isValid());QCoreApplication::setOrganizationName("KEEFI Synthetic C9");QCoreApplication::setApplicationName("Synthetic Movement Presentations");QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings_.path());}
void MovementPresentationsTest::mapHeatAndLegendUseTheOwningWindowWithoutChangingData(){
    QWidget parent;MovementColorPolicy policy(&parent);MarketMapWidget map(&parent);map.resize(900,500);parent.resize(900,500);
    MarketMapSection section;section.key="coin";section.title="SYNTHETIC";section.available=true;
    section.tiles={{"up","UP","Synthetic up","100","3","",1},{"down","DOWN","Synthetic down","100","-3","",1},{"flat","FLAT","Synthetic flat","100","0","",1}};
    map.setSections({section});parent.show();QVERIFY(map.tileRects().size()==3);QSignalSpy selected(&map,&MarketMapWidget::tileSelected);
    const auto originalUp=policy.heat(MovementDirection::Up,2);QCOMPARE(tileInk(map,"up").rgba(),originalUp.rgba());QCOMPARE(tileInk(map,"down").rgba(),policy.heat(MovementDirection::Down,2).rgba());const auto flat=tileInk(map,"flat");
    policy.setScheme(MovementScheme::KoreanRedBlue);QCoreApplication::processEvents();
    QCOMPARE(tileInk(map,"up").rgba(),policy.heat(MovementDirection::Up,2).rgba());QCOMPARE(tileInk(map,"down").rgba(),policy.heat(MovementDirection::Down,2).rgba());
    QVERIFY(tileInk(map,"up").rgba()!=originalUp.rgba());QCOMPARE(tileInk(map,"flat"),flat);QVERIFY(pixels(map.grab().toImage(),themeColors().koreanDownText)>0);QCOMPARE(selected.size(),0);
}
void MovementPresentationsTest::spotlightRepaintsCachedDirectionalValuesInBothWindows(){
    QWidget first,second;MovementColorPolicy a(&first),b(&second);SpotlightCard left("coin","Synthetic",&first),right("coin","Synthetic",&second);
    const QList<SpotlightMode> modes{{"synthetic","Synthetic",{{"u","SYN-UP","synthetic","100","2"},{"d","SYN-DOWN","synthetic","100","-2"}}}};
    left.setModes(modes);right.setModes(modes);left.resize(500,300);right.resize(500,300);first.resize(500,300);second.resize(500,300);first.show();second.show();
    const auto before=left.visibleItems();QSignalSpy changed(&left,&SpotlightCard::itemSelected);a.setScheme(MovementScheme::KoreanRedBlue);QCoreApplication::processEvents();
    auto* l=left.findChild<QWidget*>("spotlightRows");auto* r=right.findChild<QWidget*>("spotlightRows");QVERIFY(l&&r);
    QVERIFY(pixels(l->grab().toImage(),themeColors().koreanDownText)>0);QCOMPARE(pixels(l->grab().toImage(),themeColors().upText),0);
    QVERIFY(pixels(r->grab().toImage(),themeColors().upText)>0);QCOMPARE(left.visibleItems().size(),before.size());QCOMPARE(left.visibleItems().first().changePercent,before.first().changePercent);QCOMPARE(changed.size(),0);
}
void MovementPresentationsTest::legacyWatchlistRecolorsExistingCellsAndFoldsMissingHistory(){
    QSettings().setValue("explore/watchlist/coin",QStringList{"SYN-UP","SYN-DOWN","SYN-FLAT"});QWidget parent;MovementColorPolicy policy(&parent);ExploreWatchlist view(&parent);
    QList<WatchlistQuote> quotes;for(const auto& pair:{qMakePair("SYN-UP","2"),qMakePair("SYN-DOWN","-2"),qMakePair("SYN-FLAT","0.0001")}){WatchlistQuote row;row.section="coin";row.id=pair.first;row.price="100";row.change=pair.second;quotes.append(row);}
    view.setQuotes(quotes);view.resize(1100,350);parent.resize(1100,350);parent.show();auto* table=view.findChild<QTableWidget*>("watchlistTable");QVERIFY(table);QCOMPARE(table->rowCount(),3);
    table->setCurrentCell(1,1);const auto saved=QSettings().value("explore/watchlist/coin");QSignalSpy favourites(&view,&ExploreWatchlist::favouritesChanged);
    policy.setScheme(MovementScheme::KoreanRedBlue);QCoreApplication::processEvents();
    QCOMPARE(table->item(0,3)->foreground().color(),themeColors().downText);QCOMPARE(table->item(1,3)->foreground().color(),themeColors().koreanDownText);
    QCOMPARE(table->item(2,3)->foreground().color(),themeColors().text3);QVERIFY(!table->item(2,3)->text().contains(QChar(0x25b2)));QVERIFY(table->isColumnHidden(4));
    QCOMPARE(table->currentRow(),1);QCOMPARE(QSettings().value("explore/watchlist/coin"),saved);QCOMPARE(favourites.size(),0);
    quotes.first().history={{1,100},{2,102}};view.setQuotes(quotes);QVERIFY(!table->isColumnHidden(4));
}
void MovementPresentationsTest::rowDelegateUsesTypedDirectionWithTheCurrentWindowPolicy(){
    QWidget owner;MovementColorPolicy policy(&owner);QTreeWidget view(&owner);view.setColumnCount(2);view.resize(500,100);owner.resize(500,100);view.setItemDelegate(new WorkbenchRowDelegate(&view));
    auto* item=new QTreeWidgetItem(&view,{"Synthetic","ACTUAL_TYPED_VALUE"});item->setData(1,ChangeDirectionRole,-1);owner.show();policy.setScheme(MovementScheme::KoreanRedBlue);
    const auto image=view.viewport()->grab().toImage();QVERIFY(pixels(image,themeColors().koreanDownText)>0);QCOMPARE(item->text(1),QString("ACTUAL_TYPED_VALUE"));
}
void MovementPresentationsTest::typedBadgesRemainTransientAndPrivateRowsClear(){
    WatchlistWorkspaceList lists;const WatchlistIdentity identity{"coin","upbit","KRW-SYN"};const auto id=lists.createList("Synthetic");QVERIFY(lists.addIdentity(id,identity));const auto before=lists.publicState(),csv=lists.exportCsv(id);
    WatchlistRowState row;row.completedAtMs=QDateTime::currentMSecsSinceEpoch();row.badges={"legacy"};row.badgePresentations={{"SYNTHETIC_TYPED_PRIVATE",MovementDirection::Down,true}};row.privateData=true;lists.setRowState(identity,row);
    QCOMPARE(lists.rowState(identity).badgePresentations,row.badgePresentations);QCOMPARE(lists.publicState(),before);QCOMPARE(lists.exportCsv(id),csv);lists.clearPrivate();QVERIFY(lists.rowState(identity).badgePresentations.isEmpty());
}
void MovementPresentationsTest::invalidTypedBadgeCannotReplaceAnAcceptedObservation_data(){
    QTest::addColumn<QString>("fault");
    for(const auto* fault:{"count17","negative-enum","unknown-enum","text161","invalid-high-surrogate","invalid-low-surrogate","format-control","nul-control"})QTest::newRow(fault)<<QString(fault);
}
void MovementPresentationsTest::invalidTypedBadgeCannotReplaceAnAcceptedObservation(){
    QFETCH(QString,fault);WatchlistWorkspaceList lists;const WatchlistIdentity identity{"coin","upbit","KRW-SYN"};
    WatchlistRowState accepted;accepted.price="100";accepted.completedAtMs=1;accepted.badgePresentations={{QStringLiteral("SYNTHETIC E\u0301 合成"),MovementDirection::Down,false}};
    lists.setRowState(identity,accepted);QCOMPARE(lists.rowState(identity),accepted);QSignalSpy changed(&lists,&WatchlistWorkspaceList::changed);
    auto invalid=accepted;invalid.price="999";
    if(fault=="count17")while(invalid.badgePresentations.size()<17)invalid.badgePresentations.append(accepted.badgePresentations.first());
    if(fault=="negative-enum")invalid.badgePresentations.first().direction=static_cast<MovementDirection>(-1);
    if(fault=="unknown-enum")invalid.badgePresentations.first().direction=static_cast<MovementDirection>(99);
    if(fault=="text161")invalid.badgePresentations.first().text=QString(161,'x');
    if(fault=="invalid-high-surrogate")invalid.badgePresentations.first().text=QString(QChar(0xd800));
    if(fault=="invalid-low-surrogate")invalid.badgePresentations.first().text=QString(QChar(0xdc00));
    if(fault=="format-control")invalid.badgePresentations.first().text=QStringLiteral("SYN\u200dTHETIC");
    if(fault=="nul-control")invalid.badgePresentations.first().text=QStringLiteral("SYN")+QChar(0)+QStringLiteral("THETIC");
    lists.setRowState(identity,invalid);QCOMPARE(lists.rowState(identity),accepted);QCOMPARE(changed.size(),0);
}
void MovementPresentationsTest::invalidTypedBadgeBatchRejectsAtomically(){
    WatchlistWorkspaceList lists;const WatchlistIdentity a{"coin","upbit","KRW-SYN"},b{"coin","binance","SYNUSDT"};
    WatchlistRowState accepted;accepted.price="100";accepted.badgePresentations={{"SYNTHETIC_VALID",MovementDirection::Up,false}};lists.setRowStates({{a.key(),accepted},{b.key(),accepted}});
    QCOMPARE(lists.rowState(a),accepted);QCOMPARE(lists.rowState(b),accepted);QSignalSpy changed(&lists,&WatchlistWorkspaceList::changed);
    auto valid=accepted;valid.price="200";auto invalid=accepted;invalid.badgePresentations.first().direction=static_cast<MovementDirection>(99);
    lists.setRowStates({{a.key(),valid},{b.key(),invalid}});QCOMPARE(lists.rowState(a),accepted);QCOMPARE(lists.rowState(b),accepted);QCOMPARE(changed.size(),0);
}
void MovementPresentationsTest::workspaceRendersEachTypedBadgeWithoutParsingItsText(){
    QWidget owner;MovementColorPolicy policy(&owner);ChartWorkspace workspace(nullptr,nullptr,&owner);workspace.indicatorPreferences()->markIntroSeen();auto* lists=workspace.watchlistModel();
    const WatchlistIdentity identity{"coin","upbit","KRW-SYN"};const auto id=lists->createList("Synthetic typed badges");QVERIFY(lists->addIdentity(id,identity));
    WatchlistRowState row;row.price="123456789";row.change="+1.25%";row.completedAtMs=QDateTime::currentMSecsSinceEpoch();row.badges={"LEGACY_PLUS_UP_WORD"};row.badgePresentations={{"TYPED_DOWN_DESPITE_PLUS_WORD",MovementDirection::Down,false},{"TYPED_RISK",MovementDirection::Unknown,true}};lists->setRowState(identity,row);
    workspace.resize(1250,900);owner.resize(1250,900);owner.show();
    QVERIFY(test::openInstrumentPanel(workspace));
    auto* selector=workspace.findChild<QComboBox*>("chartWorkspaceListSelector");QVERIFY(selector);selector->setCurrentIndex(selector->findData(id));
    auto* table=workspace.findChild<QTreeWidget*>("chartWorkspaceWatchlist");QVERIFY(table);QCOMPARE(table->topLevelItemCount(),1);auto* item=table->topLevelItem(0);
    QVERIFY(item->text(0).contains("TYPED_DOWN_DESPITE_PLUS_WORD"));QVERIFY(!item->text(0).contains("LEGACY_PLUS_UP_WORD"));QCOMPARE(item->data(0,WatchlistBadgesRole).value<QList<WatchlistBadge>>(),row.badgePresentations);
    const auto before=lists->publicState();policy.setScheme(MovementScheme::KoreanRedBlue);
    QCoreApplication::processEvents();
    const auto capture=qEnvironmentVariable("KEEFI_MOVEMENT_RENDER_FILE");
    if(!capture.isEmpty()){
        QVERIFY(table->viewport()->grab().save(capture));
        qInfo()<<"Synthetic badge viewport"<<table->viewport()->size()<<"columns"<<table->columnWidth(0)<<table->columnWidth(1)
            <<"row"<<table->visualItemRect(item);
    }
    // Policy and layout updates must reach the actual rendered row; keep both
    // typed movement and fixed-risk pixel requirements after the event turn.
    QTRY_VERIFY_WITH_TIMEOUT(pixels(table->viewport()->grab().toImage(),themeColors().koreanDownText)>0,1000);
    const auto image=table->viewport()->grab().toImage();QVERIFY(pixels(image,themeColors().danger)>0);
    QCOMPARE(table->visualItemRect(item).height(),32);QCOMPARE(item->text(1),QString("123,456,789\n+1.25%"));
    QSignalSpy changedCells(table->model(),&QAbstractItemModel::dataChanged);
    const auto priceCellInvalidated=[&]{
        for(const auto& change:changedCells){const auto first=qvariant_cast<QModelIndex>(change[0]),last=qvariant_cast<QModelIndex>(change[1]);
            if(first.row()==0&&last.row()==0&&first.column()<=1&&last.column()>=1)return true;}
        return false;
    };
    row.badgePresentations={{QStringLiteral("합성 상승"),MovementDirection::Up,false}};lists->setRowState(identity,row);
    QVERIFY2(priceCellInvalidated(),"Badge-only updates must repaint the shared footer in the price column");
    changedCells.clear();workspace.setRealtimeDelayedIdentities({identity.key()});QVERIFY(priceCellInvalidated());
    changedCells.clear();workspace.setRealtimeDelayedIdentities({});QVERIFY(priceCellInvalidated());
    changedCells.clear();row.badges.clear();row.badgePresentations.clear();lists->setRowState(identity,row);
    QVERIFY2(priceCellInvalidated(),"Removing the last badge must repaint the price baseline");
    QCOMPARE(item->text(1),QString("123,456,789\n+1.25%"));QCOMPARE(table->visualItemRect(item).height(),32);
    QCOMPARE(lists->publicState(),before);
}
QTEST_MAIN(MovementPresentationsTest)
#include "tst_movement_presentations.moc"
