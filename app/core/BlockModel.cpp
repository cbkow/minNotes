#include "BlockModel.h"
#include <QStringBuilder>
#include <QStandardPaths>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QVariant>
#include <QUrl>
#include <QSet>
#include <algorithm>

namespace {
// Deterministic per-row hash (no RNG: must be stable across rebuilds and frames).
inline uint32_t rowHash(int row) {
    uint32_t x = static_cast<uint32_t>(row) * 2654435761u + 0x9E3779B9u;
    x ^= x >> 16; x *= 0x7FEB352Du; x ^= x >> 15;
    return x;
}

// Spike layout constants (square-pixel; real heights come from the delegate).
constexpr double kLine     = 22.0;   // px per text line
constexpr double kPadV     = 16.0;   // vertical padding per block
constexpr double kHeading  = 40.0;
constexpr double kWidthEst = 760.0;  // assumed content width for media estimate
constexpr double kVideoBar = 40.0;   // transport toolbar reserved under a video
                                     // (keep in sync with Editor.qml videoTransportH)

// Fractional-rank alphabet: 62 digits in ascending ASCII order, so plain
// string comparison == numeric order. Shared by encode62 + rankBetween.
const QString kRankAlpha =
    QStringLiteral("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz");

// Fixed-width base-62 of v. Used to seed evenly-spaced, non-minimum ranks
// (NOT all-'0', which would leave no room for inserts before the first block).
QString encode62(quint64 v, int width) {
    QString s(width, QLatin1Char('0'));
    for (int i = width - 1; i >= 0; --i) { s[i] = kRankAlpha[int(v % 62)]; v /= 62; }
    return s;
}
}

BlockModel::BlockModel(QObject* parent) : QAbstractListModel(parent) {
    // Open the document database (one scratch doc for now; the open-file flow
    // and per-document instances come with the menu/chassis work).
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    const QString path = dir + QStringLiteral("/scratch.mndb");
    docPath_ = path;
    mediaStore_ = std::make_unique<MediaStore>(docPath_);
    if (!doc_.open(path)) {
        qWarning("BlockModel: store unavailable, falling back to in-memory synthetic doc");
        rebuild(10000, Uniform);
        return;
    }
    if (doc_.count() == 0)
        seedSyntheticStore(2000);
    loadFromStore();
}

BlockModel::BlockType BlockModel::typeFromString(const QString& s) {
    if (s == QLatin1String("heading"))   return Heading;
    if (s == QLatin1String("code"))      return Code;
    if (s == QLatin1String("media"))     return Media;
    if (s == QLatin1String("quote"))     return Quote;
    if (s == QLatin1String("list_item")) return ListItem;
    if (s == QLatin1String("divider"))   return Divider;
    if (s == QLatin1String("table"))     return Table;
    return Paragraph;
}

const char* BlockModel::typeToString(uint8_t t) {
    switch (t) {
    case Heading:  return "heading";
    case Code:     return "code";
    case Media:    return "media";
    case Quote:    return "quote";
    case ListItem: return "list_item";
    case Divider:  return "divider";
    case Table:    return "table";
    default:       return "paragraph";
    }
}

QString BlockModel::rankBetween(const QString& a, const QString& b) {
    const QString& kAlpha = kRankAlpha;
    const int B = kAlpha.size();
    auto val = [&](QChar c) { return kAlpha.indexOf(c); };

    QString r;
    int i = 0;
    bool bInf = b.isEmpty();   // b exhausted → treat as +infinity
    while (true) {
        const int ca = (i < a.size()) ? val(a[i]) : 0;           // a padded with min digit
        const int cb = (bInf || i >= b.size()) ? B : val(b[i]);  // b padded with one-past-max
        if (ca + 1 < cb) {                 // room for a digit strictly between
            r += kAlpha[(ca + cb) / 2];
            return r;
        }
        r += kAlpha[ca];                   // ca == cb, or adjacent (ca+1 == cb)
        if (ca < cb) bInf = true;          // placed a digit below b → b no longer bounds us
        ++i;
    }
}

void BlockModel::persistContent(int row) {
    if (doc_.isOpen() && row >= 0 && row < static_cast<int>(ids_.size()))
        doc_.updateContent(ids_[row], content_[row]);
}

void BlockModel::seedSyntheticStore(int n) {
    // One-time fill of an empty doc so there's something to edit. Evenly-spaced
    // width-4 base-62 ranks (NOT zero-padded — those are all-minimum and leave
    // no room to insert before block 0); rankBetween subdivides for inserts.
    const quint64 span = 62ull * 62 * 62 * 62;
    const quint64 step = std::max<quint64>(1, span / static_cast<quint64>(n + 1));
    doc_.begin();
    for (int i = 0; i < n; ++i) {
        Row r{}; r.type = Paragraph; r.param = 1;
        const QString rank = encode62(static_cast<quint64>(i + 1) * step, 4);
        doc_.appendBlock(makeUlid(), rank, 0, QString::fromLatin1(typeToString(r.type)),
                         QString(), genBase(i, r));
    }
    doc_.commit();
}

void BlockModel::loadFromStore() {
    beginResetModel();
    rows_.clear();
    ids_.clear();
    ranks_.clear();
    content_.clear();

    const std::vector<Document::BlockMeta> metas = doc_.skinnyScan();
    rows_.reserve(metas.size());
    ids_.reserve(metas.size());
    ranks_.reserve(metas.size());
    content_.reserve(metas.size());
    std::vector<double> heights;
    heights.reserve(metas.size());

    for (const Document::BlockMeta& m : metas) {
        QString text = doc_.contentFor(m.id);   // Phase 1a: load eagerly
        Row r{};
        r.type = typeFromString(m.type);
        r.param = static_cast<uint16_t>(std::max<int>(1, text.count(QLatin1Char('\n')) + 1));
        const QJsonObject o = QJsonDocument::fromJson(m.attrs.toUtf8()).object();
        if (r.type == Heading)
            r.level = static_cast<uint8_t>(std::clamp(o.value(QStringLiteral("level")).toInt(1), 1, 6));
        if (r.type == Code)
            r.lang = o.value(QStringLiteral("lang")).toString();
        if (r.type == Table)   // content is the grid JSON; param = row count for height estimate
            r.param = static_cast<uint16_t>(std::max(1, TableGrid::fromJson(text).rows()));
        if (r.type == Media) { // content is the descriptor JSON; param = aspect*100 (h/w)
            const QJsonObject mo = QJsonDocument::fromJson(text.toUtf8()).object();
            const int mw = mo.value(QStringLiteral("w")).toInt(), mh = mo.value(QStringLiteral("h")).toInt();
            if (mw > 0 && mh > 0) r.param = static_cast<uint16_t>(std::clamp(int(100.0 * mh / mw + 0.5), 1, 1000));
            r.mediaW = static_cast<uint16_t>(std::clamp(mw, 0, 65535));
            r.mediaH = static_cast<uint16_t>(std::clamp(mh, 0, 65535));
            r.isVideo = mo.value(QStringLiteral("kind")).toString() == QLatin1String("video");
        }
        for (const QJsonValue& sv : o.value(QStringLiteral("spans")).toArray()) {
            const QJsonObject so = sv.toObject();
            const uint8_t k = spanKindFromString(so.value(QStringLiteral("k")).toString());
            const int s = so.value(QStringLiteral("s")).toInt(), e = so.value(QStringLiteral("e")).toInt();
            if (k && e > s) r.spans.push_back({s, e, k});
        }
        // Markdown is an input method, not storage: consume any inline markers
        // into spans on load so non-active blocks render clean (markers only ever
        // appear while you're typing them). In-memory only; the DB migrates lazily
        // as blocks are edited / committed.
        if (r.type == Paragraph || r.type == Quote || r.type == ListItem) {
            QString clean; std::vector<Span> spans;
            if (convertMarkdown(text, r.spans, clean, spans)) { text = clean; r.spans = spans; }
        }
        // Defensive: drop/clamp spans that exceed the content (stale data must
        // never push the highlighter / positionToRectangle out of range).
        {
            const int len = text.size();
            std::vector<Span> ok;
            for (Span sp : r.spans) {
                sp.s = std::clamp(sp.s, 0, len);
                sp.e = std::clamp(sp.e, 0, len);
                if (sp.e > sp.s) ok.push_back(sp);
            }
            r.spans.swap(ok);
        }
        rows_.push_back(r);
        ids_.push_back(m.id);
        ranks_.push_back(m.rank);
        content_.push_back(text);
        heights.push_back(estimatedHeight(r));
    }
    fenwick_.reset(std::move(heights));
    endResetModel();
    ++layoutRevision_;
    clearUndo();
    emit modelReset();
    emit layoutChangedSpike();
}

void BlockModel::rebuild(int n, int distribution) {
    beginResetModel();
    rows_.clear();
    content_.clear();
    ids_.clear();
    ranks_.clear();
    rows_.reserve(static_cast<size_t>(std::max(0, n)));
    content_.reserve(static_cast<size_t>(std::max(0, n)));
    std::vector<double> heights;
    heights.reserve(static_cast<size_t>(std::max(0, n)));

    for (int i = 0; i < n; ++i) {
        const uint32_t h = rowHash(i);
        Row r{};
        if (distribution == Uniform) {
            r.type = Paragraph; r.param = 1;
        } else {
            const uint32_t bucket = h % 100;
            if (distribution == Adversarial && (h % 50) == 0) {
                r.type = (h & 1) ? Code : Paragraph;        // heavy tail
                r.param = static_cast<uint16_t>(200 + (h % 120));
            } else if (bucket < 70) {
                r.type = Paragraph; r.param = static_cast<uint16_t>(1 + (h % 3));
            } else if (bucket < 85) {
                r.type = Heading;   r.param = 0;
            } else if (bucket < 95) {
                r.type = Code;      r.param = static_cast<uint16_t>(5 + (h % 36));
            } else {
                r.type = Media;     r.param = static_cast<uint16_t>(40 + (h % 120)); // aspect h/w *100
            }
        }
        rows_.push_back(r);
        content_.push_back(genBase(i, r));   // generate text ONCE, here — not per scroll frame
        ids_.push_back(makeUlid());
        ranks_.push_back(encode62(static_cast<quint64>(i + 1)
                         * std::max<quint64>(1, (62ull*62*62*62) / static_cast<quint64>(n + 1)), 4));
        heights.push_back(estimatedHeight(r));
    }
    fenwick_.reset(std::move(heights));
    endResetModel();
    ++layoutRevision_;
    clearUndo();
    emit modelReset();
    emit layoutChangedSpike();
}

// Displayed media frame height: dispW = min(contentWidth, w) (never upscaled),
// height = round(dispW * h/w). Pure function of the probed dims + the layout
// width — recomputed when setContentWidth changes (resize). Falls back to the
// aspect param if intrinsic dims are missing.
double BlockModel::mediaFrameHeight(const Row& r) const {
    if (r.mediaW > 0 && r.mediaH > 0) {
        const double dispW = std::min<double>(contentWidth_, r.mediaW);
        return std::floor(dispW * r.mediaH / r.mediaW + 0.5);
    }
    return contentWidth_ * (r.param / 100.0);
}

double BlockModel::estimatedHeight(const Row& r) const {
    switch (r.type) {
    case Heading: return kHeading + kPadV;
    case Media:   // 12px vertical pad + the transport toolbar for video. Matches
                  // the Editor cell delegate exactly, and media never measures
                  // back, so this IS the authoritative height (no scroll-in jump).
        return 12.0 + mediaFrameHeight(r) + (r.isVideo ? kVideoBar : 0.0);
    case Divider: return 24.0;
    case Table:   return r.param * 34.0 + 44.0;     // param = row count; +header/strip
    case Code:
    case Paragraph:
    default:      return r.param * kLine + kPadV;  // quote/list ≈ paragraph
    }
}

int BlockModel::clampRow(int row) const {
    if (rows_.empty()) return 0;
    return std::clamp(row, 0, static_cast<int>(rows_.size()) - 1);
}

int BlockModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

int BlockModel::typeForRow(int row) const {
    if (rows_.empty()) return Paragraph;
    return rows_[clampRow(row)].type;
}

int BlockModel::levelForRow(int row) const {
    if (rows_.empty()) return 0;
    return rows_[clampRow(row)].level;
}

bool BlockModel::matchMarkdownPrefix(const QString& content, BlockType& type, int& level, int& strip) {
    level = 0;
    // Headings: 1–6 leading '#' then a space → heading of that level.
    int h = 0;
    while (h < content.size() && h < 6 && content[h] == QLatin1Char('#')) ++h;
    if (h > 0 && h < content.size() && content[h] == QLatin1Char(' ')) {
        type = Heading; level = h; strip = h + 1; return true;
    }
    // Quote: "> "
    if (content.startsWith(QLatin1String("> "))) { type = Quote; strip = 2; return true; }
    // Unordered list: "- ", "* ", or "+ "
    if (content.startsWith(QLatin1String("- ")) || content.startsWith(QLatin1String("* "))
        || content.startsWith(QLatin1String("+ "))) { type = ListItem; strip = 2; return true; }
    // Divider: "--- " (markers + content consumed entirely)
    if (content.startsWith(QLatin1String("--- "))) { type = Divider; strip = 4; return true; }
    return false;
}

QString BlockModel::attrsJson(uint8_t type, uint8_t level, const QString& lang,
                              const std::vector<Span>& spans) const {
    QJsonObject o;
    if (type == Heading && level > 0) o.insert(QStringLiteral("level"), level);
    if (type == Code && !lang.isEmpty()) o.insert(QStringLiteral("lang"), lang);
    if (!spans.empty()) {
        QJsonArray arr;
        for (const Span& sp : spans) {
            QJsonObject so;
            so.insert(QStringLiteral("s"), sp.s);
            so.insert(QStringLiteral("e"), sp.e);
            so.insert(QStringLiteral("k"), QString::fromLatin1(spanKindToString(sp.kind)));
            arr.append(so);
        }
        o.insert(QStringLiteral("spans"), arr);
    }
    return o.isEmpty() ? QString() : QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

void BlockModel::persistMeta(int row) {
    if (!doc_.isOpen() || row < 0 || row >= static_cast<int>(ids_.size())) return;
    const Row& r = rows_[row];
    doc_.updateMeta(ids_[row], QString::fromLatin1(typeToString(r.type)),
                    attrsJson(r.type, r.level, r.lang, r.spans));
}

// === Undo / redo: region-snapshot transactions ===========================
BlockModel::BlockSnap BlockModel::snapAt(int row) const {
    BlockSnap s;
    s.id = ids_[row]; s.rank = ranks_[row]; s.content = content_[row];
    s.type = rows_[row].type; s.level = rows_[row].level; s.spans = rows_[row].spans;
    s.lang = rows_[row].lang;
    return s;
}

std::vector<BlockModel::BlockSnap> BlockModel::snapshotRange(int lo, int hi) const {
    std::vector<BlockSnap> out;
    lo = std::max(0, lo);
    hi = std::min(hi, static_cast<int>(rows_.size()) - 1);
    for (int i = lo; i <= hi; ++i) out.push_back(snapAt(i));
    return out;
}

void BlockModel::applySnapshot(int lo, int oldCount, const std::vector<BlockSnap>& snaps) {
    applying_ = true;
    beginResetModel();
    const int last = std::min(lo + oldCount, static_cast<int>(rows_.size()));
    QSet<QString> newIds, oldIds;
    for (const BlockSnap& s : snaps) newIds.insert(s.id);
    for (int i = lo; i < last; ++i) {
        oldIds.insert(ids_[i]);
        if (doc_.isOpen() && !newIds.contains(ids_[i])) doc_.deleteBlock(ids_[i]);   // left the region
    }
    rows_.erase(rows_.begin() + lo, rows_.begin() + last);
    content_.erase(content_.begin() + lo, content_.begin() + last);
    ids_.erase(ids_.begin() + lo, ids_.begin() + last);
    ranks_.erase(ranks_.begin() + lo, ranks_.begin() + last);
    int at = lo;
    for (const BlockSnap& s : snaps) {
        Row r{}; r.type = s.type; r.level = s.level; r.spans = s.spans; r.lang = s.lang;
        r.param = static_cast<uint16_t>(std::max<int>(1, s.content.count(QLatin1Char('\n')) + 1));
        rows_.insert(rows_.begin() + at, r);
        content_.insert(content_.begin() + at, s.content);
        ids_.insert(ids_.begin() + at, s.id);
        ranks_.insert(ranks_.begin() + at, s.rank);
        if (doc_.isOpen()) {
            const QString attrs = attrsJson(s.type, s.level, s.lang, s.spans);
            const QString type = QString::fromLatin1(typeToString(s.type));
            if (oldIds.contains(s.id)) {   // survived: content/meta/rank may all have changed (incl. reorder)
                doc_.updateContent(s.id, s.content);
                doc_.updateMeta(s.id, type, attrs);
                doc_.updateRank(s.id, s.rank);
            } else {
                doc_.appendBlock(s.id, s.rank, 0, type, attrs, s.content);   // re-born (undo of a delete)
            }
        }
        ++at;
    }
    std::vector<double> heights; heights.reserve(rows_.size());
    for (const Row& r : rows_) heights.push_back(estimatedHeight(r));
    fenwick_.reset(std::move(heights));
    endResetModel();
    ++layoutRevision_; ++contentRevision_;
    emit modelReset(); emit layoutChangedSpike(); emit contentChangedSpike();
    applying_ = false;
}

void BlockModel::beginTxn(int lo, int hi) {
    if (applying_) return;                      // no recording during undo/redo apply
    if (txnDepth_ == 0) {                        // outermost: capture `before`
        txnLo_ = std::max(0, lo);
        txnHi_ = hi;
        txnSize_ = rows_.size();
        txnBefore_ = snapshotRange(txnLo_, txnHi_);
    }
    ++txnDepth_;                                 // inner mutations just nest in
}

void BlockModel::endTxn(const QString& coalesce) {
    if (applying_) return;
    if (txnDepth_ == 0) return;
    if (--txnDepth_ > 0) return;                 // wait for the outermost to commit
    const int delta = static_cast<int>(rows_.size()) - static_cast<int>(txnSize_);
    std::vector<BlockSnap> after = snapshotRange(txnLo_, txnHi_ + delta);

    // No-op group (e.g. "clear" on an already-plain paragraph) → no undo entry.
    auto sameSnaps = [](const std::vector<BlockSnap>& a, const std::vector<BlockSnap>& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            const BlockSnap& x = a[i]; const BlockSnap& y = b[i];
            if (x.id != y.id || x.rank != y.rank || x.type != y.type
                || x.level != y.level || x.content != y.content
                || x.spans.size() != y.spans.size()) return false;
            for (size_t j = 0; j < x.spans.size(); ++j)
                if (x.spans[j].s != y.spans[j].s || x.spans[j].e != y.spans[j].e
                    || x.spans[j].kind != y.spans[j].kind) return false;
        }
        return true;
    };
    if (sameSnaps(txnBefore_, after)) return;

    // Coalesce a run of typing into the previous entry (same key, same single
    // block, contiguous caret) so undo removes the whole run at once.
    if (!coalesce.isEmpty() && undoCur_ >= 0) {
        UndoEntry& prev = undo_[undoCur_];
        if (prev.coalesce == coalesce && prev.lo == txnLo_
            && prev.after.size() == 1 && txnBefore_.size() == 1 && after.size() == 1
            && prev.cRowA == cRow_ && prev.cColA == cCol_) {
            prev.after = std::move(after);
            awaitingAfter_ = true;     // next noteCaret stamps prev's caret-after
            emit undoStackChanged();
            return;
        }
    }
    UndoEntry e;
    e.lo = txnLo_;
    e.before = std::move(txnBefore_);
    e.after = std::move(after);
    e.cRowB = cRow_; e.cColB = cCol_; e.aRowB = aRow_; e.aColB = aCol_;
    e.cRowA = cRow_; e.cColA = cCol_; e.aRowA = aRow_; e.aColA = aCol_;   // until noteCaret stamps
    e.parent = undoCur_;
    e.coalesce = coalesce;
    undo_.push_back(std::move(e));
    undoCur_ = static_cast<int>(undo_.size()) - 1;
    awaitingAfter_ = true;
    emit undoStackChanged();
}

void BlockModel::clearUndo() {
    undo_.clear(); undoCur_ = -1; txnDepth_ = 0; awaitingAfter_ = false;
    emit undoStackChanged();
}

void BlockModel::beginGroup(int loRow, int hiRow) { beginTxn(loRow, hiRow); }
void BlockModel::endGroup() { endTxn(); }

bool BlockModel::hasFormat(int row, int start, int end, const QString& kind) const {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return false;
    const uint8_t k = spanKindFromString(kind);
    if (!k) return false;
    const int len = content_[row].size();
    start = std::clamp(start, 0, len); end = std::clamp(end, 0, len);
    if (start >= end) return false;
    return spansCover(rows_[row].spans, start, end, k);
}

void BlockModel::setFormat(int row, int start, int end, const QString& kind, bool on) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return;
    const uint8_t k = spanKindFromString(kind);
    if (!k) return;
    const int len = content_[row].size();
    start = std::clamp(start, 0, len); end = std::clamp(end, 0, len);
    if (start >= end) return;
    beginTxn(row, row);
    if (on) addSpan(rows_[row].spans, start, end, k);
    else    removeSpan(rows_[row].spans, start, end, k);
    persistMeta(row);
    emit dataChanged(index(row), index(row), {});
    ++contentRevision_; emit contentChangedSpike();
    endTxn();
}

bool BlockModel::canUndo() const { return undoCur_ >= 0; }
bool BlockModel::canRedo() const {
    for (int i = static_cast<int>(undo_.size()) - 1; i >= 0; --i)
        if (undo_[i].parent == undoCur_) return true;
    return false;
}

void BlockModel::undo() {
    if (undoCur_ < 0) return;
    awaitingAfter_ = false;
    const UndoEntry e = undo_[undoCur_];          // copy (applySnapshot won't touch undo_, but be safe)
    applySnapshot(e.lo, static_cast<int>(e.after.size()), e.before);
    undoCur_ = e.parent;
    emit caretRestoreRequested(e.cRowB, e.cColB, e.aRowB, e.aColB);
    emit undoStackChanged();
}

void BlockModel::redo() {
    awaitingAfter_ = false;
    int child = -1;                               // newest child of the current node
    for (int i = static_cast<int>(undo_.size()) - 1; i >= 0; --i)
        if (undo_[i].parent == undoCur_) { child = i; break; }
    if (child < 0) return;
    const UndoEntry e = undo_[child];
    applySnapshot(e.lo, static_cast<int>(e.before.size()), e.after);
    undoCur_ = child;
    emit caretRestoreRequested(e.cRowA, e.cColA, e.aRowA, e.aColA);
    emit undoStackChanged();
}

void BlockModel::noteCaret(int row, int col, int anchorRow, int anchorCol) {
    cRow_ = row; cCol_ = col; aRow_ = anchorRow; aCol_ = anchorCol;
    if (awaitingAfter_ && undoCur_ >= 0) {
        UndoEntry& e = undo_[undoCur_];
        e.cRowA = row; e.cColA = col; e.aRowA = anchorRow; e.aColA = anchorCol;
        awaitingAfter_ = false;
    }
}

void BlockModel::setHeading(int row, int level) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return;
    Row& r = rows_[row];
    if (r.type != Paragraph && r.type != Heading && r.type != Quote && r.type != ListItem) return;
    level = std::clamp(level, 0, 6);
    const uint8_t newType  = level == 0 ? static_cast<uint8_t>(Paragraph) : static_cast<uint8_t>(Heading);
    const uint8_t newLevel = level == 0 ? 0 : static_cast<uint8_t>(level);
    if (r.type == newType && r.level == newLevel) return;            // no-op
    beginTxn(row, row);
    r.type = newType;
    r.level = newLevel;
    persistMeta(row);
    emit dataChanged(index(row), index(row), {TypeRole});
    bumpLayout();                                                    // heading height differs
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
}

void BlockModel::setBlockType(int row, int type) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return;
    Row& r = rows_[row];
    if (r.type != Paragraph && r.type != Heading && r.type != Quote
        && r.type != ListItem && r.type != Code) return;
    const uint8_t t = static_cast<uint8_t>(type);
    if (t != Paragraph && t != Quote && t != ListItem) return;   // headings/code have their own paths
    if (r.type == t) return;
    beginTxn(row, row);
    r.type = t;
    r.level = 0;                 // leaving a heading clears its level
    r.lang.clear();              // leaving a code block clears its language
    persistMeta(row);
    emit dataChanged(index(row), index(row), {TypeRole});
    bumpLayout();
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
}

void BlockModel::insertDivider(int afterRow) {
    afterRow = std::clamp(afterRow, -1, static_cast<int>(rows_.size()) - 1);
    const int at = afterRow + 1;
    beginTxn(at, at - 1);        // empty `before`; after = [at,at]
    const QString newId = makeUlid();
    const QString newRank = rankBetween(
        (at > 0) ? ranks_[at - 1] : QString(),
        (at < static_cast<int>(ranks_.size())) ? ranks_[at] : QString());
    beginInsertRows({}, at, at);
    Row r{}; r.type = Divider; r.param = 1;
    rows_.insert(rows_.begin() + at, r);
    content_.insert(content_.begin() + at, QString());
    ids_.insert(ids_.begin() + at, newId);
    ranks_.insert(ranks_.begin() + at, newRank);
    fenwick_.insert(static_cast<size_t>(at), estimatedHeight(r));
    endInsertRows();
    if (doc_.isOpen())
        doc_.appendBlock(newId, newRank, 0, QStringLiteral("divider"), QString(), QString());
    bumpLayout();
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
}

QString BlockModel::languageForRow(int row) const {
    if (rows_.empty()) return {};
    return rows_[clampRow(row)].lang;
}

void BlockModel::makeCodeBlock(int row, const QString& lang) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return;
    Row& r = rows_[row];
    if (r.type == Code && r.lang == lang) return;
    beginTxn(row, row);
    r.type = Code; r.level = 0; r.lang = lang;
    r.spans.clear();             // inline markdown/spans are literal inside code
    persistContent(row);         // (spans went to attrs; content unchanged but persist meta)
    persistMeta(row);
    emit dataChanged(index(row), index(row), {TypeRole});
    bumpLayout();
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
}

void BlockModel::setCodeLanguage(int row, const QString& lang) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return;
    Row& r = rows_[row];
    if (r.type != Code || r.lang == lang) return;
    beginTxn(row, row);
    r.lang = lang;
    persistMeta(row);
    ++contentRevision_;          // CodeHighlighter.language binding depends on contentRevision
    emit contentChangedSpike();
    endTxn();
}

bool BlockModel::makeCodeBlockIfFence(int row) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return false;
    if (rows_[row].type != Paragraph) return false;
    if (!content_[row].startsWith(QLatin1String("```"))) return false;
    const QString lang = content_[row].mid(3).trimmed();   // "```python" → "python"
    beginTxn(row, row);
    rows_[row].type = Code; rows_[row].level = 0; rows_[row].lang = lang;
    rows_[row].spans.clear();
    content_[row].clear();                                  // consume the fence line
    persistContent(row);
    persistMeta(row);
    emit dataChanged(index(row), index(row), {TypeRole, ContentRole});
    bumpLayout();
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
    return true;
}

// === Tables ==============================================================
// The grid lives as compact JSON in `content`; mutations reserialize and persist
// through the existing txn chokepoint, so undo/redo/coalescing work unchanged.

const TableGrid& BlockModel::gridFor(int row) const {
    row = clampRow(row);
    if (tableCacheRow_ == row && tableCacheRev_ == contentRevision_) return tableCache_;
    tableCache_ = TableGrid::fromJson(content_[row]);
    tableCacheRow_ = row;
    tableCacheRev_ = contentRevision_;
    return tableCache_;
}

void BlockModel::mutateTable(int row, const std::function<void(TableGrid&)>& fn,
                             const QString& coalesce) {
    if (row < 0 || row >= static_cast<int>(rows_.size()) || rows_[row].type != Table) return;
    TableGrid g = TableGrid::fromJson(content_[row]);
    fn(g);
    beginTxn(row, row);
    content_[row] = g.toJson();
    rows_[row].param = static_cast<uint16_t>(std::clamp(g.rows(), 1, 65535));
    persistContent(row);
    tableCacheRow_ = -1;                                  // invalidate the parse cache
    emit dataChanged(index(row), index(row), {ContentRole});
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn(coalesce);
}

int BlockModel::tableRows(int row) const {
    if (rows_[clampRow(row)].type != Table) return 0;
    return gridFor(row).rows();
}
int BlockModel::tableColumns(int row) const {
    if (rows_[clampRow(row)].type != Table) return 0;
    return gridFor(row).cols();
}
int BlockModel::tableHeaderRows(int row) const {
    if (rows_[clampRow(row)].type != Table) return 0;
    return gridFor(row).headerRows();
}
QString BlockModel::tableCell(int row, int r, int c) const {
    if (rows_[clampRow(row)].type != Table) return {};
    return gridFor(row).cellText(r, c);
}
int BlockModel::tableColWidth(int row, int c) const {
    if (rows_[clampRow(row)].type != Table) return 0;
    return gridFor(row).colWidth(c);
}
int BlockModel::tableColAlign(int row, int c) const {
    if (rows_[clampRow(row)].type != Table) return 0;
    return gridFor(row).colAlign(c);
}

void BlockModel::tableSetCell(int row, int r, int c, const QString& text) {
    mutateTable(row, [&](TableGrid& g){ g.setCellText(r, c, text); },
                QStringLiteral("tcell:%1:%2").arg(r).arg(c));
}
void BlockModel::tableInsertRow(int row, int at)    { mutateTable(row, [&](TableGrid& g){ g.insertRow(at); }); }
void BlockModel::tableInsertColumn(int row, int at) { mutateTable(row, [&](TableGrid& g){ g.insertCol(at); }); }
void BlockModel::tableDeleteRow(int row, int at)    { mutateTable(row, [&](TableGrid& g){ g.deleteRow(at); }); }
void BlockModel::tableDeleteColumn(int row, int at) { mutateTable(row, [&](TableGrid& g){ g.deleteCol(at); }); }
void BlockModel::tableSetColWidth(int row, int c, int w) { mutateTable(row, [&](TableGrid& g){ g.setColWidth(c, w); }); }
void BlockModel::tableSetColAlign(int row, int c, int a) { mutateTable(row, [&](TableGrid& g){ g.setColAlign(c, a); }); }
void BlockModel::tableSetHeaderRows(int row, int n)      { mutateTable(row, [&](TableGrid& g){ g.setHeaderRows(n); }); }

void BlockModel::tablePasteTSV(int row, int r, int c, const QString& tsv) {
    const TableGrid src = TableGrid::fromTSV(tsv);
    mutateTable(row, [&](TableGrid& g){
        while (g.rows() < r + src.rows()) g.insertRow(g.rows());   // grow to fit the paste
        while (g.cols() < c + src.cols()) g.insertCol(g.cols());
        for (int i = 0; i < src.rows(); ++i)
            for (int j = 0; j < src.cols(); ++j)
                g.setCellText(r + i, c + j, src.cellText(i, j));
    });
}

QString BlockModel::tableRangeTSV(int row, int r0, int c0, int r1, int c1) const {
    if (rows_[clampRow(row)].type != Table) return {};
    const TableGrid& g = gridFor(row);
    const int R0 = std::min(r0, r1), R1 = std::max(r0, r1);
    const int C0 = std::min(c0, c1), C1 = std::max(c0, c1);
    QString out;
    for (int r = R0; r <= R1; ++r) {
        for (int c = C0; c <= C1; ++c) {
            if (c > C0) out += QLatin1Char('\t');
            QString t = g.cellText(r, c);
            t.replace(QLatin1Char('\t'), QLatin1Char(' ')).replace(QLatin1Char('\n'), QLatin1Char(' '));
            out += t;
        }
        if (r < R1) out += QLatin1Char('\n');
    }
    return out;
}

QString BlockModel::tableRangeHtml(int row, int r0, int c0, int r1, int c1) const {
    if (rows_[clampRow(row)].type != Table) return {};
    const TableGrid& g = gridFor(row);
    const int R0 = std::min(r0, r1), R1 = std::max(r0, r1);
    const int C0 = std::min(c0, c1), C1 = std::max(c0, c1);
    QString out = QStringLiteral("<table>");
    for (int r = R0; r <= R1; ++r) {
        out += QStringLiteral("<tr>");
        for (int c = C0; c <= C1; ++c)
            out += QStringLiteral("<td>") + g.cellText(r, c).toHtmlEscaped() + QStringLiteral("</td>");
        out += QStringLiteral("</tr>");
    }
    return out + QStringLiteral("</table>");
}

void BlockModel::tableClearRange(int row, int r0, int c0, int r1, int c1) {
    mutateTable(row, [&](TableGrid& g) {
        const int R0 = std::min(r0, r1), R1 = std::max(r0, r1);
        const int C0 = std::min(c0, c1), C1 = std::max(c0, c1);
        for (int r = R0; r <= R1; ++r)
            for (int c = C0; c <= C1; ++c) g.setCellText(r, c, QString());
    });
}

QStringList BlockModel::tableBlockIds() const {
    QStringList out;
    for (size_t i = 0; i < rows_.size(); ++i)
        if (rows_[i].type == Table) out.append(ids_[i]);
    return out;
}

int BlockModel::rowForId(const QString& id) const {
    for (size_t i = 0; i < ids_.size(); ++i)
        if (ids_[i] == id) return static_cast<int>(i);
    return -1;
}

QString BlockModel::idForRow(int row) const {
    return (row >= 0 && row < static_cast<int>(ids_.size())) ? ids_[row] : QString();
}

void BlockModel::insertTable(int afterRow, int nRows, int nCols) {
    const int at = std::clamp(afterRow + 1, 0, static_cast<int>(rows_.size()));
    nRows = std::max(1, nRows); nCols = std::max(1, nCols);
    const QString json = TableGrid::makeEmpty(nRows, nCols).toJson();
    beginTxn(at, at - 1);                        // empty `before`; after = [at,at]
    const QString newId = makeUlid();
    const QString newRank = rankBetween(
        (at > 0) ? ranks_[at - 1] : QString(),
        (at < static_cast<int>(ranks_.size())) ? ranks_[at] : QString());

    beginInsertRows({}, at, at);
    Row r{}; r.type = Table; r.param = static_cast<uint16_t>(nRows);
    rows_.insert(rows_.begin() + at, r);
    content_.insert(content_.begin() + at, json);
    ids_.insert(ids_.begin() + at, newId);
    ranks_.insert(ranks_.begin() + at, newRank);
    fenwick_.insert(static_cast<size_t>(at), estimatedHeight(r));
    endInsertRows();

    if (doc_.isOpen())
        doc_.appendBlock(newId, newRank, 0, QString::fromLatin1(typeToString(r.type)), QString(), json);

    bumpLayout();
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
}

// === Media ===============================================================
// The descriptor ({src,w,h}) lives as JSON in `content` (like tables), so undo +
// persistence reuse the chokepoint. Bytes are never stored here (see MediaStore).

void BlockModel::insertMedia(int afterRow, const QString& json, uint16_t aspectParam) {
    const int at = std::clamp(afterRow + 1, 0, static_cast<int>(rows_.size()));
    beginTxn(at, at - 1);
    const QString newId = makeUlid();
    const QString newRank = rankBetween(
        (at > 0) ? ranks_[at - 1] : QString(),
        (at < static_cast<int>(ranks_.size())) ? ranks_[at] : QString());

    beginInsertRows({}, at, at);
    Row r{}; r.type = Media; r.param = aspectParam;
    {
        const QJsonObject mo = QJsonDocument::fromJson(json.toUtf8()).object();
        r.mediaW = static_cast<uint16_t>(std::clamp(mo.value(QStringLiteral("w")).toInt(), 0, 65535));
        r.mediaH = static_cast<uint16_t>(std::clamp(mo.value(QStringLiteral("h")).toInt(), 0, 65535));
        r.isVideo = mo.value(QStringLiteral("kind")).toString() == QLatin1String("video");
    }
    rows_.insert(rows_.begin() + at, r);
    content_.insert(content_.begin() + at, json);
    ids_.insert(ids_.begin() + at, newId);
    ranks_.insert(ranks_.begin() + at, newRank);
    fenwick_.insert(static_cast<size_t>(at), estimatedHeight(r));
    endInsertRows();

    if (doc_.isOpen())
        doc_.appendBlock(newId, newRank, 0, QString::fromLatin1(typeToString(r.type)), QString(), json);

    bumpLayout();
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
}

static QString mediaJson(const MediaStore::ImageRef& ref) {
    QJsonObject o;
    o.insert(QStringLiteral("src"), ref.src);
    o.insert(QStringLiteral("w"), ref.w);
    o.insert(QStringLiteral("h"), ref.h);
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}
static QString videoMediaJson(const MediaStore::VideoRef& ref) {
    QJsonObject o;
    o.insert(QStringLiteral("src"), ref.src);
    o.insert(QStringLiteral("w"), ref.w);
    o.insert(QStringLiteral("h"), ref.h);
    o.insert(QStringLiteral("kind"), QStringLiteral("video"));
    o.insert(QStringLiteral("durMs"), ref.durationMs);
    o.insert(QStringLiteral("frames"), ref.frames);
    o.insert(QStringLiteral("fps"), ref.fps);
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}
static uint16_t aspectParam(int w, int h) {
    if (w <= 0 || h <= 0) return 56;     // 16:9 fallback (no jump until real dims)
    return static_cast<uint16_t>(std::clamp(int(100.0 * h / w + 0.5), 1, 1000));
}
static uint16_t aspectParam(const MediaStore::ImageRef& ref) {
    return aspectParam(ref.w, ref.h);
}

bool BlockModel::insertImageFromUrl(int afterRow, const QString& fileUrl) {
    if (!mediaStore_) return false;
    const MediaStore::ImageRef ref = mediaStore_->importFile(fileUrl);
    if (!ref.ok()) return false;
    insertMedia(afterRow, mediaJson(ref), aspectParam(ref));
    return true;
}

bool BlockModel::insertImageFromClipboard(int afterRow) {
    if (!mediaStore_) return false;
    const MediaStore::ImageRef ref = mediaStore_->importClipboardImage();
    if (!ref.ok()) return false;
    insertMedia(afterRow, mediaJson(ref), aspectParam(ref));
    return true;
}

bool BlockModel::insertVideoFromUrl(int afterRow, const QString& fileUrl) {
    if (!mediaStore_) return false;
    const MediaStore::VideoRef ref = mediaStore_->importVideoFile(fileUrl);
    if (!ref.ok()) return false;
    insertMedia(afterRow, videoMediaJson(ref), aspectParam(ref.w, ref.h));
    return true;
}

bool BlockModel::insertMediaFromUrl(int afterRow, const QString& fileUrl) {
    return MediaStore::isVideoPath(fileUrl) ? insertVideoFromUrl(afterRow, fileUrl)
                                            : insertImageFromUrl(afterRow, fileUrl);
}

QString BlockModel::mediaUrl(int row) const {
    row = clampRow(row);
    if (rows_[row].type != Media || !mediaStore_) return {};
    const QJsonObject o = QJsonDocument::fromJson(content_[row].toUtf8()).object();
    return mediaStore_->resolveUrl(o.value(QStringLiteral("src")).toString());
}
QString BlockModel::mediaLocalPath(int row) const {
    const QString url = mediaUrl(row);
    return url.isEmpty() ? QString() : QUrl(url).toLocalFile();
}
int BlockModel::mediaW(int row) const {
    row = clampRow(row);
    if (rows_[row].type != Media) return 0;
    return QJsonDocument::fromJson(content_[row].toUtf8()).object().value(QStringLiteral("w")).toInt();
}
int BlockModel::mediaH(int row) const {
    row = clampRow(row);
    if (rows_[row].type != Media) return 0;
    return QJsonDocument::fromJson(content_[row].toUtf8()).object().value(QStringLiteral("h")).toInt();
}
QString BlockModel::mediaKind(int row) const {
    row = clampRow(row);
    if (rows_[row].type != Media) return {};
    const QString k = QJsonDocument::fromJson(content_[row].toUtf8())
                          .object().value(QStringLiteral("kind")).toString();
    return k.isEmpty() ? QStringLiteral("image") : k;   // legacy {src,w,h} = image
}
double BlockModel::mediaFps(int row) const {
    row = clampRow(row);
    if (rows_[row].type != Media) return 0.0;
    return QJsonDocument::fromJson(content_[row].toUtf8()).object().value(QStringLiteral("fps")).toDouble();
}
int BlockModel::mediaFrames(int row) const {
    row = clampRow(row);
    if (rows_[row].type != Media) return 0;
    return QJsonDocument::fromJson(content_[row].toUtf8()).object().value(QStringLiteral("frames")).toInt();
}
qreal BlockModel::mediaDurationMs(int row) const {
    row = clampRow(row);
    if (rows_[row].type != Media) return 0;
    return QJsonDocument::fromJson(content_[row].toUtf8()).object().value(QStringLiteral("durMs")).toDouble();
}

void BlockModel::clearFormat(int row, int start, int end) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return;
    const int len = content_[row].size();
    start = std::clamp(start, 0, len); end = std::clamp(end, 0, len);
    if (start >= end || rows_[row].spans.empty()) return;
    beginTxn(row, row);
    removeSpan(rows_[row].spans, start, end, SpanBold);
    removeSpan(rows_[row].spans, start, end, SpanItalic);
    removeSpan(rows_[row].spans, start, end, SpanCode);
    persistMeta(row);
    emit dataChanged(index(row), index(row), {});
    ++contentRevision_; emit contentChangedSpike();
    endTxn();
}

int BlockModel::applyMarkdownTrigger(int row) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return 0;
    if (rows_[row].type != Paragraph) return 0;   // only transform plain paragraphs
    BlockType t = Paragraph; int level = 0, strip = 0;
    if (!matchMarkdownPrefix(content_[row], t, level, strip)) return 0;

    beginTxn(row, row);
    rows_[row].type = static_cast<uint8_t>(t);
    rows_[row].level = static_cast<uint8_t>(level);
    content_[row] = content_[row].mid(strip);
    persistContent(row);
    persistMeta(row);
    emit dataChanged(index(row), index(row), {TypeRole, ContentRole});
    bumpLayout();                 // type change → height changes; delegate re-measures
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
    return strip;
}

bool BlockModel::makeDividerIfMarker(int row) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return false;
    if (rows_[row].type != Paragraph) return false;
    const QString c = content_[row].trimmed();
    if (c != QLatin1String("---") && c != QLatin1String("***") && c != QLatin1String("___"))
        return false;
    beginTxn(row, row);
    rows_[row].type = Divider;
    rows_[row].level = 0;
    content_[row].clear();
    rows_[row].spans.clear();
    persistContent(row);
    persistMeta(row);
    emit dataChanged(index(row), index(row), {TypeRole, ContentRole});
    bumpLayout();
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
    return true;
}

QString BlockModel::genBase(int row, const Row& r) const {
    static const char* kWords[] = {
        "block", "virtual", "scroll", "height", "index", "fenwick", "delegate",
        "reflow", "viewport", "markdown", "render", "cache", "ring", "buffer",
        "cursor", "selection", "offset", "settle", "estimate", "measure" };
    constexpr int kN = int(sizeof(kWords) / sizeof(kWords[0]));

    switch (r.type) {
    case Heading:
        return QStringLiteral("Section %1 — %2 %3")
            .arg(row).arg(kWords[rowHash(row) % kN]).arg(kWords[rowHash(row + 7) % kN]);
    case Media:
        return QStringLiteral("media #%1  (aspect %2)").arg(row).arg(r.param / 100.0, 0, 'f', 2);
    case Code:
    case Paragraph:
    default: {
        QString s;
        const int lines = std::max<int>(1, r.param);
        for (int l = 0; l < lines; ++l) {
            const uint32_t seed = rowHash(row * 131 + l);
            const int wc = 6 + (seed % 9);
            for (int w = 0; w < wc; ++w)
                s += QString::fromLatin1(kWords[(seed + w * 2654435761u) % kN]) % u' ';
            if (l + 1 < lines) s += u'\n';
        }
        if (r.type == Code) s = QStringLiteral("fn block_%1() {\n").arg(row) % s % u"\n}";
        else s = QStringLiteral("[%1]  ").arg(row) % s;   // number paragraphs for legible caret tracking
        return s.trimmed();
    }
    }
}

QString BlockModel::textAt(int row) const {
    return (row >= 0 && row < static_cast<int>(content_.size())) ? content_[row] : QString();
}

QString BlockModel::contentForRow(int row) const {
    if (rows_.empty()) return {};
    return textAt(clampRow(row));
}

// --- Semantic format spans --------------------------------------------------
uint8_t BlockModel::spanKindFromString(const QString& s) {
    if (s == QLatin1String("bold"))      return SpanBold;
    if (s == QLatin1String("italic"))    return SpanItalic;
    if (s == QLatin1String("code"))      return SpanCode;
    if (s == QLatin1String("strike"))    return SpanStrike;
    if (s == QLatin1String("underline")) return SpanUnderline;
    return 0;
}
const char* BlockModel::spanKindToString(uint8_t k) {
    switch (k) {
    case SpanBold:      return "bold";
    case SpanItalic:    return "italic";
    case SpanCode:      return "code";
    case SpanStrike:    return "strike";
    case SpanUnderline: return "underline";
    default:            return "";
    }
}

// Does the union of same-kind spans fully cover [start,end)?
bool BlockModel::spansCover(const std::vector<Span>& v, int start, int end, uint8_t kind) {
    if (start >= end) return true;
    std::vector<Span> k;
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

// Add [start,end) of `kind`, then merge overlapping/adjacent same-kind spans.
void BlockModel::addSpan(std::vector<Span>& v, int start, int end, uint8_t kind) {
    if (start >= end) return;
    std::vector<Span> k, others;
    for (const Span& sp : v) (sp.kind == kind ? k : others).push_back(sp);
    k.push_back({start, end, kind});
    std::sort(k.begin(), k.end(), [](const Span& a, const Span& b){ return a.s < b.s; });
    std::vector<Span> merged;
    for (const Span& sp : k) {
        if (!merged.empty() && sp.s <= merged.back().e)
            merged.back().e = std::max(merged.back().e, sp.e);
        else
            merged.push_back(sp);
    }
    v = others;
    v.insert(v.end(), merged.begin(), merged.end());
}

// Subtract [start,end) from same-kind spans (splitting where it lands inside).
void BlockModel::removeSpan(std::vector<Span>& v, int start, int end, uint8_t kind) {
    if (start >= end) return;
    std::vector<Span> out;
    for (const Span& sp : v) {
        if (sp.kind != kind || sp.e <= start || sp.s >= end) { out.push_back(sp); continue; }
        if (sp.s < start) out.push_back({sp.s, start, kind});   // left remainder
        if (sp.e > end)   out.push_back({end, sp.e, kind});     // right remainder
    }
    v = out;
}

// Offset bookkeeping: text inserted at `at` (len chars), or [from,to) deleted.
void BlockModel::shiftSpansInsert(std::vector<Span>& v, int at, int len) {
    for (Span& sp : v) {
        if (at <= sp.s)      { sp.s += len; sp.e += len; }   // wholly after the caret
        else if (at < sp.e)  { sp.e += len; }                // typed inside → grow (not at exact end)
    }
}
void BlockModel::shiftSpansDelete(std::vector<Span>& v, int from, int to) {
    const int len = to - from;
    if (len <= 0) return;
    std::vector<Span> out;
    for (const Span& sp : v) {
        if (sp.e <= from) { out.push_back(sp); continue; }                       // before cut
        if (sp.s >= to)   { out.push_back({sp.s - len, sp.e - len, sp.kind}); continue; }  // after cut
        const int ns = std::min(sp.s, from);                 // surviving head + shifted tail collapse
        const int ne = (sp.e > to) ? sp.e - len : from;
        if (ne > ns) out.push_back({ns, ne, sp.kind});
    }
    v = out;
}

QVariantList BlockModel::spansForRow(int row) const {
    QVariantList out;
    if (rows_.empty()) return out;
    for (const Span& sp : rows_[clampRow(row)].spans) {
        QVariantMap m;
        m.insert(QStringLiteral("s"), sp.s);
        m.insert(QStringLiteral("e"), sp.e);
        m.insert(QStringLiteral("k"), sp.kind);
        out.append(m);
    }
    return out;
}

QVariantList BlockModel::codeRangesForRow(int row) const {
    QVariantList out;
    if (rows_.empty()) return out;
    row = clampRow(row);
    auto push = [&](int s, int e) {
        if (e <= s) return;
        QVariantMap m; m.insert(QStringLiteral("s"), s); m.insert(QStringLiteral("e"), e);
        out.append(m);
    };
    const QString& s = content_[row];
    // No inline-code chips inside a real code block, nor on a "```" fence line
    // (the backticks there are a code-block trigger, kept literal).
    if (rows_[row].type == Code || s.startsWith(QLatin1String("```"))) return out;
    for (int i = 0, n = s.size(); i < n; ) {                  // markdown `code` inner runs
        if (s[i] == QLatin1Char('`')) {
            const int j = s.indexOf(QLatin1Char('`'), i + 1);
            if (j > i) { push(i + 1, j); i = j + 1; continue; }
        }
        ++i;
    }
    for (const Span& sp : rows_[row].spans)                   // semantic code spans
        if (sp.kind == SpanCode) push(sp.s, sp.e);
    return out;
}

bool BlockModel::convertMarkdown(const QString& src, const std::vector<Span>& existing,
                                 QString& cleanText, std::vector<Span>& outSpans) {
    const int n = src.size();
    QString out; out.reserve(n);
    std::vector<int> map(n + 1, 0);          // old col → clean col
    std::vector<Span> found;
    auto keep = [&](int p) { map[p] = out.size(); out.append(src[p]); };
    auto drop = [&](int p) { map[p] = out.size(); };   // marker removed → maps to current clean pos

    int i = 0;
    while (i < n) {
        const QChar c = src[i];
        if (c == QLatin1Char('`')) {
            const int j = src.indexOf(QLatin1Char('`'), i + 1);
            if (j > i) {
                drop(i); const int s = out.size();
                for (int p = i + 1; p < j; ++p) keep(p);
                found.push_back({s, static_cast<int>(out.size()), SpanCode});
                drop(j); i = j + 1; continue;
            }
        } else if (c == QLatin1Char('*') && i + 1 < n && src[i + 1] == QLatin1Char('*')) {
            const int j = src.indexOf(QStringLiteral("**"), i + 2);
            if (j > i + 1) {
                drop(i); drop(i + 1); const int s = out.size();
                for (int p = i + 2; p < j; ++p) keep(p);
                found.push_back({s, static_cast<int>(out.size()), SpanBold});
                drop(j); drop(j + 1); i = j + 2; continue;
            }
        } else if (c == QLatin1Char('*')) {
            const int j = src.indexOf(QLatin1Char('*'), i + 1);
            if (j > i) {
                drop(i); const int s = out.size();
                for (int p = i + 1; p < j; ++p) keep(p);
                found.push_back({s, static_cast<int>(out.size()), SpanItalic});
                drop(j); i = j + 1; continue;
            }
        }
        keep(i); ++i;
    }
    map[n] = out.size();
    if (out == src) return false;            // no markers consumed

    std::vector<Span> merged;
    for (const Span& sp : existing)          // pre-existing spans → clean coords
        addSpan(merged, map[std::clamp(sp.s, 0, n)], map[std::clamp(sp.e, 0, n)], sp.kind);
    for (const Span& sp : found)
        addSpan(merged, sp.s, sp.e, sp.kind);
    cleanText = out;
    outSpans = merged;
    return true;
}

void BlockModel::commitMarkdown(int row) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return;
    const uint8_t t = rows_[row].type;
    if (t != Paragraph && t != Quote && t != ListItem) return;   // where inline md renders
    // A "```"/"```lang" fence is a code-block trigger (consumed on Enter); never
    // run inline conversion on it, which would eat the backticks into a stray span.
    if (content_[row].startsWith(QLatin1String("```"))) return;
    QString clean; std::vector<Span> spans;
    if (!convertMarkdown(content_[row], rows_[row].spans, clean, spans)) return;
    beginTxn(row, row);
    content_[row] = clean;
    rows_[row].spans = spans;
    persistContent(row);
    persistMeta(row);
    emit dataChanged(index(row), index(row), {ContentRole});
    bumpLayout();                            // markers gone → height may change
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();                                // distinct undo step (Cmd-Z restores the markdown)
}

void BlockModel::toggleFormat(int row, int start, int end, const QString& kind) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return;
    const uint8_t k = spanKindFromString(kind);
    if (!k) return;
    const int len = content_[row].size();
    start = std::clamp(start, 0, len);
    end   = std::clamp(end, 0, len);
    if (start >= end) return;
    beginTxn(row, row);
    std::vector<Span>& spans = rows_[row].spans;
    if (spansCover(spans, start, end, k)) removeSpan(spans, start, end, k);
    else                                  addSpan(spans, start, end, k);
    persistMeta(row);
    emit dataChanged(index(row), index(row), {});
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
}

QVariant BlockModel::data(const QModelIndex& index, int role) const {
    const int row = index.row();
    if (row < 0 || row >= static_cast<int>(rows_.size())) return {};
    const Row& r = rows_[row];
    switch (role) {
    case TypeRole:     return r.type;
    case ContentRole:  return textAt(row);
    case HeightRole:   return fenwick_.height(row);
    case MeasuredRole: return r.measured;
    default:           return {};
    }
}

QHash<int, QByteArray> BlockModel::roleNames() const {
    return {
        {TypeRole, "blockType"},
        {ContentRole, "blockContent"},
        {HeightRole, "blockHeight"},
        {MeasuredRole, "blockMeasured"},
    };
}

void BlockModel::bumpLayout() {
    ++layoutRevision_;
    emit layoutChangedSpike();
}

void BlockModel::setMeasuredHeight(int row, qreal h) {
    if (row < 0 || row >= static_cast<int>(rows_.size()) || h <= 0.0) return;
    const double delta = fenwick_.setHeight(static_cast<size_t>(row), h);
    if (!rows_[row].measured) rows_[row].measured = true;
    if (delta != 0.0) {
        emit heightSettled(row, delta);
        bumpLayout();
    }
}

qreal BlockModel::mediaDisplayHeight(int row) const {
    row = clampRow(row);
    return (rows_[row].type == Media) ? mediaFrameHeight(rows_[row]) : 0.0;
}

void BlockModel::setContentWidth(qreal w) {
    if (w <= 0.0 || std::abs(w - contentWidth_) < 0.5) return;
    contentWidth_ = w;
    // Media heights are derived from this width — re-derive them in the Fenwick.
    // (Media never measures back, so the estimate is the authoritative height.)
    bool any = false;
    for (size_t i = 0; i < rows_.size(); ++i)
        if (rows_[i].type == Media)
            if (fenwick_.setHeight(i, estimatedHeight(rows_[i])) != 0.0) any = true;
    if (any) bumpLayout();
}

void BlockModel::setContent(int row, const QString& text) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return;
    beginTxn(row, row);
    content_[row] = text;
    persistContent(row);                          // write-through to SQLite
    const QModelIndex idx = index(row);
    emit dataChanged(idx, idx, {ContentRole});
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
    // Height re-measure follows from the delegate's implicitHeight change.
}

void BlockModel::deleteRange(int aRow, int aCol, int fRow, int fCol) {
    int loRow = aRow, loCol = aCol, hiRow = fRow, hiCol = fCol;
    if (aRow > fRow || (aRow == fRow && aCol > fCol)) {
        loRow = fRow; loCol = fCol; hiRow = aRow; hiCol = aCol;
    }
    if (rows_.empty()) return;
    loRow = clampRow(loRow);
    hiRow = clampRow(hiRow);
    beginTxn(loRow, hiRow);

    // Merge surviving head of lo block with surviving tail of hi block.
    const QString loText = textAt(loRow);
    const QString hiText = textAt(hiRow);
    const int loClip = std::min<int>(loCol, loText.size());
    const int hiClip = std::min<int>(hiCol, hiText.size());
    const QString merged = loText.left(loClip) + hiText.mid(hiClip);
    content_[loRow] = merged;
    persistContent(loRow);
    emit dataChanged(index(loRow), index(loRow), {ContentRole});

    // Span bookkeeping (rows_[hiRow] still valid — the erase happens below).
    if (hiRow == loRow) {
        shiftSpansDelete(rows_[loRow].spans, loClip, hiClip);
    } else {
        std::vector<Span> kept;
        for (const Span& sp : rows_[loRow].spans)            // lo keeps [0, loClip)
            if (sp.s < loClip) kept.push_back({sp.s, std::min(sp.e, loClip), sp.kind});
        for (const Span& sp : rows_[hiRow].spans) {          // hi tail [hiClip,*) → after loClip
            const int s = std::max(sp.s, hiClip);
            if (sp.e > s) kept.push_back({s - hiClip + loClip, sp.e - hiClip + loClip, sp.kind});
        }
        rows_[loRow].spans = kept;
    }
    persistMeta(loRow);

    if (hiRow > loRow) {
        const int first = loRow + 1, last = hiRow, cnt = last - first + 1;
        for (int i = first; i <= last; ++i)
            if (doc_.isOpen()) doc_.deleteBlock(ids_[i]);   // ids still valid pre-erase
        beginRemoveRows({}, first, last);
        // Gather current (measured/estimated) heights of surviving rows.
        std::vector<double> hs;
        hs.reserve(rows_.size() - static_cast<size_t>(cnt));
        for (int i = 0; i < static_cast<int>(rows_.size()); ++i)
            if (i < first || i > last) hs.push_back(fenwick_.height(static_cast<size_t>(i)));
        rows_.erase(rows_.begin() + first, rows_.begin() + last + 1);
        content_.erase(content_.begin() + first, content_.begin() + last + 1);
        ids_.erase(ids_.begin() + first, ids_.begin() + last + 1);
        ranks_.erase(ranks_.begin() + first, ranks_.begin() + last + 1);
        fenwick_.reset(std::move(hs));
        endRemoveRows();
        bumpLayout();
    }
    ++contentRevision_;
    emit contentChangedSpike();
    // Single-char same-block deletes (backspace/delete key) coalesce.
    endTxn((hiRow == loRow && hiClip - loClip == 1) ? QStringLiteral("del") : QString());
}

void BlockModel::insertText(int row, int col, const QString& text, int marks) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return;
    beginTxn(row, row);
    const QString s = content_[row];
    col = std::clamp(col, 0, static_cast<int>(s.size()));
    content_[row] = s.left(col) + text + s.mid(col);
    persistContent(row);
    std::vector<Span>& spans = rows_[row].spans;
    if (!spans.empty())                              // keep existing span offsets aligned
        shiftSpansInsert(spans, col, text.size());
    if (marks) {                                     // armed typing attributes → span the new run
        const int e = col + text.size();
        if (marks & 1)  addSpan(spans, col, e, SpanBold);
        if (marks & 2)  addSpan(spans, col, e, SpanItalic);
        if (marks & 4)  addSpan(spans, col, e, SpanCode);
        if (marks & 8)  addSpan(spans, col, e, SpanStrike);
        if (marks & 16) addSpan(spans, col, e, SpanUnderline);
    }
    if (!spans.empty()) persistMeta(row);
    emit dataChanged(index(row), index(row), {ContentRole});
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn(text.size() == 1 ? QStringLiteral("type") : QString());   // coalesce typing
}

void BlockModel::splitBlock(int row, int col) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return;
    beginTxn(row, row);                          // after grows to [row, row+1]
    const QString s = content_[row];
    col = std::clamp(col, 0, static_cast<int>(s.size()));
    const QString left = s.left(col), right = s.mid(col);
    const int at = row + 1;
    const QString newId = makeUlid();
    const QString newRank = rankBetween(ranks_[row],
        (at < static_cast<int>(ranks_.size())) ? ranks_[at] : QString());

    // Split spans at `col`: left part stays, right part moves to the new block.
    std::vector<Span> leftS, rightS;
    for (const Span& sp : rows_[row].spans) {
        if (sp.s < col) leftS.push_back({sp.s, std::min(sp.e, col), sp.kind});
        if (sp.e > col) rightS.push_back({std::max(sp.s, col) - col, sp.e - col, sp.kind});
    }
    rows_[row].spans = leftS;

    content_[row] = left;
    persistContent(row);
    persistMeta(row);

    beginInsertRows({}, at, at);
    Row r{}; r.type = Paragraph; r.param = 1; r.spans = rightS;
    rows_.insert(rows_.begin() + at, r);
    content_.insert(content_.begin() + at, right);
    ids_.insert(ids_.begin() + at, newId);
    ranks_.insert(ranks_.begin() + at, newRank);
    fenwick_.insert(static_cast<size_t>(at), estimatedHeight(r));
    endInsertRows();

    if (doc_.isOpen())
        doc_.appendBlock(newId, newRank, 0, QString::fromLatin1(typeToString(r.type)), QString(), right);
    if (!rightS.empty()) persistMeta(at);    // write the moved spans into attrs

    bumpLayout();
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
}

void BlockModel::insertBlock(int row) {
    row = std::clamp(row, 0, static_cast<int>(rows_.size()));
    beginTxn(row, row - 1);                      // empty `before`; after = [row,row]
    const QString newId = makeUlid();
    const QString newRank = rankBetween(
        (row > 0) ? ranks_[row - 1] : QString(),
        (row < static_cast<int>(ranks_.size())) ? ranks_[row] : QString());

    beginInsertRows({}, row, row);
    Row r{}; r.type = Paragraph; r.param = 1;
    rows_.insert(rows_.begin() + row, r);
    content_.insert(content_.begin() + row, QString());
    ids_.insert(ids_.begin() + row, newId);
    ranks_.insert(ranks_.begin() + row, newRank);
    fenwick_.insert(static_cast<size_t>(row), estimatedHeight(r));
    endInsertRows();

    if (doc_.isOpen())
        doc_.appendBlock(newId, newRank, 0, QString::fromLatin1(typeToString(r.type)), QString(), QString());

    bumpLayout();
    ++contentRevision_;            // row→content mapping shifted: refresh content bindings
    emit contentChangedSpike();
    endTxn();
}

void BlockModel::duplicateBlock(int row) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return;
    const int at = row + 1;
    beginTxn(at, at - 1);                        // empty `before`; after = [at,at]
    const QString newId = makeUlid();
    const QString newRank = rankBetween(ranks_[row],
        (at < static_cast<int>(ranks_.size())) ? ranks_[at] : QString());

    beginInsertRows({}, at, at);
    Row r = rows_[row];                          // copies type/level/lang/spans/param
    r.measured = false;
    const QString text = content_[row];
    rows_.insert(rows_.begin() + at, r);
    content_.insert(content_.begin() + at, text);
    ids_.insert(ids_.begin() + at, newId);
    ranks_.insert(ranks_.begin() + at, newRank);
    fenwick_.insert(static_cast<size_t>(at), estimatedHeight(r));
    endInsertRows();

    if (doc_.isOpen())
        doc_.appendBlock(newId, newRank, 0, QString::fromLatin1(typeToString(r.type)),
                         attrsJson(r.type, r.level, r.lang, r.spans), text);

    bumpLayout();
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
}

void BlockModel::removeBlock(int row) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return;
    beginTxn(row, row);                          // after = empty
    if (doc_.isOpen()) doc_.deleteBlock(ids_[row]);
    beginRemoveRows({}, row, row);
    rows_.erase(rows_.begin() + row);
    content_.erase(content_.begin() + row);
    ids_.erase(ids_.begin() + row);
    ranks_.erase(ranks_.begin() + row);
    fenwick_.erase(static_cast<size_t>(row));
    endRemoveRows();
    bumpLayout();
    ++contentRevision_;            // row→content mapping shifted: refresh content bindings
    emit contentChangedSpike();
    endTxn();
}

void BlockModel::moveBlock(int from, int to) {
    const int n = static_cast<int>(rows_.size());
    if (from < 0 || from >= n || to < 0 || to >= n || from == to) return;
    beginTxn(std::min(from, to), std::max(from, to));

    // Lift the block out (its content/type/spans travel with it).
    Row r = rows_[from];
    const QString id = ids_[from], content = content_[from];
    const double h = fenwick_.height(static_cast<size_t>(from));
    rows_.erase(rows_.begin() + from);
    content_.erase(content_.begin() + from);
    ids_.erase(ids_.begin() + from);
    ranks_.erase(ranks_.begin() + from);
    fenwick_.erase(static_cast<size_t>(from));

    // New fractional rank between the destination neighbours (reduced list).
    const int sz = static_cast<int>(ranks_.size());
    const QString prev = (to > 0)  ? ranks_[to - 1] : QString();
    const QString next = (to < sz) ? ranks_[to]     : QString();
    const QString newRank = rankBetween(prev, next);

    rows_.insert(rows_.begin() + to, r);
    content_.insert(content_.begin() + to, content);
    ids_.insert(ids_.begin() + to, id);
    ranks_.insert(ranks_.begin() + to, newRank);
    fenwick_.insert(static_cast<size_t>(to), h);

    if (doc_.isOpen()) doc_.updateRank(id, newRank);

    bumpLayout();                 // positions change; cells re-read yForRow/content
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
}
