#include "SearchEngine.h"
#include "CanvasDocument.h"
#include "Document.h"
#include "MediaStore.h"
#include "../notes/annotation_io.h"
#include "../notes/annotation_note.h"
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPdfDocument>
#include <QPdfSelection>
#include <QRunnable>
#include <QThreadPool>
#include <algorithm>

namespace {

QString esc(const QString& s) {
    QString o = s;
    o.replace(QLatin1Char('&'), QLatin1String("&amp;")).replace(QLatin1Char('<'), QLatin1String("&lt;")).replace(QLatin1Char('>'), QLatin1String("&gt;"));
    return o;
}

// The snippet around a match in `text` (folded positions == original positions: folding drops only combining
// marks, which `fold` keeps aligned by replacing them with nothing — see fold()).
QString snippetFor(const QString& text, int at, int len) {
    const int radius = 36;
    int lo = std::max(0, at - radius), hi = std::min(int(text.size()), at + len + radius);
    QString s = (lo > 0 ? QStringLiteral("…") : QString()) + esc(text.mid(lo, at - lo)) + QStringLiteral("<b>") + esc(text.mid(at, len)) + QStringLiteral("</b>")
              + esc(text.mid(at + len, hi - at - len)) + (hi < text.size() ? QStringLiteral("…") : QString());
    s.replace(QLatin1Char('\n'), QLatin1Char(' '));
    return s;
}

QString srcKeyOf(const QJsonValue& src) {
    return src.isString() ? src.toString() : QString::fromUtf8(QJsonDocument(src.toObject()).toJson(QJsonDocument::Compact));
}

} // namespace

QString SearchEngine::fold(const QString& s) {
    // NFD, then every combining mark becomes a placeholder of ZERO width is not possible in a QString — instead keep
    // the string length-aligned by mapping each original char to its folded base char (one char in, one char out),
    // so a match's position in the folded text is its position in the original.
    QString out; out.reserve(s.size());
    for (const QChar ch : s) {
        const QString d = QString(ch).normalized(QString::NormalizationForm_D);
        QChar base = d.isEmpty() ? ch : d.at(0);
        if (base.category() == QChar::Mark_NonSpacing) base = ch;
        out += base.toCaseFolded();
    }
    return out;
}

QStringList SearchEngine::pdfPageTexts(const QString& path) {
    QStringList out;
    QPdfDocument doc;
    if (doc.load(path) != QPdfDocument::Error::None) return out;
    for (int p = 0; p < doc.pageCount(); ++p) out << doc.getAllText(p).text();
    return out;
}

void SearchEngine::setCanvas(CanvasDocument* c) {
    if (canvas_ == c) return;
    canvas_ = c;
    queued_.clear();
}

std::vector<SearchEngine::MediaRef> SearchEngine::mediaRefs() const {
    std::vector<MediaRef> out;
    if (!canvas_ || !canvas_->documentOpen()) return out;
    const Document& db = canvas_->db();
    for (const QString& pid : canvas_->pageIds()) {
        for (const Document::RawBlock& b : db.rawBlocksOfPage(pid)) {
            if (b.type != QLatin1String("media")) continue;
            const QJsonObject md = QJsonDocument::fromJson(b.content.toUtf8()).object();
            const QString kind = md.value(QStringLiteral("kind")).toString();
            if (kind != QLatin1String("video") && kind != QLatin1String("pdf")) continue;
            MediaRef m; m.kind = kind; m.pageId = pid; m.blockId = b.id; m.src = md.value(QStringLiteral("src")).toVariant();
            m.srcKey = srcKeyOf(md.value(QStringLiteral("src"))); m.path = canvas_->resolveMediaPath(m.src);
            m.title = md.value(QStringLiteral("name")).toString();
            if (m.title.isEmpty()) m.title = QFileInfo(m.path).fileName();
            out.push_back(m);
        }
    }
    for (const Document::Element& e : canvas_->elements()) {
        if (e.kind != QLatin1String("media")) continue;
        const QJsonObject md = QJsonDocument::fromJson(e.json.toUtf8()).object().value(QStringLiteral("media")).toObject();
        const QString kind = md.value(QStringLiteral("kind")).toString();
        if (kind != QLatin1String("video") && kind != QLatin1String("pdf")) continue;
        MediaRef m; m.kind = kind; m.elementId = e.id; m.src = md.value(QStringLiteral("src")).toVariant();
        m.srcKey = srcKeyOf(md.value(QStringLiteral("src"))); m.path = canvas_->resolveMediaPath(m.src);
        m.title = canvas_->mediaElementName(e.id);
        out.push_back(m);
    }
    return out;
}

QVariantList SearchEngine::search(const QString& queryIn, const QString& currentPage) {
    QVariantList out;
    const QString needle = fold(queryIn.trimmed());
    if (needle.isEmpty() || !canvas_ || !canvas_->documentOpen()) return out;
    CanvasDocument* c = canvas_;
    const Document& db = c->db();
    auto hit = [&](const char* kind, const QString& title, const QString& snippet) {
        QVariantMap h; h.insert(QStringLiteral("kind"), QLatin1String(kind)); h.insert(QStringLiteral("title"), title); h.insert(QStringLiteral("snippet"), snippet);
        return h;
    };
    auto find = [&](const QString& text) { return fold(text).indexOf(needle); };

    // Pages: the current page first, then the rest in canvas order. Media blocks are skipped here (their names
    // are card-like but they live inline; videos/PDFs get their own sources below).
    QStringList pages = c->pageIds();
    if (!currentPage.isEmpty() && pages.removeAll(currentPage) > 0) pages.prepend(currentPage);
    std::vector<Document::RawBlock> blockCache;   // every page's rows, for the comment anchors below
    for (const QString& pid : pages) {
        const QString title = c->pageTitle(pid);
        for (const Document::RawBlock& b : db.rawBlocksOfPage(pid)) {
            blockCache.push_back(b);
            if (b.type == QLatin1String("media") || b.type == QLatin1String("divider")) continue;
            const int at = find(b.content);
            if (at < 0) continue;
            QVariantMap h = hit("page", title, snippetFor(b.content, at, needle.size()));
            h.insert(QStringLiteral("pageId"), pid); h.insert(QStringLiteral("blockId"), b.id);
            h.insert(QStringLiteral("col"), at); h.insert(QStringLiteral("len"), int(needle.size()));
            out << h;
        }
    }

    // Cards: what the canvas prints on them.
    for (const Document::Element& e : c->elements()) {
        if (e.kind == QLatin1String("page")) continue;
        const QJsonObject o = QJsonDocument::fromJson(e.json.toUtf8()).object();
        QStringList fields; QString title;
        if (e.kind == QLatin1String("media")) {
            const QJsonObject md = o.value(QStringLiteral("media")).toObject();
            title = c->mediaElementName(e.id);
            fields << title;
            const QString kind = md.value(QStringLiteral("kind")).toString(QStringLiteral("image"));
            if (kind == QLatin1String("bookmark")) {
                fields << md.value(QStringLiteral("title")).toString() << md.value(QStringLiteral("host")).toString() << md.value(QStringLiteral("src")).toString();
            } else {
                const QString path = c->elementLocalPath(e.id);
                if (!path.isEmpty()) fields << path;
                for (const QJsonValue& v : md.value(QStringLiteral("entries")).toArray()) fields << v.toString();
            }
        } else if (e.kind == QLatin1String("shape") && o.value(QStringLiteral("shape")).toString() == QLatin1String("text")) {
            title = QStringLiteral("Text");
            fields << o.value(QStringLiteral("text")).toString();
        } else continue;
        for (const QString& f : fields) {
            if (f.isEmpty()) continue;
            const int at = find(f);
            if (at < 0) continue;
            QVariantMap h = hit("card", title, snippetFor(f, at, needle.size()));
            h.insert(QStringLiteral("elementId"), e.id);
            out << h;
            break;   // one hit per card
        }
    }

    // PDFs (cached page text; un-cached ones queue for extraction) and video notes (the QCView sidecars).
    for (const MediaRef& m : mediaRefs()) {
        if (m.path.isEmpty()) continue;
        if (m.kind == QLatin1String("pdf")) {
            const QFileInfo fi(m.path);
            if (!fi.exists()) continue;
            const qint64 mtime = fi.lastModified().toMSecsSinceEpoch(), size = fi.size();
            const QStringList texts = db.pdfTextCache(m.srcKey, mtime, size);
            if (texts.isEmpty()) { schedulePdf(m, mtime, size); continue; }
            for (int p = 0; p < texts.size(); ++p) {
                const int at = find(texts.at(p));
                if (at < 0) continue;
                QVariantMap h = hit("pdf", QStringLiteral("%1 · page %2").arg(m.title).arg(p + 1), snippetFor(texts.at(p), at, needle.size()));
                h.insert(QStringLiteral("page"), p); h.insert(QStringLiteral("viewPath"), m.path);
                h.insert(QStringLiteral("pageId"), m.pageId); h.insert(QStringLiteral("blockId"), m.blockId); h.insert(QStringLiteral("elementId"), m.elementId);
                out << h;
            }
        } else {
            std::vector<qcv::AnnotationNote> notes;
            if (!qcv::annotation_io::loadNotes(notes, m.path)) continue;
            for (const qcv::AnnotationNote& n : notes) {
                const int at = find(n.text);
                if (at < 0) continue;
                QVariantMap h = hit("video", QStringLiteral("%1 · %2").arg(m.title, n.timecode), snippetFor(n.text, at, needle.size()));
                h.insert(QStringLiteral("frame"), n.frame); h.insert(QStringLiteral("timecode"), n.timecode);
                h.insert(QStringLiteral("key"), c->mediaAnchorPath(m.src));
                h.insert(QStringLiteral("pageId"), m.pageId); h.insert(QStringLiteral("blockId"), m.blockId); h.insert(QStringLiteral("elementId"), m.elementId);
                out << h;
            }
        }
    }

    // Comments: the message text; the hit lands on the block anchoring the thread (the first page mentioning it).
    for (const auto& [tid, body] : db.allCommentMessages()) {
        const int at = find(body);
        if (at < 0) continue;
        QString pageId, blockId;
        for (const Document::RawBlock& b : blockCache) if (b.attrs.contains(tid)) { pageId = b.pageId; blockId = b.id; break; }
        if (pageId.isEmpty()) continue;   // an orphan thread (its text is gone)
        QVariantMap h = hit("comment", c->pageTitle(pageId), snippetFor(body, at, needle.size()));
        h.insert(QStringLiteral("threadId"), tid); h.insert(QStringLiteral("pageId"), pageId); h.insert(QStringLiteral("blockId"), blockId);
        out << h;
    }
    return out;
}

void SearchEngine::schedulePdf(const MediaRef& m, qint64 mtime, qint64 size) {
    const QString qkey = m.srcKey + QLatin1Char('|') + QString::number(mtime) + QLatin1Char('|') + QString::number(size);
    if (queued_.contains(qkey)) return;
    queued_.insert(qkey);
    ++pending_; emit indexingChanged();
    class Job : public QRunnable {
    public:
        Job(SearchEngine* e, QString path, QString srcKey, qint64 mtime, qint64 size) : e_(e), path_(std::move(path)), srcKey_(std::move(srcKey)), mtime_(mtime), size_(size) {}
        void run() override {
            const QStringList pages = SearchEngine::pdfPageTexts(path_);
            QMetaObject::invokeMethod(e_, [e = e_, k = srcKey_, m = mtime_, s = size_, pages] { if (e) e->pdfDone(k, m, s, pages); }, Qt::QueuedConnection);
        }
    private:
        QPointer<SearchEngine> e_; QString path_, srcKey_; qint64 mtime_, size_;
    };
    QThreadPool::globalInstance()->start(new Job(this, m.path, m.srcKey, mtime, size));
}

void SearchEngine::pdfDone(const QString& srcKey, qint64 mtime, qint64 size, const QStringList& pages) {
    --pending_; emit indexingChanged();
    if (!canvas_ || !canvas_->documentOpen()) return;
    canvas_->db().setPdfTextCache(srcKey, mtime, size, pages.isEmpty() ? QStringList{QString()} : pages);   // an unreadable PDF caches as one empty page (no re-extract loop)
    emit cacheUpdated();
}

void SearchEngine::indexPdfsNow() {
    if (!canvas_ || !canvas_->documentOpen()) return;
    for (const MediaRef& m : mediaRefs()) {
        if (m.kind != QLatin1String("pdf") || m.path.isEmpty()) continue;
        const QFileInfo fi(m.path);
        if (!fi.exists()) continue;
        const qint64 mtime = fi.lastModified().toMSecsSinceEpoch(), size = fi.size();
        if (!canvas_->db().pdfTextCache(m.srcKey, mtime, size).isEmpty()) continue;
        const QStringList pages = pdfPageTexts(m.path);
        canvas_->db().setPdfTextCache(m.srcKey, mtime, size, pages.isEmpty() ? QStringList{QString()} : pages);
    }
    emit cacheUpdated();
}
