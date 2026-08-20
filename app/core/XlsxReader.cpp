#include "XlsxReader.h"
#include "PackageFormat.h"
#include "TableGrid.h"

#include <QHash>
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

QHash<QString, QString> parseWorkbookRels(const QString& zipPath) {
    QHash<QString, QString> out;   // rId → part path under xl/
    const QByteArray data =
        mnpkg::readEntry(zipPath, QStringLiteral("xl/_rels/workbook.xml.rels"));
    QXmlStreamReader xml(data);
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement() && xml.name() == QLatin1String("Relationship")) {
            QString target = xml.attributes().value(QLatin1String("Target")).toString();
            if (target.startsWith(QLatin1String("/")))
                target = target.mid(1);                      // absolute part path
            else
                target = QStringLiteral("xl/") + target;     // relative to xl/
            out.insert(xml.attributes().value(QLatin1String("Id")).toString(), target);
        }
    }
    return out;
}

// One worksheet part → a TableGrid ("" grid = empty sheet, skipped).
bool parseSheet(const QString& zipPath, const QString& part, TableGrid& grid) {
    const QByteArray data = mnpkg::readEntry(zipPath, part);
    if (data.isEmpty()) return false;
    std::vector<std::vector<QString>> rows;
    int maxCols = 0;
    QXmlStreamReader xml(data);
    while (!xml.atEnd()) {
        xml.readNext();
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
            int col = colFromRef(ref);
            if (col < 0) col = int(cells.size());       // no ref → append
            if (col >= kMaxCols) continue;
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
    // Trim trailing fully-empty rows.
    while (!rows.empty()) {
        bool empty = true;
        for (const QString& c : rows.back())
            if (!c.isEmpty()) { empty = false; break; }
        if (!empty) break;
        rows.pop_back();
    }
    if (rows.empty() || maxCols == 0) return false;
    grid = TableGrid::makeEmpty(int(rows.size()), maxCols);
    for (int r = 0; r < int(rows.size()); ++r)
        for (int c = 0; c < int(rows[size_t(r)].size()); ++c)
            grid.setCellText(r, c, rows[size_t(r)][size_t(c)]);
    grid.setHeaderRows(1);
    return true;
}

} // namespace

std::vector<BlockModel::BlockSpec> XlsxReader::read(const QString& xlsxPath) {
    std::vector<BlockModel::BlockSpec> specs;
    const std::vector<SheetRef> sheets = parseWorkbook(xlsxPath);
    if (sheets.empty()) return specs;
    const QHash<QString, QString> rels = parseWorkbookRels(xlsxPath);
    const std::vector<QString> shared = parseSharedStrings(xlsxPath);

    for (const SheetRef& s : sheets) {
        const QString part = rels.value(s.relId);
        if (part.isEmpty()) continue;
        TableGrid grid;
        if (!parseSheet(xlsxPath, part, grid)) continue;
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
