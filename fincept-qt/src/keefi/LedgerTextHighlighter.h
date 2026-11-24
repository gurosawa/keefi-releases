#pragma once
#include "keefi/KeefiFonts.h"
#include <QSyntaxHighlighter>
#include <QTextDocument>

namespace keefi {
// Presentation only: the document's original text and native copy/selection
// remain untouched. The owning editor supplies its ordinary Korean font role.
class LedgerTextHighlighter final : public QSyntaxHighlighter {
  public:
    explicit LedgerTextHighlighter(QTextDocument* document) : QSyntaxHighlighter(document) {}
  protected:
    void highlightBlock(const QString& text) override {
        for(const auto& run:KeefiFonts::numberRuns(text))setFormat(run.start,run.length,run.format);
    }
};
} // namespace keefi
