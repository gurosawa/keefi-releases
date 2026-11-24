#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <cstddef>

namespace keefi {
namespace xml_input_detail {

// SEC-XML-01 / CVE-2026-16762: this is an ingress budget, not an XML validator.
// Keep the caller's byte limit. Qt still validates names, namespaces and structure.
inline constexpr qsizetype MaximumAttributesPerElement = 64;

inline bool xmlSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

inline bool nameByte(char c) {
    const auto u = static_cast<unsigned char>(c);
    return (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') ||
           (u >= '0' && u <= '9') || c == '_' || c == ':' || c == '.' ||
           c == '-' || u >= 0x80;
}

inline char asciiLower(char c) {
    return c >= 'A' && c <= 'Z' ? char(c + ('a' - 'A')) : c;
}

template <std::size_t N>
inline bool equals(QByteArrayView value, const char (&word)[N], bool ignoreCase = false) {
    if (value.size() != qsizetype(N - 1)) return false;
    for (qsizetype i = 0; i < value.size(); ++i) {
        if ((ignoreCase ? asciiLower(value[i]) : value[i]) != word[i]) return false;
    }
    return true;
}

inline bool asciiCompatibleEncoding(QByteArrayView value) {
    return equals(value, "utf-8", true) || equals(value, "utf8", true) ||
           equals(value, "ascii", true) || equals(value, "us-ascii", true) ||
           equals(value, "iso-8859-1", true) || equals(value, "iso8859-1", true) ||
           equals(value, "latin1", true) || equals(value, "latin-1", true);
}

class AttributeBudgetLexer final {
  public:
    explicit AttributeBudgetLexer(QByteArrayView input) : input_(input) {}

    bool accepts() {
        consume("\xEF\xBB\xBF");
        declarationOffset_ = cursor_;
        while (cursor_ < input_.size()) {
            if (input_[cursor_] != '<') {
                ++cursor_;
                continue;
            }
            const auto markupOffset = cursor_;
            if (consume("<!--")) {
                if (!skipUntil("-->")) return false;
            } else if (consume("<![CDATA[")) {
                if (!skipUntil("]]>")) return false;
            } else if (consume("<?")) {
                if (!processingInstruction(markupOffset)) return false;
            } else if (consume("</")) {
                if (readName().isEmpty()) return false;
                skipSpace();
                if (!consume(">")) return false;
            } else if (consume("<!")) {
                // Reject DTD/default attributes before Qt can process them.
                return false;
            } else {
                consume("<");
                if (!startTag()) return false;
            }
        }
        return true;
    }

  private:
    template <std::size_t N>
    bool consume(const char (&token)[N]) {
        static_assert(N > 1);
        constexpr qsizetype length = qsizetype(N - 1);
        if (length > input_.size() - cursor_) return false;
        for (qsizetype i = 0; i < length; ++i) {
            if (input_[cursor_ + i] != token[i]) return false;
        }
        cursor_ += length;
        return true;
    }

    template <std::size_t N>
    bool skipUntil(const char (&token)[N]) {
        while (cursor_ < input_.size()) {
            if (consume(token)) return true;
            ++cursor_;
        }
        return false;
    }

    bool skipSpace() {
        const auto begin = cursor_;
        while (cursor_ < input_.size() && xmlSpace(input_[cursor_])) ++cursor_;
        return cursor_ != begin;
    }

    QByteArrayView readName() {
        const auto begin = cursor_;
        while (cursor_ < input_.size() && nameByte(input_[cursor_])) ++cursor_;
        return input_.sliced(begin, cursor_ - begin);
    }

    QByteArrayView attributeName() {
        const auto name = readName();
        if (name.isEmpty()) return {};
        skipSpace();
        if (!consume("=")) return {};
        skipSpace();
        return name;
    }

    bool quotedValue(QByteArrayView* value) {
        if (cursor_ == input_.size()) return false;
        const char quote = input_[cursor_];
        if (quote != '\'' && quote != '"') return false;
        const auto begin = ++cursor_;
        while (cursor_ < input_.size() && input_[cursor_] != quote) {
            if (input_[cursor_] == '<') return false;
            ++cursor_;
        }
        if (cursor_ == input_.size()) return false;
        *value = input_.sliced(begin, cursor_ - begin);
        ++cursor_;
        return true;
    }

    bool startTag() {
        if (readName().isEmpty()) return false;
        qsizetype count = 0;
        while (cursor_ < input_.size()) {
            const bool separated = skipSpace();
            if (consume("/>") || consume(">")) return true;
            if (!separated || attributeName().isEmpty()) return false;
            // Count every explicit attribute, including xmlns declarations.
            if (++count > MaximumAttributesPerElement) return false;
            QByteArrayView value;
            if (!quotedValue(&value)) return false;
        }
        return false;
    }

    bool declaration() {
        bool version = false, encoding = false, standalone = false;
        while (cursor_ < input_.size()) {
            const bool separated = skipSpace();
            if (consume("?>")) return version;
            if (!separated) return false;
            const auto name = attributeName();
            QByteArrayView value;
            if (name.isEmpty() || !quotedValue(&value)) return false;
            if (!version) {
                if (!equals(name, "version") || !equals(value, "1.0")) return false;
                version = true;
            } else if (equals(name, "encoding") && !encoding && !standalone) {
                if (!asciiCompatibleEncoding(value)) return false;
                encoding = true;
            } else if (equals(name, "standalone") && !standalone) {
                if (!equals(value, "yes") && !equals(value, "no")) return false;
                standalone = true;
            } else {
                return false;
            }
        }
        return false;
    }

    bool processingInstruction(qsizetype markupOffset) {
        const auto target = readName();
        if (target.isEmpty()) return false;
        if (equals(target, "xml", true)) {
            // XML declarations are case-sensitive and may only follow a UTF-8 BOM.
            if (!equals(target, "xml") || markupOffset != declarationOffset_) return false;
            return declaration();
        }
        if (consume("?>")) return true;
        if (!skipSpace()) return false;
        // Quotes have no special meaning inside PI data.
        return skipUntil("?>");
    }

    QByteArrayView input_;
    qsizetype cursor_ = 0;
    qsizetype declarationOffset_ = 0;
};

} // namespace xml_input_detail

inline bool xmlAttributeBudgetAllows(const QByteArray& input) {
    // NUL also excludes UTF-16/32 byte streams before automatic Qt decoding.
    if (input.isEmpty() || input.contains('\0')) return false;
    return xml_input_detail::AttributeBudgetLexer(QByteArrayView(input)).accepts();
}

} // namespace keefi
