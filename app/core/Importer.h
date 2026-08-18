#pragma once
#include "BlockModel.h"

class QTextDocument;
class MediaStore;

// File-import engine (Exporter's mirror). Current scope: the shared
// QTextDocument → BlockSpec walker — HTML paste and every rich-text importer
// (MD, HTML, DOCX, RTF, ENEX) converge on a QTextDocument, so this is the ONE
// place Qt's reader conventions are decoded. Property behavior pinned against
// Qt 6.11 readers:
//   blockquote → BlockQuoteLevel (both HTML + Markdown readers)
//   code       → BlockCodeLanguage (fences/indented md; lang may be "") OR
//                nonBreakableLines (<pre>); one QTextBlock PER LINE → coalesced
//   hr         → BlockTrailingHorizontalRulerWidth on an empty block
//   ordered    → QTextListFormat style ListDecimal…ListUpperRoman
//   depth      → QTextListFormat indent (1-based)
//   tasks      → block marker Unchecked/Checked — but the marker property LEAKS
//                onto following non-list paragraphs, so it's only read under
//                textList() (do not "simplify" that guard away)
//   thead      → QTextTableFormat headerRowCount
// The facade mirrors Exporter: a context property tracking the active model
// (main.cpp re-points it on tab switch), Q_INVOKABLEs for QML, static
// headless per-format cores for the regression suite.
class Importer : public QObject {
    Q_OBJECT
public:
    explicit Importer(QObject* parent = nullptr) : QObject(parent) {}

    void setModel(BlockModel* m) { model_ = m; }

    // Import-format token for a path/URL by extension — "md", "txt", "csv",
    // "tsv", "html" so far; "" = not importable. THE classifier: menus, drop
    // intercepts, and the welcome screen all consult this (and the package
    // lane's canOpenDirectly FIRST — direct-open formats never import).
    Q_INVOKABLE QString formatFor(const QString& fileUrlOrPath) const {
        return formatForPath(fileUrlOrPath);
    }
    // The classifier itself, callable without an instance (BlockModel's
    // drop/paste intercept runs it before the file-chip fallback).
    static QString formatForPath(const QString& fileUrlOrPath);
    // Whether the file expands to MULTIPLE documents (ENEX/Notion zips later;
    // everything current is single-doc → lands in the active untitled tab).
    Q_INVOKABLE bool isMultiDocument(const QString& fileUrlOrPath) const;
    // Import a single-document file into the ACTIVE model (QML opens a fresh
    // tab first; the initial blank paragraph is consumed by the insert).
    // False on unknown format or read failure.
    Q_INVOKABLE bool importFile(const QString& fileUrlOrPath);

    // --- Headless per-format cores (regression-suite entry points) ---
    // Markdown: tri-state `- [/] ` sentinel pre-scan → setMarkdown(GitHub) →
    // walker (relative images resolve against the file's directory) →
    // sentinel post-pass → insertSpecs.
    static bool importMarkdownFile(const QString& path, BlockModel* m);
    // Plain text: decode (BOM sniff, UTF-8 default) → pasteText smart rules.
    static bool importTextFile(const QString& path, BlockModel* m);
    // CSV / TSV → one Table block (first row = header, the app default).
    // CSV parses RFC-4180-ish via TableGrid::fromCSV; TSV splits on tabs.
    static bool importCsvFile(const QString& path, BlockModel* m, bool tsv = false);
    // HTML file → setHtml → walker (relative images resolve against the
    // file's directory; remote ones localize async post-insert).
    static bool importHtmlFile(const QString& path, BlockModel* m);

    // Walk `doc`'s frame tree into BlockSpecs (BlockModel::insertSpecs's IR).
    // `store` receives embedded/local images (null = skip images). `baseDir`
    // resolves relative image srcs (a file import's directory; empty for paste).
    static std::vector<BlockModel::BlockSpec> specsFromTextDocument(
        QTextDocument& doc, MediaStore* store, const QString& baseDir = QString());

private:
    BlockModel* model_ = nullptr;
};
