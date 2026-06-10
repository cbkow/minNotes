#pragma once
#include <QObject>
#include <QColor>
#include <QVariantList>
#include <QQuickTextDocument>
#include <QtQml/qqmlregistration.h>

// Renders inline markdown (**bold**, *italic*, `code`) by applying CHARACTER
// FORMATS to a TextEdit's live document — NOT by rewriting it to HTML. The
// TextEdit stays PlainText (raw markdown source, identity caret positions, so
// caret/selection/hit-testing/editing need no column mapping); this just styles
// the inner runs and DIMS the markers in place. Attach one per (pooled) TextEdit:
//
//     TextEdit { id: te; textFormat: TextEdit.PlainText; ... }
//     InlineMarkdownHighlighter { document: te.textDocument; enabled: ... }
//
// It's a plain QObject that OWNS an internal QSyntaxHighlighter (composition, so
// the QML registration never sees the non-QML highlighter base).
//
// Endgame (see STATUS / DESIGN §5): bold is a SEMANTIC fact — stored as a span,
// not as `**` in the text. The menu and markdown are both just triggers; raw
// markdown becomes a power-user view (source mode) + export. At that point this
// same component is driven from stored spans instead of parsed markers, and
// regular users see clean styled text with no markers at all.
class InlineMarkdownHighlighter : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QQuickTextDocument* document READ document WRITE setDocument NOTIFY documentChanged)
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)
    // Themable from QML (bind to Theme.colors.*) so nothing is hard-coded here.
    Q_PROPERTY(QColor markerColor READ markerColor WRITE setMarkerColor NOTIFY markerColorChanged)
    // Markdown markers within the current selection render in this colour.
    Q_PROPERTY(QColor selectedMarkerColor READ selectedMarkerColor WRITE setSelectedMarkerColor NOTIFY selectedMarkerColorChanged)
    Q_PROPERTY(QColor codeColor READ codeColor WRITE setCodeColor NOTIFY codeColorChanged)
    // Link spans render underlined in this colour (bind to Theme.colors.accent).
    Q_PROPERTY(QColor linkColor READ linkColor WRITE setLinkColor NOTIFY linkColorChanged)
    Q_PROPERTY(QString codeFontFamily READ codeFontFamily WRITE setCodeFontFamily NOTIFY codeFontFamilyChanged)
    // Selection range (block-content cols) so markers in it can flip colour. -1 = none.
    Q_PROPERTY(int selStart READ selStart WRITE setSelStart NOTIFY selStartChanged)
    Q_PROPERTY(int selEnd READ selEnd WRITE setSelEnd NOTIFY selEndChanged)
    // Semantic format spans for this block: [{s,e,k}] (k: 1 bold, 2 italic, 3 code).
    // Rendered as clean bold/italic/mono with NO markers (this is the spans path).
    Q_PROPERTY(QVariantList spans READ spans WRITE setSpans NOTIFY spansChanged)
    // When true, highlight spans (kind 8) do NOT paint a char-format background —
    // the view draws them as overlay rects BELOW its selection layer instead
    // (a char background paints inside the TextEdit, above the selection, so
    // selecting highlighted text showed no selection). Same trick as code chips.
    Q_PROPERTY(bool highlightAsOverlay READ highlightAsOverlay WRITE setHighlightAsOverlay NOTIFY highlightAsOverlayChanged)
public:
    explicit InlineMarkdownHighlighter(QObject* parent = nullptr);

    QQuickTextDocument* document() const { return quickDoc_; }
    void setDocument(QQuickTextDocument* doc);

    bool enabled() const;
    void setEnabled(bool on);

    QColor markerColor() const;
    void setMarkerColor(const QColor& c);
    QColor selectedMarkerColor() const;
    void setSelectedMarkerColor(const QColor& c);
    QColor codeColor() const;
    void setCodeColor(const QColor& c);
    QColor linkColor() const;
    void setLinkColor(const QColor& c);
    QString codeFontFamily() const;
    void setCodeFontFamily(const QString& f);

    int selStart() const;
    void setSelStart(int v);
    int selEnd() const;
    void setSelEnd(int v);

    QVariantList spans() const;
    void setSpans(const QVariantList& v);

    bool highlightAsOverlay() const;
    void setHighlightAsOverlay(bool on);

signals:
    void documentChanged();
    void enabledChanged();
    void markerColorChanged();
    void selectedMarkerColorChanged();
    void codeColorChanged();
    void codeFontFamilyChanged();
    void selStartChanged();
    void selEndChanged();
    void spansChanged();
    void linkColorChanged();
    void highlightAsOverlayChanged();

private:
    class Impl;                       // the actual QSyntaxHighlighter (in the .cpp)
    QQuickTextDocument* quickDoc_ = nullptr;
    Impl* hl_ = nullptr;              // owned via QObject parent (== this)
    QVariantList spansCache_;         // last-set spans, for the property getter
};
