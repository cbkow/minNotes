#include "Importer.h"
#include "DocxReader.h"
#include "XlsxReader.h"
#include "OdfReader.h"
#include "MediaStore.h"
#include "RtfConvert.h"
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
#include <QCryptographicHash>
#include <QDirIterator>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QPointer>
#include <QUuid>
#include <QXmlStreamReader>
#include "PackageFormat.h"
#include <functional>
#include <algorithm>
#include <map>

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

// Source-code import (2026-08-20): the extension IS the stored lang —
// resolveCodeDefinition's definitionForFileName("f."+lang) path resolves
// bare extensions, so no name mapping is needed here.
static bool isCodeExtension(const QString& ext) {
    static const QSet<QString> exts = {
        QStringLiteral("py"),   QStringLiteral("js"),    QStringLiteral("ts"),
        QStringLiteral("jsx"),  QStringLiteral("tsx"),   QStringLiteral("c"),
        QStringLiteral("cc"),   QStringLiteral("cpp"),   QStringLiteral("cxx"),
        QStringLiteral("h"),    QStringLiteral("hpp"),   QStringLiteral("m"),
        QStringLiteral("mm"),   QStringLiteral("cs"),    QStringLiteral("java"),
        QStringLiteral("kt"),   QStringLiteral("swift"), QStringLiteral("go"),
        QStringLiteral("rs"),   QStringLiteral("rb"),    QStringLiteral("php"),
        QStringLiteral("sh"),   QStringLiteral("bash"),  QStringLiteral("zsh"),
        QStringLiteral("sql"),  QStringLiteral("json"),  QStringLiteral("yaml"),
        QStringLiteral("yml"),  QStringLiteral("toml"),  QStringLiteral("xml"),
        QStringLiteral("ini"),  QStringLiteral("css"),   QStringLiteral("scss"),
        QStringLiteral("lua"),  QStringLiteral("pl"),    QStringLiteral("r"),
        QStringLiteral("cmake"), QStringLiteral("ps1"),
    };
    return exts.contains(ext);
}

QString Importer::formatForPath(const QString& fileUrlOrPath) {
    const QString path = localPath(fileUrlOrPath);
    const QString ext = QFileInfo(path).suffix().toLower();
    if (ext == QLatin1String("md") || ext == QLatin1String("markdown")
        || ext == QLatin1String("mdown"))                                 return QStringLiteral("md");
    if (ext == QLatin1String("txt") || ext == QLatin1String("text")
        || ext == QLatin1String("log"))                                   return QStringLiteral("txt");
    if (ext == QLatin1String("csv"))                                      return QStringLiteral("csv");
    if (ext == QLatin1String("tsv") || ext == QLatin1String("tab"))       return QStringLiteral("tsv");
    if (ext == QLatin1String("html") || ext == QLatin1String("htm"))      return QStringLiteral("html");
    if (ext == QLatin1String("docx"))                                     return QStringLiteral("docx");
    if (ext == QLatin1String("rtf") && mn::rtfImportSupported())          return QStringLiteral("rtf");
    if (ext == QLatin1String("enex"))                                     return QStringLiteral("enex");
    // Office containers (2026-08-20). These are ZIPs — they MUST match
    // before the Notion zip sniff below, which would happily claim any
    // archive containing a .csv entry.
    if (ext == QLatin1String("xlsx"))                                     return QStringLiteral("xlsx");
    if (ext == QLatin1String("ods"))                                      return QStringLiteral("ods");
    if (ext == QLatin1String("odt"))                                      return QStringLiteral("odt");
    // Source-code files → one syntax-colored Code block (ruling 2026-08-20;
    // they landed as file chips before).
    if (isCodeExtension(ext))                                             return QStringLiteral("code");
    if (ext == QLatin1String("zip")) {
        // Only NOTION-shaped zips import (md/csv entries in the central
        // directory — one cheap read); any other zip stays a file chip.
        const auto entries = mnpkg::entrySizes(path);
        for (auto it = entries.constBegin(); it != entries.constEnd(); ++it)
            if (it.key().endsWith(QLatin1String(".md"), Qt::CaseInsensitive)
                || it.key().endsWith(QLatin1String(".csv"), Qt::CaseInsensitive))
                return QStringLiteral("notion");
        return {};
    }
    return {};
}

bool Importer::isMultiDocument(const QString& fileUrlOrPath) const {
    const QString fmt = formatForPath(fileUrlOrPath);
    return fmt == QLatin1String("enex") || fmt == QLatin1String("notion");
}

QVariantMap Importer::importToFolder(const QString& fileUrlOrPath,
                                     const QString& destDirUrlOrPath) {
    QVariantMap out;
    const QString path = localPath(fileUrlOrPath);
    const QString destDir = localPath(destDirUrlOrPath);
    QString firstPath;
    int count = 0;
    const QString fmt = formatForPath(path);
    if (fmt == QLatin1String("enex"))
        count = importEnexToFolder(path, destDir, &firstPath);
    else if (fmt == QLatin1String("notion"))
        count = importNotionZipToFolder(path, destDir, &firstPath);
    out.insert(QStringLiteral("ok"), count > 0);
    out.insert(QStringLiteral("count"), count);
    out.insert(QStringLiteral("firstPath"), firstPath);
    out.insert(QStringLiteral("error"),
               count > 0 ? QString() : QStringLiteral("Nothing importable found"));
    return out;
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
    if (fmt == QLatin1String("docx")) return importDocxFile(path, model_);
    if (fmt == QLatin1String("rtf"))  return importRtfFile(path, model_);
    if (fmt == QLatin1String("xlsx")) return importXlsxFile(path, model_);
    if (fmt == QLatin1String("ods"))  return importOdsFile(path, model_);
    if (fmt == QLatin1String("odt"))  return importOdtFile(path, model_);
    if (fmt == QLatin1String("code")) return importCodeFile(path, model_);
    return false;
}

bool Importer::importRtfFile(const QString& path, BlockModel* m) {
    if (!m || m->rowCountQml() < 1) return false;
    return applySpecs(m, buildFileSpecs(path, QStringLiteral("rtf"), m->mediaStore()));
}

// Build one file's spec output — worker-safe: file IO + MediaStore's const
// import seams only, never the model.
Importer::FileSpecs Importer::buildFileSpecs(const QString& path, const QString& fmt,
                                             MediaStore* store) {
    FileSpecs out;
    if (fmt == QLatin1String("md")) {
        bool ok = false;
        QString text = readTextFile(path, &ok);
        if (!ok) return out;
        static const QRegularExpression doingRe(
            QStringLiteral("^(\\s*(?:[-*+]|\\d+[.)])\\s)\\[/\\]\\s"),
            QRegularExpression::MultilineOption);
        text.replace(doingRe, QStringLiteral("\\1[ ] ") + kDoingSentinel);
        QTextDocument doc;
        doc.setMarkdown(text, QTextDocument::MarkdownDialectGitHub);
        out.specs = specsFromTextDocument(doc, store, QFileInfo(path).absolutePath());
        BlockModel::promoteGridRuns(out.specs);   // a file's table always has a head (R-I4: first row when none detected)
        for (auto& sp : out.specs) {
            if (sp.type == BlockModel::TaskListItem && sp.text.startsWith(kDoingSentinel)) {
                sp.text.remove(0, 1);
                for (auto& x : sp.spans) { x.s = std::max(0, x.s - 1); x.e = std::max(0, x.e - 1); }
                sp.taskState = BlockModel::TaskDoing;
            }
        }
        out.ok = true;
    } else if (fmt == QLatin1String("html")) {
        bool ok = false;
        const QString html = readTextFile(path, &ok);
        if (!ok) return out;
        QTextDocument doc;
        doc.setHtml(html);
        out.specs = specsFromTextDocument(doc, store, QFileInfo(path).absolutePath(), htmlHeaderRowsPerTable(html));
        BlockModel::promoteGridRuns(out.specs);
        out.ok = true;
    } else if (fmt == QLatin1String("csv") || fmt == QLatin1String("tsv")) {
        bool ok = false;
        const QString text = readTextFile(path, &ok);
        if (!ok) return out;
        TableGrid g = fmt == QLatin1String("tsv") ? TableGrid::fromTSV(text)
                                                  : TableGrid::fromCSV(text);
        g.trimTrailingEmpty();   // "a,b,,,," padding never reaches the doc
        if (g.rows() >= 1 && g.cols() >= 1)   // the grid IR → a table's records and cells (S10)
            for (BlockModel::BlockSpec& sp : BlockModel::tableSpecsFromGrid(g.toJson())) out.specs.push_back(std::move(sp));
        out.ok = true;
    } else if (fmt == QLatin1String("docx")) {
        DocxReader::Result res = DocxReader::read(path, store);
        if (!res.ok) return out;
        out.specs = std::move(res.specs);
        BlockModel::promoteGridRuns(out.specs);   // no w:tblHeader → the first row heads the table (R-I4 4b)
        out.comments = std::move(res.comments);
        out.ok = true;
    } else if (fmt == QLatin1String("rtf")) {
        const QString html = mn::rtfFileToHtml(path);
        if (html.isEmpty()) return out;
        QTextDocument doc;
        doc.setHtml(html);
        out.specs = specsFromTextDocument(doc, store, QFileInfo(path).absolutePath(), htmlHeaderRowsPerTable(html));
        BlockModel::promoteGridRuns(out.specs);
        // Cocoa's writer leaves trailing spaces on paragraphs — trim them
        // (and clamp spans), code blocks excepted.
        for (auto& sp : out.specs) {
            if (sp.type == BlockModel::Code) continue;
            int len = sp.text.size();
            while (len > 0 && (sp.text.at(len - 1) == QLatin1Char(' ')
                               || sp.text.at(len - 1) == QLatin1Char('\t')))
                --len;
            if (len != sp.text.size()) {
                sp.text.truncate(len);
                for (auto& x : sp.spans) {
                    x.s = std::min(x.s, len);
                    x.e = std::min(x.e, len);
                }
            }
        }
        out.ok = true;
    } else if (fmt == QLatin1String("code")) {
        // Source file → ONE Code block; the lang is the bare extension
        // (resolveCodeDefinition's extension path handles it).
        bool ok = false;
        QString text = readTextFile(path, &ok);
        if (!ok) return out;
        text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"))
            .replace(QLatin1Char('\r'), QLatin1Char('\n'));
        while (text.endsWith(QLatin1Char('\n'))) text.chop(1);
        BlockModel::BlockSpec sp;
        sp.type = BlockModel::Code;
        sp.lang = QFileInfo(path).suffix().toLower();
        sp.text = text;
        out.specs.push_back(std::move(sp));
        out.ok = true;
    } else if (fmt == QLatin1String("xlsx")) {
        out.specs = XlsxReader::read(path, store);
        out.ok = !out.specs.empty();
    } else if (fmt == QLatin1String("ods")) {
        out.specs = OdfReader::readOds(path);
        out.ok = !out.specs.empty();
    } else if (fmt == QLatin1String("odt")) {
        out.specs = OdfReader::readOdt(path, store);
        out.ok = !out.specs.empty();
    }
    return out;
}

bool Importer::importCodeFile(const QString& path, BlockModel* m) {
    if (!m || m->rowCountQml() < 1) return false;
    return applySpecs(m, buildFileSpecs(path, QStringLiteral("code"), m->mediaStore()));
}
bool Importer::importXlsxFile(const QString& path, BlockModel* m) {
    if (!m || m->rowCountQml() < 1) return false;
    return applySpecs(m, buildFileSpecs(path, QStringLiteral("xlsx"), m->mediaStore()));
}
bool Importer::importOdsFile(const QString& path, BlockModel* m) {
    if (!m || m->rowCountQml() < 1) return false;
    return applySpecs(m, buildFileSpecs(path, QStringLiteral("ods"), m->mediaStore()));
}
bool Importer::importOdtFile(const QString& path, BlockModel* m) {
    if (!m || m->rowCountQml() < 1) return false;
    return applySpecs(m, buildFileSpecs(path, QStringLiteral("odt"), m->mediaStore()));
}

// GUI-thread half: land the specs + comments + async remote localize.
QString Importer::inlineExcelPictures(const QString& html) {
    if (!html.contains(QLatin1String("v:imagedata"))) return html;
    static const QRegularExpression trRe(QStringLiteral("<tr\\b[^>]*>"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression tdRe(QStringLiteral("<t[dh]\\b[^>]*>"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression ptRe(QStringLiteral("(height|width):\\s*([0-9.]+)pt"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression shapeRe(QStringLiteral("<v:shape\\b.*?</v:shape>"),
                                            QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression srcRe(QStringLiteral("v:imagedata[^>]*\\bsrc=\"([^\"]+)\""), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression topRe(QStringLiteral("margin-top:\\s*([0-9.]+)pt"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression leftRe(QStringLiteral("margin-left:\\s*([0-9.]+)pt"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression compositeRe(QStringLiteral("<img\\b[^>]*v:shapes=[^>]*>"), QRegularExpression::CaseInsensitiveOption);
    auto pt = [](const QString& tag, const char* which, double fallback) {
        auto it = ptRe.globalMatch(tag);
        while (it.hasNext()) { const auto m = it.next(); if (m.captured(1).compare(QLatin1String(which), Qt::CaseInsensitive) == 0) return m.captured(2).toDouble(); }
        return fallback;
    };
    // Rows: start offset + height; per row its cells' start offsets + widths.
    struct Row { qsizetype start = 0; double h = 15.0; std::vector<qsizetype> tdStart; std::vector<double> tdW; };
    std::vector<Row> rows;
    for (auto it = trRe.globalMatch(html); it.hasNext();) {
        const auto m = it.next();
        Row r; r.start = m.capturedStart(); r.h = pt(m.captured(0), "height", 15.0);
        rows.push_back(r);
    }
    if (rows.empty()) return html;
    for (auto it = tdRe.globalMatch(html); it.hasNext();) {
        const auto m = it.next();
        size_t ri = 0;
        while (ri + 1 < rows.size() && rows[ri + 1].start < m.capturedStart()) ++ri;
        if (rows[ri].start > m.capturedStart()) continue;   // a cell before the first row
        rows[ri].tdStart.push_back(m.capturedStart());
        rows[ri].tdW.push_back(pt(m.captured(0), "width", 64.0));
    }
    // Pictures → (row, col) → the <img> to inject.
    std::map<std::pair<size_t, size_t>, QString> inject;
    for (auto it = shapeRe.globalMatch(html); it.hasNext();) {
        const auto m = it.next();
        const QString body = m.captured(0);
        const auto src = srcRe.match(body);
        if (!src.hasMatch()) continue;
        size_t ri = 0;
        while (ri + 1 < rows.size() && rows[ri + 1].start < m.capturedStart()) ++ri;
        if (rows[ri].start > m.capturedStart() || rows[ri].tdStart.empty()) continue;
        size_t ci = 0;
        while (ci + 1 < rows[ri].tdStart.size() && rows[ri].tdStart[ci + 1] < m.capturedStart()) ++ci;
        const auto top = topRe.match(body), left = leftRe.match(body);
        double t = top.hasMatch() ? top.captured(1).toDouble() : 0.0;
        double l = left.hasMatch() ? left.captured(1).toDouble() : 0.0;
        while (ri + 1 < rows.size() && t >= rows[ri].h - 0.5) { t -= rows[ri].h; ++ri; }   // down the rows
        const Row& row = rows[ri];
        if (row.tdStart.empty()) continue;
        ci = std::min(ci, row.tdStart.size() - 1);
        while (ci + 1 < row.tdW.size() && l >= row.tdW[ci] - 0.5) { l -= row.tdW[ci]; ++ci; }   // across the columns
        inject[{ri, ci}] += QStringLiteral("<img src=\"%1\">").arg(src.captured(1).toHtmlEscaped());
    }
    if (inject.empty()) return html;
    // Rewrite back to front so offsets stay valid: the composite <img>s go, the real ones land
    // right after their cell's opening tag.
    QString out = html;
    std::vector<std::pair<qsizetype, QString>> edits;
    for (const auto& [rc, img] : inject) {
        const Row& row = rows[rc.first];
        const qsizetype tdStart = row.tdStart[rc.second];
        const qsizetype close = out.indexOf(QLatin1Char('>'), tdStart);
        if (close >= 0) edits.push_back({ close + 1, img });
    }
    std::sort(edits.begin(), edits.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (const auto& [at, img] : edits) out.insert(at, img);
    out.remove(compositeRe);
    return out;
}

bool Importer::applySpecs(BlockModel* m, const FileSpecs& fs) {
    if (!fs.ok || !m) return false;
    if (fs.specs.empty()) return true;   // empty file → empty doc, not a failure
    const std::vector<BlockModel::BlockSpec>& specs = fs.specs;
    const auto [caretRow, caretCol] = m->insertSpecs(0, specs, true);
    Q_UNUSED(caretCol);
    // Reuse folds spec 0 into row 0, the rest follow — spec i is row i (a table's records and cells
    // are specs of their own, so a comment anchored by spec index lands on its block).
    for (const DocxReader::CommentOut& co : fs.comments) {
        if (co.specIndex < 0 || co.specIndex >= static_cast<int>(specs.size())) continue;
        const QString threadId = m->addComment(co.specIndex, co.start, co.end);
        if (threadId.isEmpty()) continue;
        for (const QString& body : co.messages)
            m->addCommentMessage(threadId, body);
    }
    m->localizeRemoteMedia(0, caretRow);
    return true;
}

Importer::~Importer() {
    cancel_ = true;
    if (worker_.joinable()) worker_.join();
}

void Importer::setBusy(bool running, const QString& item) {
    running_ = running;
    currentItem_ = item;
    emit runningChanged();
    emit progressChanged();
}

void Importer::finishOnGui(bool ok, int count, const QString& firstPath,
                           const QString& error) {
    QMetaObject::invokeMethod(this, [this, ok, count, firstPath, error] {
        setBusy(false, QString());
        emit importFinished(ok, count, firstPath, error);
    }, Qt::QueuedConnection);
}

void Importer::startImportFile(const QString& fileUrlOrPath) {
    if (running_ || !model_ || !model_->mediaStore()) return;
    if (worker_.joinable()) worker_.join();
    cancel_ = false;
    const QString path = localPath(fileUrlOrPath);
    const QString fmt = formatForPath(path);
    if (fmt.isEmpty()) {
        emit importFinished(false, 0, {}, QStringLiteral("Unsupported file"));
        return;
    }
    if (fmt == QLatin1String("txt")) {   // trivial read — no worker needed
        const bool ok = importTextFile(path, model_);
        emit importFinished(ok, 1, {}, ok ? QString() : QStringLiteral("Read failed"));
        return;
    }
    setBusy(true, QFileInfo(path).fileName());
    QPointer<BlockModel> target(model_);
    MediaStore* store = model_->mediaStore();   // modal popup pins the tab open
    worker_ = std::thread([this, path, fmt, store, target] {
        const FileSpecs fs = buildFileSpecs(path, fmt, store);
        QMetaObject::invokeMethod(this, [this, fs, target, path] {
            const int cells = fs.ok ? tableCellCount(fs.specs) : 0;
            if (fs.ok && target && cells > kCellCap) {          // over the cap: ask before applying
                pending_ = fs;
                pendingPath_ = path;
                std::vector<BlockModel::BlockSpec> cut = fs.specs;
                truncateToCellCap(cut, kCellCap);
                int rows = 0, kept = 0;
                for (const BlockModel::BlockSpec& sp : fs.specs) if (sp.type == BlockModel::Split && sp.cell < 0) ++rows;
                for (const BlockModel::BlockSpec& sp : cut) if (sp.type == BlockModel::Split && sp.cell < 0) ++kept;
                setBusy(false, QString());
                emit importNeedsDecision(cells, rows, kept, QFileInfo(path).fileName());
                return;
            }
            const bool ok = target ? applySpecs(target, fs) : false;
            setBusy(false, QString());
            emit importFinished(ok, 1, {},
                                ok ? QString() : QStringLiteral("Import failed"));
        }, Qt::QueuedConnection);
    });
}

int Importer::tableCellCount(const std::vector<BlockModel::BlockSpec>& specs) {
    int n = 0;
    for (const BlockModel::BlockSpec& sp : specs) if (sp.cell >= 0) ++n;
    return n;
}

void Importer::truncateToCellCap(std::vector<BlockModel::BlockSpec>& specs, int cap) {
    // Keep specs while the cell count fits; a cut lands at the start of the row that overflowed,
    // so every kept table row is whole. Nothing after the cut survives.
    int cells = 0;
    size_t lastRecord = specs.size();
    for (size_t k = 0; k < specs.size(); ++k) {
        const BlockModel::BlockSpec& sp = specs[k];
        if (sp.type == BlockModel::Split && sp.cell < 0) lastRecord = k;
        if (sp.cell >= 0 && ++cells > cap) {
            specs.resize(lastRecord < specs.size() ? lastRecord : k);
            return;
        }
    }
}

void Importer::resolvePendingImport(const QString& choice) {
    FileSpecs fs = std::move(pending_);
    const QString path = pendingPath_;
    pending_ = FileSpecs();
    pendingPath_.clear();
    if (!model_ || !fs.ok) { emit importFinished(false, 0, {}, QStringLiteral("Import failed")); return; }
    if (choice == QLatin1String("rows")) {
        truncateToCellCap(fs.specs, kCellCap);
        const bool ok = applySpecs(model_, fs);
        emit importFinished(ok, 1, {}, ok ? QString() : QStringLiteral("Import failed"));
    } else if (choice == QLatin1String("attach")) {
        const int row = model_->insertFileFromUrl(std::max(0, model_->rowCountQml() - 1), QUrl::fromLocalFile(path).toString());
        emit importFinished(row >= 0, 1, {}, row >= 0 ? QString() : QStringLiteral("Import failed"));
    } else {
        emit importFinished(false, 0, {}, QStringLiteral("cancelled"));
    }
}

void Importer::startImportToFolder(const QString& fileUrlOrPath,
                                   const QString& destDirUrlOrPath) {
    if (running_) return;
    if (worker_.joinable()) worker_.join();
    cancel_ = false;
    const QString path = localPath(fileUrlOrPath);
    const QString destDir = localPath(destDirUrlOrPath);
    const QString fmt = formatForPath(path);
    if (fmt != QLatin1String("enex") && fmt != QLatin1String("notion")) {
        emit importFinished(false, 0, {}, QStringLiteral("Unsupported file"));
        return;
    }
    setBusy(true, QFileInfo(path).fileName());
    worker_ = std::thread([this, path, destDir, fmt] {
        QString firstPath;
        const FolderProgress progress = [this](int done, const QString& name) {
            QMetaObject::invokeMethod(this, [this, done, name] {
                currentItem_ = QStringLiteral("%1 (%2)").arg(name).arg(done);
                emit progressChanged();
            }, Qt::QueuedConnection);
            return !cancel_.load();
        };
        const int n = fmt == QLatin1String("enex")
            ? importEnexToFolder(path, destDir, &firstPath, progress)
            : importNotionZipToFolder(path, destDir, &firstPath, progress);
        const bool cancelled = cancel_.load();
        finishOnGui(n > 0, n, firstPath,
                    n > 0 ? QString()
                          : (cancelled ? QStringLiteral("Cancelled")
                                       : QStringLiteral("Nothing importable found")));
    });
}

bool Importer::importDocxFile(const QString& path, BlockModel* m) {
    if (!m || m->rowCountQml() < 1) return false;
    return applySpecs(m, buildFileSpecs(path, QStringLiteral("docx"), m->mediaStore()));
}

bool Importer::importMarkdownFile(const QString& path, BlockModel* m) {
    if (!m || m->rowCountQml() < 1) return false;
    return applySpecs(m, buildFileSpecs(path, QStringLiteral("md"), m->mediaStore()));
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
    return applySpecs(m, buildFileSpecs(
        path, tsv ? QStringLiteral("tsv") : QStringLiteral("csv"), m->mediaStore()));
}

bool Importer::importHtmlFile(const QString& path, BlockModel* m) {
    if (!m || m->rowCountQml() < 1) return false;
    return applySpecs(m, buildFileSpecs(path, QStringLiteral("html"), m->mediaStore()));
}

QList<int> Importer::htmlHeaderRowsPerTable(const QString& html) {
    // Leading <tr>s whose cells are all <th> head the table (R-I3 3a); a <thead> is Qt's job.
    // A raw scan, like htmlIsBareRemoteImage: tags only, no parse.
    // Outer tables only (the walker flattens nested ones), so the tags are walked with a depth.
    QList<int> out;
    static const QRegularExpression tagRe(QStringLiteral("<(/?)(table|tr|td|th)\\b"), QRegularExpression::CaseInsensitiveOption);
    int depth = 0, hdr = 0;
    bool counting = false, rowOpen = false, rowAny = false, rowAllTh = true;
    auto closeRow = [&] {
        if (!rowOpen) return;
        rowOpen = false;
        if (counting) {
            if (rowAny && rowAllTh) ++hdr;
            else counting = false;
        }
    };
    auto it = tagRe.globalMatch(html);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const bool close = !m.captured(1).isEmpty();
        const QString tag = m.captured(2).toLower();
        if (tag == QLatin1String("table")) {
            if (!close) {
                if (++depth == 1) { hdr = 0; counting = true; rowOpen = false; }
            } else if (depth > 0) {
                if (depth == 1) { closeRow(); out << hdr; }
                --depth;
            }
            continue;
        }
        if (depth != 1) continue;                       // a nested table's rows aren't the outer table's
        if (tag == QLatin1String("tr")) {
            if (close) closeRow();
            else { closeRow(); rowOpen = true; rowAny = false; rowAllTh = true; }
        } else if (!close && rowOpen) {                 // td / th
            rowAny = true;
            if (tag == QLatin1String("td")) rowAllTh = false;
        }
    }
    return out;
}

std::vector<BlockModel::BlockSpec> Importer::specsFromTextDocument(
        QTextDocument& doc, MediaStore* store, const QString& baseDir, const QList<int>& thHeaders) {
    using Spec = BlockModel::BlockSpec;
    using Span = BlockModel::Span;
    std::vector<Spec> specs;
    std::vector<Spec>* out = &specs;   // buildText writes here: the document, or a table cell's blocks

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
            out->push_back(std::move(sp));
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
                && !out->empty() && out->back().type == BlockModel::Code) {
                out->back().text += QLatin1Char('\n') + line;
            } else {
                Spec sp; sp.type = BlockModel::Code; sp.lang = lang; sp.text = line;
                out->push_back(std::move(sp));
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
                out->push_back(std::move(sp));
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
                    // Notion exports link images with %-encoded relative
                    // paths while the files on disk carry literal spaces —
                    // fall back to the decoded form when the raw one misses.
                    if (!QFileInfo::exists(path)) {
                        const QString dec = QUrl::fromPercentEncoding(path.toUtf8());
                        if (dec != path && QFileInfo::exists(dec)) path = dec;
                    }
                    ref = store->importFile(path);
                }
                if (ref.ok()) {
                    flushText();
                    Spec sp; sp.type = BlockModel::Media;
                    sp.mediaJson = MediaStore::imageDescriptorJson(ref);
                    out->push_back(std::move(sp));
                } else if (src.startsWith(QLatin1String("http"))) {   // remote → fetched after insert
                    flushText();
                    Spec sp; sp.type = BlockModel::Media;
                    sp.mediaJson = MediaStore::remoteImageDescriptorJson(src);
                    out->push_back(std::move(sp));
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

    // A QTextTable → derived-table specs (SR-4 S8e, R-I3): a record per row plus each cell's blocks
    // through the same walker (lists, spans, links, images — 3d). Header rows = <thead> (Qt's
    // headerRowCount) or the raw HTML's <th>-only leading rows (3a); a merged cell's content sits in
    // its origin and the covered positions stay empty (3b); a nested table flattens into one
    // paragraph per inner row, its cells joined by " · " (3c); fixed column widths, cell backgrounds
    // and block alignment carry (4c). The first record's header count is the DETECTED one (0 =
    // none: a paste into a cell fills by position; a file import promotes it, promoteGridRuns).
    // Columns cap at 63 (a cell index is an int8).
    int tableIx = 0;
    auto plainCellText = [](const QTextTableCell& cell) {
        QString txt;
        for (auto bit = cell.begin(); !bit.atEnd(); ++bit) {
            const QTextBlock cb = bit.currentBlock();
            if (!cb.isValid()) continue;
            QString bt = cb.text(); bt.replace(QChar(0xFFFC), QString());
            if (bt.isEmpty()) continue;
            if (!txt.isEmpty()) txt += QLatin1Char(' ');
            txt += bt;
        }
        return txt;
    };
    auto buildTable = [&](QTextTable* t) {
        prevCode = false;
        const int nr = t->rows(), nc = std::min(t->columns(), 63);
        if (nr < 1 || nc < 1) return;
        int hdr = std::clamp(t->format().headerRowCount(), 0, nr);
        if (tableIx < thHeaders.size()) hdr = std::max(hdr, std::clamp(thHeaders.at(tableIx), 0, nr));
        ++tableIx;
        std::vector<int> width(size_t(nc), 0), align(size_t(nc), 0);
        const QList<QTextLength> cons = t->format().columnWidthConstraints();
        for (int c = 0; c < nc; ++c)
            if (c < cons.size() && cons.at(c).type() == QTextLength::FixedLength && cons.at(c).rawValue() > 0)
                width[size_t(c)] = int(std::lround(cons.at(c).rawValue()));
        const size_t nrS = static_cast<size_t>(nr), ncS = static_cast<size_t>(nc);   // not size_t(n): a vexing parse
        std::vector<std::vector<std::vector<Spec>>> cells(nrS, std::vector<std::vector<Spec>>(ncS));
        std::vector<std::vector<QString>> bg(nrS, std::vector<QString>(ncS));
        for (int r = 0; r < nr; ++r)
            for (int c = 0; c < nc; ++c) {
                const QTextTableCell cell = t->cellAt(r, c);
                if (!cell.isValid() || cell.row() != r || cell.column() != c) continue;   // covered by a span: empty
                const QTextCharFormat cf = cell.format();
                if (cf.hasProperty(QTextFormat::BackgroundBrush)) {
                    const QColor bc = cf.background().color();
                    if (bc.alpha() > 0 && bc != QColor(Qt::white)) bg[size_t(r)][size_t(c)] = bc.name();
                }
                if (align[size_t(c)] == 0) {
                    const Qt::Alignment a = cell.firstCursorPosition().block().blockFormat().alignment();
                    if (a & Qt::AlignHCenter) align[size_t(c)] = 1;
                    else if (a & Qt::AlignRight) align[size_t(c)] = 2;
                }
                std::vector<Spec> blocks;
                out = &blocks;
                for (auto it = cell.begin(); !it.atEnd(); ++it) {
                    if (QTextFrame* child = it.currentFrame()) {
                        if (QTextTable* inner = qobject_cast<QTextTable*>(child)) {   // 3c: one paragraph per inner row
                            prevCode = false;
                            for (int ir = 0; ir < inner->rows(); ++ir) {
                                QStringList parts;
                                for (int ic = 0; ic < inner->columns(); ++ic) {
                                    const QTextTableCell ic2 = inner->cellAt(ir, ic);
                                    if (!ic2.isValid() || ic2.row() != ir || ic2.column() != ic) continue;
                                    parts << plainCellText(ic2);
                                }
                                Spec p; p.type = BlockModel::Paragraph; p.text = parts.join(QStringLiteral(" · "));
                                out->push_back(std::move(p));
                            }
                        } else {
                            prevCode = false;
                            for (auto jt = child->begin(); !jt.atEnd(); ++jt) {
                                const QTextBlock b = jt.currentBlock();
                                if (b.isValid()) buildText(b);
                            }
                        }
                    } else {
                        const QTextBlock b = it.currentBlock();
                        if (b.isValid()) buildText(b);
                    }
                }
                out = &specs;
                prevCode = false;
                cells[size_t(r)][size_t(c)] = std::move(blocks);
            }
        QJsonArray cols;
        for (int c = 0; c < nc; ++c) {
            QJsonObject col;
            if (width[size_t(c)] > 0) col.insert(QStringLiteral("w"), width[size_t(c)]);
            if (align[size_t(c)] != 0) col.insert(QStringLiteral("a"), align[size_t(c)]);
            cols.append(col);
        }
        const std::vector<float> equal(size_t(nc), 1.0f / static_cast<float>(nc));
        for (int r = 0; r < nr; ++r) {
            Spec rec;
            rec.type = BlockModel::Split;
            rec.ratios = equal;
            rec.header = r == 0 ? static_cast<uint8_t>(hdr) : uint8_t(0);
            QJsonObject tj;
            if (r == 0) tj.insert(QStringLiteral("cols"), cols);
            QJsonArray cbg;
            for (int c = 0; c < nc; ++c) cbg.append(bg[size_t(r)][size_t(c)]);
            while (!cbg.isEmpty() && cbg.last().toString().isEmpty()) cbg.removeLast();
            if (!cbg.isEmpty()) tj.insert(QStringLiteral("cbg"), cbg);
            rec.table = tj.isEmpty() ? QString() : QString::fromUtf8(QJsonDocument(tj).toJson(QJsonDocument::Compact));
            specs.push_back(std::move(rec));
            for (int c = 0; c < nc; ++c) {
                std::vector<Spec>& blocks = cells[size_t(r)][size_t(c)];
                if (blocks.empty()) { Spec e; e.type = BlockModel::Paragraph; blocks.push_back(std::move(e)); }
                for (Spec& b : blocks) {
                    b.cell = static_cast<int8_t>(c);
                    specs.push_back(std::move(b));
                }
            }
        }
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

// ---------------------------------------------------------------------------
// Multi-document imports (ENEX / Notion) — one .mnd per note/page, written
// into a user-chosen folder (the OS organizes; no vault).
// ---------------------------------------------------------------------------

namespace {

// Filesystem-safe doc name from a note/page title.
QString sanitizeDocName(QString s) {
    static const QString invalid = QStringLiteral("<>:\"/\\|?*");
    for (QChar& c : s)
        if (invalid.contains(c) || c.unicode() < 0x20) c = QLatin1Char('-');
    s = s.trimmed();
    if (s.size() > 80) s = s.left(80).trimmed();
    return s.isEmpty() ? QStringLiteral("Untitled note") : s;
}

// destDir/<base>.mnd, -2/-3… on collision (against both this run and disk).
QString uniqueDocPath(const QString& destDir, const QString& base,
                      QSet<QString>& taken) {
    QString name = base;
    for (int n = 2;
         taken.contains(name.toLower())
             || QFileInfo::exists(destDir + QLatin1Char('/') + name + QStringLiteral(".mnd"));
         ++n)
        name = base + QLatin1Char('-') + QString::number(n);
    taken.insert(name.toLower());
    return destDir + QLatin1Char('/') + name + QStringLiteral(".mnd");
}

// Fresh headless model in the untitled scratch (the regression pattern):
// caller inserts, then saveAs materializes doc + .minnotes into destDir.
void freshDoc(BlockModel& m) {
    m.newDocument();
    while (m.rowCountQml() > 0) m.removeBlock(0);
    m.insertBlock(0);
}

// ENEX sentinels (PUA): en-todo state + non-image resource placeholders
// survive the QTextDocument round-trip as text, then post-process into real
// task states / file-chip specs.
const QChar kEnexTodo(0xE0D1);
const QChar kEnexFileRes(0xE0D2);

struct EnexResource {
    QByteArray bytes;
    QString mime;
    QString fileName;
    QString md5;        // hex — the en-media linkage key
};

// One <note>'s ENML + resources → a saved .mnd. Returns false on write
// failure (malformed notes still produce whatever converted).
bool writeEnexNote(const QString& title, const QString& enml,
                   const QList<EnexResource>& resources,
                   const QString& docPath) {
    // ENML → HTML: drop the XML/DOCTYPE preamble, neutralize <en-note>,
    // inline image media via pre-registered resources, sentinel the rest.
    QString html = enml;
    static const QRegularExpression preamble(
        QStringLiteral("^.*?<en-note[^>]*>"),
        QRegularExpression::DotMatchesEverythingOption);
    html.replace(preamble, QStringLiteral("<div>"));
    html.replace(QLatin1String("</en-note>"), QLatin1String("</div>"));
    static const QRegularExpression todoRe(
        QStringLiteral("<en-todo([^>]*)/?>"),
        QRegularExpression::CaseInsensitiveOption);
    // Replace todos first (checked attr decides the state glyph).
    {
        QRegularExpressionMatchIterator it = todoRe.globalMatch(html);
        QString outHtml;
        int last = 0;
        while (it.hasNext()) {
            const QRegularExpressionMatch mt = it.next();
            outHtml += html.mid(last, mt.capturedStart() - last);
            const bool checked = mt.captured(1).contains(
                QStringLiteral("checked=\"true\""), Qt::CaseInsensitive);
            outHtml += QString(kEnexTodo) + (checked ? QLatin1Char('1') : QLatin1Char('0'));
            last = mt.capturedEnd();
        }
        outHtml += html.mid(last);
        html = outHtml;
    }
    static const QRegularExpression mediaRe(
        QStringLiteral("<en-media([^>]*)/?>(</en-media>)?"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression hashRe(QStringLiteral("hash=\"([0-9a-fA-F]+)\""));
    static const QRegularExpression typeRe(QStringLiteral("type=\"([^\"]+)\""));
    {
        QRegularExpressionMatchIterator it = mediaRe.globalMatch(html);
        QString outHtml;
        int last = 0;
        while (it.hasNext()) {
            const QRegularExpressionMatch mt = it.next();
            outHtml += html.mid(last, mt.capturedStart() - last);
            const QString attrs = mt.captured(1);
            const QString hash = hashRe.match(attrs).captured(1).toLower();
            const QString mime = typeRe.match(attrs).captured(1).toLower();
            if (mime.startsWith(QLatin1String("image/")))
                outHtml += QStringLiteral("<img src=\"enres:%1\">").arg(hash);
            else
                outHtml += QStringLiteral("<p>%1%2</p>").arg(kEnexFileRes).arg(hash);
            last = mt.capturedEnd();
        }
        outHtml += html.mid(last);
        html = outHtml;
    }

    BlockModel m;
    freshDoc(m);

    QTextDocument doc;
    QHash<QString, const EnexResource*> byHash;
    for (const EnexResource& r : resources) {
        byHash.insert(r.md5, &r);
        if (r.mime.startsWith(QLatin1String("image/"))) {
            const QImage img = QImage::fromData(r.bytes);
            if (!img.isNull())
                doc.addResource(QTextDocument::ImageResource,
                                QUrl(QStringLiteral("enres:") + r.md5), img);
        }
    }
    doc.setHtml(html);
    std::vector<BlockModel::BlockSpec> specs =
        Importer::specsFromTextDocument(doc, m.mediaStore(), QString());

    // Post-pass: sentinels → real structures.
    std::vector<BlockModel::BlockSpec> outSpecs;
    {   // title first (H1)
        BlockModel::BlockSpec t;
        t.type = BlockModel::Heading; t.level = 1; t.text = title;
        outSpecs.push_back(std::move(t));
    }
    for (auto& sp : specs) {
        if (!sp.text.isEmpty() && sp.text.at(0) == kEnexFileRes) {
            const QString hash = sp.text.mid(1).trimmed().toLower();
            const EnexResource* r = byHash.value(hash, nullptr);
            if (r && m.mediaStore()) {
                const QString name = r->fileName.isEmpty()
                    ? QStringLiteral("attachment") : r->fileName;
                const QString rel = m.mediaStore()->importBytes(r->bytes, name);
                if (!rel.isEmpty()) {
                    QJsonObject o;
                    o.insert(QStringLiteral("src"), rel);
                    o.insert(QStringLiteral("kind"), QStringLiteral("file"));
                    o.insert(QStringLiteral("name"), name);
                    o.insert(QStringLiteral("ext"), QFileInfo(name).suffix().toLower());
                    BlockModel::BlockSpec fs;
                    fs.type = BlockModel::Media;
                    fs.mediaJson = QString::fromUtf8(
                        QJsonDocument(o).toJson(QJsonDocument::Compact));
                    outSpecs.push_back(std::move(fs));
                }
            }
            continue;   // unmatched placeholder drops (capability map)
        }
        while (!sp.text.isEmpty() && sp.text.at(0) == kEnexTodo) {
            const QChar state = sp.text.size() > 1 ? sp.text.at(1) : QLatin1Char('0');
            sp.text.remove(0, sp.text.size() > 2 && sp.text.at(2) == QLatin1Char(' ') ? 3 : 2);
            for (auto& x : sp.spans) {
                x.s = std::max(0, x.s - 2);
                x.e = std::max(0, x.e - 2);
            }
            sp.type = BlockModel::TaskListItem;
            sp.taskState = state == QLatin1Char('1') ? BlockModel::TaskDone
                                                     : BlockModel::TaskTodo;
            break;
        }
        outSpecs.push_back(std::move(sp));
    }
    m.insertSpecs(0, outSpecs, true);
    const bool ok = m.saveAs(docPath);
    m.closeDocument();
    return ok;
}

// Trailing Notion page id: " <32 hex>" before the extension.
QString stripNotionId(const QString& baseName) {
    static const QRegularExpression idRe(QStringLiteral("\\s+[0-9a-f]{32}$"));
    QString out = baseName;
    out.remove(idRe);
    return out;
}

} // namespace

int Importer::importEnexToFolder(const QString& path, const QString& destDir,
                                 QString* firstPath, const FolderProgress& progress) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return 0;
    QDir().mkpath(destDir);
    QXmlStreamReader xml(&f);
    int count = 0;
    QSet<QString> taken;

    QString title, enml;
    QList<EnexResource> resources;
    EnexResource res;
    bool inNote = false, inResource = false;
    while (!xml.atEnd()) {
        const QXmlStreamReader::TokenType t = xml.readNext();
        if (t == QXmlStreamReader::StartElement) {
            const auto name = xml.name();
            if (name == QLatin1String("note")) {
                inNote = true; title.clear(); enml.clear(); resources.clear();
            } else if (!inNote) {
                continue;
            } else if (name == QLatin1String("title") && !inResource) {
                title = xml.readElementText();
            } else if (name == QLatin1String("content")) {
                enml = xml.readElementText();
            } else if (name == QLatin1String("resource")) {
                inResource = true; res = EnexResource();
            } else if (inResource && name == QLatin1String("data")) {
                res.bytes = QByteArray::fromBase64(
                    xml.readElementText().toLatin1(),
                    QByteArray::Base64Encoding | QByteArray::IgnoreBase64DecodingErrors);
            } else if (inResource && name == QLatin1String("mime")) {
                res.mime = xml.readElementText().toLower();
            } else if (inResource && name == QLatin1String("file-name")) {
                res.fileName = xml.readElementText();
            }
        } else if (t == QXmlStreamReader::EndElement) {
            const auto name = xml.name();
            if (name == QLatin1String("resource") && inResource) {
                res.md5 = QString::fromLatin1(
                    QCryptographicHash::hash(res.bytes, QCryptographicHash::Md5).toHex());
                resources.push_back(res);
                inResource = false;
            } else if (name == QLatin1String("note") && inNote) {
                inNote = false;
                if (progress && !progress(count, sanitizeDocName(title)))
                    return count;   // cancelled between notes
                const QString docPath =
                    uniqueDocPath(destDir, sanitizeDocName(title), taken);
                if (writeEnexNote(sanitizeDocName(title), enml, resources, docPath)) {
                    if (firstPath && firstPath->isEmpty()) *firstPath = docPath;
                    ++count;
                }
            }
        }
    }
    return count;
}

int Importer::importNotionZipToFolder(const QString& path, const QString& destDir,
                                      QString* firstPath, const FolderProgress& progress) {
    QDir().mkpath(destDir);
    const QString stage = BlockModel::scratchDir() + QStringLiteral("/import-")
        + QUuid::createUuid().toString(QUuid::Id128);
    if (!mnpkg::extractArchive(path, stage)) return 0;
    int count = 0;
    QSet<QString> taken;

    // Every .md page becomes a doc (subpages included — flat, per the
    // no-vault direction). The staging dir is under the session scratch —
    // a VOLATILE root, so referenced images copy into each doc's .minnotes
    // via the standard importFile policy.
    QStringList mdFiles, csvFiles;
    QDirIterator it(stage, {QStringLiteral("*.md"), QStringLiteral("*.csv")},
                    QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString p = it.next();
        (p.endsWith(QLatin1String(".md"), Qt::CaseInsensitive) ? mdFiles : csvFiles) << p;
    }
    mdFiles.sort();
    csvFiles.sort();
    for (const QString& md : mdFiles) {
        if (progress && !progress(count, QFileInfo(md).completeBaseName())) {
            QDir(stage).removeRecursively();
            return count;
        }
        BlockModel m;
        freshDoc(m);
        if (importMarkdownFile(md, &m)) {
            const QString docPath = uniqueDocPath(
                destDir, sanitizeDocName(stripNotionId(QFileInfo(md).completeBaseName())),
                taken);
            if (m.saveAs(docPath)) {
                if (firstPath && firstPath->isEmpty()) *firstPath = docPath;
                ++count;
            }
        }
        m.closeDocument();
    }
    for (const QString& csv : csvFiles) {
        if (progress && !progress(count, QFileInfo(csv).completeBaseName())) {
            QDir(stage).removeRecursively();
            return count;
        }
        BlockModel m;
        freshDoc(m);
        if (importCsvFile(csv, &m, false)) {
            const QString docPath = uniqueDocPath(
                destDir, sanitizeDocName(stripNotionId(QFileInfo(csv).completeBaseName())),
                taken);
            if (m.saveAs(docPath)) {
                if (firstPath && firstPath->isEmpty()) *firstPath = docPath;
                ++count;
            }
        }
        m.closeDocument();
    }
    QDir(stage).removeRecursively();
    return count;
}

bool Importer::htmlIsBareRemoteImage(const QString& html) {
    if (html.isEmpty()) return false;
    QTextDocument doc;
    doc.setHtml(html);
    int remoteImages = 0, otherImages = 0;
    for (QTextBlock b = doc.begin(); b.isValid(); b = b.next()) {
        for (auto it = b.begin(); !it.atEnd(); ++it) {
            const QTextFragment frag = it.fragment();
            if (!frag.isValid()) continue;
            const QTextCharFormat cf = frag.charFormat();
            if (cf.isImageFormat()) {
                const QString src = cf.toImageFormat().name();
                if (src.startsWith(QLatin1String("http://")) || src.startsWith(QLatin1String("https://")))
                    ++remoteImages;
                else
                    ++otherImages;
                continue;
            }
            QString t = frag.text();
            t.replace(QChar(0xFFFC), QString());
            if (!t.trimmed().isEmpty()) return false;          // real text → a rich paste
        }
    }
    return remoteImages == 1 && otherImages == 0;
}
