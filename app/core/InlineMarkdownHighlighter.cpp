#include "InlineMarkdownHighlighter.h"
#include <QSyntaxHighlighter>
#include <QTextDocument>
#include <QTextCharFormat>
#include <QVariant>
#include <vector>

// The actual highlighter. Owns the colours/flags/spans and does the work; the
// outer QObject just forwards property writes and re-highlights.
class InlineMarkdownHighlighter::Impl : public QSyntaxHighlighter {
public:
    explicit Impl(QObject* parent) : QSyntaxHighlighter(parent) {}

    struct SpanFmt { int s; int e; int k; };   // [s,e) in block-content cols; k: 1 bold 2 italic 3 code

    bool enabled = true;
    QColor markerColor         = QColor(0x01, 0x89, 0xf1);   // Theme.colors.accent
    QColor selectedMarkerColor = QColor(0xff, 0xff, 0xff);   // Theme.colors.textBright
    QColor codeColor           = QColor(0x4a, 0xa8, 0xff);   // Theme.colors.inlineCodeText (blue)
    int selStart = -1, selEnd = -1;
    std::vector<SpanFmt> spans;

protected:
    // First lays down markdown-marker styling (**bold** etc., markers dimmed/
    // accent, white inside the selection), then SEMANTIC spans (clean bold/
    // italic/mono, no markers). Spans are applied last so they win on overlap.
    void highlightBlock(const QString& text) override {
        if (!enabled) return;
        const int pos = currentBlock().position();   // this block's doc offset (multi-line content)
        const int n = text.size();

        // No background here — the code "chip" is a QML overlay rect layered
        // BELOW the selection (so selecting code highlights it). Glyphs only.
        QTextCharFormat code;
        code.setForeground(codeColor);
        code.setFontFamilies({QStringLiteral("Menlo"), QStringLiteral("monospace")});

        // A marker token at local [start,start+len): accent, or white if it sits
        // inside the current selection (so it stays legible over the highlight).
        auto mark = [&](int start, int len) {
            const int ds = pos + start;
            const bool sel = (selStart < selEnd) && ds >= selStart && (ds + len) <= selEnd;
            QTextCharFormat f; f.setForeground(sel ? selectedMarkerColor : markerColor);
            setFormat(start, len, f);
        };

        // --- markdown markers (`**b**`, `*i*`, `` `c` ``) ---
        int i = 0;
        while (i < n) {
            const QChar c = text[i];
            if (c == QLatin1Char('`')) {
                const int j = text.indexOf(QLatin1Char('`'), i + 1);
                if (j > i) { mark(i, 1); setFormat(i + 1, j - i - 1, code); mark(j, 1); i = j + 1; continue; }
            } else if (c == QLatin1Char('*') && i + 1 < n && text[i + 1] == QLatin1Char('*')) {
                const int j = text.indexOf(QStringLiteral("**"), i + 2);
                if (j > i + 1) {
                    QTextCharFormat b; b.setFontWeight(QFont::Bold);
                    mark(i, 2); setFormat(i + 2, j - i - 2, b); mark(j, 2); i = j + 2; continue;
                }
            } else if (c == QLatin1Char('*')) {
                const int j = text.indexOf(QLatin1Char('*'), i + 1);
                if (j > i) {
                    QTextCharFormat it; it.setFontItalic(true);
                    mark(i, 1); setFormat(i + 1, j - i - 1, it); mark(j, 1); i = j + 1; continue;
                }
            }
            ++i;
        }

        // --- semantic spans (no markers) — accumulate per-char flags so
        // overlapping bold+italic combine, then emit runs. ---
        if (spans.empty() || n == 0) return;
        std::vector<uint8_t> fl(n, 0);   // bit0 bold, bit1 italic, bit2 code
        bool any = false;
        for (const SpanFmt& sp : spans) {
            const int ls = std::max(0, sp.s - pos), le = std::min(n, sp.e - pos);
            const uint8_t bit = sp.k == 1 ? 1 : sp.k == 2 ? 2 : sp.k == 3 ? 4 : 0;
            for (int x = ls; x < le; ++x) { fl[x] |= bit; any = true; }
        }
        if (!any) return;
        int x = 0;
        while (x < n) {
            if (!fl[x]) { ++x; continue; }
            int y = x; while (y < n && fl[y] == fl[x]) ++y;
            QTextCharFormat f;
            if (fl[x] & 1) f.setFontWeight(QFont::Bold);
            if (fl[x] & 2) f.setFontItalic(true);
            if (fl[x] & 4) {   // code: glyphs only; chip drawn as a QML overlay
                f.setForeground(codeColor);
                f.setFontFamilies({QStringLiteral("Menlo"), QStringLiteral("monospace")});
            }
            setFormat(x, y - x, f);
            x = y;
        }
    }
};

InlineMarkdownHighlighter::InlineMarkdownHighlighter(QObject* parent)
    : QObject(parent), hl_(new Impl(this)) {}

void InlineMarkdownHighlighter::setDocument(QQuickTextDocument* doc) {
    if (quickDoc_ == doc) return;
    quickDoc_ = doc;
    hl_->setDocument(doc ? doc->textDocument() : nullptr);
    emit documentChanged();
}

bool InlineMarkdownHighlighter::enabled() const { return hl_->enabled; }
void InlineMarkdownHighlighter::setEnabled(bool on) {
    if (hl_->enabled == on) return;
    hl_->enabled = on; emit enabledChanged(); hl_->rehighlight();
}

QColor InlineMarkdownHighlighter::markerColor() const { return hl_->markerColor; }
void InlineMarkdownHighlighter::setMarkerColor(const QColor& c) {
    if (hl_->markerColor == c) return;
    hl_->markerColor = c; emit markerColorChanged(); hl_->rehighlight();
}
QColor InlineMarkdownHighlighter::selectedMarkerColor() const { return hl_->selectedMarkerColor; }
void InlineMarkdownHighlighter::setSelectedMarkerColor(const QColor& c) {
    if (hl_->selectedMarkerColor == c) return;
    hl_->selectedMarkerColor = c; emit selectedMarkerColorChanged(); hl_->rehighlight();
}
QColor InlineMarkdownHighlighter::codeColor() const { return hl_->codeColor; }
void InlineMarkdownHighlighter::setCodeColor(const QColor& c) {
    if (hl_->codeColor == c) return;
    hl_->codeColor = c; emit codeColorChanged(); hl_->rehighlight();
}

int InlineMarkdownHighlighter::selStart() const { return hl_->selStart; }
void InlineMarkdownHighlighter::setSelStart(int v) {
    if (hl_->selStart == v) return;
    hl_->selStart = v; emit selStartChanged(); hl_->rehighlight();
}
int InlineMarkdownHighlighter::selEnd() const { return hl_->selEnd; }
void InlineMarkdownHighlighter::setSelEnd(int v) {
    if (hl_->selEnd == v) return;
    hl_->selEnd = v; emit selEndChanged(); hl_->rehighlight();
}

QVariantList InlineMarkdownHighlighter::spans() const { return spansCache_; }
void InlineMarkdownHighlighter::setSpans(const QVariantList& v) {
    spansCache_ = v;
    hl_->spans.clear();
    hl_->spans.reserve(v.size());
    for (const QVariant& item : v) {
        const QVariantMap m = item.toMap();
        hl_->spans.push_back({m.value(QStringLiteral("s")).toInt(),
                              m.value(QStringLiteral("e")).toInt(),
                              m.value(QStringLiteral("k")).toInt()});
    }
    emit spansChanged();
    hl_->rehighlight();
}
