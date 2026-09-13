#pragma once
// Inline text engine (SR-1, PLAN-SR1-inline-engine.md): the text + span rules shared
// by document rows and (until split rows retire the Table block) table cells. Pure
// functions over (QString, spans) — independent of row indices, so split-row child
// blocks reuse them unchanged.
//
// Offsets are UTF-16 code units ([s,e) over QString), matching JS strings in QML.

#include <QString>

#include <cstdint>
#include <vector>

namespace mn::inl {

// [s,e) over a block's text. `href` carries the payload for payload kinds (link URL,
// colour hex, comment thread id, choice JSON); empty for every other kind.
// Aggregate-init as {s,e,kind} leaves href empty.
struct Span { int s; int e; uint8_t kind; QString href; };
using Spans = std::vector<Span>;

// Span kinds. The numeric values are persisted (cell JSON) and exposed to QML — never
// renumber. BlockModel::SpanKind mirrors these values.
enum Kind : uint8_t {
    Bold = 1, Italic = 2, Code = 3, Strike = 4, Underline = 5, Link = 6,
    FgColor = 7,     // href = colour hex
    Highlight = 8,   // href = colour hex
    Comment = 9,     // href = thread id
    Choice = 10,     // href = {"o":[{"id","l","c"}...],"v":selectedId}; the span TEXT is the label
};

// Stable names used by block attrs + the clipboard payload ("bold", "choice", ...).
uint8_t kindFromString(const QString& s);
const char* kindToString(uint8_t k);

// Kinds whose href carries a payload — serialized as "u", pushed whole, never merged
// by kind.
inline bool hasPayload(uint8_t k) {
    return k == Link || k == FgColor || k == Highlight || k == Comment || k == Choice;
}

// --- Interval ops (same-kind) ---
// Does the union of same-kind spans fully cover [start,end)?
bool spansCover(const Spans& v, int start, int end, uint8_t kind);
// Add [start,end) of `kind`, then merge overlapping/adjacent same-kind spans.
void addSpan(Spans& v, int start, int end, uint8_t kind);
// Subtract [start,end) from same-kind spans (splitting where it lands inside).
void removeSpan(Spans& v, int start, int end, uint8_t kind);
// A payload run: clear the kind's coverage in [start,end), add the run, then coalesce
// adjacent spans carrying the same payload.
void applyPayloadRun(Spans& v, int start, int end, uint8_t kind, const QString& payload);

// --- Offset bookkeeping for text edits ---
// Text inserted at `at` (len chars): spans after shift, spans the caret is strictly
// inside grow (typing at a span's exact end does not extend it).
void shiftSpansInsert(Spans& v, int at, int len);
// [from,to) deleted: spans after shift left, overlapping spans shrink, emptied spans drop.
void shiftSpansDelete(Spans& v, int from, int to);

} // namespace mn::inl
