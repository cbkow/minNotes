#include "Importer.h"
#include "MediaStore.h"
#include "TableGrid.h"
#include <QTextDocument>
#include <QTextBlock>
#include <QTextList>
#include <QTextTable>
#include <QTextFragment>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QUrl>
#include <QImage>
#include <QPixmap>
#include <QRegularExpression>
#include <QStringConverter>
#include <functional>

// file:// URL or plain path → plain path (FileDialog and drops hand us URLs).
static QString localPath(const QString& fileUrlOrPath) {
    if (fileUrlOrPath.startsWith(QLatin1String("file:")))
        return QUrl(fileUrlOrPath).toLocalFile();
    return fileUrlOrPath;
}

// Whole-file read + BOM-sniffed decode (UTF-8 when unmarked). Null QString on
// open failure — distinct from an empty file, which imports as an empty doc.
static QString readTextFile(const QString& path, bool* ok) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { *ok = false; return {}; }
    *ok = true;
    const QByteArray raw = f.readAll();
    const auto enc = QStringConverter::encodingForData(raw);
    QStringDecoder dec(enc.value_or(QStringConverter::Utf8));
    return dec.decode(raw);
}

// Tri-state task sentinel: GFM only knows [ ]/[x], so `- [/] ` (the app's
// "doing" marker) is rewritten to an unchecked task tagged with a
// private-use char before setMarkdown, then restored on the spec.
static const QChar kDoingSentinel(0xE0D0);

QString Importer::formatForPath(const QString& fileUrlOrPath) {
    const QString ext = QFileInfo(localPath(fileUrlOrPath)).suffix().toLower();
    if (ext == QLatin1String("md") || ext == QLatin1String("markdown")
        || ext == QLatin1String("mdown"))                                 return QStringLiteral("md");
    if (ext == QLatin1String("txt") || ext == QLatin1String("text")
        || ext == QLatin1String("log"))                                   return QStringLiteral("txt");
    if (ext == QLatin1String("csv"))                                      return QStringLiteral("csv");
    if (ext == QLatin1String("tsv") || ext == QLatin1String("tab"))       return QStringLiteral("tsv");
    if (ext == QLatin1String("html") || ext == QLatin1String("htm"))      return QStringLiteral("html");
    return {};
}

bool Importer::isMultiDocument(const QString& fileUrlOrPath) const {
    Q_UNUSED(fileUrlOrPath);
    return false;   // ENEX / Notion zips arrive in a later phase
}

bool Importer::importFile(const QString& fileUrlOrPath) {
    if (!model_) return false;
    const QString path = localPath(fileUrlOrPath);
    const QString fmt = formatFor(path);
    if (fmt == QLatin1String("md"))   return importMarkdownFile(path, model_);
    if (fmt == QLatin1String("txt"))  return importTextFile(path, model_);
    if (fmt == QLatin1String("csv"))  return importCsvFile(path, model_, false);
    if (fmt == QLatin1String("tsv"))  return importCsvFile(path, model_, true);
    if (fmt == QLatin1String("html")) return importHtmlFile(path, model_);
    return false;
}

bool Importer::importMarkdownFile(const QString& path, BlockModel* m) {
    if (!m || m->rowCountQml() < 1) return false;
    bool ok = false;
    QString text = readTextFile(path, &ok);
    if (!ok) return false;

    static const QRegularExpression doingRe(
        QStringLiteral("^(\\s*(?:[-*+]|\\d+[.)])\\s)\\[/\\]\\s"),
        QRegularExpression::MultilineOption);
    text.replace(doingRe, QStringLiteral("\\1[ ] ") + kDoingSentinel);

    QTextDocument doc;
    doc.setMarkdown(text, QTextDocument::MarkdownDialectGitHub);
    std::vector<BlockModel::BlockSpec> specs =
        specsFromTextDocument(doc, m->mediaStore(), QFileInfo(path).absolutePath());
    for (auto& sp : specs) {
        if (sp.type == BlockModel::TaskListItem && sp.text.startsWith(kDoingSentinel)) {
            sp.text.remove(0, 1);
            for (auto& x : sp.spans) { x.s = std::max(0, x.s - 1); x.e = std::max(0, x.e - 1); }
            sp.taskState = BlockModel::TaskDoing;
        }
    }
    if (specs.empty()) return true;   // empty file → empty doc, not a failure
    const auto [caretRow, caretCol] = m->insertSpecs(0, specs, true);
    Q_UNUSED(caretCol);
    m->localizeRemoteMedia(0, caretRow);
    return true;
}

bool Importer::importTextFile(const QString& path, BlockModel* m) {
    if (!m || m->rowCountQml() < 1) return false;
    bool ok = false;
    const QString text = readTextFile(path, &ok);
    if (!ok) return false;
    if (!text.isEmpty()) m->pasteText(0, 0, text);
    return true;
}

bool Importer::importCsvFile(const QString& path, BlockModel* m, bool tsv) {
    if (!m || m->rowCountQml() < 1) return false;
    bool ok = false;
    const QString text = readTextFile(path, &ok);
    if (!ok) return false;
    const TableGrid g = tsv ? TableGrid::fromTSV(text) : TableGrid::fromCSV(text);
    if (g.rows() < 1 || g.cols() < 1) return true;   // empty file → empty doc
    BlockModel::BlockSpec sp;
    sp.type = BlockModel::Table;
    sp.tableJson = g.toJson();
    m->insertSpecs(0, {sp}, true);
    return true;
}

bool Importer::importHtmlFile(const QString& path, BlockModel* m) {
    if (!m || m->rowCountQml() < 1) return false;
    bool ok = false;
    const QString html = readTextFile(path, &ok);
    if (!ok) return false;

    QTextDocument doc;
    doc.setHtml(html);
    std::vector<BlockModel::BlockSpec> specs =
        specsFromTextDocument(doc, m->mediaStore(), QFileInfo(path).absolutePath());
    if (specs.empty()) return true;
    const auto [caretRow, caretCol] = m->insertSpecs(0, specs, true);
    Q_UNUSED(caretCol);
    m->localizeRemoteMedia(0, caretRow);
    return true;
}

std::vector<BlockModel::BlockSpec> Importer::specsFromTextDocument(
        QTextDocument& doc, MediaStore* store, const QString& baseDir) {
    using Spec = BlockModel::BlockSpec;
    using Span = BlockModel::Span;
    std::vector<Spec> specs;

    // Consecutive code-line blocks (each source line is its own QTextBlock)
    // coalesce into ONE Code spec. Any non-code block breaks the chain, so
    // separate fences stay separate wherever the reader leaves anything between
    // them. Tracked across buildText calls; reset by tables/frames too.
    bool prevCode = false;
    QString prevCodeLang;

    // A QTextBlock → spec(s): divider/code/heading/list/quote/paragraph text
    // (with inline bold/italic/underline/strike/code/link/color spans), PLUS any
    // embedded images as their own Media specs, interleaved in fragment order
    // (so a figure's image and caption land as separate blocks in reading
    // order). Image bytes come from the QTextDocument resource cache (data:
    // URIs decode there); local file srcs are referenced in place, relative
    // ones resolved against baseDir. Remote http(s) images are left for the
    // caller's async localize pass.
    auto buildText = [&](const QTextBlock& b) {
        const QTextBlockFormat bf = b.blockFormat();

        if (bf.hasProperty(QTextFormat::BlockTrailingHorizontalRulerWidth)) {
            prevCode = false;
            Spec sp; sp.type = BlockModel::Divider;
            specs.push_back(std::move(sp));
            return;
        }

        // Code first: fenced/indented markdown carries BlockCodeLanguage
        // (possibly ""), <pre> carries only nonBreakableLines. Text is taken
        // VERBATIM (no trim, blank lines survive inside a block).
        if (bf.hasProperty(QTextFormat::BlockCodeLanguage) || bf.nonBreakableLines()) {
            const QString lang = bf.property(QTextFormat::BlockCodeLanguage).toString();
            QString line = b.text();
            line.replace(QChar(0xFFFC), QString());
            if (prevCode && prevCodeLang == lang
                && !specs.empty() && specs.back().type == BlockModel::Code) {
                specs.back().text += QLatin1Char('\n') + line;
            } else {
                Spec sp; sp.type = BlockModel::Code; sp.lang = lang; sp.text = line;
                specs.push_back(std::move(sp));
            }
            prevCode = true; prevCodeLang = lang;
            return;
        }
        prevCode = false;

        const int hl = bf.headingLevel();
        uint8_t btype = BlockModel::Paragraph, blevel = 0, btask = 0, bdepth = 0;
        if (hl >= 1 && hl <= 6) { btype = BlockModel::Heading; blevel = static_cast<uint8_t>(hl); }
        else if (QTextList* list = b.textList()) {
            // GFM task items parse with a checkbox marker; plain bullets have
            // none. Marker is ONLY meaningful under textList() — the property
            // leaks onto following paragraphs (Qt 6.11 reader quirk).
            const QTextBlockFormat::MarkerType mk = bf.marker();
            if (mk == QTextBlockFormat::MarkerType::Checked)        { btype = BlockModel::TaskListItem; btask = BlockModel::TaskDone; }
            else if (mk == QTextBlockFormat::MarkerType::Unchecked) { btype = BlockModel::TaskListItem; btask = BlockModel::TaskTodo; }
            else {
                switch (list->format().style()) {
                case QTextListFormat::ListDecimal:
                case QTextListFormat::ListLowerAlpha:
                case QTextListFormat::ListUpperAlpha:
                case QTextListFormat::ListLowerRoman:
                case QTextListFormat::ListUpperRoman:
                    btype = BlockModel::OrderedListItem; break;
                default:
                    btype = BlockModel::ListItem; break;
                }
            }
            bdepth = static_cast<uint8_t>(std::clamp(list->format().indent() - 1,
                                                     0, BlockModel::kMaxListDepth));
        }
        else if (bf.property(QTextFormat::BlockQuoteLevel).toInt() > 0) {
            btype = BlockModel::Quote;   // nesting flattens; app quotes have no depth
        }

        QString text; std::vector<Span> spans;
        auto flushText = [&]() {
            if (!text.trimmed().isEmpty()) {
                Spec sp; sp.type = btype; sp.level = blevel; sp.taskState = btask;
                sp.depth = bdepth; sp.text = text; sp.spans = spans;
                specs.push_back(std::move(sp));
            }
            text.clear(); spans.clear();
        };

        for (auto it = b.begin(); !it.atEnd(); ++it) {
            const QTextFragment frag = it.fragment();
            if (!frag.isValid()) continue;
            const QTextCharFormat cf = frag.charFormat();
            if (cf.isImageFormat() && store) {            // embedded image → Media spec
                const QString src = cf.toImageFormat().name();
                const QVariant res = doc.resource(QTextDocument::ImageResource, QUrl(src));
                MediaStore::ImageRef ref;
                if (res.canConvert<QImage>())       ref = store->importImage(res.value<QImage>());
                else if (res.canConvert<QPixmap>()) ref = store->importImage(res.value<QPixmap>().toImage());
                else if (!src.startsWith(QLatin1String("http"))) {
                    QString path = src.startsWith(QLatin1String("file:"))
                                       ? QUrl(src).toLocalFile() : src;
                    if (!baseDir.isEmpty() && QFileInfo(path).isRelative())
                        path = QDir(baseDir).filePath(path);
                    ref = store->importFile(path);
                }
                if (ref.ok()) {
                    flushText();
                    Spec sp; sp.type = BlockModel::Media;
                    sp.mediaJson = MediaStore::imageDescriptorJson(ref);
                    specs.push_back(std::move(sp));
                } else if (src.startsWith(QLatin1String("http"))) {   // remote → fetched after insert
                    flushText();
                    Spec sp; sp.type = BlockModel::Media;
                    sp.mediaJson = MediaStore::remoteImageDescriptorJson(src);
                    specs.push_back(std::move(sp));
                }
                continue;
            }
            QString t = frag.text();
            t.replace(QChar(0xFFFC), QString());          // stray object-replacement
            if (t.isEmpty()) continue;
            const int s = text.size();
            text += t;
            const int e = text.size();
            if (cf.isAnchor() && !cf.anchorHref().isEmpty()) {
                spans.push_back({s, e, BlockModel::SpanLink, cf.anchorHref()});
            } else {
                if (cf.fontWeight() >= QFont::Bold) spans.push_back({s, e, BlockModel::SpanBold, {}});
                if (cf.fontItalic())                spans.push_back({s, e, BlockModel::SpanItalic, {}});
                if (cf.fontUnderline())             spans.push_back({s, e, BlockModel::SpanUnderline, {}});
                if (cf.fontFixedPitch())            spans.push_back({s, e, BlockModel::SpanCode, {}});
                // Colors carry only when explicit AND not default ink: browsers
                // stamp explicit black fg / white bg on whole documents, which
                // would pin theme-hostile spans across entire pastes.
                if (cf.hasProperty(QTextFormat::ForegroundBrush)) {
                    const QColor c = cf.foreground().color();
                    if (c.alpha() > 0 && c != QColor(Qt::black))
                        spans.push_back({s, e, BlockModel::SpanFgColor, c.name()});
                }
                if (cf.hasProperty(QTextFormat::BackgroundBrush)) {
                    const QColor c = cf.background().color();
                    if (c.alpha() > 0 && c != QColor(Qt::white))
                        spans.push_back({s, e, BlockModel::SpanHighlight, c.name()});
                }
            }
            if (cf.fontStrikeOut())                 spans.push_back({s, e, BlockModel::SpanStrike, {}});
        }
        flushText();
    };

    // A QTextTable → Table block spec (cell text joined per cell; <thead> rows
    // become the grid's header rows).
    auto buildTable = [&](QTextTable* t) {
        prevCode = false;
        const int nr = t->rows(), nc = t->columns();
        if (nr < 1 || nc < 1) return;
        TableGrid g = TableGrid::makeEmpty(nr, nc);
        g.setHeaderRows(std::clamp(t->format().headerRowCount(), 0, nr));
        for (int r = 0; r < nr; ++r)
            for (int c = 0; c < nc; ++c) {
                QTextTableCell cell = t->cellAt(r, c);
                if (!cell.isValid()) continue;
                QString ct;
                for (auto bit = cell.begin(); !bit.atEnd(); ++bit) {
                    const QTextBlock cb = bit.currentBlock();
                    if (!cb.isValid()) continue;
                    QString bt = cb.text(); bt.replace(QChar(0xFFFC), QString());
                    if (bt.isEmpty()) continue;
                    if (!ct.isEmpty()) ct += QLatin1Char(' ');
                    ct += bt;
                }
                g.setCellText(r, c, ct);
            }
        Spec sp; sp.type = BlockModel::Table; sp.tableJson = g.toJson();
        specs.push_back(std::move(sp));
    };

    // Walk the frame tree so table cells aren't flattened into loose blocks.
    std::function<void(QTextFrame*)> walk = [&](QTextFrame* frame) {
        for (auto it = frame->begin(); !it.atEnd(); ++it) {
            if (QTextFrame* child = it.currentFrame()) {
                if (QTextTable* tbl = qobject_cast<QTextTable*>(child)) buildTable(tbl);
                else { prevCode = false; walk(child); }
            } else {
                const QTextBlock block = it.currentBlock();
                if (block.isValid()) buildText(block);
            }
        }
    };
    walk(doc.rootFrame());
    return specs;
}
