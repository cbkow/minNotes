#include "InlineText.h"

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

} // namespace mn::inl
