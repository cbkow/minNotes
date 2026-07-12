#include "CodeHighlighter.h"
#include "CodeSyntax.h"
#include <KSyntaxHighlighting/SyntaxHighlighter>
#include <KSyntaxHighlighting/Repository>
#include <KSyntaxHighlighting/Definition>
#include <KSyntaxHighlighting/Theme>
#include <QTextDocument>

using namespace KSyntaxHighlighting;

// Repository + fence-tag resolver live in CodeSyntax.{h,cpp} (shared with the
// HTML export, which colours code with the same engine + theme).

CodeHighlighter::CodeHighlighter(QObject* parent) : QObject(parent) {
    hl_ = new SyntaxHighlighter(this);                 // a QSyntaxHighlighter
    const Theme theme = codeHighlightRepo().defaultTheme(Repository::DarkTheme);
    hl_->setTheme(theme);
    bg_ = theme.editorColor(Theme::BackgroundColor);
}

void CodeHighlighter::setDocument(QQuickTextDocument* doc) {
    if (quickDoc_ == doc) return;
    quickDoc_ = doc;
    hl_->setDocument(doc ? doc->textDocument() : nullptr);
    emit documentChanged();
}

void CodeHighlighter::setLanguage(const QString& lang) {
    if (language_ == lang) return;
    language_ = lang;
    hl_->setDefinition(resolveCodeDefinition(lang));
    emit languageChanged();
    hl_->rehighlight();
}
