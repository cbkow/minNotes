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
#include <QFileInfo>
#include <QProcess>
#include <QDesktopServices>
#include <QTextDocument>
#include <QTextBlock>
#include <QTextList>
#include <QTextTable>
#include <QTextFrame>
#include <QTextCharFormat>
#include <QImage>
#include <QPixmap>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <functional>
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
constexpr double kFileChip = 56.0;   // fixed height of an unsupported-file attachment chip
constexpr double kPdfNav   = 40.0;   // page-nav strip reserved under an inline PDF page

// Span kinds whose `href` field carries a payload (URL for links, color hex for
// color/highlight) — serialized as "u", and pushed whole (never merged by kind).
static inline bool spanHasPayload(uint8_t k) {
    return k == BlockModel::SpanLink || k == BlockModel::SpanFgColor
        || k == BlockModel::SpanHighlight;
}
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
        fillMediaMeta(r, text);   // media: dims/video/aspect-param from the descriptor JSON
        for (const QJsonValue& sv : o.value(QStringLiteral("spans")).toArray()) {
            const QJsonObject so = sv.toObject();
            const uint8_t k = spanKindFromString(so.value(QStringLiteral("k")).toString());
            const int s = so.value(QStringLiteral("s")).toInt(), e = so.value(QStringLiteral("e")).toInt();
            if (k && e > s) r.spans.push_back({s, e, k, so.value(QStringLiteral("u")).toString()});
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

void BlockModel::fillMediaMeta(Row& r, const QString& content) const {
    if (r.type != Media) return;
    const QJsonObject mo = QJsonDocument::fromJson(content.toUtf8()).object();
    const int mw = mo.value(QStringLiteral("w")).toInt();
    const int mh = mo.value(QStringLiteral("h")).toInt();
    r.mediaW = static_cast<uint16_t>(std::clamp(mw, 0, 65535));
    r.mediaH = static_cast<uint16_t>(std::clamp(mh, 0, 65535));
    if (mw > 0 && mh > 0)
        r.param = static_cast<uint16_t>(std::clamp(int(100.0 * mh / mw + 0.5), 1, 1000));
    const QString kind = mo.value(QStringLiteral("kind")).toString();
    r.isVideo = kind == QLatin1String("video");
    r.isFile  = kind == QLatin1String("file");
    r.isPdf   = kind == QLatin1String("pdf");
    r.dispW   = static_cast<uint16_t>(std::clamp(mo.value(QStringLiteral("dw")).toInt(), 0, 65535));
}

// Effective displayed width of a media block: the per-block override (clamped to
// the page width) if set, else the default — PDFs fit the page, raster media is
// intrinsic-capped (never upscaled by default).
double BlockModel::mediaDisplayWidth(const Row& r) const {
    if (r.dispW > 0) return std::min<double>(r.dispW, contentWidth_);
    if (r.isPdf)     return contentWidth_;
    if (r.mediaW > 0) return std::min<double>(contentWidth_, r.mediaW);
    return contentWidth_;
}

// Displayed media frame height: dispW = min(contentWidth, w) (never upscaled),
// height = round(dispW * h/w). Pure function of the probed dims + the layout
// width — recomputed when setContentWidth changes (resize). Falls back to the
// aspect param if intrinsic dims are missing.
double BlockModel::mediaFrameHeight(const Row& r) const {
    if (r.isFile) return kFileChip;            // fixed-height attachment chip
    const double w = mediaDisplayWidth(r);
    if (r.mediaW > 0 && r.mediaH > 0)
        return std::floor(w * r.mediaH / r.mediaW + 0.5);
    return contentWidth_ * (r.param / 100.0);
}

double BlockModel::estimatedHeight(const Row& r) const {
    switch (r.type) {
    case Heading: return kHeading + kPadV;
    case Media:   // 12px vertical pad + the transport toolbar for video. Matches
                  // the Editor cell delegate exactly, and media never measures
                  // back, so this IS the authoritative height (no scroll-in jump).
        return 12.0 + mediaFrameHeight(r)
             + (r.isVideo ? kVideoBar : r.isPdf ? kPdfNav : 0.0);
    case Divider: return 24.0;
    case Table:   return r.param * 34.0 + 58.0;     // param = row count; + 6 top/20 bottom pad (+row button) + header/strip
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
            if (spanHasPayload(sp.kind) && !sp.href.isEmpty()) so.insert(QStringLiteral("u"), sp.href);
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
        fillMediaMeta(r, s.content);   // media: dims/video/param from descriptor (else height ~= 0)
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
    rows_[row].measured = false;                          // height may change (rows/cols/
                                                          // wrap/col-width) → re-measure;
                                                          // the cache (rowMeasured) is stale
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
void BlockModel::tableSetCellColor(int row, int r0, int c0, int r1, int c1, bool fg, const QString& color) {
    if (r0 > r1) std::swap(r0, r1);
    if (c0 > c1) std::swap(c0, c1);
    mutateTable(row, [&](TableGrid& g){
        for (int r = r0; r <= r1; ++r)
            for (int c = c0; c <= c1; ++c)
                fg ? g.setCellFg(r, c, color) : g.setCellBg(r, c, color);
    });
}
void BlockModel::tableSetRowColor(int row, int r, bool fg, const QString& color) {
    mutateTable(row, [&](TableGrid& g){ fg ? g.setRowFg(r, color) : g.setRowBg(r, color); });
}
void BlockModel::tableSetColColor(int row, int c, bool fg, const QString& color) {
    mutateTable(row, [&](TableGrid& g){ fg ? g.setColFg(c, color) : g.setColBg(c, color); });
}
// Effective colour for rendering: cell wins, then row, then column, then none.
QString BlockModel::tableCellBg(int row, int r, int c) const {
    if (rows_[clampRow(row)].type != Table) return {};
    const TableGrid& g = gridFor(row);
    QString v = g.cellBg(r, c);
    if (v.isEmpty()) v = g.rowBg(r);
    if (v.isEmpty()) v = g.colBg(c);
    return v;
}
QString BlockModel::tableCellFg(int row, int r, int c) const {
    if (rows_[clampRow(row)].type != Table) return {};
    const TableGrid& g = gridFor(row);
    QString v = g.cellFg(r, c);
    if (v.isEmpty()) v = g.rowFg(r);
    if (v.isEmpty()) v = g.colFg(c);
    return v;
}
// --- cell inline spans (rich text inside table cells) ----------------------
// Cells persist spans as a JSON array of {s,e,k,u?}; these converters bridge to
// the Span vector so the block-level span helpers (addSpan/removeSpan/shift…)
// drive cell editing too.
std::vector<BlockModel::Span> BlockModel::cellSpansFromJson(const QJsonArray& a) {
    std::vector<Span> v; v.reserve(a.size());
    for (const QJsonValue& it : a) {
        const QJsonObject o = it.toObject();
        v.push_back({o.value(QStringLiteral("s")).toInt(), o.value(QStringLiteral("e")).toInt(),
                     static_cast<uint8_t>(o.value(QStringLiteral("k")).toInt()),
                     o.value(QStringLiteral("u")).toString()});
    }
    return v;
}
QJsonArray BlockModel::cellSpansToJson(const std::vector<Span>& v) {
    QJsonArray a;
    for (const Span& sp : v) {
        QJsonObject o;
        o.insert(QStringLiteral("s"), sp.s);
        o.insert(QStringLiteral("e"), sp.e);
        o.insert(QStringLiteral("k"), int(sp.kind));
        if (spanHasPayload(sp.kind) && !sp.href.isEmpty()) o.insert(QStringLiteral("u"), sp.href);
        a.append(o);
    }
    return a;
}

QVariantList BlockModel::tableCellSpans(int row, int r, int c) const {
    QVariantList out;
    if (rows_[clampRow(row)].type != Table) return out;
    const QJsonArray a = gridFor(row).cellSpans(r, c);
    for (const QJsonValue& it : a) {
        const QJsonObject o = it.toObject();
        QVariantMap m;
        m.insert(QStringLiteral("s"), o.value(QStringLiteral("s")).toInt());
        m.insert(QStringLiteral("e"), o.value(QStringLiteral("e")).toInt());
        m.insert(QStringLiteral("k"), o.value(QStringLiteral("k")).toInt());
        if (o.contains(QStringLiteral("u"))) m.insert(QStringLiteral("u"), o.value(QStringLiteral("u")).toString());
        out.append(m);
    }
    return out;
}

bool BlockModel::tableCellHasFormat(int row, int r, int c, int start, int end, const QString& kind) const {
    if (rows_[clampRow(row)].type != Table) return false;
    const uint8_t k = spanKindFromString(kind);
    if (!k) return false;
    const TableGrid& g = gridFor(row);
    const int len = g.cellText(r, c).size();
    start = std::clamp(start, 0, len); end = std::clamp(end, 0, len);
    if (start >= end) return false;
    return spansCover(cellSpansFromJson(g.cellSpans(r, c)), start, end, k);
}

void BlockModel::tableSetCellFormat(int row, int r, int c, int start, int end, const QString& kind, bool on) {
    const uint8_t k = spanKindFromString(kind);
    if (!k) return;
    mutateTable(row, [&](TableGrid& g){
        const int len = g.cellText(r, c).size();
        const int s = std::clamp(start, 0, len), e = std::clamp(end, 0, len);
        if (s >= e) return;
        std::vector<Span> v = cellSpansFromJson(g.cellSpans(r, c));
        if (on) addSpan(v, s, e, k); else removeSpan(v, s, e, k);
        g.setCellSpans(r, c, cellSpansToJson(v));
    });
}

void BlockModel::tableClearCellFormat(int row, int r, int c, int start, int end) {
    mutateTable(row, [&](TableGrid& g){
        const int len = g.cellText(r, c).size();
        const int s = std::clamp(start, 0, len), e = std::clamp(end, 0, len);
        if (s >= e) return;
        std::vector<Span> kept;                       // drop any span overlapping [s,e)
        for (const Span& sp : cellSpansFromJson(g.cellSpans(r, c))) {
            if (sp.e <= s || sp.s >= e) { kept.push_back(sp); continue; }
            if (sp.s < s) kept.push_back({sp.s, s, sp.kind, sp.href});
            if (sp.e > e) kept.push_back({e, sp.e, sp.kind, sp.href});
        }
        g.setCellSpans(r, c, cellSpansToJson(kept));
    });
}

// Span-aware cell text edits: insert/delete text AND shift the cell's spans, so
// formatting stays glued to its characters as the user types (mirrors insertText/
// deleteRange for blocks). Coalesced per cell like tableSetCell.
void BlockModel::tableCellInsert(int row, int r, int c, int at, const QString& text) {
    if (text.isEmpty()) return;
    mutateTable(row, [&](TableGrid& g){
        QString t = g.cellText(r, c);
        const int p = std::clamp(at, 0, int(t.size()));
        g.setCellText(r, c, t.left(p) + text + t.mid(p));
        std::vector<Span> v = cellSpansFromJson(g.cellSpans(r, c));
        shiftSpansInsert(v, p, text.size());
        g.setCellSpans(r, c, cellSpansToJson(v));
    }, QStringLiteral("tcell:%1:%2").arg(r).arg(c));
}

void BlockModel::tableCellDelete(int row, int r, int c, int from, int to) {
    mutateTable(row, [&](TableGrid& g){
        QString t = g.cellText(r, c);
        const int f = std::clamp(from, 0, int(t.size())), e = std::clamp(to, 0, int(t.size()));
        if (f >= e) return;
        g.setCellText(r, c, t.left(f) + t.mid(e));
        std::vector<Span> v = cellSpansFromJson(g.cellSpans(r, c));
        shiftSpansDelete(v, f, e);
        g.setCellSpans(r, c, cellSpansToJson(v));
    }, QStringLiteral("tcell:%1:%2").arg(r).arg(c));
}

// --- images inside cells ---------------------------------------------------
static QString mediaJson(const MediaStore::ImageRef& ref);   // defined below with the media-block helpers
// Import via MediaStore (sidecar for clipboard bytes, in-place ref for files),
// stash the {src,w,h} descriptor in cell.media, and widen a too-narrow column
// to a sensible default so the image isn't squeezed. One undo step.
bool BlockModel::tableSetCellImageFromClipboard(int row, int r, int c) {
    if (!mediaStore_ || rows_[clampRow(row)].type != Table) return false;
    const MediaStore::ImageRef ref = mediaStore_->importClipboardImage();
    if (!ref.ok()) return false;
    const QString json = mediaJson(ref);
    const int target = std::clamp(ref.w, 140, 360);
    mutateTable(row, [&](TableGrid& g){
        g.setCellMedia(r, c, json);
        if (g.colWidth(c) < target) g.setColWidth(c, target);
    });
    return true;
}
bool BlockModel::tableSetCellImageFromUrl(int row, int r, int c, const QString& fileUrl) {
    if (!mediaStore_ || rows_[clampRow(row)].type != Table) return false;
    const MediaStore::ImageRef ref = mediaStore_->importFile(fileUrl);
    if (!ref.ok()) return false;
    const QString json = mediaJson(ref);
    const int target = std::clamp(ref.w, 140, 360);
    mutateTable(row, [&](TableGrid& g){
        g.setCellMedia(r, c, json);
        if (g.colWidth(c) < target) g.setColWidth(c, target);
    });
    return true;
}
void BlockModel::tableClearCellMedia(int row, int r, int c) {
    mutateTable(row, [&](TableGrid& g){ g.setCellMedia(r, c, QString()); });
}
QString BlockModel::tableCellMedia(int row, int r, int c) const {
    if (rows_[clampRow(row)].type != Table) return {};
    return gridFor(row).cellMedia(r, c);
}
QString BlockModel::tableCellMediaUrl(int row, int r, int c) const {
    if (!mediaStore_ || rows_[clampRow(row)].type != Table) return {};
    const QString m = gridFor(row).cellMedia(r, c);
    if (m.isEmpty()) return {};
    const QJsonObject o = QJsonDocument::fromJson(m.toUtf8()).object();
    return mediaStore_->resolveUrl(o.value(QStringLiteral("src")).toString());
}
int BlockModel::tableCellMediaW(int row, int r, int c) const {
    if (rows_[clampRow(row)].type != Table) return 0;
    const QString m = gridFor(row).cellMedia(r, c);
    if (m.isEmpty()) return 0;
    return QJsonDocument::fromJson(m.toUtf8()).object().value(QStringLiteral("w")).toInt();
}
int BlockModel::tableCellMediaH(int row, int r, int c) const {
    if (rows_[clampRow(row)].type != Table) return 0;
    const QString m = gridFor(row).cellMedia(r, c);
    if (m.isEmpty()) return 0;
    return QJsonDocument::fromJson(m.toUtf8()).object().value(QStringLiteral("h")).toInt();
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

QStringList BlockModel::pdfBlockIds() const {
    QStringList out;
    for (size_t i = 0; i < rows_.size(); ++i)
        if (rows_[i].type == Media && rows_[i].isPdf) out.append(ids_[i]);
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

int BlockModel::insertTableFromTSV(int afterRow, const QString& tsv) {
    const TableGrid g = TableGrid::fromTSV(tsv);
    if (g.rows() < 1 || g.cols() < 1) return -1;
    const QString json = g.toJson();
    const int at = std::clamp(afterRow + 1, 0, static_cast<int>(rows_.size()));
    beginTxn(at, at - 1);                         // empty `before`; after = [at,at]
    const QString newId = makeUlid();
    const QString newRank = rankBetween(
        (at > 0) ? ranks_[at - 1] : QString(),
        (at < static_cast<int>(ranks_.size())) ? ranks_[at] : QString());

    beginInsertRows({}, at, at);
    Row r{}; r.type = Table; r.param = static_cast<uint16_t>(g.rows());
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
    return at;
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
    fillMediaMeta(r, json);   // dims/video/aspect-param from the descriptor (single source)
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
static QString remoteImageJson(const QString& url) {
    QJsonObject o;
    o.insert(QStringLiteral("src"), url);    // http(s) — loads remotely + gets localized
    o.insert(QStringLiteral("w"), 480);      // placeholder dims until the download lands
    o.insert(QStringLiteral("h"), 300);
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}
static QString pdfMediaJson(const MediaStore::PdfRef& ref) {
    QJsonObject o;
    o.insert(QStringLiteral("src"),   ref.src);
    o.insert(QStringLiteral("w"),     ref.w);
    o.insert(QStringLiteral("h"),     ref.h);
    o.insert(QStringLiteral("kind"),  QStringLiteral("pdf"));
    o.insert(QStringLiteral("pages"), ref.pages);
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}
static QString fileMediaJson(const QString& path) {
    const QFileInfo fi(path);
    QJsonObject o;
    o.insert(QStringLiteral("src"),  fi.absoluteFilePath());
    o.insert(QStringLiteral("kind"), QStringLiteral("file"));
    o.insert(QStringLiteral("name"), fi.fileName());
    o.insert(QStringLiteral("ext"),  fi.suffix().toLower());
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

bool BlockModel::insertPdfFromUrl(int afterRow, const QString& fileUrl) {
    if (!mediaStore_) return false;
    const MediaStore::PdfRef ref = mediaStore_->importPdfFile(fileUrl);
    if (!ref.ok()) return false;
    insertMedia(afterRow, pdfMediaJson(ref), aspectParam(ref.w, ref.h));
    return true;
}

bool BlockModel::insertFileFromUrl(int afterRow, const QString& fileUrl) {
    const QString path = fileUrl.startsWith(QLatin1String("file:"))
                       ? QUrl(fileUrl).toLocalFile() : fileUrl;
    if (path.isEmpty()) return false;
    insertMedia(afterRow, fileMediaJson(path), static_cast<uint16_t>(kFileChip));
    return true;
}

bool BlockModel::insertMediaFromUrl(int afterRow, const QString& fileUrl) {
    // Video / PDF (by extension + a successful probe), else a loadable image,
    // else a generic file attachment chip — so any dropped/pasted file lands.
    if (MediaStore::isVideoPath(fileUrl) && insertVideoFromUrl(afterRow, fileUrl)) return true;
    if (MediaStore::isPdfPath(fileUrl)   && insertPdfFromUrl(afterRow, fileUrl))   return true;
    if (insertImageFromUrl(afterRow, fileUrl)) return true;
    return insertFileFromUrl(afterRow, fileUrl);
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
void BlockModel::revealMedia(int row) const {
    const QString path = mediaLocalPath(row);
    if (path.isEmpty() || !QFileInfo::exists(path)) return;
#if defined(Q_OS_MACOS)
    QProcess::startDetached(QStringLiteral("open"), { QStringLiteral("-R"), path });
#elif defined(Q_OS_WIN)
    QProcess::startDetached(QStringLiteral("explorer"),
                            { QStringLiteral("/select,") + QDir::toNativeSeparators(path) });
#else
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
#endif
}
QString BlockModel::mediaFileName(int row) const {
    row = clampRow(row);
    if (rows_[row].type != Media) return {};
    const QString name = QJsonDocument::fromJson(content_[row].toUtf8())
                             .object().value(QStringLiteral("name")).toString();
    return name.isEmpty() ? QFileInfo(mediaLocalPath(row)).fileName() : name;
}
int BlockModel::mediaPdfPages(int row) const {
    row = clampRow(row);
    if (rows_[row].type != Media) return 0;
    return QJsonDocument::fromJson(content_[row].toUtf8())
               .object().value(QStringLiteral("pages")).toInt();
}
// Open the media's file in the sibling ufb browser via its deep-link scheme:
// ufb:///{os}/{percent-encoded path} (slashes kept literal, matching ufb's
// build_path_uri). No-op if ufb isn't installed (no handler registered).
void BlockModel::openMediaInUfb(int row) const {
    const QString path = mediaLocalPath(row);
    if (path.isEmpty()) return;
#if defined(Q_OS_WIN)
    const QString os = QStringLiteral("win");
#else
    const QString os = QStringLiteral("mac");
#endif
    const QString enc = QString::fromLatin1(QUrl::toPercentEncoding(path, "/"));
    QDesktopServices::openUrl(QUrl(QStringLiteral("ufb:///") + os + QLatin1Char('/') + enc));
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
    removeSpan(rows_[row].spans, start, end, SpanStrike);
    removeSpan(rows_[row].spans, start, end, SpanUnderline);
    removeSpan(rows_[row].spans, start, end, SpanLink);
    removeSpan(rows_[row].spans, start, end, SpanFgColor);
    removeSpan(rows_[row].spans, start, end, SpanHighlight);
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
    if (s == QLatin1String("link"))      return SpanLink;
    if (s == QLatin1String("color"))     return SpanFgColor;
    if (s == QLatin1String("highlight")) return SpanHighlight;
    return 0;
}
const char* BlockModel::spanKindToString(uint8_t k) {
    switch (k) {
    case SpanBold:      return "bold";
    case SpanItalic:    return "italic";
    case SpanCode:      return "code";
    case SpanStrike:    return "strike";
    case SpanUnderline: return "underline";
    case SpanLink:      return "link";
    case SpanFgColor:   return "color";
    case SpanHighlight: return "highlight";
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
        if (sp.s < start) out.push_back({sp.s, start, kind, sp.href});   // left remainder
        if (sp.e > end)   out.push_back({end, sp.e, kind, sp.href});     // right remainder
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
        if (sp.s >= to)   { out.push_back({sp.s - len, sp.e - len, sp.kind, sp.href}); continue; }  // after cut
        const int ns = std::min(sp.s, from);                 // surviving head + shifted tail collapse
        const int ne = (sp.e > to) ? sp.e - len : from;
        if (ne > ns) out.push_back({ns, ne, sp.kind, sp.href});
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
        if (spanHasPayload(sp.kind)) m.insert(QStringLiteral("u"), sp.href);
        out.append(m);
    }
    return out;
}

// A payload span (link/color/highlight) over [start,end): drop any same-kind span
// overlapping the range (they don't merge — each carries its own URL/color), then
// add the new one if a payload was given (empty = remove that kind here).
void BlockModel::setPayloadSpan(int row, int start, int end, uint8_t kind, const QString& payload) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return;
    const int len = content_[row].size();
    start = std::clamp(start, 0, len); end = std::clamp(end, 0, len);
    if (start >= end) return;
    beginTxn(row, row);
    std::vector<Span>& v = rows_[row].spans;
    std::vector<Span> kept;
    for (const Span& sp : v)
        if (!(sp.kind == kind && sp.s < end && sp.e > start)) kept.push_back(sp);
    if (!payload.isEmpty()) kept.push_back({start, end, kind, payload});
    v = std::move(kept);
    persistMeta(row);
    emit dataChanged(index(row), index(row), {ContentRole});
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
}
void BlockModel::setLink(int row, int start, int end, const QString& url) {
    setPayloadSpan(row, start, end, SpanLink, url);
}
void BlockModel::setTextColor(int row, int start, int end, const QString& color) {
    setPayloadSpan(row, start, end, SpanFgColor, color);
}
void BlockModel::setHighlight(int row, int start, int end, const QString& color) {
    setPayloadSpan(row, start, end, SpanHighlight, color);
}

QString BlockModel::linkAt(int row, int col) const {
    if (rows_.empty()) return {};
    for (const Span& sp : rows_[clampRow(row)].spans)
        if (sp.kind == SpanLink && col >= sp.s && col < sp.e) return sp.href;
    return {};
}

QVariantList BlockModel::linkRangeAt(int row, int col) const {
    if (rows_.empty()) return {};
    for (const Span& sp : rows_[clampRow(row)].spans)
        if (sp.kind == SpanLink && col >= sp.s && col < sp.e) return QVariantList{ sp.s, sp.e };
    return {};
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
        } else if (c == QLatin1Char('~') && i + 1 < n && src[i + 1] == QLatin1Char('~')) {
            const int j = src.indexOf(QStringLiteral("~~"), i + 2);
            if (j > i + 1) {
                drop(i); drop(i + 1); const int s = out.size();
                for (int p = i + 2; p < j; ++p) keep(p);
                found.push_back({s, static_cast<int>(out.size()), SpanStrike});
                drop(j); drop(j + 1); i = j + 2; continue;
            }
        } else if (c == QLatin1Char('[')) {              // [label](url) → link span
            const int j = src.indexOf(QLatin1Char(']'), i + 1);
            if (j > i && j + 1 < n && src[j + 1] == QLatin1Char('(')) {
                const int k = src.indexOf(QLatin1Char(')'), j + 2);
                if (k > j + 1) {
                    drop(i); const int s = out.size();
                    for (int p = i + 1; p < j; ++p) keep(p);          // the label
                    const QString url = src.mid(j + 2, k - (j + 2));
                    found.push_back({s, static_cast<int>(out.size()), SpanLink, url});
                    drop(j);                                          // ]
                    for (int p = j + 1; p <= k; ++p) drop(p);         // (url)
                    i = k + 1; continue;
                }
            }
        }
        keep(i); ++i;
    }
    map[n] = out.size();
    if (out == src) return false;            // no markers consumed

    std::vector<Span> merged;
    for (const Span& sp : existing) {        // pre-existing spans → clean coords
        const int s = map[std::clamp(sp.s, 0, n)], e = map[std::clamp(sp.e, 0, n)];
        if (spanHasPayload(sp.kind)) { if (e > s) merged.push_back({s, e, sp.kind, sp.href}); }
        else addSpan(merged, s, e, sp.kind);
    }
    for (const Span& sp : found) {
        if (spanHasPayload(sp.kind)) merged.push_back(sp);   // keep payload; don't merge
        else addSpan(merged, sp.s, sp.e, sp.kind);
    }
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
int BlockModel::mediaDispWidth(int row) const {
    row = clampRow(row);
    return (rows_[row].type == Media) ? int(mediaDisplayWidth(rows_[row]) + 0.5) : 0;
}
void BlockModel::setMediaWidth(int row, int w) {
    if (row < 0 || row >= static_cast<int>(rows_.size()) || rows_[row].type != Media) return;
    QJsonObject o = QJsonDocument::fromJson(content_[row].toUtf8()).object();
    if (w <= 0) o.remove(QStringLiteral("dw"));                 // reset to intrinsic/default
    else        o.insert(QStringLiteral("dw"), std::clamp(w, 80, int(contentWidth_)));
    const QString json = QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
    beginTxn(row, row);
    content_[row] = json;
    fillMediaMeta(rows_[row], json);
    rows_[row].measured = false;
    fenwick_.setHeight(static_cast<size_t>(row), estimatedHeight(rows_[row]));
    persistContent(row);
    emit dataChanged(index(row), index(row), {ContentRole});
    bumpLayout();
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
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
            if (sp.s < loClip) kept.push_back({sp.s, std::min(sp.e, loClip), sp.kind, sp.href});
        for (const Span& sp : rows_[hiRow].spans) {          // hi tail [hiClip,*) → after loClip
            const int s = std::max(sp.s, hiClip);
            if (sp.e > s) kept.push_back({s - hiClip + loClip, sp.e - hiClip + loClip, sp.kind, sp.href});
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

QVariantList BlockModel::pasteText(int row, int col, const QString& text) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return {};

    // Normalize newlines, split into non-blank lines (blank lines are paragraph
    // separators — collapsed). Each remaining line becomes one block.
    QString t = text;
    t.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    t.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    QStringList segs;
    const QStringList lines = t.split(QLatin1Char('\n'));
    for (const QString& ln : lines)
        if (!ln.trimmed().isEmpty()) segs << ln;
    if (segs.isEmpty()) return {};

    // Parse one line → (type, level, clean text, spans): block-prefix then inline
    // markdown. A "```" line renders literally (fenced code not yet reconstructed).
    auto parseSeg = [&](const QString& seg, uint8_t& type, uint8_t& level,
                        QString& outText, std::vector<Span>& outSpans, bool& hadPrefix) {
        type = Paragraph; level = 0; outText = seg; outSpans.clear(); hadPrefix = false;
        const bool fence = seg.startsWith(QLatin1String("```"));
        QString body = seg;
        if (!fence) {
            BlockType bt; int lvl = 0, strip = 0;
            if (matchMarkdownPrefix(seg, bt, lvl, strip)) {
                hadPrefix = true;
                type = static_cast<uint8_t>(bt);
                level = (bt == Heading) ? static_cast<uint8_t>(lvl) : 0;
                if (bt == Divider) { outText.clear(); return; }
                body = seg.mid(strip);
            }
            QString clean; std::vector<Span> sp;
            if (convertMarkdown(body, {}, clean, sp)) { outText = clean; outSpans = sp; return; }
        }
        outText = body;
    };

    const int n = segs.size();
    // Opaque targets (media/table/divider) hold non-prose content — never write
    // text into them; leave them intact and splice everything AFTER instead.
    const bool opaque = (rows_[row].type == Media || rows_[row].type == Table
                         || rows_[row].type == Divider);

    QString left, right;
    std::vector<Span> leftS, rightS;
    if (!opaque) {
        const QString s = content_[row];
        col = std::clamp(col, 0, static_cast<int>(s.size()));
        left = s.left(col); right = s.mid(col);
        // Split the current block's spans at the caret: left stays, right (rebased
        // to 0) moves to the tail of the LAST pasted block.
        for (const Span& sp : rows_[row].spans) {
            if (sp.s < col) leftS.push_back({sp.s, std::min(sp.e, col), sp.kind, sp.href});
            if (sp.e > col) rightS.push_back({std::max(sp.s, col) - col, sp.e - col, sp.kind, sp.href});
        }
    }

    beginTxn(row, row);
    int caretRow = row, caretCol = 0;

    // Append a span, preserving the link target: addSpan() only carries a kind (it
    // merges same-kind runs), which would drop a link's href — so push links whole.
    auto appendSpan = [](std::vector<Span>& v, int s, int e, uint8_t kind, const QString& href) {
        if (s >= e) return;
        if (spanHasPayload(kind)) v.push_back({s, e, kind, href});
        else addSpan(v, s, e, kind);
    };

    // Splice segs[startSeg..] as fresh blocks after `afterRow`; the LAST one gets
    // the trailing tail (right/rightS) and the caret lands at its boundary.
    auto appendBlocksAfter = [&](int afterRow, int startSeg) {
        const int cnt = n - startSeg;
        if (cnt <= 0) return;
        const int first = afterRow + 1, last = afterRow + cnt;
        QString prevRank = ranks_[afterRow];
        const QString nextRank = (first < static_cast<int>(ranks_.size())) ? ranks_[first] : QString();
        struct NewBlk { QString id, rank, content; uint8_t type, level; std::vector<Span> spans; };
        std::vector<NewBlk> made;
        beginInsertRows({}, first, last);
        for (int k = 0; k < cnt; ++k) {
            const int at = first + k;
            uint8_t tj, lj; QString textj; std::vector<Span> spj; bool pfxj;
            parseSeg(segs[startSeg + k], tj, lj, textj, spj, pfxj);
            QString contentj = textj;
            std::vector<Span> spans = spj;
            if (k == cnt - 1) {                        // last block carries the tail
                caretRow = at; caretCol = textj.size();
                for (const Span& sp : rightS)
                    appendSpan(spans, sp.s + contentj.size(), sp.e + contentj.size(), sp.kind, sp.href);
                contentj += right;
            }
            Row r{}; r.type = tj; r.level = lj; r.param = 1; r.spans = spans;
            const QString newId = makeUlid();
            const QString newRank = rankBetween(prevRank, nextRank);
            prevRank = newRank;
            rows_.insert(rows_.begin() + at, r);
            content_.insert(content_.begin() + at, contentj);
            ids_.insert(ids_.begin() + at, newId);
            ranks_.insert(ranks_.begin() + at, newRank);
            fenwick_.insert(static_cast<size_t>(at), estimatedHeight(r));
            made.push_back({newId, newRank, contentj, tj, lj, spans});
        }
        endInsertRows();
        if (doc_.isOpen())
            for (const NewBlk& b : made)
                doc_.appendBlock(b.id, b.rank, 0, QString::fromLatin1(typeToString(b.type)),
                                 attrsJson(b.type, b.level, QString(), b.spans), b.content);
    };

    if (opaque) {
        appendBlocksAfter(row, 0);                     // opaque block stays untouched
    } else {
        // ---- Block 0: merge seg[0] into the current block at the caret ----
        uint8_t t0, l0; QString text0; std::vector<Span> sp0; bool pfx0;
        parseSeg(segs[0], t0, l0, text0, sp0, pfx0);
        // Adopt the parsed block type only at a clean start (nothing to the left)
        // AND when the line carried a real prefix; else keep the block's type and
        // treat seg[0] as inline-styled text.
        if (left.isEmpty() && pfx0) { rows_[row].type = t0; rows_[row].level = l0; }
        QString newContent = left + text0;
        std::vector<Span> newSpans = leftS;
        for (const Span& sp : sp0)
            appendSpan(newSpans, sp.s + left.size(), sp.e + left.size(), sp.kind, sp.href);
        if (n == 1) {                                  // single line: tail stays here
            caretRow = row; caretCol = newContent.size();
            for (const Span& sp : rightS)
                appendSpan(newSpans, sp.s + newContent.size(), sp.e + newContent.size(), sp.kind, sp.href);
            newContent += right;
        }
        content_[row] = newContent;
        rows_[row].spans = newSpans;
        rows_[row].param = 1;
        persistContent(row);
        persistMeta(row);
        emit dataChanged(index(row), index(row), {ContentRole});
        if (n >= 2) appendBlocksAfter(row, 1);         // remaining lines → new blocks
    }

    bumpLayout();
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
    return QVariantList{ caretRow, caretCol };
}

QVariantList BlockModel::pasteHtml(int row, int col, const QString& html) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return {};

    QTextDocument doc;
    doc.setHtml(html);

    struct Spec { uint8_t type = Paragraph; uint8_t level = 0;
                  QString text; std::vector<Span> spans; QString tableJson; QString mediaJson; };
    std::vector<Spec> specs;

    // A QTextBlock → spec(s): heading/list/paragraph text (with inline bold/italic/
    // underline/strike/code/link spans), PLUS any embedded images as their own
    // Media specs, interleaved in fragment order (so a figure's image and caption
    // land as separate blocks in reading order). Image bytes come from the
    // QTextDocument resource cache (data: URIs decode there); local file:// images
    // are referenced in place. Remote http(s) images are left for a later pass.
    auto buildText = [&](const QTextBlock& b) {
        const int hl = b.blockFormat().headingLevel();
        uint8_t btype = Paragraph, blevel = 0;
        if (hl >= 1 && hl <= 6) { btype = Heading; blevel = static_cast<uint8_t>(hl); }
        else if (b.textList())  { btype = ListItem; }

        QString text; std::vector<Span> spans;
        auto flushText = [&]() {
            if (!text.trimmed().isEmpty()) {
                Spec sp; sp.type = btype; sp.level = blevel; sp.text = text; sp.spans = spans;
                specs.push_back(std::move(sp));
            }
            text.clear(); spans.clear();
        };

        for (auto it = b.begin(); !it.atEnd(); ++it) {
            const QTextFragment frag = it.fragment();
            if (!frag.isValid()) continue;
            const QTextCharFormat cf = frag.charFormat();
            if (cf.isImageFormat() && mediaStore_) {       // embedded image → Media spec
                const QString src = cf.toImageFormat().name();
                const QVariant res = doc.resource(QTextDocument::ImageResource, QUrl(src));
                MediaStore::ImageRef ref;
                if (res.canConvert<QImage>())       ref = mediaStore_->importImage(res.value<QImage>());
                else if (res.canConvert<QPixmap>()) ref = mediaStore_->importImage(res.value<QPixmap>().toImage());
                else if (!src.startsWith(QLatin1String("http"))) ref = mediaStore_->importFile(src);
                if (ref.ok()) {
                    flushText();
                    Spec sp; sp.type = Media; sp.mediaJson = mediaJson(ref);
                    specs.push_back(std::move(sp));
                } else if (src.startsWith(QLatin1String("http"))) {   // remote → fetched after insert
                    flushText();
                    Spec sp; sp.type = Media; sp.mediaJson = remoteImageJson(src);
                    specs.push_back(std::move(sp));
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
                spans.push_back({s, e, SpanLink, cf.anchorHref()});
            } else {
                if (cf.fontWeight() >= QFont::Bold) spans.push_back({s, e, SpanBold});
                if (cf.fontItalic())                spans.push_back({s, e, SpanItalic});
                if (cf.fontUnderline())             spans.push_back({s, e, SpanUnderline});
                if (cf.fontFixedPitch())            spans.push_back({s, e, SpanCode});
            }
            if (cf.fontStrikeOut())                 spans.push_back({s, e, SpanStrike});
        }
        flushText();
    };

    // A QTextTable → Table block spec (cell text joined per cell).
    auto buildTable = [&](QTextTable* t) {
        const int nr = t->rows(), nc = t->columns();
        if (nr < 1 || nc < 1) return;
        TableGrid g = TableGrid::makeEmpty(nr, nc);
        for (int r = 0; r < nr; ++r)
            for (int c = 0; c < nc; ++c) {
                QTextTableCell cell = t->cellAt(r, c);
                if (!cell.isValid()) continue;
                QString ct;
                for (auto bit = cell.begin(); !bit.atEnd(); ++bit) {
                    const QTextBlock cb = bit.currentBlock();
                    if (!cb.isValid()) continue;
                    QString bt = cb.text(); bt.replace(QChar(0xFFFC), QString());
                    if (bt.isEmpty()) continue;
                    if (!ct.isEmpty()) ct += QLatin1Char(' ');
                    ct += bt;
                }
                g.setCellText(r, c, ct);
            }
        Spec sp; sp.type = Table; sp.tableJson = g.toJson();
        specs.push_back(std::move(sp));
    };

    // Walk the frame tree so table cells aren't flattened into loose blocks.
    std::function<void(QTextFrame*)> walk = [&](QTextFrame* frame) {
        for (auto it = frame->begin(); !it.atEnd(); ++it) {
            if (QTextFrame* child = it.currentFrame()) {
                if (QTextTable* tbl = qobject_cast<QTextTable*>(child)) buildTable(tbl);
                else walk(child);
            } else {
                const QTextBlock block = it.currentBlock();
                if (block.isValid()) buildText(block);
            }
        }
    };
    walk(doc.rootFrame());
    if (specs.empty()) return {};

    // Turn a spec into (Row, content).
    auto specRow = [&](const Spec& sp, Row& r, QString& content) {
        r = Row{};
        if (!sp.mediaJson.isEmpty()) {              // embedded image → Media block
            r.type = Media;
            content = sp.mediaJson;
            fillMediaMeta(r, content);              // dims/aspect-param from the descriptor
        } else if (sp.type == Table) {
            r.type = Table;
            r.param = static_cast<uint16_t>(std::max(1, TableGrid::fromJson(sp.tableJson).rows()));
            content = sp.tableJson;
        } else {
            r.type = sp.type; r.level = sp.level; r.param = 1; r.spans = sp.spans;
            content = sp.text;
        }
    };

    const bool opaque = (rows_[row].type == Media || rows_[row].type == Table
                         || rows_[row].type == Divider);

    // A single plain paragraph → splice it INLINE at the caret (so pasting a few
    // styled words from a browser stays in the sentence and keeps its formatting),
    // unless the target block is opaque (then fall through to insert-after).
    if (specs.size() == 1 && specs[0].type == Paragraph && !opaque) {
        const Spec& sp = specs[0];
        const QString s = content_[row];
        col = std::clamp(col, 0, static_cast<int>(s.size()));
        beginTxn(row, row);
        shiftSpansInsert(rows_[row].spans, col, sp.text.size());
        for (const Span& x : sp.spans) {
            if (spanHasPayload(x.kind)) rows_[row].spans.push_back({x.s + col, x.e + col, x.kind, x.href});
            else addSpan(rows_[row].spans, x.s + col, x.e + col, x.kind);
        }
        content_[row] = s.left(col) + sp.text + s.mid(col);
        persistContent(row);
        persistMeta(row);
        emit dataChanged(index(row), index(row), {ContentRole});
        bumpLayout();
        ++contentRevision_;
        emit contentChangedSpike();
        endTxn();
        return QVariantList{ row, col + static_cast<int>(sp.text.size()) };
    }

    const bool reuse = !opaque && content_[row].isEmpty()
        && (rows_[row].type == Paragraph || rows_[row].type == Heading
            || rows_[row].type == Quote || rows_[row].type == ListItem);

    beginTxn(row, row);
    int caretRow = row, caretCol = 0;
    int startSpec = 0;

    if (reuse) {                                          // fold the 1st block into the blank row
        Row r; QString content; specRow(specs[0], r, content);
        rows_[row] = r;
        content_[row] = content;
        persistContent(row);
        persistMeta(row);
        emit dataChanged(index(row), index(row), {ContentRole});
        caretRow = row; caretCol = (specs[0].type == Table) ? 0 : content.size();
        startSpec = 1;
    }

    const int cnt = static_cast<int>(specs.size()) - startSpec;
    if (cnt > 0) {
        const int first = row + 1, last = row + cnt;
        QString prevRank = ranks_[row];
        const QString nextRank = (first < static_cast<int>(ranks_.size())) ? ranks_[first] : QString();
        struct NewBlk { QString id, rank, content; Row r; };
        std::vector<NewBlk> made;
        beginInsertRows({}, first, last);
        for (int k = 0; k < cnt; ++k) {
            const int at = first + k;
            const Spec& sp = specs[startSpec + k];
            Row r; QString content; specRow(sp, r, content);
            const QString id = makeUlid();
            const QString rk = rankBetween(prevRank, nextRank); prevRank = rk;
            rows_.insert(rows_.begin() + at, r);
            content_.insert(content_.begin() + at, content);
            ids_.insert(ids_.begin() + at, id);
            ranks_.insert(ranks_.begin() + at, rk);
            fenwick_.insert(static_cast<size_t>(at), estimatedHeight(r));
            made.push_back({id, rk, content, r});
            caretRow = at; caretCol = (sp.type == Table) ? 0 : content.size();
        }
        endInsertRows();
        if (doc_.isOpen())
            for (const NewBlk& b : made)
                doc_.appendBlock(b.id, b.rank, 0, QString::fromLatin1(typeToString(b.r.type)),
                                 attrsJson(b.r.type, b.r.level, QString(), b.r.spans), b.content);
    }

    bumpLayout();
    ++contentRevision_;
    emit contentChangedSpike();
    endTxn();
    fetchRemoteMediaIn(row, caretRow);     // localize any remote <img> in the background
    return QVariantList{ caretRow, caretCol };
}

// Scan [lo,hi] for media blocks whose src is a remote http(s) URL and download
// each into the sidecar; on completion swap the descriptor to the local copy
// (keyed by block id, so it survives reorders). The block displays the remote
// URL until then. Best-effort: a failed fetch just leaves the remote src.
void BlockModel::fetchRemoteMediaIn(int loRow, int hiRow) {
    if (!mediaStore_) return;
    loRow = std::max(0, loRow);
    hiRow = std::min(hiRow, static_cast<int>(rows_.size()) - 1);
    for (int r = loRow; r <= hiRow; ++r) {
        if (rows_[r].type != Media) continue;
        const QString src = QJsonDocument::fromJson(content_[r].toUtf8())
                                .object().value(QStringLiteral("src")).toString();
        if (!src.startsWith(QLatin1String("http"))) continue;
        if (!net_) net_ = new QNetworkAccessManager(this);
        const QString id = ids_[r];
        QNetworkRequest req((QUrl(src)));
        req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("minNotes"));
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
        QNetworkReply* reply = net_->get(req);
        connect(reply, &QNetworkReply::finished, this, [this, reply, id]() {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) return;   // leave remote src
            QImage img;
            if (!img.loadFromData(reply->readAll()) || img.isNull()) return;
            const MediaStore::ImageRef ref = mediaStore_->importImage(img);
            if (ref.ok()) updateMediaDescriptor(id, QString::fromUtf8(
                QJsonDocument(QJsonObject{{QStringLiteral("src"), ref.src},
                                          {QStringLiteral("w"), ref.w},
                                          {QStringLiteral("h"), ref.h}}).toJson(QJsonDocument::Compact)));
        });
    }
}

// Swap a media block's descriptor in place (a remote image finished localizing).
// Not a txn — it's a background system update, not a user edit; undoing the paste
// still removes the whole block.
void BlockModel::updateMediaDescriptor(const QString& blockId, const QString& json) {
    const int row = rowForId(blockId);
    if (row < 0 || rows_[row].type != Media) return;
    content_[row] = json;
    fillMediaMeta(rows_[row], json);
    rows_[row].measured = false;
    fenwick_.setHeight(static_cast<size_t>(row), estimatedHeight(rows_[row]));
    persistContent(row);
    emit dataChanged(index(row), index(row), {ContentRole});
    bumpLayout();
    ++contentRevision_;
    emit contentChangedSpike();
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
        if (sp.s < col) leftS.push_back({sp.s, std::min(sp.e, col), sp.kind, sp.href});
        if (sp.e > col) rightS.push_back({std::max(sp.s, col) - col, sp.e - col, sp.kind, sp.href});
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
