#pragma once
// Search (v2.0.1): ONE FILE, everything on its canvas — never other files (the no-vault ruling). Sources, in the
// result order: the CURRENT page's block text, the other pages' block text, canvas CARDS (media names + paths,
// bookmark title / URL / host, folder names + entries, text chips), PDF page text (extracted once per PDF on a
// worker into the file's `pdf_text` cache, keyed by the descriptor's src + mtime + size, so the cache travels with
// the .mnd), video NOTES (each clip's QCView sidecar: text + timecode). Matching is a plain
// substring over folded text (case + accents). Every hit carries what its jump needs (page + block, element,
// PDF page + view path, video frame + anchor key) — the QML search bar does the jumping.
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantList>

class CanvasDocument;

class SearchEngine : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool indexing READ indexing NOTIFY indexingChanged)   // a PDF's text is being extracted
public:
    explicit SearchEngine(QObject* parent = nullptr) : QObject(parent) {}
    void setCanvas(CanvasDocument* c);
    bool indexing() const { return pending_ > 0; }

    // The hits for `query` (≥ 1 char after trimming), the current page's first. Each hit is a map:
    //   kind: "page" | "card" | "pdf" | "video"; title (the page's / card's name); snippet (StyledText,
    //   the match in <b>); pageId, blockId (page / inline media hits); elementId (canvas hits);
    //   col, len (a page hit's match in the block's text); page (a PDF page index); viewPath (the PDF's local
    //   path = the editor's pdf key); frame + timecode + key (a video note; key = the player's anchor path).
    // PDFs without a cached text are queued for extraction; `cacheUpdated` fires when one lands (re-run the search).
    Q_INVOKABLE QVariantList search(const QString& query, const QString& currentPage);
    // Tests / tooling: extract every un-cached PDF's text NOW on this thread.
    Q_INVOKABLE void indexPdfsNow();

    // Case + accent folding (NFD, marks dropped, case-folded).
    static QString fold(const QString& s);
    // A PDF's page texts (Qt PDF), empty when it can't be loaded. Safe on a worker thread.
    static QStringList pdfPageTexts(const QString& path);

signals:
    void indexingChanged();
    void cacheUpdated();

private:
    struct MediaRef { QString kind, srcKey, path, pageId, blockId, elementId, title; QVariant src; };
    std::vector<MediaRef> mediaRefs() const;
    void schedulePdf(const MediaRef& m, qint64 mtime, qint64 size);
    void pdfDone(const QString& srcKey, qint64 mtime, qint64 size, const QStringList& pages);

    QPointer<CanvasDocument> canvas_;
    int pending_ = 0;
    QSet<QString> queued_;
};
