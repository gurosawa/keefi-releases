#pragma once

#include <QString>
#include <QStringList>

class QLabel;
class QPushButton;
class QWidget;

namespace keefi {
enum class FontRole;
struct PortfolioPresentation;

namespace local_window_detail {
bool hasPortfolioFigure(const PortfolioPresentation& view);
QLabel* textLabel(const QString& text, QWidget* parent, const char* name = nullptr);
QLabel* ledgerTextLabel(const QString& text, FontRole role, QWidget* parent, const char* name = nullptr);
QPushButton* button(const QString& text, const char* name, QWidget* parent);
QString displayChoice(const QString& value);
const QStringList& problemConnectionStates();
void showTransientFeedback(QLabel* label, const QString& text);
} // namespace local_window_detail
} // namespace keefi
