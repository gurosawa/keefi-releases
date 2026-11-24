#pragma once
#include <QAbstractTextDocumentLayout>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextFragment>
#include <QUrl>

namespace keefi::test {
struct TooltipProjection {
    QString text;
    bool image = false;
    bool anchor = false;
    int resourceRequests = 0;
};

// Observe Qt's AutoText interpretation without allowing any resource access.
inline TooltipProjection projectTooltip(const QString& text) {
    class Document final : public QTextDocument {
      public:
        int requested = 0;
      protected:
        QVariant loadResource(int, const QUrl&) override {
            ++requested;
            return {};
        }
    } document;
    if (Qt::mightBeRichText(text)) document.setHtml(text);
    else document.setPlainText(text);
    (void)document.documentLayout()->documentSize();
    TooltipProjection result;
    result.text = document.toPlainText();
    result.resourceRequests = document.requested;
    for (auto block = document.begin(); block.isValid(); block = block.next())
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto format = it.fragment().charFormat();
            result.image |= format.isImageFormat();
            result.anchor |= format.isAnchor();
        }
    return result;
}
} // namespace keefi::test
