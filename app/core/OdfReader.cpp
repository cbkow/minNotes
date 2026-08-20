#include "OdfReader.h"
#include "MediaStore.h"
#include "PackageFormat.h"
#include "TableGrid.h"

#include <QHash>
#include <QImage>
#include <QSet>
#include <QXmlStreamReader>

namespace {

// ODF repeat clamps: number-columns-repeated="1015" to the sheet edge is
// normal LibreOffice output — expand real data, never the padding.
constexpr int kMaxRepeat = 1024;
constexpr int kMaxCols   = 512;
constexpr int kMaxRows   = 20000;

// --- Style resolution (odt) -------------------------------------------
// <office:automatic-styles> maps style names to text properties; text:span
// and paragraph styles reference them by name. We resolve only what the
// block model can hold: bold / italic / underline, plus two structural
// hints (list style ordered-ness, "Preformatted" paragraph → Code).
struct TextStyle { bool bold = false; bool italic = false; bool underline = false; };

struct Styles {
    QHash<QString, TextStyle> text;      // style-name → char props
    QSet<QString> orderedLists;          // list-style names whose level 1 numbers
    QSet<QString> preformatted;          // paragraph styles that mean Code
};

void parseStyles(QXmlStreamReader& xml, Styles& st) {
    // Inside <office:automatic-styles> (or <office:styles>): consume until
    // the matching end element.
    const QString endName = xml.qualifiedName().toString();
    QString curStyle;                    // style:style being read
    QString curFamily;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isEndElement() && xml.qualifiedName() == endName) break;
        if (!xml.isStartElement()) continue;
        const auto q = xml.qualifiedName();
        if (q == QLatin1String("style:style")) {
            curStyle = xml.attributes().value(QLatin1String("style:name")).toString();
            curFamily = xml.attributes().value(QLatin1String("style:family")).toString();
            const QString parent =
                xml.attributes().value(QLatin1String("style:parent-style-name")).toString();
            if (curFamily == QLatin1String("paragraph")
                && parent.contains(QLatin1String("Preformatted")))
                st.preformatted.insert(curStyle);
        } else if (q == QLatin1String("style:text-properties") && !curStyle.isEmpty()) {
            TextStyle ts = st.text.value(curStyle);
            if (xml.attributes().value(QLatin1String("fo:font-weight"))
                    .contains(QLatin1String("bold")))
                ts.bold = true;
            if (xml.attributes().value(QLatin1String("fo:font-style"))
                    == QLatin1String("italic"))
                ts.italic = true;
            const auto ul =
                xml.attributes().value(QLatin1String("style:text-underline-style"));
            if (!ul.isEmpty() && ul != QLatin1String("none"))
                ts.underline = true;
            st.text.insert(curStyle, ts);
        } else if (q == QLatin1String("text:list-style")) {
            const QString name =
                xml.attributes().value(QLatin1String("style:name")).toString();
            // Peek its children: a level-1 number style ⇒ ordered list.
            const QString listEnd = QStringLiteral("text:list-style");
            while (!xml.atEnd()) {
                xml.readNext();
                if (xml.isEndElement() && xml.qualifiedName() == listEnd) break;
                if (xml.isStartElement()
                    && xml.qualifiedName() == QLatin1String("text:list-level-style-number")
                    && xml.attributes().value(QLatin1String("text:level"))
                           == QLatin1String("1"))
                    st.orderedLists.insert(name);
            }
        }
    }
}

// --- Paragraph text + spans (odt) --------------------------------------
// Flattens text:p content: text:span (styled runs → spans), text:s (spaces),
// text:tab, text:line-break, text:a (link spans). Nested spans keep a
// style stack; properties union down the stack.
void readParagraphText(QXmlStreamReader& xml, const Styles& st,
                       BlockModel::BlockSpec& sp) {
    const QString endName = xml.qualifiedName().toString();
    struct Open { TextStyle ts; QString href; int start; };
    std::vector<Open> stack;
    auto effective = [&]() {
        Open o; o.start = sp.text.size();
        for (const Open& s : stack) {
            o.ts.bold |= s.ts.bold;
            o.ts.italic |= s.ts.italic;
            o.ts.underline |= s.ts.underline;
            if (!s.href.isEmpty()) o.href = s.href;
        }
        return o;
    };
    auto closeRun = [&](const Open& open) {
        const int s = open.start, e = sp.text.size();
        if (e <= s) return;
        // The union at open time describes THIS run only (stack-scoped).
        if (open.ts.bold)      sp.spans.push_back({s, e, BlockModel::SpanBold, {}});
        if (open.ts.italic)    sp.spans.push_back({s, e, BlockModel::SpanItalic, {}});
        if (open.ts.underline) sp.spans.push_back({s, e, BlockModel::SpanUnderline, {}});
        if (!open.href.isEmpty())
            sp.spans.push_back({s, e, BlockModel::SpanLink, open.href});
    };
    std::vector<Open> runs;   // committed on element close
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isEndElement()) {
            const auto q = xml.qualifiedName();
            if (q == endName) break;
            if ((q == QLatin1String("text:span") || q == QLatin1String("text:a"))
                && !stack.empty()) {
                closeRun(stack.back());
                stack.pop_back();
            }
            continue;
        }
        if (xml.isCharacters()) {
            sp.text += xml.text();
            continue;
        }
        if (!xml.isStartElement()) continue;
        const auto q = xml.qualifiedName();
        if (q == QLatin1String("text:span")) {
            Open o;
            o.ts = st.text.value(
                xml.attributes().value(QLatin1String("text:style-name")).toString());
            o.start = sp.text.size();
            // Union parent styles so nested spans accumulate.
            for (const Open& p : stack) {
                o.ts.bold |= p.ts.bold;
                o.ts.italic |= p.ts.italic;
                o.ts.underline |= p.ts.underline;
                if (o.href.isEmpty()) o.href = p.href;
            }
            stack.push_back(o);
        } else if (q == QLatin1String("text:a")) {
            Open o;
            o.href = xml.attributes().value(QLatin1String("xlink:href")).toString();
            o.start = sp.text.size();
            stack.push_back(o);
        } else if (q == QLatin1String("text:s")) {
            const int n = std::max(1,
                xml.attributes().value(QLatin1String("text:c")).toInt());
            sp.text += QString(n, QLatin1Char(' '));
        } else if (q == QLatin1String("text:tab")) {
            sp.text += QLatin1Char('\t');
        } else if (q == QLatin1String("text:line-break")) {
            sp.text += QLatin1Char('\n');
        }
    }
    while (!stack.empty()) { closeRun(stack.back()); stack.pop_back(); }
}

// --- Spreadsheet table (shared by ods sheets and odt inline tables) ----
// Returns false when the table holds nothing.
bool readOdfTable(QXmlStreamReader& xml, TableGrid& grid) {
    const QString endName = xml.qualifiedName().toString();   // table:table
    std::vector<std::vector<QString>> rows;
    int maxCols = 0;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isEndElement() && xml.qualifiedName() == endName) break;
        if (!xml.isStartElement()
            || xml.qualifiedName() != QLatin1String("table:table-row")) continue;
        const int rowRepeat = std::min(kMaxRepeat, std::max(1,
            xml.attributes().value(QLatin1String("table:number-rows-repeated")).toInt()));
        std::vector<QString> cells;
        // Cell loop until </table:table-row>.
        while (!xml.atEnd()) {
            xml.readNext();
            if (xml.isEndElement()
                && xml.qualifiedName() == QLatin1String("table:table-row")) break;
            if (!xml.isStartElement()) continue;
            const auto q = xml.qualifiedName();
            if (q != QLatin1String("table:table-cell")
                && q != QLatin1String("table:covered-table-cell")) continue;
            const int colRepeat = std::min(kMaxRepeat, std::max(1,
                xml.attributes().value(
                    QLatin1String("table:number-columns-repeated")).toInt()));
            const bool covered = (q == QLatin1String("table:covered-table-cell"));
            // Cell text: concatenated text:p children ("\n" between).
            QString value;
            const QString cellEnd = q.toString();
            while (!xml.atEnd()) {
                xml.readNext();
                if (xml.isEndElement() && xml.qualifiedName() == cellEnd) break;
                if (xml.isStartElement()
                    && xml.qualifiedName() == QLatin1String("text:p")) {
                    if (!value.isEmpty()) value += QLatin1Char('\n');
                    BlockModel::BlockSpec tmp;
                    Styles none;
                    readParagraphText(xml, none, tmp);
                    value += tmp.text;
                }
            }
            if (covered) value.clear();
            for (int i = 0; i < colRepeat && int(cells.size()) < kMaxCols; ++i)
                cells.push_back(value);
        }
        // Trim the trailing empty run each row carries to the sheet edge.
        while (!cells.empty() && cells.back().isEmpty()) cells.pop_back();
        for (int i = 0; i < rowRepeat && int(rows.size()) < kMaxRows; ++i) {
            rows.push_back(cells);
            if (!cells.empty()) maxCols = std::max(maxCols, int(cells.size()));
            // Repeated non-empty rows are real data; repeated EMPTY rows are
            // sheet-edge padding — keep one and stop.
            if (cells.empty()) break;
        }
    }
    while (!rows.empty() && rows.back().empty()) rows.pop_back();
    if (rows.empty() || maxCols == 0) return false;
    TableGrid g = TableGrid::makeEmpty(int(rows.size()), maxCols);
    for (int r = 0; r < int(rows.size()); ++r)
        for (int c = 0; c < int(rows[size_t(r)].size()); ++c)
            g.setCellText(r, c, rows[size_t(r)][size_t(c)]);
    g.setHeaderRows(1);
    grid = g;
    return true;
}

} // namespace

std::vector<BlockModel::BlockSpec> OdfReader::readOds(const QString& path) {
    std::vector<BlockModel::BlockSpec> specs;
    const QByteArray data = mnpkg::readEntry(path, QStringLiteral("content.xml"));
    if (data.isEmpty()) return specs;
    // Count sheets first (heading rule needs the total before emission).
    int sheetCount = 0;
    {
        QXmlStreamReader probe(data);
        while (!probe.atEnd()) {
            probe.readNext();
            if (probe.isStartElement()
                && probe.qualifiedName() == QLatin1String("table:table"))
                ++sheetCount;
        }
    }
    QXmlStreamReader xml(data);
    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement()
            || xml.qualifiedName() != QLatin1String("table:table")) continue;
        const QString name =
            xml.attributes().value(QLatin1String("table:name")).toString();
        TableGrid grid;
        if (!readOdfTable(xml, grid)) continue;
        if (sheetCount > 1) {
            BlockModel::BlockSpec h;
            h.type = BlockModel::Heading;
            h.level = 2;
            h.text = name;
            specs.push_back(std::move(h));
        }
        BlockModel::BlockSpec t;
        t.type = BlockModel::Table;
        t.tableJson = grid.toJson();
        specs.push_back(std::move(t));
    }
    return specs;
}

std::vector<BlockModel::BlockSpec> OdfReader::readOdt(const QString& path,
                                                      MediaStore* store) {
    std::vector<BlockModel::BlockSpec> specs;
    const QByteArray data = mnpkg::readEntry(path, QStringLiteral("content.xml"));
    if (data.isEmpty()) return specs;

    Styles st;
    int listDepth = 0;                    // current text:list nesting (0 = none)
    std::vector<bool> listOrdered;        // per nesting level
    QXmlStreamReader xml(data);
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isEndElement()) {
            if (xml.qualifiedName() == QLatin1String("text:list") && listDepth > 0) {
                --listDepth;
                listOrdered.pop_back();
            }
            continue;
        }
        if (!xml.isStartElement()) continue;
        const auto q = xml.qualifiedName();

        if (q == QLatin1String("office:automatic-styles")
            || q == QLatin1String("office:styles")) {
            parseStyles(xml, st);
        } else if (q == QLatin1String("text:list")) {
            const QString lstyle =
                xml.attributes().value(QLatin1String("text:style-name")).toString();
            // Nested lists usually omit the style — inherit the parent's.
            const bool ordered = lstyle.isEmpty()
                ? (!listOrdered.empty() && listOrdered.back())
                : st.orderedLists.contains(lstyle);
            ++listDepth;
            listOrdered.push_back(ordered);
        } else if (q == QLatin1String("text:h")) {
            BlockModel::BlockSpec sp;
            sp.type = BlockModel::Heading;
            sp.level = uint8_t(std::clamp(
                xml.attributes().value(QLatin1String("text:outline-level")).toInt(), 1, 6));
            readParagraphText(xml, st, sp);
            specs.push_back(std::move(sp));
        } else if (q == QLatin1String("text:p")) {
            const QString pstyle =
                xml.attributes().value(QLatin1String("text:style-name")).toString();
            BlockModel::BlockSpec sp;
            if (listDepth > 0) {
                sp.type = listOrdered.back() ? BlockModel::OrderedListItem
                                             : BlockModel::ListItem;
                sp.depth = uint8_t(std::clamp(listDepth - 1, 0,
                                              int(BlockModel::kMaxListDepth)));
            } else if (st.preformatted.contains(pstyle)) {
                sp.type = BlockModel::Code;
            } else {
                sp.type = BlockModel::Paragraph;
            }
            readParagraphText(xml, st, sp);
            specs.push_back(std::move(sp));
        } else if (q == QLatin1String("table:table")) {
            TableGrid grid;
            if (readOdfTable(xml, grid)) {
                BlockModel::BlockSpec t;
                t.type = BlockModel::Table;
                t.tableJson = grid.toJson();
                specs.push_back(std::move(t));
            }
        } else if (q == QLatin1String("draw:image") && store) {
            const QString href =
                xml.attributes().value(QLatin1String("xlink:href")).toString();
            if (!href.isEmpty() && !href.startsWith(QLatin1String("http"))) {
                const QImage img =
                    QImage::fromData(mnpkg::readEntry(path, href));
                if (!img.isNull()) {
                    const MediaStore::ImageRef ref = store->importImage(img);
                    if (ref.ok()) {
                        BlockModel::BlockSpec m;
                        m.type = BlockModel::Media;
                        m.mediaJson = MediaStore::imageDescriptorJson(ref);
                        specs.push_back(std::move(m));
                    }
                }
            }
        }
    }
    // Coalesce consecutive Code paragraphs (the DocxReader convention: each
    // preformatted line arrives as its own paragraph).
    std::vector<BlockModel::BlockSpec> out;
    for (auto& sp : specs) {
        if (sp.type == BlockModel::Code && !out.empty()
            && out.back().type == BlockModel::Code) {
            out.back().text += QLatin1Char('\n') + sp.text;
        } else {
            out.push_back(std::move(sp));
        }
    }
    return out;
}
