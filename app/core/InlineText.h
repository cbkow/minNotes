#pragma once
// Inline text engine (SR-1, PLAN-SR1-inline-engine.md): the text + span rules shared
// by document rows and (until split rows retire the Table block) table cells. Pure
// functions over (QString, spans) — independent of row indices, so split-row child
// blocks reuse them unchanged.
//
// Offsets are UTF-16 code units ([s,e) over QString), matching JS strings in QML.

#include <QJsonObject>
#include <QString>
#include <QVariantList>

#include <cstdint>
#include <functional>
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

// --- Text edits: text and spans change together ---
// Insert `ins` at `at` (clamped to the text). Returns the clamped position.
int insertText(QString& text, Spans& spans, int at, const QString& ins);
// Delete [from,to) (clamped). Returns false — nothing changed — for an empty range.
bool deleteRange(QString& text, Spans& spans, int from, int to);
// Replace [s,e) (clamped, e >= s) with `with`. A span covering the whole replaced range
// keeps covering it (a bold or linked word stays bold/linked after a fix); other spans
// shift or clip around it. Returns false when nothing would change.
bool replaceRange(QString& text, Spans& spans, int s, int e, const QString& with);
// Replace the whole text: spans clamp to the new length and emptied spans drop.
void setText(QString& text, Spans& spans, const QString& with);
// Typing attributes for a freshly inserted run [s,e): `marks` bitmask (1 bold, 2 italic,
// 4 code, 8 strike, 16 underline) plus optional colour / highlight pens.
void applyTypingAttributes(Spans& spans, int s, int e, int marks,
                           const QString& fg, const QString& bg);

// --- Format ops: ranges clamp to the text; an empty range or unknown kind is a no-op ---
// Is [start,end) fully covered by `kind`?
bool hasFormat(const QString& text, const Spans& spans, int start, int end, uint8_t kind);
// Add (on) or remove `kind` over [start,end). Returns false when nothing applies.
bool setFormat(const QString& text, Spans& spans, int start, int end, uint8_t kind, bool on);
// Flip `kind` over [start,end): removed where it already covers the whole range, else added.
bool toggleFormat(const QString& text, Spans& spans, int start, int end, uint8_t kind);
// Clear formatting over [start,end): bold … underline, link, text colour, highlight.
// Comment and choice spans survive — they carry meaning (a thread; a chip whose text is
// its label), not styling.
bool clearFormat(const QString& text, Spans& spans, int start, int end);
// Do payload spans of `kind` carrying exactly `value` cover [start,end)?
bool payloadCovers(const QString& text, const Spans& spans, int start, int end,
                   uint8_t kind, const QString& value);

// The span feed QML and overlays read: [{s, e, k, u?}], with `u` present exactly for
// payload kinds.
QVariantList spansToVariantList(const Spans& spans);

// --- Choice chips (DT-2) ---
// A chip is a Choice span whose TEXT is the selected option's label and whose href is
// the payload {"o":[{"id","l","c"}...],"v":selectedId}. Labels never carry
// markdown-active characters (convertMarkdown scans raw text).
QString sanitizeChoiceLabel(QString label);
QString choiceLabelFor(const QJsonObject& payload, const QString& id);
QString choiceColorFor(const QJsonObject& payload, const QString& id);
QString encodeChoicePayload(const QJsonObject& payload);   // compact JSON for href
// The chip covering `col` (s <= col < e), or nullptr.
const Span* choiceAt(const Spans& spans, int col);
// Insert a chip carrying `payload` at `col` (clamped; moved past a chip it lands inside).
// Returns the chip's start.
int insertChoice(QString& text, Spans& spans, int col, const QJsonObject& payload);
// Rewrite the chip starting at `spanStart`: `edit` changes the payload and names the label
// to show, or returns false to abort. The label text and every later span shift together.
// Returns false (nothing changed) when there is no chip there or the edit aborts.
using ChoiceEdit = std::function<bool(QJsonObject& payload, QString& label)>;
bool editChoice(QString& text, Spans& spans, int spanStart, const ChoiceEdit& edit);
// Payload edits for editChoice.
// Select option `id`: false when it is already selected or unknown.
bool selectOption(QJsonObject& payload, const QString& id, QString& label);
// Append option `id` (label sanitized) and select it.
void addOption(QJsonObject& payload, const QString& id, const QString& label,
               const QString& color, QString& shownLabel);
// Replace the option set ([{id?, label, color?}]; missing ids minted). The selection
// survives if its id does, else falls to the first option. False for an empty list.
bool setOptions(QJsonObject& payload, const QVariantList& options,
                const std::function<QString()>& mintId, QString& label);
// The chip overlay feed: [{s, e, color}] with the selected option's colour.
QVariantList choiceRanges(const Spans& spans);

} // namespace mn::inl
