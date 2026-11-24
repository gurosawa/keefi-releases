#pragma once

#include <QColor>
#include <QObject>
#include <QPalette>
#include <QPointer>
#include <QString>
#include <functional>

class QSettings;
class QWidget;

namespace keefi {
enum class ThemeMode { System, Light, Dark };

// Fill/line colours preserve the approved mockup. The *Text colours alone
// receive the smallest contrast correction needed for ordinary-size text.
struct ThemeColors {
    QColor bg, surface, rule, grid, text, text2, text3;
    QColor accent, accentText, up, upText, down, downText;
    QColor koreanDown, koreanDownText, field, input, danger;
    QColor studyPurple, studyBlue, studyOrange, sessionDay, flagYellow, level;
};

class KeefiTheme final : public QObject {
    Q_OBJECT
  public:
    static KeefiTheme* instance();
    static const ThemeColors& colorsFor(bool dark);
    static const ThemeColors& colors();
    static double contrast(const QColor&, const QColor&);
    static QColor blend(const QColor& foreground, const QColor& background, qreal amount);
    static QPalette palette();
    static QString localStyleSheet(const QObject* movementContext = nullptr);
    ThemeMode mode() const { return mode_; }
    bool isDark() const;
    void setMode(ThemeMode);
    // Borrow the active app-profile settings. Never reads or writes the vault.
    void setSettings(QSettings*);
  signals:
    void changed();
  private:
    explicit KeefiTheme(QObject* parent);
    void apply();
    ThemeMode mode_ = ThemeMode::System;
    QPointer<QSettings> settings_;
};

inline const ThemeColors& themeColors() { return KeefiTheme::colors(); }
QString themeStyle(QString source);
// Keep the original semantic template so already-open dialogs repaint too.
void setThemeStyle(QWidget*, const QString& source);
void bindThemePresentation(QWidget*, std::function<void()> refresh);
} // namespace keefi
