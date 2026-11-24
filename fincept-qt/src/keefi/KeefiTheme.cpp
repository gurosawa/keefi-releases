#include "keefi/KeefiTheme.h"
#include "keefi/KeefiFonts.h"
#include "keefi/MovementColorPolicy.h"
#include <QApplication>
#include <QHash>
#include <QSettings>
#include <QStyleHints>
#include <QWidget>
#include <cmath>

namespace keefi {
namespace {
const ThemeColors light{
    QColor("#FFFFFF"), QColor("#F2F4F6"), QColor("#E5E8EB"), QColor("#F2F4F6"),
    QColor("#191F28"), QColor("#4E5968"), QColor("#66717E"),
    QColor("#D9480F"), QColor("#C7420E"), QColor("#0E8A45"), QColor("#0D8140"),
    QColor("#D92332"), QColor("#D82332"), QColor("#1B64DA"), QColor("#1B64DA"),
    QColor("#D1D6DB"), QColor("#FFFFFF"), QColor("#B91C1C"),
    QColor("#7050A6"), QColor("#2364B7"), QColor("#C7420E"), QColor("#13756B"), QColor("#806000"), QColor("#6941A5")};
const ThemeColors dark{
    QColor("#17171C"), QColor("#22222A"), QColor("#2C2C35"), QColor("#212129"),
    QColor("#E9EBED"), QColor("#B0B8C1"), QColor("#8B95A1"),
    QColor("#FF7A45"), QColor("#FF7A45"), QColor("#3BD37F"), QColor("#3BD37F"),
    QColor("#FF6066"), QColor("#FF6066"), QColor("#6AA8FF"), QColor("#6AA8FF"),
    QColor("#3A3A45"), QColor("#1E1E24"), QColor("#FF8A8A"),
    QColor("#C4B5FD"), QColor("#93C5FD"), QColor("#FBBF24"), QColor("#5EEAD4"), QColor("#FBBF24"), QColor("#C5A3FF")};
constexpr const char* preferenceKey = "display/theme/v1";
QString modeName(ThemeMode mode) {
    return mode == ThemeMode::Light ? QStringLiteral("light") : mode == ThemeMode::Dark ? QStringLiteral("dark") : QStringLiteral("system");
}
}
KeefiTheme::KeefiTheme(QObject* parent) : QObject(parent) {
    if (qobject_cast<QGuiApplication*>(QCoreApplication::instance())) {
        connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, [this] {
            if (mode_ == ThemeMode::System) apply();
        });
    }
}
KeefiTheme* KeefiTheme::instance() {
    static QPointer<KeefiTheme> value;
    if (!value) value = new KeefiTheme(QCoreApplication::instance());
    return value;
}
const ThemeColors& KeefiTheme::colorsFor(bool useDark) { return useDark ? dark : light; }
const ThemeColors& KeefiTheme::colors() { return colorsFor(instance()->isDark()); }
bool KeefiTheme::isDark() const {
    return mode_ == ThemeMode::Dark || (mode_ == ThemeMode::System &&
        qobject_cast<QGuiApplication*>(QCoreApplication::instance()) &&
        QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark);
}
void KeefiTheme::setMode(ThemeMode value) {
    if (value != ThemeMode::System && value != ThemeMode::Light && value != ThemeMode::Dark) return;
    if (settings_) settings_->setValue(preferenceKey, modeName(value));
    if (mode_ == value) return;
    mode_ = value; apply();
}
void KeefiTheme::setSettings(QSettings* value) {
    settings_ = value;
    const auto stored = value ? value->value(preferenceKey).toString() : QString();
    const auto next = stored == "light" ? ThemeMode::Light : stored == "dark" ? ThemeMode::Dark : ThemeMode::System;
    mode_ = next; apply();
}
void KeefiTheme::apply() {
    const bool widgets = qobject_cast<QApplication*>(QCoreApplication::instance());
    if (widgets) {
        QApplication::setPalette(palette());
        QApplication::setFont(KeefiFonts::font(FontRole::Body15));
    }
    emit changed();
    if (widgets) for (auto* widget : QApplication::allWidgets()) widget->update();
}
double KeefiTheme::contrast(const QColor& a, const QColor& b) {
    const auto luminance=[](const QColor& c) {
        const auto linear=[](double x) { return x <= 0.04045 ? x / 12.92 : std::pow((x + 0.055) / 1.055, 2.4); };
        return 0.2126 * linear(c.redF()) + 0.7152 * linear(c.greenF()) + 0.0722 * linear(c.blueF());
    };
    const auto la=luminance(a), lb=luminance(b);
    return (std::max(la,lb)+0.05)/(std::min(la,lb)+0.05);
}
QColor KeefiTheme::blend(const QColor& foreground, const QColor& background, qreal amount) {
    amount=qBound<qreal>(0,amount,1);
    return QColor::fromRgbF(foreground.redF()*amount+background.redF()*(1-amount),
        foreground.greenF()*amount+background.greenF()*(1-amount),foreground.blueF()*amount+background.blueF()*(1-amount));
}
QPalette KeefiTheme::palette() {
    const auto& c=colors(); QPalette result;
    result.setColor(QPalette::Window,c.bg);result.setColor(QPalette::Base,c.input);result.setColor(QPalette::Button,c.surface);
    result.setColor(QPalette::AlternateBase,c.surface);
    for (const auto role:{QPalette::WindowText,QPalette::Text,QPalette::ButtonText}) {
        result.setColor(role,c.text); result.setColor(QPalette::Disabled,role,c.text3);
    }
    result.setColor(QPalette::Highlight,c.surface); result.setColor(QPalette::HighlightedText,c.text);
    result.setColor(QPalette::Link,c.accentText); result.setColor(QPalette::LinkVisited,c.text2);
    result.setColor(QPalette::ToolTipBase,c.text); result.setColor(QPalette::ToolTipText,c.bg);
    return result;
}
QString themeStyle(QString source) {
    const auto& c=themeColors();
    const QHash<QString,QColor> colors{{"bg",c.bg},{"surface",c.surface},{"rule",c.rule},{"grid",c.grid},
        {"text",c.text},{"text2",c.text2},{"text3",c.text3},{"accent",c.accent},{"accentText",c.accentText},
        {"up",c.up},{"upText",c.upText},{"down",c.down},{"downText",c.downText},
        {"koreanDown",c.koreanDown},{"koreanDownText",c.koreanDownText},{"field",c.field},{"input",c.input},{"danger",c.danger},
        {"studyPurple",c.studyPurple},{"studyBlue",c.studyBlue},{"studyOrange",c.studyOrange},{"sessionDay",c.sessionDay},{"level",c.level}};
    for (auto it=colors.cbegin();it!=colors.cend();++it) source.replace("${"+it.key()+"}",it.value().name());
    source.replace("${bodyFamily}",'"'+KeefiFonts::bodyFamily()+'"');
    source.replace("${numberFamily}",'"'+KeefiFonts::numberFamily()+'"');
    const QHash<QString,FontRole> fonts{{"Price44",FontRole::Price44},{"Value22",FontRole::Value22},{"Number11",FontRole::Number11},
        {"Hero44",FontRole::Hero44},{"Title17",FontRole::Title17},{"Body15",FontRole::Body15},
        {"Secondary14",FontRole::Secondary14},{"Caption12",FontRole::Caption12},
        {"BodyStrong15",FontRole::BodyStrong15},{"SecondaryStrong14",FontRole::SecondaryStrong14},{"CaptionStrong12",FontRole::CaptionStrong12}};
    for(auto it=fonts.cbegin();it!=fonts.cend();++it) source.replace("${"+it.key()+"Px}",QString::number(KeefiFonts::font(it.value()).pixelSize()));
    return source;
}
void bindThemePresentation(QWidget* target, std::function<void()> refresh) {
    QObject::connect(KeefiTheme::instance(),&KeefiTheme::changed,target,[refresh]{if(refresh)refresh();});
    if(refresh)refresh();
}
void setThemeStyle(QWidget* target, const QString& source) {
    target->setProperty("keefiThemeTemplate",source);
    target->setStyleSheet(themeStyle(source));
    if(target->property("keefiThemeTemplateBound").toBool())return;
    target->setProperty("keefiThemeTemplateBound",true);
    QObject::connect(KeefiTheme::instance(),&KeefiTheme::changed,target,[target]{
        target->setStyleSheet(themeStyle(target->property("keefiThemeTemplate").toString()));
    });
}
QString KeefiTheme::localStyleSheet(const QObject* movementContext) {
    auto source=QStringLiteral(R"(

        QWidget { background: ${bg}; color: ${text}; }
        QLabel { background: transparent; }
        QLabel[type="brand"] { color: ${text}; font-family: ${numberFamily}; font-size: ${Value22Px}px; font-weight: 400; }
        QLabel[type="badge"] { color: ${text3}; font-size: ${Caption12Px}px; border: 1px solid ${rule}; border-radius: 4px; padding: 1px 6px; }
        QLabel[type="hero"] { font-family: ${numberFamily}; font-size: ${Price44Px}px; font-weight: 400; }
        QLabel[type="hero"][quiet="true"] { font-size: ${Value22Px}px; color: ${text3}; font-weight: 400; }
        QLabel[type="heroUnit"] { font-family: ${numberFamily}; font-size: ${Number11Px}px; font-weight: 400; color: ${text3}; padding-bottom: 5px; }
        QLabel#assetDetailHeadline[quiet="true"] { font-family: ${numberFamily}; font-size: ${Value22Px}px; color: ${text3}; font-weight: 400; }
        QLabel[type="section"] { font-size: ${Title17Px}px; font-weight: 700; }
        QLabel[type="sectionHeading"] { font-size: ${Title17Px}px; font-weight: 700; }
        QLabel[type="metric"] { font-family: ${numberFamily}; font-size: ${Value22Px}px; font-weight: 400; }
        QLabel[type="observedAmount"] { font-family: ${numberFamily}; font-size: ${Value22Px}px; font-weight: 400; }
        QLabel[type="caption"] { font-size: ${Caption12Px}px; color: ${text3}; }
        QLabel[type="banner"] { background: ${surface}; color: ${text2}; border: 1px solid ${rule}; border-radius: 12px; padding: 10px 12px; }
        QLabel[type="muted"] { color: ${text3}; }
        QLabel[tone="error"] { color: ${danger}; }
        QLabel[tone="warning"] { color: ${accentText}; }
        QFrame[card="true"], QWidget[card="true"] { background: ${bg}; border: 1px solid ${rule}; border-radius: 12px; padding: 12px; }
        QFrame#portfolioLoadingCard { padding: 0; border-color: ${rule}; }
        QLabel#portfolioLoading { font-size: ${Title17Px}px; font-weight: 700; }
        QWidget#portfolioLoadingSpinner { background: transparent; }
        QLabel#assetDetailHeadline { font-family: ${numberFamily}; font-size: ${Price44Px}px; font-weight: 400; }
        QLabel[tone="up"] { color: ${movementUp}; }
        QLabel[tone="down"] { color: ${movementDown}; }
        QWidget[placeRow="true"] { background: ${bg}; border: 1px solid ${surface}; border-radius: 12px; }
        QPushButton[disclose="true"] { background: ${input}; border: 1px solid ${surface}; border-radius: 12px; padding: 10px 14px; text-align: left; color: ${text2}; }
        QPushButton[disclose="true"]:hover { border-color: ${field}; }
        QLabel[detailBlock="true"] { border-top: 1px solid ${surface}; padding-top: 8px; color: ${text2}; }
        QWidget#headerTabs { background: transparent; border: 0; }
        QPushButton[headerTab="true"] { background: transparent; border: 0; border-bottom: 2px solid transparent; border-radius: 0; padding: 7px 18px; color: ${text3}; font-weight: 600; }
        QPushButton[headerTab="true"][compactHeader="true"] { padding: 7px 12px; }
        QPushButton[headerTab="true"]:hover { background: ${surface}; color: ${text}; }
        QPushButton[headerTab="true"]:checked { background: transparent; border-bottom-color: ${accent}; color: ${text}; }
        QPushButton { background: ${surface}; border: 1px solid ${rule}; padding: 8px 16px; border-radius: 10px; }
        QPushButton:hover { background: ${surface}; border-color: ${field}; }
        QPushButton:focus { border: 1px solid ${accent}; }
        QPushButton[compact="true"] { padding: 6px 12px; }
        QPushButton[primary="true"] { background: ${text}; color: ${bg}; border-color: ${text}; font-weight: 700; }
        QPushButton[primary="true"]:hover { background: ${text2}; color: ${bg}; }
        QPushButton:disabled { color: ${text3}; background: ${surface}; border-color: ${surface}; }
        QPushButton[pill="true"] { background: ${surface}; border: 1px solid ${rule}; border-radius: 12px; padding: 5px 14px; min-height: 18px; font-size: ${Caption12Px}px; color: ${text2}; }
        QPushButton[pill="true"][tone="warn"] { color: ${accentText}; border-color: ${rule}; }
        QPushButton[pill="true"][tone="info"] { color: ${text2}; border-color: ${rule}; }
        QPushButton[chip="warn"] { color: ${accentText}; background: ${surface}; border: 1px solid ${rule}; border-radius: 11px; padding: 3px 10px; min-height: 16px; font-size: ${Caption12Px}px; }
        QPushButton[chip="info"] { color: ${text2}; background: ${surface}; border: 1px solid ${rule}; border-radius: 11px; padding: 3px 10px; min-height: 16px; font-size: ${Caption12Px}px; }
        QPushButton[chip="muted"] { color: ${text3}; background: ${surface}; border: 1px solid ${field}; border-radius: 11px; padding: 3px 10px; min-height: 16px; font-size: ${Caption12Px}px; }
        QLineEdit { background: ${input}; border: 1px solid ${field}; border-radius: 10px; padding: 8px 12px; min-height: 20px; }
        QComboBox { background: ${input}; border: 1px solid ${field}; border-radius: 10px; }
        QLineEdit:focus, QComboBox:focus { border-color: ${accent}; }
        QLineEdit:disabled, QComboBox:disabled { color: ${text3}; }
        QTreeView, QTableView, QListView { background: ${input}; border: 1px solid ${rule}; border-radius: 16px; padding: 6px; outline: 0;
            selection-background-color: ${surface}; selection-color: ${text}; }
        QListView::item { padding: 10px 8px; border-radius: 10px; }
        QListView::item:selected { background: ${surface}; color: ${text}; }
        QHeaderView { background: transparent; border: 0; }
        QHeaderView::section { background: ${input}; color: ${text3}; border: 0; padding: 6px 10px; font-size: ${Caption12Px}px; }
        QTabWidget::pane { border: 0; }
        QSplitter::handle { background: transparent; width: 14px; }
        QScrollArea { border: 0; }
        QScrollBar:vertical { background: transparent; width: 10px; margin: 4px 2px; }
        QScrollBar::handle:vertical { background: ${rule}; border-radius: 3px; min-height: 30px; }
        QScrollBar:horizontal { background: transparent; height: 10px; margin: 2px 4px; }
        QScrollBar::handle:horizontal { background: ${rule}; border-radius: 3px; min-width: 30px; }
        QScrollBar::add-line, QScrollBar::sub-line { height: 0; width: 0; }
        QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
        QWidget#avatarBadge, QWidget#logoMark { background: transparent; }
        QWidget#assetDetailContent, QWidget#assetDetailHoldings, QWidget#assetDetailPlaces, QScrollArea#assetDetailScroll { background: transparent; }
        QFrame#trustProcess { background: ${bg}; border: 1px solid ${rule}; border-radius: 16px; }
        QCheckBox { spacing: 7px; color: ${text3}; }
        QCheckBox::indicator { width: 16px; height: 16px; border: 1px solid ${text3}; border-radius: 4px; background: ${surface}; }
        QCheckBox::indicator:checked { background: ${text}; border-color: ${text}; }
        QCheckBox::indicator:unchecked:hover { border-color: ${accent}; }
        QCheckBox::indicator:disabled { background: ${surface}; border-color: ${field}; }
        QCheckBox:disabled { color: ${text3}; }
        QDialog#optionsDialog QListWidget#optionsNav,
        QDialog#recordTransferDialog QTableWidget#importedRecordTable { border-radius: 12px; }
        QDialog#optionsDialog QCheckBox,
        QDialog#recordTransferDialog QCheckBox { color: ${text2}; }
        /* These IDs belong to modal forms. Embedded VaultGate forms have their
           own IDs and retain their 48 px inputs and 52 px primary action. */
        QDialog#vaultDialog QLineEdit,
        QDialog#accountDialog QLineEdit,
        QDialog#recordTransferDialog QLineEdit { min-height: 26px; }
        QDialog#optionsDialog QPushButton#optionEraseVault:disabled {
            color: ${text3}; border-color: ${rule}; background: ${surface};
        }
        QDialog#vaultDialog QToolButton[secretReveal="true"],
        QDialog#accountDialog QToolButton[secretReveal="true"] {
            background: ${surface}; color: ${text2}; border: 1px solid ${rule};
            border-radius: 8px; padding: 0 10px; font-family: ${bodyFamily}; font-size: ${Body15Px}px;
        }
        QDialog#vaultDialog QToolButton[secretReveal="true"]:hover,
        QDialog#accountDialog QToolButton[secretReveal="true"]:hover { border-color: ${field}; }
        QDialog#vaultDialog QToolButton[secretReveal="true"]:focus,
        QDialog#accountDialog QToolButton[secretReveal="true"]:focus { border-color: ${accent}; }
        QDialog#vaultDialog QToolButton[secretReveal="true"]:disabled,
        QDialog#accountDialog QToolButton[secretReveal="true"]:disabled { color: ${text3}; }
        QDialog#recordTransferDialog QTabBar::tab {
            min-height: 42px; padding: 0 14px; background: transparent; color: ${text3};
            border: 0; border-bottom: 2px solid ${rule};
        }
        QDialog#recordTransferDialog QTabBar::tab:selected { color: ${text}; border-bottom-color: ${accent}; }
        QDialog#recordTransferDialog QTabBar::tab:hover { color: ${text}; background: ${surface}; }
        QMenu { background: ${bg}; border: 1px solid ${field}; border-radius: 8px; }
        QMenu::item { padding: 7px 18px; }
        QMenu::item:selected { background: ${surface}; }
        QMenu::item:disabled { color: ${text3}; }
        QPushButton#openOptions { padding: 0; border-radius: 8px; background: transparent; border: 0; }
        QPushButton#trustPill { background: transparent; border: 0; border-radius: 0; padding: 5px 8px; }
        QLabel#buildBadge { border: 0; padding: 0; }
        QPushButton[danger="true"] { color: ${danger}; border-color: ${danger}; }
        QPushButton[danger="true"]:hover { background: ${surface}; border-color: ${danger}; }
        QFrame[spotlight="true"], QFrame#marketMapCard { background: ${bg}; border: 0; border-top: 1px solid ${rule}; border-radius: 0; }
        QFrame#marketMapCard { padding: 0; }
        QTableWidget#exploreMarkets { background: ${bg}; border: 0; border-top: 1px solid ${rule}; border-radius: 0; padding: 0; }
        QTableWidget#exploreMarkets QHeaderView::section, QTableWidget#watchlistTable QHeaderView::section { background: transparent; }
        QLabel#spotlightTitle { font-size: ${Title17Px}px; font-weight: 700; color: ${text}; }
        QPushButton[spotlightMode="true"] { background: transparent; border: 0; padding: 2px 4px; font-size: ${Secondary14Px}px; font-weight: 700; color: ${text3}; }
        QPushButton[spotlightMode="true"]:hover { color: ${text2}; }
        QPushButton[spotlightIcon="true"] { background: transparent; border: 0; border-radius: 8px; min-width: 36px; max-width: 36px; min-height: 36px; max-height: 36px; padding: 0; font-size: ${Title17Px}px; color: ${text2}; }
        QPushButton[spotlightIcon="true"]:hover { background: ${surface}; }
        QPushButton[spotlightIcon="true"]:disabled { background: transparent; color: ${text3}; }
        QWidget#spotlightCards, QWidget#spotlightRows, QWidget#spotlightDots { background: transparent; }
        QTabBar#watchlistMarkets::tab { background: transparent; color: ${text3}; padding: 6px 9px; border-bottom: 2px solid transparent; font-size: ${Caption12Px}px; }
        QTabBar#watchlistMarkets::tab:selected { color: ${text}; border-bottom-color: ${accent}; }
        QTableWidget#watchlistTable { background: ${bg}; border: 0; }
        QTableWidget#watchlistTable::item { border-bottom: 1px solid ${rule}; padding: 4px; }
        QLabel#watchlistBasis { font-size: ${Caption12Px}px; }
        QDialog { background: ${bg}; }

        QListWidget#optionsNav { border-radius: 12px; padding: 6px; }
        QListWidget#optionsNav::item { padding: 9px 12px; border-radius: 8px; }
        QListWidget#optionsNav::item:selected { background: ${surface}; color: ${text}; }
        QWidget#themeSwitch { border: 1px solid ${rule}; border-radius: 8px; }
        QPushButton[themeChoice="true"] { background: transparent; color: ${text3}; border: 0; border-radius: 8px; padding: 4px 10px; }
        QPushButton[themeChoice="true"]:checked { background: ${text}; color: ${bg}; }

    )");
    source.replace("${movementUp}",MovementColorPolicy::colorFor(movementContext,MovementDirection::Up).name());
    source.replace("${movementDown}",MovementColorPolicy::colorFor(movementContext,MovementDirection::Down).name());
    return themeStyle(source);
}
} // namespace keefi
