#pragma once
#include <QObject>
#include <QColor>
#include <QString>
#include <QQuickTextDocument>
#include <QtQml/qqmlregistration.h>

namespace KSyntaxHighlighting { class SyntaxHighlighter; }

// Syntax-colours a code block's TextEdit document with KSyntaxHighlighting — a
// QSyntaxHighlighter over QTextDocument, the same mechanism the inline markdown
// highlighter uses, so it attaches per (pooled) code-block TextEdit:
//
//     TextEdit { id: te; textFormat: TextEdit.PlainText; ... }
//     CodeHighlighter { document: te.textDocument; language: "Python" }
//
// `language` selects the definition (name like "C++"/"Python"/"JSON", or a bare
// extension like "py"); empty = no colouring. The theme's editor background is
// exposed so the block fill can match the token colours.
class CodeHighlighter : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QQuickTextDocument* document READ document WRITE setDocument NOTIFY documentChanged)
    Q_PROPERTY(QString language READ language WRITE setLanguage NOTIFY languageChanged)
    Q_PROPERTY(QColor backgroundColor READ backgroundColor NOTIFY backgroundColorChanged)
public:
    explicit CodeHighlighter(QObject* parent = nullptr);

    QQuickTextDocument* document() const { return quickDoc_; }
    void setDocument(QQuickTextDocument* doc);

    QString language() const { return language_; }
    void setLanguage(const QString& lang);

    QColor backgroundColor() const { return bg_; }

signals:
    void documentChanged();
    void languageChanged();
    void backgroundColorChanged();

private:
    QQuickTextDocument* quickDoc_ = nullptr;
    KSyntaxHighlighting::SyntaxHighlighter* hl_ = nullptr;   // owned via QObject parent
    QString language_;
    QColor bg_;
};
