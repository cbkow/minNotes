#include "InlineText.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QVariantMap>

#include <algorithm>

namespace mn::inl {

uint8_t kindFromString(const QString& s) {
    if (s == QLatin1String("bold"))      return Bold;
    if (s == QLatin1String("italic"))    return Italic;
    if (s == QLatin1String("code"))      return Code;
    if (s == QLatin1String("strike"))    return Strike;
    if (s == QLatin1String("underline")) return Underline;
    if (s == QLatin1String("link"))      return Link;
    if (s == QLatin1String("color"))     return FgColor;
    if (s == QLatin1String("highlight")) return Highlight;
    if (s == QLatin1String("comment"))   return Comment;
    if (s == QLatin1String("choice"))    return Choice;
    return 0;
}

const char* kindToString(uint8_t k) {
    switch (k) {
    case Bold:      return "bold";
    case Italic:    return "italic";
    case Code:      return "code";
    case Strike:    return "strike";
    case Underline: return "underline";
    case Link:      return "link";
    case FgColor:   return "color";
    case Highlight: return "highlight";
    case Comment:   return "comment";
    case Choice:    return "choice";
    default:        return "";
    }
}

bool spansCover(const Spans& v, int start, int end, uint8_t kind) {
    if (start >= end) return true;
    Spans k;
    for (const Span& sp : v) if (sp.kind == kind) k.push_back(sp);
    std::sort(k.begin(), k.end(), [](const Span& a, const Span& b){ return a.s < b.s; });
    int cur = start;
    for (const Span& sp : k) {
        if (sp.s > cur) break;                 // gap before coverage reaches `cur`
        cur = std::max(cur, sp.e);
        if (cur >= end) return true;
    }
    return cur >= end;
}

void addSpan(Spans& v, int start, int end, uint8_t kind) {
    if (start >= end) return;
    Spans k, others;
    for (const Span& sp : v) (sp.kind == kind ? k : others).push_back(sp);
    k.push_back({start, end, kind});
    std::sort(k.begin(), k.end(), [](const Span& a, const Span& b){ return a.s < b.s; });
    Spans merged;
    for (const Span& sp : k) {
        if (!merged.empty() && sp.s <= merged.back().e)
            merged.back().e = std::max(merged.back().e, sp.e);
        else
            merged.push_back(sp);
    }
    v = others;
    v.insert(v.end(), merged.begin(), merged.end());
}

void applyPayloadRun(Spans& v, int start, int end, uint8_t kind, const QString& payload) {
    if (start >= end || payload.isEmpty()) return;
    removeSpan(v, start, end, kind);                       // run owns this range
    Spans same, others;
    for (const Span& sp : v) (sp.kind == kind ? same : others).push_back(sp);
    same.push_back({start, end, kind, payload});
    std::sort(same.begin(), same.end(), [](const Span& a, const Span& b){ return a.s < b.s; });
    Spans merged;
    for (const Span& sp : same) {
        if (!merged.empty() && sp.s <= merged.back().e && sp.href == merged.back().href)
            merged.back().e = std::max(merged.back().e, sp.e);
        else
            merged.push_back(sp);
    }
    v = others;
    v.insert(v.end(), merged.begin(), merged.end());
}

void removeSpan(Spans& v, int start, int end, uint8_t kind) {
    if (start >= end) return;
    Spans out;
    for (const Span& sp : v) {
        if (sp.kind != kind || sp.e <= start || sp.s >= end) { out.push_back(sp); continue; }
        if (sp.s < start) out.push_back({sp.s, start, kind, sp.href});   // left remainder
        if (sp.e > end)   out.push_back({end, sp.e, kind, sp.href});     // right remainder
    }
    v = out;
}

void shiftSpansInsert(Spans& v, int at, int len) {
    for (Span& sp : v) {
        if (at <= sp.s)      { sp.s += len; sp.e += len; }   // wholly after the caret
        else if (at < sp.e)  { sp.e += len; }                // typed inside → grow (not at exact end)
    }
}

void shiftSpansDelete(Spans& v, int from, int to) {
    const int len = to - from;
    if (len <= 0) return;
    Spans out;
    for (const Span& sp : v) {
        if (sp.e <= from) { out.push_back(sp); continue; }                       // before cut
        if (sp.s >= to)   { out.push_back({sp.s - len, sp.e - len, sp.kind, sp.href}); continue; }  // after cut
        const int ns = std::min(sp.s, from);                 // surviving head + shifted tail collapse
        const int ne = (sp.e > to) ? sp.e - len : from;
        if (ne > ns) out.push_back({ns, ne, sp.kind, sp.href});
    }
    v = out;
}

int insertText(QString& text, Spans& spans, int at, const QString& ins) {
    at = std::clamp(at, 0, int(text.size()));
    text.insert(at, ins);
    if (!spans.empty()) shiftSpansInsert(spans, at, int(ins.size()));
    return at;
}

bool deleteRange(QString& text, Spans& spans, int from, int to) {
    const int len = int(text.size());
    from = std::clamp(from, 0, len);
    to = std::clamp(to, 0, len);
    if (from >= to) return false;
    text.remove(from, to - from);
    shiftSpansDelete(spans, from, to);
    return true;
}

bool replaceRange(QString& text, Spans& spans, int s, int e, const QString& with) {
    const int len = int(text.size());
    s = std::clamp(s, 0, len);
    e = std::clamp(e, s, len);
    if (s == e && with.isEmpty()) return false;
    text.replace(s, e - s, with);
    if (!spans.empty()) {
        const int n = int(with.size());
        const int delta = n - (e - s);
        Spans kept;
        for (Span sp : spans) {
            if (sp.e <= s) { kept.push_back(sp); continue; }                                  // before
            if (sp.s >= e) { sp.s += delta; sp.e += delta; kept.push_back(sp); continue; }    // after
            if (sp.s <= s && sp.e >= e) { sp.e += delta; if (sp.e > sp.s) kept.push_back(sp); continue; }  // covers
            if (sp.s < s) { sp.e = s; if (sp.e > sp.s) kept.push_back(sp); continue; }        // overlaps the start
            sp.s = s + n; sp.e += delta; if (sp.e > sp.s) kept.push_back(sp);                  // overlaps the end
        }
        spans = std::move(kept);
    }
    return true;
}

void setText(QString& text, Spans& spans, const QString& with) {
    text = with;
    const int len = int(with.size());
    Spans kept;
    for (Span sp : spans) {
        sp.s = std::min(sp.s, len);
        sp.e = std::min(sp.e, len);
        if (sp.e > sp.s) kept.push_back(sp);
    }
    spans = std::move(kept);
}

void applyTypingAttributes(Spans& spans, int s, int e, int marks,
                           const QString& fg, const QString& bg) {
    if (s >= e) return;
    if (marks & 1)  addSpan(spans, s, e, Bold);
    if (marks & 2)  addSpan(spans, s, e, Italic);
    if (marks & 4)  addSpan(spans, s, e, Code);
    if (marks & 8)  addSpan(spans, s, e, Strike);
    if (marks & 16) addSpan(spans, s, e, Underline);
    if (!fg.isEmpty()) applyPayloadRun(spans, s, e, FgColor, fg);
    if (!bg.isEmpty()) applyPayloadRun(spans, s, e, Highlight, bg);
}

namespace {
// Clamp [start,end) to the text; true when the clamped range is non-empty.
bool clampRange(const QString& text, int& start, int& end) {
    const int len = int(text.size());
    start = std::clamp(start, 0, len);
    end = std::clamp(end, 0, len);
    return start < end;
}
} // namespace

bool hasFormat(const QString& text, const Spans& spans, int start, int end, uint8_t kind) {
    if (!kind || !clampRange(text, start, end)) return false;
    return spansCover(spans, start, end, kind);
}

bool setFormat(const QString& text, Spans& spans, int start, int end, uint8_t kind, bool on) {
    if (!kind || !clampRange(text, start, end)) return false;
    if (on) addSpan(spans, start, end, kind);
    else    removeSpan(spans, start, end, kind);
    return true;
}

bool toggleFormat(const QString& text, Spans& spans, int start, int end, uint8_t kind) {
    if (!kind || !clampRange(text, start, end)) return false;
    if (spansCover(spans, start, end, kind)) removeSpan(spans, start, end, kind);
    else                                     addSpan(spans, start, end, kind);
    return true;
}

bool clearFormat(const QString& text, Spans& spans, int start, int end) {
    if (spans.empty() || !clampRange(text, start, end)) return false;
    for (const Kind k : {Bold, Italic, Code, Strike, Underline, Link, FgColor, Highlight})
        removeSpan(spans, start, end, k);
    return true;
}

bool payloadCovers(const QString& text, const Spans& spans, int start, int end,
                   uint8_t kind, const QString& value) {
    if (!clampRange(text, start, end)) return false;
    std::vector<std::pair<int, int>> segs;
    for (const Span& sp : spans)
        if (sp.kind == kind && sp.href == value && sp.e > start && sp.s < end)
            segs.emplace_back(sp.s, sp.e);
    std::sort(segs.begin(), segs.end());
    int cov = start;
    for (const auto& seg : segs) {
        if (seg.first > cov) break;          // gap before this segment
        cov = std::max(cov, seg.second);
        if (cov >= end) return true;
    }
    return cov >= end;
}

QVariantList spansToVariantList(const Spans& spans) {
    QVariantList out;
    out.reserve(qsizetype(spans.size()));
    for (const Span& sp : spans) {
        QVariantMap m;
        m.insert(QStringLiteral("s"), sp.s);
        m.insert(QStringLiteral("e"), sp.e);
        m.insert(QStringLiteral("k"), int(sp.kind));
        if (hasPayload(sp.kind)) m.insert(QStringLiteral("u"), sp.href);
        out.append(m);
    }
    return out;
}

// --- Choice chips ---

QString sanitizeChoiceLabel(QString label) {
    static const QString bad = QStringLiteral("`*_~[]\\\n\r");
    QString out;
    out.reserve(label.size());
    for (const QChar ch : label)
        if (!bad.contains(ch)) out += ch;
    out = out.trimmed();
    return out.isEmpty() ? QStringLiteral("Option") : out;
}

namespace {
QString optionField(const QJsonObject& payload, const QString& id, const QString& field) {
    for (const QJsonValue& v : payload.value(QStringLiteral("o")).toArray()) {
        const QJsonObject o = v.toObject();
        if (o.value(QStringLiteral("id")).toString() == id)
            return o.value(field).toString();
    }
    return {};
}
} // namespace

QString choiceLabelFor(const QJsonObject& payload, const QString& id) {
    return optionField(payload, id, QStringLiteral("l"));
}

QString choiceColorFor(const QJsonObject& payload, const QString& id) {
    return optionField(payload, id, QStringLiteral("c"));
}

QString encodeChoicePayload(const QJsonObject& payload) {
    return QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact));
}

const Span* choiceAt(const Spans& spans, int col) {
    for (const Span& sp : spans)
        if (sp.kind == Choice && col >= sp.s && col < sp.e) return &sp;
    return nullptr;
}

int insertChoice(QString& text, Spans& spans, int col, const QJsonObject& payload) {
    col = std::clamp(col, 0, int(text.size()));
    for (const Span& sp : spans)   // never inside another chip
        if (sp.kind == Choice && col > sp.s && col < sp.e) col = sp.e;
    const QString label = choiceLabelFor(payload, payload.value(QStringLiteral("v")).toString());
    shiftSpansInsert(spans, col, int(label.size()));
    text.insert(col, label);
    spans.push_back({col, col + int(label.size()), Choice, encodeChoicePayload(payload)});
    return col;
}

bool editChoice(QString& text, Spans& spans, int spanStart, const ChoiceEdit& edit) {
    const auto it = std::find_if(spans.begin(), spans.end(), [&](const Span& sp) {
        return sp.kind == Choice && sp.s == spanStart;
    });
    if (it == spans.end()) return false;
    QJsonObject payload = QJsonDocument::fromJson(it->href.toUtf8()).object();
    QString label;
    if (!edit(payload, label)) return false;
    const Span chip = *it;
    spans.erase(it);
    shiftSpansDelete(spans, chip.s, chip.e);
    shiftSpansInsert(spans, chip.s, int(label.size()));
    text.replace(chip.s, chip.e - chip.s, label);
    spans.push_back({chip.s, chip.s + int(label.size()), Choice, encodeChoicePayload(payload)});
    return true;
}

bool selectOption(QJsonObject& payload, const QString& id, QString& label) {
    if (payload.value(QStringLiteral("v")).toString() == id) return false;   // already selected
    label = choiceLabelFor(payload, id);
    if (label.isEmpty()) return false;                                      // unknown id
    payload.insert(QStringLiteral("v"), id);
    return true;
}

void addOption(QJsonObject& payload, const QString& id, const QString& label,
               const QString& color, QString& shownLabel) {
    shownLabel = sanitizeChoiceLabel(label);
    QJsonArray opts = payload.value(QStringLiteral("o")).toArray();
    QJsonObject o;
    o.insert(QStringLiteral("id"), id);
    o.insert(QStringLiteral("l"), shownLabel);
    if (!color.isEmpty()) o.insert(QStringLiteral("c"), color);
    opts.append(o);
    payload.insert(QStringLiteral("o"), opts);
    payload.insert(QStringLiteral("v"), id);   // quick-add implies intent
}

bool setOptions(QJsonObject& payload, const QVariantList& options,
                const std::function<QString()>& mintId, QString& label) {
    if (options.isEmpty()) return false;
    QJsonArray opts;
    for (const QVariant& v : options) {
        const QVariantMap m = v.toMap();
        QString id = m.value(QStringLiteral("id")).toString();
        if (id.isEmpty()) id = mintId();
        QJsonObject o;
        o.insert(QStringLiteral("id"), id);
        o.insert(QStringLiteral("l"), sanitizeChoiceLabel(m.value(QStringLiteral("label")).toString()));
        const QString c = m.value(QStringLiteral("color")).toString();
        if (!c.isEmpty()) o.insert(QStringLiteral("c"), c);
        opts.append(o);
    }
    QString sel = payload.value(QStringLiteral("v")).toString();
    payload.insert(QStringLiteral("o"), opts);
    label = choiceLabelFor(payload, sel);
    if (label.isEmpty()) {   // the selected option was deleted → first option
        sel = opts.first().toObject().value(QStringLiteral("id")).toString();
        label = opts.first().toObject().value(QStringLiteral("l")).toString();
    }
    payload.insert(QStringLiteral("v"), sel);
    return true;
}

QVariantList choiceRanges(const Spans& spans) {
    QVariantList out;
    for (const Span& sp : spans) {
        if (sp.kind != Choice || sp.e <= sp.s) continue;
        const QJsonObject payload = QJsonDocument::fromJson(sp.href.toUtf8()).object();
        QVariantMap m;
        m.insert(QStringLiteral("s"), sp.s);
        m.insert(QStringLiteral("e"), sp.e);
        m.insert(QStringLiteral("color"), choiceColorFor(payload, payload.value(QStringLiteral("v")).toString()));
        out.append(m);
    }
    return out;
}

} // namespace mn::inl
