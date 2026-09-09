#pragma once
#include <QObject>
#include <QString>
#include <QStringList>

class QClipboard;

// A small mime-aware bridge over the system clipboard: text / html / image /
// local file URLs, plus (0.5.0) the in-app rich flavour — application/
// x-minnotes-blocks, a BlockClipboard JSON payload that every document copy
// writes ALONGSIDE the plain text (and a table's TSV + <table>, a lone image's
// raster), so pasting into other apps keeps working while an in-app paste
// gets the block run back with its types, spans, chips, ink and comments.
// Tables copy a cell range as TSV + an HTML <table> (round-trips into
// Excel/Sheets/Docs); paste reads the TSV plain-text form those apps write.
class Clipboard : public QObject {
    Q_OBJECT
public:
    explicit Clipboard(QObject* parent = nullptr);

    Q_INVOKABLE QString readText() const;
    Q_INVOKABLE QString readHtml() const;
    Q_INVOKABLE bool    hasImage() const;          // shaped for future media paste
    Q_INVOKABLE bool    hasHtml() const;           // rich paste (Word/Docs/Excel/web)
    Q_INVOKABLE QStringList readUrls() const;      // copied local files (Finder/Preview) → import as media

    Q_INVOKABLE void writeText(const QString& text);
    // Publish both representations at once so a copied cell range pastes as a
    // table into spreadsheets/docs and as TSV into plain-text targets.
    Q_INVOKABLE void writeTable(const QString& tsv, const QString& html);
    // Copy an image (a media block's / cell's resolved file URL) as a raster.
    // Pastes anywhere as a raster. (An in-app paste prefers the blocks flavour,
    // which references the sidecar file itself — no re-encode; a raster-only
    // paste re-imports as PNG.) Returns false if the file can't be loaded.
    Q_INVOKABLE bool writeImageFromFile(const QString& fileUrl);

    // --- The in-app rich flavour (0.5.0): application/x-minnotes-blocks, the
    // BlockClipboard JSON. writeBlocks publishes EVERY flavour at once so a
    // paste into another app keeps working: the blocks JSON, the plain text
    // (TSV for a table), optional HTML (a table's <table>) and an optional
    // raster (a single image block's file, loaded as an image).
    Q_INVOKABLE bool    hasBlocks() const;
    Q_INVOKABLE QString readBlocks() const;      // "" when absent
    Q_INVOKABLE void    writeBlocks(const QString& blocksJson, const QString& text,
                                    const QString& html = QString(),
                                    const QString& imageFileUrl = QString());

private:
    QClipboard* clip_ = nullptr;
};
