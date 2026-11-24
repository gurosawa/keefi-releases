#include "keefi/KeefiVisuals.h"
#include <QImage>
#include <QPaintEngine>
#include <QStandardItemModel>
#include <QTest>
#include <QTreeWidget>
#include <QHeaderView>
#include <QDir>

using namespace keefi;
namespace {
struct PaintedText { QString text; QFont font; QPointF at; QColor ink; qreal width; };
// Observe QPainter's public device boundary, not delegate-private layout state.
// Real Qt shaping supplies each text run and its actual selected QFont.
class TextEngine final : public QPaintEngine {
  public:
    TextEngine():QPaintEngine(QPaintEngine::AllFeatures){}
    QList<PaintedText> texts;
    bool begin(QPaintDevice*)override{setActive(true);return true;}
    bool end()override{setActive(false);return true;}
    Type type()const override{return User;}
    void updateState(const QPaintEngineState& s)override{ink=s.pen().color();}
    void drawPixmap(const QRectF&,const QPixmap&,const QRectF&)override{}
    void drawPath(const QPainterPath&)override{}
    void drawTextItem(const QPointF& p,const QTextItem& t)override{texts.append({t.text(),t.font(),p,ink,t.width()});}
  private: QColor ink;
};
class TextDevice final : public QPaintDevice {
  public:
    mutable TextEngine engine;
    QPaintEngine* paintEngine()const override{return &engine;}
  protected:
    int metric(PaintDeviceMetric m)const override {
        switch(m){
        case PdmWidth:return 800;case PdmHeight:return 100;
        case PdmWidthMM:return 212;case PdmHeightMM:return 27;
        case PdmDpiX:case PdmDpiY:case PdmPhysicalDpiX:case PdmPhysicalDpiY:return 96;
        case PdmDepth:return 32;case PdmNumColors:return 0;
        case PdmDevicePixelRatio:return 1;
        case PdmDevicePixelRatioScaled:return int(devicePixelRatioFScale());
        default:return QPaintDevice::metric(m);
        }
    }
};
QStyleOptionViewItem option(QWidget* view,const QRect& rect=QRect(0,0,300,64)) {
    QStyleOptionViewItem o;o.initFrom(view);o.widget=view;o.rect=rect;
    o.palette=KeefiTheme::palette();o.state=QStyle::State_Enabled;return o;
}
QList<PaintedText> painted(const QStyledItemDelegate& d,const QModelIndex& i,const QStyleOptionViewItem& o) {
    TextDevice dev;QPainter p(&dev);d.paint(&p,o,i);p.end();return dev.engine.texts;
}
QImage pixels(const QStyledItemDelegate& d,const QModelIndex& i,const QStyleOptionViewItem& o) {
    QImage image(o.rect.size(),QImage::Format_ARGB32_Premultiplied);image.fill(themeColors().bg);
    QPainter p(&image);d.paint(&p,o,i);p.end();return image;
}
QString joined(const QList<PaintedText>& runs){QString result;for(const auto& r:runs)result+=r.text;return result;}
int countColor(const QImage& image,const QColor& color){int count=0;for(int y=0;y<image.height();++y)for(int x=0;x<image.width();++x)count+=image.pixelColor(x,y)==color;return count;}
}
class RowDelegatesTest final:public QObject {
    Q_OBJECT
  private slots:
    void initTestCase(){QVERIFY(KeefiFonts::registerBundledFonts());}
    void chartNamesReserveTheWholeCodeAt260Pixels_data(){
        QTest::addColumn<QString>("name");QTest::addColumn<QString>("code");QTest::addColumn<QString>("market");
        QTest::newRow("korean")<<QStringLiteral("삼성전자 긴 합성 이름")<<QString("005930")<<QStringLiteral("국내 주식");
        QTest::newRow("english")<<QString("NVIDIA Corporation")<<QString("NVDA")<<QStringLiteral("미국 주식");
        QTest::newRow("pair")<<QString{}<<QString("BTC/USDT")<<QString("Binance Spot");
    }
    void chartNamesReserveTheWholeCodeAt260Pixels(){
        QFETCH(QString,name);QFETCH(QString,code);QFETCH(QString,market);
        QWidget view;WatchlistBadgeDelegate delegate;QStandardItemModel model(1,2);
        auto* item=new QStandardItem((name.isEmpty()?QString{}:name+" ")+code+"\n"+market+"\n완료 봉 대기");
        item->setData(QVariant::fromValue(QList<WatchlistBadge>{{"완료 봉 대기"}}),WatchlistBadgesRole);
        item->setData(QStringList{name,code,market},ChartInstrumentLabelRole);model.setItem(0,0,item);
        // A 260px tree retains the existing 100px price allocation and 2px frame.
        const auto runs=painted(delegate,model.index(0,0),option(&view,QRect(0,0,158,64)));
        QVERIFY2(joined(runs).contains(code),qPrintable(joined(runs)));
        bool paintedCode=false;
        for(const auto& run:runs)if(run.text==code){paintedCode=true;QCOMPARE(run.font.family(),QString("Departure Mono"));QCOMPARE(run.font.pixelSize(),11);QVERIFY(run.at.x()>=12);QVERIFY(run.at.x()+run.width<=146.1);}
        QVERIFY(paintedCode);
        // The wide row must render even all-uppercase English names in Pretendard.
        const auto wide=painted(delegate,model.index(0,0),option(&view,QRect(0,0,700,64)));
        if(!name.isEmpty()){bool named=false;for(const auto& run:wide)if(name.contains(run.text)&&!run.text.trimmed().isEmpty()){named=true;QCOMPARE(run.font.family(),QString("Pretendard"));}QVERIFY(named);}
        QVERIFY(joined(runs).contains(QStringLiteral("대기")));
    }
    void compactChartRowsKeepNameCodePriceAndChangeSeparate_data(){
        QTest::addColumn<bool>("dark");QTest::addColumn<QString>("name");QTest::addColumn<QString>("code");QTest::addColumn<QString>("price");
        for(bool dark:{false,true}){
            const auto prefix=dark?"dark-":"light-";
            QTest::newRow(qPrintable(QString(prefix)+"stock"))<<dark<<QStringLiteral("삼성전자")<<QString("005930")<<QString("123,400");
            QTest::newRow(qPrintable(QString(prefix)+"btc"))<<dark<<QStringLiteral("비트코인")<<QString("BTC/KRW")<<QString("123,456,789");
            QTest::newRow(qPrintable(QString(prefix)+"binance"))<<dark<<QString{}<<QString("HYPE/USDT")<<QString("123.456");
        }
    }
    void compactChartRowsKeepNameCodePriceAndChangeSeparate(){
        QFETCH(bool,dark);QFETCH(QString,name);QFETCH(QString,code);QFETCH(QString,price);
        KeefiTheme::instance()->setMode(dark?ThemeMode::Dark:ThemeMode::Light);
        QTreeWidget view;view.setColumnCount(2);view.setHeaderHidden(true);view.setRootIsDecorated(false);view.setStyleSheet(KeefiTheme::localStyleSheet(&view));
        view.header()->setStretchLastSection(false);view.resize(194,110);
        auto* section=new QTreeWidgetItem(&view,{QStringLiteral("합성 공개 목록")});section->setExpanded(true);
        auto* row=new QTreeWidgetItem(section,{name+" "+code,price+"\n+2.34%"});
        row->setData(0,ChartInstrumentLabelRole,QStringList{name,code,{}});
        row->setData(0,WatchlistBadgesRole,QVariant::fromValue(QList<WatchlistBadge>{{QStringLiteral("확인 필요")}}));
        row->setData(1,ChangeDirectionRole,1);row->setSizeHint(0,QSize(0,64));
        auto* delegate=new WatchlistBadgeDelegate(&view);view.setItemDelegate(delegate);view.show();
        view.setColumnWidth(0,72);view.setColumnWidth(1,120);QTest::qWait(20);
        QList<PaintedText> all;
        for(int column:{0,1}){
            const auto index=view.indexFromItem(row,column);
            all.append(painted(*delegate,index,option(&view,view.visualRect(index))));
        }
        QVERIFY2(joined(all).contains(code),qPrintable(joined(all)));QVERIFY(joined(all).contains(price));
        if(!name.isEmpty())QVERIFY2(joined(all).contains(name),qPrintable(joined(all)));
        qreal nameY=-1,codeY=-1,priceY=-1,changeY=-1;
        for(const auto& run:all){
            if(run.text==name&&!name.isEmpty()){nameY=run.at.y();QCOMPARE(run.font.family(),QString("Pretendard"));}
            if(run.text==code){codeY=run.at.y();QCOMPARE(run.font.family(),QString("Departure Mono"));QCOMPARE(run.font.pixelSize(),11);}
            if(run.text==price){priceY=run.at.y();QCOMPARE(run.font.family(),QString("Departure Mono"));QCOMPARE(run.font.pixelSize(),11);}
            if(run.text.contains("2.34%"))changeY=run.at.y();
        }
        QVERIFY(codeY>=0&&priceY>=0&&changeY>priceY);
        if(!name.isEmpty()){QVERIFY(nameY<codeY);QCOMPARE(codeY,priceY);}else QVERIFY(codeY<priceY);
        const auto output=qEnvironmentVariable("KEEFI_COMPACT_ROW_RENDER_DIR");
        if(!output.isEmpty()){
            QVERIFY(QDir().mkpath(output));view.setCurrentItem(row);
            QVERIFY(view.grab().save(output+"/"+QString::fromLatin1(QTest::currentDataTag())+".png"));
        }
    }
    void evidenceNumbersStayInTheModelAndNeverReachThePainter(){
        QWidget view;WorkbenchRowDelegate delegate;QStandardItemModel model(1,2);
        auto* amount=new QStandardItem("1,234 KRW");amount->setData(QVariant::fromValue(QList<int>{7,12}),EvidenceNumbersRole);model.setItem(0,1,amount);
        const auto runs=painted(delegate,model.index(0,1),option(&view));
        QVERIFY(!joined(runs).contains("[7]"));QVERIFY(!joined(runs).contains("[12]"));QVERIFY(joined(runs).contains("1,234"));
        QCOMPARE(amount->data(EvidenceNumbersRole).value<QList<int>>(),QList<int>({7,12}));
    }
    void cleanup(){KeefiTheme::instance()->setSettings(nullptr);KeefiTheme::instance()->setMode(ThemeMode::System);}
    void workbenchShowsTheWholeTickerWithoutAnAvatar_data(){QTest::addColumn<bool>("dark");QTest::newRow("light")<<false;QTest::newRow("dark")<<true;}
    void workbenchShowsTheWholeTickerWithoutAnAvatar(){
        QFETCH(bool,dark);KeefiTheme::instance()->setMode(dark?ThemeMode::Dark:ThemeMode::Light);
        QWidget view;WorkbenchRowDelegate delegate;QStandardItemModel model(1,1);
        auto* item=new QStandardItem(QStringLiteral("합성 자산"));item->setData("SYN",AvatarRole);model.setItem(0,item);
        const auto o=option(&view);const auto image=pixels(delegate,model.index(0,0),o);
        QCOMPARE(countColor(image,themeColors().surface),0);
        const auto runs=painted(delegate,model.index(0,0),o);
        QVERIFY(joined(runs).contains("SYN"));QVERIFY(joined(runs).contains("합성 자산"));
        QCOMPARE(item->text(),QString("합성 자산"));QCOMPARE(item->data(AvatarRole).toString(),QString("SYN"));
    }
    void workbenchSeparatesPriceDigitsFromKoreanUnits(){
        QWidget view;WorkbenchRowDelegate delegate;QStandardItemModel model(1,2);
        model.setItem(0,0,new QStandardItem(QStringLiteral("합성 자산")));
        auto* value=new QStandardItem(QStringLiteral("1,234.50 원"));value->setTextAlignment(Qt::AlignRight|Qt::AlignVCenter);model.setItem(0,1,value);
        const auto runs=painted(delegate,model.index(0,1),option(&view));
        bool digits=false,korean=false;
        for(const auto& r:runs){
            if(r.text.contains("1,234.50")){digits=true;QCOMPARE(r.font.family(),QString("Departure Mono"));QCOMPARE(r.font.pixelSize(),11);QCOMPARE(r.font.weight(),QFont::Normal);}
            if(r.text.contains("원")){korean=true;QCOMPARE(r.font.family(),QString("Pretendard"));QCOMPARE(r.font.pixelSize(),14);}
        }
        QVERIFY(digits);QVERIFY(korean);
        const auto title=painted(delegate,model.index(0,0),option(&view));
        QVERIFY(!title.isEmpty());QCOMPARE(title.first().font.family(),QString("Pretendard"));QCOMPARE(title.first().font.weight(),QFont::Bold);
        QCOMPARE(value->text(),QString("1,234.50 원"));
    }
    void watchlistPricesAndChangesUseTheNumberFaceAndRightEdge(){
        QWidget view;MovementColorPolicy policy(&view);WatchlistBadgeDelegate delegate;QStandardItemModel model(1,2);
        auto* title=new QStandardItem(QStringLiteral("SYN\ncoin · upbit\n완료 봉"));title->setData(QVariant::fromValue(QList<WatchlistBadge>{{"완료 봉"}}),WatchlistBadgesRole);model.setItem(0,0,title);
        auto* value=new QStandardItem(QStringLiteral("1,234.50\n+2.34%"));value->setData(1,ChangeDirectionRole);model.setItem(0,1,value);
        const auto runs=painted(delegate,model.index(0,1),option(&view,QRect(0,0,160,64)));
        bool price=false,change=false;
        for(const auto& r:runs){
            if(r.text.contains("1,234.50")||r.text.contains("2.34%")){
                QCOMPARE(r.font.family(),QString("Departure Mono"));QCOMPARE(r.font.pixelSize(),11);QCOMPARE(r.font.weight(),QFont::Normal);
                QVERIFY2(qAbs(r.at.x()+r.width-148)<2,"Price/change must align with the 12px right inset");
                if(r.text.contains("2.34%")){change=true;QCOMPARE(r.ink,themeColors().upText);}else price=true;
            }
        }
        QVERIFY(price);QVERIFY(change);QVERIFY(joined(runs).contains(QStringLiteral("▲")));
        QCOMPARE(value->text(),QString("1,234.50\n+2.34%"));
    }
    void mixedTextKeepsOrdinaryEnglishInTheBodyFace(){
        QWidget view;WorkbenchRowDelegate delegate;QStandardItemModel model(1,2);
        const QString plain=QStringLiteral("No data · ");
        const QString text=plain+QStringLiteral("SYN · BTC/KRW · 4H · +12.30% 원");
        model.setItem(0,1,new QStandardItem(text));
        const auto runs=painted(delegate,model.index(0,1),option(&view,QRect(0,0,700,64)));
        int offset=0;bool ticker=false,pair=false,timeframe=false,number=false;
        for(const auto& r:runs){
            if(offset<plain.size())QCOMPARE(r.font.family(),QString("Pretendard"));
            if(r.text.contains("SYN")){ticker=true;QCOMPARE(r.font.family(),QString("Departure Mono"));}
            if(r.text.contains("BTC/KRW")){pair=true;QCOMPARE(r.font.family(),QString("Departure Mono"));}
            if(r.text.contains("4H")){timeframe=true;QCOMPARE(r.font.family(),QString("Departure Mono"));}
            if(r.text.contains("12.30%")){number=true;QCOMPARE(r.font.family(),QString("Departure Mono"));}
            offset+=r.text.size();
        }
        QCOMPARE(joined(runs),text);QVERIFY(ticker);QVERIFY(pair);QVERIFY(timeframe);QVERIFY(number);
    }
    void watchlistKeepsSourceAndTypedStatusesInTwoLines(){
        QWidget view;MovementColorPolicy policy(&view);WatchlistBadgeDelegate delegate;QStandardItemModel model(1,2);
        const QList<WatchlistBadge> badges{{"합성 하락",MovementDirection::Down,false},{"합성 경고",MovementDirection::Unknown,true}};
        auto* title=new QStandardItem(QStringLiteral("합성 종목\nstock · KR\n합성 하락 · 합성 경고"));title->setData(QVariant::fromValue(badges),WatchlistBadgesRole);model.setItem(0,0,title);
        const auto runs=painted(delegate,model.index(0,0),option(&view,QRect(0,0,350,64)));
        QVERIFY(joined(runs).contains("합성 종목"));QVERIFY(joined(runs).contains("stock"));
        QVERIFY(joined(runs).contains("합성 하락"));QVERIFY(joined(runs).contains("합성 경고"));
        qreal top=1000,bottom=-1000;bool risk=false,down=false;
        for(const auto& r:runs){top=qMin(top,r.at.y());bottom=qMax(bottom,r.at.y());risk|=r.text.contains("경고")&&r.ink==themeColors().danger;down|=r.text.contains("하락")&&r.ink==themeColors().downText;}
        QVERIFY2(bottom-top<=30,"Code/source and status must occupy two visual rows, not three");QVERIFY(risk);QVERIFY(down);
        QCOMPARE(title->data(WatchlistBadgesRole).value<QList<WatchlistBadge>>(),badges);
    }
};
QTEST_MAIN(RowDelegatesTest)
#include "tst_row_delegates.moc"
