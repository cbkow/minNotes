#include "CodeHighlighter.h"
#include <KSyntaxHighlighting/SyntaxHighlighter>
#include <KSyntaxHighlighting/Repository>
#include <KSyntaxHighlighting/Definition>
#include <KSyntaxHighlighting/Theme>
#include <QTextDocument>
#include <QHash>

using namespace KSyntaxHighlighting;

// One shared repository (loads all definitions + themes from the library's
// embedded resources). Constructed on first use.
static Repository& sharedRepo() {
    static Repository repo;
    return repo;
}

// Resolve a user-typed fence tag ("js", "bash", "JavaScript", "c++"…) to a
// definition, leniently. Tries, in order: exact name, bare extension
// ("js" → f.js), a small friendly-alias table, then a case-insensitive sweep
// over every definition's name. Invalid (no highlighting) if nothing matches.
static Definition resolveDefinition(const QString& lang) {
    if (lang.isEmpty()) return Definition();
    Repository& repo = sharedRepo();

    Definition d = repo.definitionForName(lang);                          // "C++", "JSON"
    if (d.isValid()) return d;
    d = repo.definitionForFileName(QStringLiteral("f.") + lang);          // bare ext: "js", "sh", "py"
    if (d.isValid()) return d;

    const QString l = lang.toLower();
    // Friendly words the name/extension lookups miss (KSyntax calls these "Bash",
    // "GNU Make"…). Most short forms already resolve via the extension path.
    static const QHash<QString, QString> alias = {
        {QStringLiteral("bash"),       QStringLiteral("Bash")},
        {QStringLiteral("sh"),         QStringLiteral("Bash")},
        {QStringLiteral("shell"),      QStringLiteral("Bash")},
        {QStringLiteral("zsh"),        QStringLiteral("Bash")},
        {QStringLiteral("node"),       QStringLiteral("JavaScript")},
        {QStringLiteral("golang"),     QStringLiteral("Go")},
        {QStringLiteral("yml"),        QStringLiteral("YAML")},
        {QStringLiteral("rs"),         QStringLiteral("Rust")},
        {QStringLiteral("md"),         QStringLiteral("Markdown")},
        {QStringLiteral("text"),       QString()},
        {QStringLiteral("txt"),        QString()},
        {QStringLiteral("plain"),      QString()},
    };
    if (const auto it = alias.constFind(l); it != alias.constEnd()) {
        if (it->isEmpty()) return Definition();                            // explicit "no highlighting"
        d = repo.definitionForName(*it);
        if (d.isValid()) return d;
    }

    for (const Definition& def : repo.definitions())                      // case-insensitive name sweep
        if (def.name().toLower() == l) return def;
    return Definition();
}

CodeHighlighter::CodeHighlighter(QObject* parent) : QObject(parent) {
    hl_ = new SyntaxHighlighter(this);                 // a QSyntaxHighlighter
    const Theme theme = sharedRepo().defaultTheme(Repository::DarkTheme);
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
    hl_->setDefinition(resolveDefinition(lang));
    emit languageChanged();
    hl_->rehighlight();
}
