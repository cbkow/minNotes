#include "XlsxReader.h"
#include "MediaStore.h"
#include "PackageFormat.h"
#include "TableGrid.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QXmlStreamReader>

namespace {

// Defensive caps: a stray dimension record or a sheet formatted to the
// spreadsheet's edge must not allocate a monster grid.
constexpr int kMaxCols = 512;
constexpr int kMaxRows = 20000;

// "BC12" → 0-based column (letters only; row digits ignored).
int colFromRef(const QString& ref) {
    int col = 0;
    for (const QChar ch : ref) {
        if (!ch.isLetter()) break;
        col = col * 26 + (ch.toUpper().unicode() - 'A' + 1);
    }
    return col - 1;
}

// <si>/<is> rich text: concatenate every descendant <t> (runs flatten —
// per-run formatting has no cell-level home in TableGrid).
QString readInlineString(QXmlStreamReader& xml, const QString& endElement) {
    QString out;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isEndElement() && xml.name() == endElement) break;
        if (xml.isStartElement() && xml.name() == QLatin1String("t"))
            out += xml.readElementText();
    }
    return out;
}

std::vector<QString> parseSharedStrings(const QString& zipPath) {
    std::vector<QString> out;
    const QByteArray data =
        mnpkg::readEntry(zipPath, QStringLiteral("xl/sharedStrings.xml"));
    if (data.isEmpty()) return out;
    QXmlStreamReader xml(data);
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement() && xml.name() == QLatin1String("si"))
            out.push_back(readInlineString(xml, QStringLiteral("si")));
    }
    return out;
}

struct SheetRef { QString name; QString relId; };

std::vector<SheetRef> parseWorkbook(const QString& zipPath) {
    std::vector<SheetRef> out;
    const QByteArray data =
        mnpkg::readEntry(zipPath, QStringLiteral("xl/workbook.xml"));
    QXmlStreamReader xml(data);
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement() && xml.name() == QLatin1String("sheet")) {
            SheetRef s;
            s.name = xml.attributes().value(QLatin1String("name")).toString();
            s.relId = xml.attributes().value(QLatin1String("r:id")).toString();
            out.push_back(s);
        }
    }
    return out;
}

// A part's _rels file: rId → part path, targets resolved against `baseDir`
// (the directory of the part the rels belong to, e.g. "xl/worksheets").
// cleanPath collapses the "../drawings/…" hops OPC targets are full of.
QHash<QString, QString> parseRels(const QString& zipPath, const QString& relsPart,
                                  const QString& baseDir) {
    QHash<QString, QString> out;
    const QByteArray data = mnpkg::readEntry(zipPath, relsPart);
    QXmlStreamReader xml(data);
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement() && xml.name() == QLatin1String("Relationship")) {
            QString target = xml.attributes().value(QLatin1String("Target")).toString();
            target = target.startsWith(QLatin1String("/"))
                ? target.mid(1)                                   // absolute part path
                : QDir::cleanPath(baseDir + QLatin1Char('/') + target);
            out.insert(xml.attributes().value(QLatin1String("Id")).toString(), target);
        }
    }
    return out;
}

// "xl/worksheets/sheet1.xml" → its rels part + base dir.
QString relsPartFor(const QString& part) {
    const QFileInfo fi(part);
    return fi.path() + QStringLiteral("/_rels/") + fi.fileName()
        + QStringLiteral(".rels");
}

QHash<QString, QString> parseWorkbookRels(const QString& zipPath) {
    return parseRels(zipPath, QStringLiteral("xl/_rels/workbook.xml.rels"),
                     QStringLiteral("xl"));
}

// One image placement resolved to a cell: from a drawing anchor or the
// rich-value chain. `clearText` = the cell's "#VALUE!" placeholder goes.
struct CellImage { int row = -1; int col = -1; QString mediaPart; bool clearText = false; };

// xl/drawings/drawingN.xml: every anchored picture → its from-cell.
// twoCellAnchor/oneCellAnchor both open with <xdr:from><xdr:col/><xdr:row/>;
// the <xdr:to> of a twoCellAnchor never matches the `inFrom` gate.
void parseDrawing(const QString& zipPath, const QString& drawingPart,
                  std::vector<CellImage>& out) {
    const QHash<QString, QString> rels = parseRels(
        zipPath, relsPartFor(drawingPart), QFileInfo(drawingPart).path());
    const QByteArray data = mnpkg::readEntry(zipPath, drawingPart);
    if (data.isEmpty()) return;
    QXmlStreamReader xml(data);
    int fromCol = -1, fromRow = -1;
    bool inFrom = false;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            if (xml.name() == QLatin1String("from")) inFrom = true;
            else if (inFrom && xml.name() == QLatin1String("col"))
                fromCol = xml.readElementText().toInt();
            else if (inFrom && xml.name() == QLatin1String("row"))
                fromRow = xml.readElementText().toInt();
            else if (xml.name() == QLatin1String("blip")) {
                const QString part = rels.value(
                    xml.attributes().value(QLatin1String("r:embed")).toString());
                if (!part.isEmpty() && fromRow >= 0 && fromCol >= 0)
                    out.push_back({fromRow, fromCol, part, false});
            }
        } else if (xml.isEndElement()) {
            if (xml.name() == QLatin1String("from")) inFrom = false;
            else if (xml.name().endsWith(QLatin1String("CellAnchor")))
                { fromCol = -1; fromRow = -1; }
        }
    }
}

// Rich-value in-cell images ("place in cell"): a cell carries vm="N"
// (1-based) and a #VALUE! placeholder; the chain runs
//   valueMetadata bk[N-1] → rc v (futureMetadata index)
//   futureMetadata[XLRICHVALUE] bk → xlrd:rvb i (rich value index)
//   rdrichvalue.xml rv → first <v> (index into richValueRel's rel list)
//   richValueRel.xml.rels → xl/media part.
// Returns vm → media part; empty when the workbook has none of this.
QHash<int, QString> parseRichValueImages(const QString& zipPath) {
    QHash<int, QString> out;
    // richValueRel.xml: ordered <rel r:id>, resolved through its own rels.
    std::vector<QString> relParts;
    {
        const QByteArray data = mnpkg::readEntry(
            zipPath, QStringLiteral("xl/richData/richValueRel.xml"));
        if (data.isEmpty()) return out;
        const QHash<QString, QString> rels = parseRels(
            zipPath, QStringLiteral("xl/richData/_rels/richValueRel.xml.rels"),
            QStringLiteral("xl/richData"));
        QXmlStreamReader xml(data);
        while (!xml.atEnd()) {
            xml.readNext();
            if (xml.isStartElement() && xml.name() == QLatin1String("rel"))
                relParts.push_back(rels.value(
                    xml.attributes().value(QLatin1String("r:id")).toString()));
        }
    }
    // rdrichvalue.xml: rv index → first <v> = local-image rel index.
    std::vector<int> rvToRel;
    {
        const QByteArray data = mnpkg::readEntry(
            zipPath, QStringLiteral("xl/richData/rdrichvalue.xml"));
        QXmlStreamReader xml(data);
        bool inRv = false, gotFirst = false;
        while (!xml.atEnd()) {
            xml.readNext();
            if (xml.isStartElement() && xml.name() == QLatin1String("rv")) {
                inRv = true; gotFirst = false;
                rvToRel.push_back(-1);
            } else if (xml.isEndElement() && xml.name() == QLatin1String("rv")) {
                inRv = false;
            } else if (inRv && !gotFirst && xml.isStartElement()
                       && xml.name() == QLatin1String("v")) {
                bool ok = false;
                const int idx = xml.readElementText().toInt(&ok);
                if (ok) rvToRel.back() = idx;
                gotFirst = true;
            }
        }
    }
    // metadata.xml: the two bk sequences.
    std::vector<int> fmToRv;   // futureMetadata[XLRICHVALUE] bk order → rvb i
    std::vector<int> vmToFm;   // valueMetadata bk order → rc v
    {
        const QByteArray data = mnpkg::readEntry(
            zipPath, QStringLiteral("xl/metadata.xml"));
        QXmlStreamReader xml(data);
        enum { None, Future, Value } section = None;
        while (!xml.atEnd()) {
            xml.readNext();
            if (xml.isStartElement()) {
                if (xml.name() == QLatin1String("futureMetadata"))
                    section = (xml.attributes().value(QLatin1String("name"))
                               == QLatin1String("XLRICHVALUE")) ? Future : None;
                else if (xml.name() == QLatin1String("valueMetadata"))
                    section = Value;
                else if (section == Future && xml.name() == QLatin1String("rvb"))
                    fmToRv.push_back(
                        xml.attributes().value(QLatin1String("i")).toInt());
                else if (section == Value && xml.name() == QLatin1String("rc"))
                    vmToFm.push_back(
                        xml.attributes().value(QLatin1String("v")).toInt());
            } else if (xml.isEndElement()
                       && (xml.name() == QLatin1String("futureMetadata")
                           || xml.name() == QLatin1String("valueMetadata"))) {
                section = None;
            }
        }
    }
    for (int vm = 1; vm <= int(vmToFm.size()); ++vm) {
        const int fm = vmToFm[size_t(vm - 1)];
        if (fm < 0 || fm >= int(fmToRv.size())) continue;
        const int rv = fmToRv[size_t(fm)];
        if (rv < 0 || rv >= int(rvToRel.size())) continue;
        const int rel = rvToRel[size_t(rv)];
        if (rel < 0 || rel >= int(relParts.size())) continue;
        if (!relParts[size_t(rel)].isEmpty()) out.insert(vm, relParts[size_t(rel)]);
    }
    return out;
}

// One worksheet part, parsed raw: text cells + the hooks images need (the
// <drawing r:id> reference and every vm= rich-value cell). The caller
// assembles the TableGrid once image placements are known — an image can
// sit beyond the text extent and must still get a cell.
struct SheetData {
    std::vector<std::vector<QString>> rows;
    int maxCols = 0;
    QString drawingRelId;
    struct VmCell { int row; int col; int vm; };
    std::vector<VmCell> vmCells;
};

bool parseSheet(const QString& zipPath, const QString& part, SheetData& sd) {
    const QByteArray data = mnpkg::readEntry(zipPath, part);
    if (data.isEmpty()) return false;
    std::vector<std::vector<QString>>& rows = sd.rows;
    int& maxCols = sd.maxCols;
    QXmlStreamReader xml(data);
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement() && xml.name() == QLatin1String("drawing")) {
            sd.drawingRelId =
                xml.attributes().value(QLatin1String("r:id")).toString();
            continue;
        }
        if (!xml.isStartElement() || xml.name() != QLatin1String("row")) continue;
        // Sparse rows: honour r="N" so skipped rows stay blank.
        const int rowRef = xml.attributes().value(QLatin1String("r")).toInt() - 1;
        const int rowIdx = rowRef >= 0 ? rowRef : int(rows.size());
        if (rowIdx >= kMaxRows) break;
        while (int(rows.size()) <= rowIdx) rows.emplace_back();
        std::vector<QString>& cells = rows[size_t(rowIdx)];
        // Cell loop until </row>.
        while (!xml.atEnd()) {
            xml.readNext();
            if (xml.isEndElement() && xml.name() == QLatin1String("row")) break;
            if (!xml.isStartElement() || xml.name() != QLatin1String("c")) continue;
            const QString ref = xml.attributes().value(QLatin1String("r")).toString();
            const QString type = xml.attributes().value(QLatin1String("t")).toString();
            const int vm = xml.attributes().value(QLatin1String("vm")).toInt();
            int col = colFromRef(ref);
            if (col < 0) col = int(cells.size());       // no ref → append
            if (col >= kMaxCols) continue;
            if (vm > 0) sd.vmCells.push_back({rowIdx, col, vm});   // in-cell image?
            // Value: <v> (shared index / number / cached formula) or <is><t>.
            QString value;
            bool sharedIdx = (type == QLatin1String("s"));
            while (!xml.atEnd()) {
                xml.readNext();
                if (xml.isEndElement() && xml.name() == QLatin1String("c")) break;
                if (!xml.isStartElement()) continue;
                if (xml.name() == QLatin1String("v"))
                    value = xml.readElementText();
                else if (xml.name() == QLatin1String("is"))
                    value = readInlineString(xml, QStringLiteral("is"));
            }
            if (type == QLatin1String("b"))
                value = (value == QLatin1String("1")) ? QStringLiteral("TRUE")
                                                      : QStringLiteral("FALSE");
            if (sharedIdx) value = QStringLiteral("\x01s:") + value;   // resolved below
            while (int(cells.size()) <= col) cells.emplace_back();
            cells[size_t(col)] = value;
            maxCols = std::max(maxCols, col + 1);
        }
    }
    // Resolve shared-string markers (deferred so sharedStrings parses once
    // at the caller and this function stays part-local).
    // — handled by caller via the marker; see read().
    // Trim trailing fully-empty rows. (vm cells hold their "#VALUE!"
    // placeholder at this point, so an image-only tail row survives.)
    while (!rows.empty()) {
        bool empty = true;
        for (const QString& c : rows.back())
            if (!c.isEmpty()) { empty = false; break; }
        if (!empty) break;
        rows.pop_back();
    }
    return true;
}

} // namespace

std::vector<BlockModel::BlockSpec> XlsxReader::read(const QString& xlsxPath,
                                                    MediaStore* store) {
    std::vector<BlockModel::BlockSpec> specs;
    const std::vector<SheetRef> sheets = parseWorkbook(xlsxPath);
    if (sheets.empty()) return specs;
    const QHash<QString, QString> rels = parseWorkbookRels(xlsxPath);
    const std::vector<QString> shared = parseSharedStrings(xlsxPath);
    // The rich-value chain is workbook-global; parsed lazily on the first
    // vm cell. Imported descriptors cache per media part (a repeated image
    // decodes once; the sidecar dedups by content anyway).
    QHash<int, QString> richByVm;
    bool richParsed = false;
    QHash<QString, QString> descByPart;
    auto descriptorFor = [&](const QString& mediaPart) -> QString {
        if (!store) return {};
        const auto it = descByPart.constFind(mediaPart);
        if (it != descByPart.constEnd()) return it.value();
        QString desc;
        QImage img;
        if (img.loadFromData(mnpkg::readEntry(xlsxPath, mediaPart)) && !img.isNull()) {
            const MediaStore::ImageRef ref = store->importImage(img);
            if (ref.ok()) desc = MediaStore::imageDescriptorJson(ref);
        }
        descByPart.insert(mediaPart, desc);
        return desc;
    };

    for (const SheetRef& s : sheets) {
        const QString part = rels.value(s.relId);
        if (part.isEmpty()) continue;
        SheetData sd;
        if (!parseSheet(xlsxPath, part, sd)) continue;

        // Image placements: drawing-anchored pics land at their from-cell;
        // vm cells resolve through the rich-value chain (and lose their
        // "#VALUE!" placeholder).
        std::vector<CellImage> images;
        if (store && !sd.drawingRelId.isEmpty()) {
            const QString drawingPart = parseRels(
                xlsxPath, relsPartFor(part), QFileInfo(part).path())
                .value(sd.drawingRelId);
            if (!drawingPart.isEmpty()) parseDrawing(xlsxPath, drawingPart, images);
        }
        if (store && !sd.vmCells.empty()) {
            if (!richParsed) { richByVm = parseRichValueImages(xlsxPath); richParsed = true; }
            for (const SheetData::VmCell& vc : sd.vmCells) {
                const QString mp = richByVm.value(vc.vm);
                if (!mp.isEmpty()) images.push_back({vc.row, vc.col, mp, true});
            }
        }
        // An anchored image may sit beyond the text extent — grow the grid
        // to give it a cell (same caps as the data pass).
        for (const CellImage& ci : images) {
            if (ci.row >= kMaxRows || ci.col >= kMaxCols) continue;
            while (int(sd.rows.size()) <= ci.row) sd.rows.emplace_back();
            sd.maxCols = std::max(sd.maxCols, ci.col + 1);
        }
        if (sd.rows.empty() || sd.maxCols == 0) continue;   // truly empty sheet

        TableGrid grid = TableGrid::makeEmpty(int(sd.rows.size()), sd.maxCols);
        for (int r = 0; r < int(sd.rows.size()); ++r)
            for (int c = 0; c < int(sd.rows[size_t(r)].size()); ++c)
                grid.setCellText(r, c, sd.rows[size_t(r)][size_t(c)]);
        grid.setHeaderRows(1);
        // Resolve the shared-string markers left by parseSheet.
        for (int r = 0; r < grid.rows(); ++r)
            for (int c = 0; c < grid.cols(); ++c) {
                const QString v = grid.cellText(r, c);
                if (v.startsWith(QLatin1String("\x01s:"))) {
                    bool ok = false;
                    const int idx = v.mid(3).toInt(&ok);
                    grid.setCellText(r, c,
                        (ok && idx >= 0 && idx < int(shared.size()))
                            ? shared[size_t(idx)] : QString());
                }
            }
        for (const CellImage& ci : images) {
            if (ci.row >= grid.rows() || ci.col >= grid.cols()) continue;
            const QString desc = descriptorFor(ci.mediaPart);
            if (desc.isEmpty()) continue;
            grid.setCellMedia(ci.row, ci.col, desc);
            if (ci.clearText) grid.setCellText(ci.row, ci.col, QString());
        }

        if (sheets.size() > 1) {   // name each sheet only when there are several
            BlockModel::BlockSpec h;
            h.type = BlockModel::Heading;
            h.level = 2;
            h.text = s.name;
            specs.push_back(std::move(h));
        }
        BlockModel::BlockSpec t;
        t.type = BlockModel::Table;
        t.tableJson = grid.toJson();
        specs.push_back(std::move(t));
    }
    return specs;
}
