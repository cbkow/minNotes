#include "DocxReader.h"
#include "MediaStore.h"
#include "PackageFormat.h"
#include "TableGrid.h"

#include <QHash>
#include <QImage>
#include <QRegularExpression>
#include <QXmlStreamReader>

namespace {

// Word's named highlight palette (w:highlight values).
QString highlightHex(const QString& name) {
    static const QHash<QString, QString> map = {
        {QStringLiteral("yellow"), QStringLiteral("#ffff00")},
        {QStringLiteral("green"), QStringLiteral("#00ff00")},
        {QStringLiteral("cyan"), QStringLiteral("#00ffff")},
        {QStringLiteral("magenta"), QStringLiteral("#ff00ff")},
        {QStringLiteral("blue"), QStringLiteral("#0000ff")},
        {QStringLiteral("red"), QStringLiteral("#ff0000")},
        {QStringLiteral("darkYellow"), QStringLiteral("#808000")},
        {QStringLiteral("darkGreen"), QStringLiteral("#008000")},
        {QStringLiteral("darkCyan"), QStringLiteral("#008080")},
        {QStringLiteral("darkMagenta"), QStringLiteral("#800080")},
        {QStringLiteral("darkBlue"), QStringLiteral("#000080")},
        {QStringLiteral("darkRed"), QStringLiteral("#800000")},
        {QStringLiteral("darkGray"), QStringLiteral("#808080")},
        {QStringLiteral("lightGray"), QStringLiteral("#c0c0c0")},
    };
    return map.value(name);
}

// Toggle-property value: absent attr = on; "0"/"false"/"none" = off.
bool onOff(const QXmlStreamAttributes& a) {
    const QString v = a.value(QLatin1String("w:val")).toString();
    return v.isEmpty()
        || !(v == QLatin1String("0") || v == QLatin1String("false")
             || v == QLatin1String("none"));
}

struct RunProps {
    bool b = false, i = false, u = false, strike = false, code = false;
    QString fg;         // "#rrggbb" or ""
    QString highlight;  // "#rrggbb" or ""
    int halfPtSize = 0;
    bool courier = false, codeShd = false;
};

struct ParaProps {
    int headingLevel = 0;   // from pStyle / outlineLvl
    int numId = -1, ilvl = 0;
    int uniformHalfPt = -1; // -1 unknown; -2 mixed — the size-heuristic input
    bool uniformBold = true;
    bool allCourier = true; // true only if every text run was code-shaped
    bool anyText = false;
};

struct Ctx {
    QString zipPath;
    MediaStore* store = nullptr;
    QHash<QString, QString> rels;        // rId → target
    QHash<int, bool> numOrdered;         // numId → ordered?
    QHash<QString, QStringList> comments;// comment id → bodies
    std::vector<BlockModel::BlockSpec>* specs = nullptr;
    std::vector<DocxReader::CommentOut>* commentsOut = nullptr;
    struct OpenRange { int specIndex; int start; };
    QHash<QString, OpenRange> openRanges;   // comment id → started at
};

void parseRels(Ctx& c) {
    QXmlStreamReader xml(mnpkg::readEntry(
        c.zipPath, QStringLiteral("word/_rels/document.xml.rels")));
    while (!xml.atEnd()) {
        if (xml.readNext() == QXmlStreamReader::StartElement
            && xml.name() == QLatin1String("Relationship")) {
            c.rels.insert(xml.attributes().value(QLatin1String("Id")).toString(),
                          xml.attributes().value(QLatin1String("Target")).toString());
        }
    }
}

void parseNumbering(Ctx& c) {
    const QByteArray bytes =
        mnpkg::readEntry(c.zipPath, QStringLiteral("word/numbering.xml"));
    if (bytes.isEmpty()) {
        // Our own files (and a graceful fallback): numId 2 = ordered.
        c.numOrdered.insert(1, false);
        c.numOrdered.insert(2, true);
        return;
    }
    QXmlStreamReader xml(bytes);
    QHash<int, bool> abstractOrdered;   // abstractNumId → ordered (lvl 0)
    QHash<int, int> numToAbstract;
    int curAbstract = -1;
    bool sawLvl0Fmt = false;
    int curNum = -1;
    while (!xml.atEnd()) {
        const auto t = xml.readNext();
        if (t != QXmlStreamReader::StartElement) continue;
        const auto n = xml.name();
        if (n == QLatin1String("abstractNum")) {
            curAbstract = xml.attributes()
                              .value(QLatin1String("w:abstractNumId")).toInt();
            sawLvl0Fmt = false;
        } else if (n == QLatin1String("lvl") && curAbstract >= 0) {
            sawLvl0Fmt = xml.attributes().value(QLatin1String("w:ilvl")).toInt() == 0
                         ? false : sawLvl0Fmt;
        } else if (n == QLatin1String("numFmt") && curAbstract >= 0 && !sawLvl0Fmt) {
            const QString fmt = xml.attributes().value(QLatin1String("w:val")).toString();
            abstractOrdered.insert(curAbstract, fmt != QLatin1String("bullet"));
            sawLvl0Fmt = true;
        } else if (n == QLatin1String("num")) {
            curNum = xml.attributes().value(QLatin1String("w:numId")).toInt();
        } else if (n == QLatin1String("abstractNumId") && curNum >= 0) {
            numToAbstract.insert(curNum,
                xml.attributes().value(QLatin1String("w:val")).toInt());
        }
    }
    for (auto it = numToAbstract.constBegin(); it != numToAbstract.constEnd(); ++it)
        c.numOrdered.insert(it.key(), abstractOrdered.value(it.value(), false));
}

void parseComments(Ctx& c) {
    const QByteArray bytes =
        mnpkg::readEntry(c.zipPath, QStringLiteral("word/comments.xml"));
    if (bytes.isEmpty()) return;
    QXmlStreamReader xml(bytes);
    QString id, author, text;
    while (!xml.atEnd()) {
        const auto t = xml.readNext();
        if (t == QXmlStreamReader::StartElement) {
            if (xml.name() == QLatin1String("comment")) {
                id = xml.attributes().value(QLatin1String("w:id")).toString();
                author = xml.attributes().value(QLatin1String("w:author")).toString();
                text.clear();
            } else if (xml.name() == QLatin1String("t")) {
                if (!text.isEmpty()) text += QLatin1Char('\n');
                text += xml.readElementText();
            }
        } else if (t == QXmlStreamReader::EndElement
                   && xml.name() == QLatin1String("comment") && !id.isEmpty()) {
            // The app's messages have no author field — prefix it.
            c.comments[id] << (author.isEmpty() ? text
                                                : author + QStringLiteral(" — ") + text);
            id.clear();
        }
    }
}

RunProps parseRunProps(QXmlStreamReader& xml) {
    RunProps rp;
    while (!xml.atEnd()) {
        const auto t = xml.readNext();
        if (t == QXmlStreamReader::EndElement && xml.name() == QLatin1String("rPr"))
            break;
        if (t != QXmlStreamReader::StartElement) continue;
        const auto n = xml.name();
        const auto a = xml.attributes();
        if (n == QLatin1String("b")) rp.b = onOff(a);
        else if (n == QLatin1String("i")) rp.i = onOff(a);
        else if (n == QLatin1String("u")) rp.u = onOff(a);
        else if (n == QLatin1String("strike")) rp.strike = onOff(a);
        else if (n == QLatin1String("sz"))
            rp.halfPtSize = a.value(QLatin1String("w:val")).toInt();
        else if (n == QLatin1String("color")) {
            const QString v = a.value(QLatin1String("w:val")).toString().toLower();
            if (!v.isEmpty() && v != QLatin1String("auto") && v != QLatin1String("000000"))
                rp.fg = QLatin1Char('#') + v;
        } else if (n == QLatin1String("highlight")) {
            rp.highlight = highlightHex(a.value(QLatin1String("w:val")).toString());
        } else if (n == QLatin1String("shd")) {
            const QString fill = a.value(QLatin1String("w:fill")).toString().toUpper();
            if (fill == QLatin1String("EFEFEF")) rp.codeShd = true;
            else if (!fill.isEmpty() && fill != QLatin1String("AUTO"))
                rp.highlight = QLatin1Char('#') + fill.toLower();
        } else if (n == QLatin1String("rFonts")) {
            const QString f = a.value(QLatin1String("w:ascii")).toString();
            rp.courier = f.startsWith(QLatin1String("Courier"), Qt::CaseInsensitive)
                || f.contains(QLatin1String("Mono"), Qt::CaseInsensitive);
        }
    }
    // THE shd overload: Courier + EFEFEF is our code chrome, not a highlight.
    rp.code = rp.courier && rp.codeShd;
    return rp;
}

// Flush helpers -------------------------------------------------------------

void addSpanIfRange(std::vector<BlockModel::Span>& spans, int s, int e,
                    uint8_t kind, const QString& payload = QString()) {
    if (e > s) spans.push_back({s, e, kind, payload});
}

// One <w:p>. Appends text/media specs to ctx.specs and comment ranges.
// In `cellTextMode`, only the flattened text is produced (tables).
QString parseParagraph(QXmlStreamReader& xml, Ctx& c, bool cellTextMode) {
    QString text;
    std::vector<BlockModel::Span> spans;
    ParaProps pp;
    QString linkHref;          // inside <w:hyperlink>
    int linkStart = -1;
    int glyphTask = -1;        // painted task glyph (wp:docPr "mnTask<state>")
    std::vector<BlockModel::BlockSpec> mediaSpecs;
    QHash<QString, int> rangeStarts;   // comment id → start offset (this para)
    std::vector<std::tuple<QString, int, int>> closedRanges;

    auto noteRun = [&](const RunProps& rp, int s, int e) {
        if (e <= s) return;
        pp.anyText = true;
        if (pp.uniformHalfPt == -1) pp.uniformHalfPt = rp.halfPtSize;
        else if (pp.uniformHalfPt != rp.halfPtSize) pp.uniformHalfPt = -2;
        pp.uniformBold = pp.uniformBold && rp.b;
        pp.allCourier = pp.allCourier && rp.code;
        if (linkHref.isEmpty()) {
            if (rp.b) addSpanIfRange(spans, s, e, BlockModel::SpanBold);
            if (rp.i) addSpanIfRange(spans, s, e, BlockModel::SpanItalic);
            if (rp.u) addSpanIfRange(spans, s, e, BlockModel::SpanUnderline);
            if (rp.code) addSpanIfRange(spans, s, e, BlockModel::SpanCode);
            if (!rp.fg.isEmpty())
                addSpanIfRange(spans, s, e, BlockModel::SpanFgColor, rp.fg);
            if (!rp.highlight.isEmpty())
                addSpanIfRange(spans, s, e, BlockModel::SpanHighlight, rp.highlight);
        }
        if (rp.strike) addSpanIfRange(spans, s, e, BlockModel::SpanStrike);
    };

    while (!xml.atEnd()) {
        const auto t = xml.readNext();
        if (t == QXmlStreamReader::EndElement) {
            if (xml.name() == QLatin1String("p")) break;
            if (xml.name() == QLatin1String("hyperlink") && linkStart >= 0) {
                if (!linkHref.isEmpty())
                    addSpanIfRange(spans, linkStart, text.size(),
                                   BlockModel::SpanLink, linkHref);
                linkHref.clear();
                linkStart = -1;
            }
            continue;
        }
        if (t != QXmlStreamReader::StartElement) continue;
        const auto n = xml.name();
        if (n == QLatin1String("pStyle")) {
            const QString v = xml.attributes().value(QLatin1String("w:val"))
                                  .toString().toLower();
            static const QRegularExpression hRe(QStringLiteral("^heading\\s?([1-6])$"));
            const auto mt = hRe.match(v);
            if (mt.hasMatch()) pp.headingLevel = mt.captured(1).toInt();
        } else if (n == QLatin1String("outlineLvl")) {
            if (pp.headingLevel == 0)
                pp.headingLevel = std::clamp(
                    xml.attributes().value(QLatin1String("w:val")).toInt() + 1, 1, 6);
        } else if (n == QLatin1String("numId")) {
            pp.numId = xml.attributes().value(QLatin1String("w:val")).toInt();
        } else if (n == QLatin1String("ilvl")) {
            pp.ilvl = xml.attributes().value(QLatin1String("w:val")).toInt();
        } else if (n == QLatin1String("hyperlink")) {
            const QString rid = xml.attributes()
                                    .value(QLatin1String("r:id")).toString();
            linkHref = c.rels.value(rid);
            linkStart = text.size();
        } else if (n == QLatin1String("commentRangeStart")) {
            rangeStarts.insert(
                xml.attributes().value(QLatin1String("w:id")).toString(), text.size());
        } else if (n == QLatin1String("commentRangeEnd")) {
            const QString id = xml.attributes().value(QLatin1String("w:id")).toString();
            const int s = rangeStarts.take(id);
            closedRanges.emplace_back(id, s, text.size());
        } else if (n == QLatin1String("r")) {
            // One run: props then t/br/tab/drawing children.
            RunProps rp;
            QString docPrName;   // last wp:docPr name seen before a blip
            while (!xml.atEnd()) {
                const auto rt = xml.readNext();
                if (rt == QXmlStreamReader::EndElement
                    && xml.name() == QLatin1String("r")) break;
                if (rt != QXmlStreamReader::StartElement) continue;
                const auto rn = xml.name();
                if (rn == QLatin1String("rPr")) {
                    rp = parseRunProps(xml);
                } else if (rn == QLatin1String("t")) {
                    const int s = text.size();
                    text += xml.readElementText();
                    noteRun(rp, s, text.size());
                } else if (rn == QLatin1String("br") || rn == QLatin1String("cr")) {
                    text += QLatin1Char('\n');
                } else if (rn == QLatin1String("tab")) {
                    text += QLatin1Char('\t');
                } else if (rn == QLatin1String("docPr")) {
                    docPrName = xml.attributes().value(QLatin1String("name")).toString();
                } else if (rn == QLatin1String("blip")
                           && docPrName.startsWith(QLatin1String("mnTask"))) {
                    // OUR painted task checkbox (2026-08-22): the docPr name
                    // carries the state — reconstruct it, never import the
                    // raster as media.
                    glyphTask = std::clamp(
                        docPrName.mid(6).toInt(), 0, 2);
                    docPrName.clear();
                } else if (rn == QLatin1String("blip") && !cellTextMode && c.store) {
                    const QString rid = xml.attributes()
                        .value(QLatin1String("r:embed")).toString();
                    QString target = c.rels.value(rid);
                    if (!target.isEmpty()) {
                        if (!target.startsWith(QLatin1String("word/")))
                            target = QStringLiteral("word/") + target;
                        const QImage img =
                            QImage::fromData(mnpkg::readEntry(c.zipPath, target));
                        if (!img.isNull()) {
                            const MediaStore::ImageRef ref = c.store->importImage(img);
                            if (ref.ok()) {
                                BlockModel::BlockSpec sp;
                                sp.type = BlockModel::Media;
                                sp.mediaJson = MediaStore::imageDescriptorJson(ref);
                                mediaSpecs.push_back(std::move(sp));
                            }
                        }
                    }
                }
            }
        }
    }

    if (cellTextMode) return text;

    // Paragraph → spec. Heading: pStyle/outlineLvl first, then OUR direct
    // format (all runs bold + one of the emitter's six sizes).
    BlockModel::BlockSpec sp;
    if (!text.trimmed().isEmpty()) {
        sp.text = text;
        sp.spans = spans;
        int level = pp.headingLevel;
        if (level == 0 && pp.anyText && pp.uniformBold && pp.uniformHalfPt > 0) {
            static const int half[6] = {36, 32, 28, 26, 24, 22};
            for (int i = 0; i < 6; ++i)
                if (pp.uniformHalfPt == half[i]) { level = i + 1; break; }
        }
        if (level > 0) {
            sp.type = BlockModel::Heading;
            sp.level = static_cast<uint8_t>(level);
            sp.spans.clear();   // heading chrome, not content spans
        } else if (pp.anyText && pp.allCourier) {
            sp.type = BlockModel::Code;
            sp.spans.clear();
        } else if (pp.numId >= 0) {
            sp.type = c.numOrdered.value(pp.numId, false)
                          ? BlockModel::OrderedListItem : BlockModel::ListItem;
            sp.depth = static_cast<uint8_t>(
                std::clamp(pp.ilvl, 0, BlockModel::kMaxListDepth));
        }
        // Tri-state glyph sniff (our task emit: glyph-prefixed bullets).
        const auto glyphState = [](const QString& s) -> int {
            if (s.startsWith(QStringLiteral("☐ "))) return BlockModel::TaskTodo;
            if (s.startsWith(QStringLiteral("◐ "))) return BlockModel::TaskDoing;
            if (s.startsWith(QStringLiteral("☑ "))) return BlockModel::TaskDone;
            return -1;
        };
        const int st = (sp.type == BlockModel::ListItem
                        || sp.type == BlockModel::Paragraph) ? glyphState(sp.text) : -1;
        if (st >= 0) {
            sp.type = BlockModel::TaskListItem;
            sp.taskState = static_cast<uint8_t>(st);
            sp.text.remove(0, 2);
            for (auto& x : sp.spans) {
                x.s = std::max(0, x.s - 2);
                x.e = std::max(0, x.e - 2);
            }
            for (auto& [id, s, e] : closedRanges) { s = std::max(0, s - 2); e = std::max(0, e - 2); }
        } else if (glyphTask >= 0 && (sp.type == BlockModel::ListItem
                                      || sp.type == BlockModel::Paragraph)) {
            // Painted-checkbox emit (2026-08-22): state rode the docPr name.
            // The glyph run is followed by a one-space spacer run (our
            // chrome, like the legacy "☐ " prefix) — strip it.
            sp.type = BlockModel::TaskListItem;
            sp.taskState = static_cast<uint8_t>(glyphTask);
            if (sp.text.startsWith(QLatin1Char(' '))) {
                sp.text.remove(0, 1);
                for (auto& x : sp.spans) {
                    x.s = std::max(0, x.s - 1);
                    x.e = std::max(0, x.e - 1);
                }
                for (auto& [id, s, e] : closedRanges) {
                    s = std::max(0, s - 1);
                    e = std::max(0, e - 1);
                }
            }
        }
        const int specIndex = static_cast<int>(c.specs->size());
        c.specs->push_back(sp);
        for (const auto& [id, s, e] : closedRanges) {
            DocxReader::CommentOut co;
            co.specIndex = specIndex;
            co.start = std::min<int>(s, int(sp.text.size()));
            co.end = std::min<int>(e, int(sp.text.size()));
            co.messages = c.comments.value(id);
            if (!co.messages.isEmpty() && co.end > co.start)
                c.commentsOut->push_back(std::move(co));
        }
        // A range that OPENED here but closes in a later paragraph clamps to
        // this one (capability map).
        for (auto it = rangeStarts.constBegin(); it != rangeStarts.constEnd(); ++it) {
            DocxReader::CommentOut co;
            co.specIndex = specIndex;
            co.start = std::min<int>(it.value(), int(sp.text.size()));
            co.end = int(sp.text.size());
            co.messages = c.comments.value(it.key());
            if (!co.messages.isEmpty() && co.end > co.start)
                c.commentsOut->push_back(std::move(co));
        }
    }
    for (auto& msp : mediaSpecs) c.specs->push_back(std::move(msp));
    return {};
}

void parseTable(QXmlStreamReader& xml, Ctx& c) {
    std::vector<QStringList> rows;
    while (!xml.atEnd()) {
        const auto t = xml.readNext();
        if (t == QXmlStreamReader::EndElement && xml.name() == QLatin1String("tbl"))
            break;
        if (t != QXmlStreamReader::StartElement) continue;
        if (xml.name() == QLatin1String("tr")) {
            rows.emplace_back();
        } else if (xml.name() == QLatin1String("tc") && !rows.empty()) {
            QString cell;
            while (!xml.atEnd()) {
                const auto ct = xml.readNext();
                if (ct == QXmlStreamReader::EndElement
                    && xml.name() == QLatin1String("tc")) break;
                if (ct == QXmlStreamReader::StartElement
                    && xml.name() == QLatin1String("p")) {
                    const QString ptxt = parseParagraph(xml, c, /*cellTextMode*/true);
                    if (!ptxt.isEmpty()) {
                        if (!cell.isEmpty()) cell += QLatin1Char('\n');
                        cell += ptxt;
                    }
                }
            }
            rows.back() << cell;
        }
    }
    if (rows.empty()) return;
    int cols = 0;
    for (const QStringList& r : rows) cols = std::max<int>(cols, r.size());
    if (cols == 0) return;
    TableGrid g = TableGrid::makeEmpty(static_cast<int>(rows.size()), cols);
    for (int r = 0; r < static_cast<int>(rows.size()); ++r)
        for (int cix = 0; cix < rows[r].size(); ++cix)
            g.setCellText(r, cix, rows[r][cix]);
    BlockModel::BlockSpec sp;
    sp.type = BlockModel::Table;
    sp.tableJson = g.toJson();
    c.specs->push_back(std::move(sp));
}

} // namespace

DocxReader::Result DocxReader::read(const QString& docxPath, MediaStore* store) {
    Result out;
    const QByteArray docXml =
        mnpkg::readEntry(docxPath, QStringLiteral("word/document.xml"));
    if (docXml.isEmpty()) return out;

    Ctx c;
    c.zipPath = docxPath;
    c.store = store;
    c.specs = &out.specs;
    c.commentsOut = &out.comments;
    parseRels(c);
    parseNumbering(c);
    parseComments(c);

    QXmlStreamReader xml(docXml);
    while (!xml.atEnd()) {
        const auto t = xml.readNext();
        if (t != QXmlStreamReader::StartElement) continue;
        if (xml.name() == QLatin1String("p"))
            parseParagraph(xml, c, /*cellTextMode*/false);
        else if (xml.name() == QLatin1String("tbl"))
            parseTable(xml, c);
    }
    if (xml.hasError() && out.specs.empty()) return out;

    // Consecutive Code paragraphs coalesce (the emitter writes one per line).
    std::vector<BlockModel::BlockSpec> merged;
    for (auto& sp : out.specs) {
        if (sp.type == BlockModel::Code && !merged.empty()
            && merged.back().type == BlockModel::Code) {
            // Comment indices past this point shift — code blocks don't carry
            // comments in our emitter, so remap conservatively.
            for (auto& co : out.comments)
                if (co.specIndex >= static_cast<int>(merged.size())) co.specIndex -= 1;
            merged.back().text += QLatin1Char('\n') + sp.text;
        } else {
            merged.push_back(std::move(sp));
        }
    }
    out.specs = std::move(merged);
    out.ok = true;
    return out;
}
