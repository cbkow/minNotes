#pragma once
#include <QObject>
#include <QString>

class QClipboard;

// A small mime-aware bridge over the system clipboard. Built as a general tier
// (text / html / image-shaped) though only the text + table surface is wired so
// far: tables copy a cell range as TSV + an HTML <table> at once (so it round-
// trips into Excel/Sheets/Docs), and paste reads the TSV plain-text form (which
// is exactly what those apps put on the clipboard for a copied range). Image
// read is shaped for a future media-paste pass.
class Clipboard : public QObject {
    Q_OBJECT
public:
    explicit Clipboard(QObject* parent = nullptr);

    Q_INVOKABLE QString readText() const;
    Q_INVOKABLE QString readHtml() const;
    Q_INVOKABLE bool    hasImage() const;          // shaped for future media paste

    Q_INVOKABLE void writeText(const QString& text);
    // Publish both representations at once so a copied cell range pastes as a
    // table into spreadsheets/docs and as TSV into plain-text targets.
    Q_INVOKABLE void writeTable(const QString& tsv, const QString& html);

private:
    QClipboard* clip_ = nullptr;
};
