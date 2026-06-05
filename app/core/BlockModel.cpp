#include "BlockModel.h"
#include <QStringBuilder>
#include <QStandardPaths>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
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
        const QString text = doc_.contentFor(m.id);   // Phase 1a: load eagerly
        Row r{};
        r.type = typeFromString(m.type);
        r.param = static_cast<uint16_t>(std::max<int>(1, text.count(QLatin1Char('\n')) + 1));
        if (r.type == Heading) {
            const QJsonObject o = QJsonDocument::fromJson(m.attrs.toUtf8()).object();
            r.level = static_cast<uint8_t>(std::clamp(o.value(QStringLiteral("level")).toInt(1), 1, 6));
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
    emit modelReset();
    emit layoutChangedSpike();
}

double BlockModel::estimatedHeight(const Row& r) const {
    switch (r.type) {
    case Heading: return kHeading + kPadV;
    case Media:   return kWidthEst * (r.param / 100.0) + kPadV;  // aspect = param/100
    case Divider: return 24.0;
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

void BlockModel::persistMeta(int row) {
    if (!doc_.isOpen() || row < 0 || row >= static_cast<int>(ids_.size())) return;
    QString attrs;
    if (rows_[row].type == Heading && rows_[row].level > 0) {
        QJsonObject o; o.insert(QStringLiteral("level"), rows_[row].level);
        attrs = QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
    }
    doc_.updateMeta(ids_[row], QString::fromLatin1(typeToString(rows_[row].type)), attrs);
}

int BlockModel::applyMarkdownTrigger(int row) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return 0;
    if (rows_[row].type != Paragraph) return 0;   // only transform plain paragraphs
    BlockType t = Paragraph; int level = 0, strip = 0;
    if (!matchMarkdownPrefix(content_[row], t, level, strip)) return 0;

    rows_[row].type = static_cast<uint8_t>(t);
    rows_[row].level = static_cast<uint8_t>(level);
    content_[row] = content_[row].mid(strip);
    persistContent(row);
    persistMeta(row);
    emit dataChanged(index(row), index(row), {TypeRole, ContentRole});
    bumpLayout();                 // type change → height changes; delegate re-measures
    ++contentRevision_;
    emit contentChangedSpike();
    return strip;
}

bool BlockModel::makeDividerIfMarker(int row) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return false;
    if (rows_[row].type != Paragraph) return false;
    const QString c = content_[row].trimmed();
    if (c != QLatin1String("---") && c != QLatin1String("***") && c != QLatin1String("___"))
        return false;
    rows_[row].type = Divider;
    rows_[row].level = 0;
    content_[row].clear();
    persistContent(row);
    persistMeta(row);
    emit dataChanged(index(row), index(row), {TypeRole, ContentRole});
    bumpLayout();
    ++contentRevision_;
    emit contentChangedSpike();
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

void BlockModel::setContent(int row, const QString& text) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return;
    content_[row] = text;
    persistContent(row);                          // write-through to SQLite
    const QModelIndex idx = index(row);
    emit dataChanged(idx, idx, {ContentRole});
    ++contentRevision_;
    emit contentChangedSpike();
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

    // Merge surviving head of lo block with surviving tail of hi block.
    const QString loText = textAt(loRow);
    const QString hiText = textAt(hiRow);
    const QString merged = loText.left(std::min<int>(loCol, loText.size()))
                         + hiText.mid(std::min<int>(hiCol, hiText.size()));
    content_[loRow] = merged;
    persistContent(loRow);
    emit dataChanged(index(loRow), index(loRow), {ContentRole});

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
}

void BlockModel::insertText(int row, int col, const QString& text) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return;
    const QString s = content_[row];
    col = std::clamp(col, 0, static_cast<int>(s.size()));
    content_[row] = s.left(col) + text + s.mid(col);
    persistContent(row);
    emit dataChanged(index(row), index(row), {ContentRole});
    ++contentRevision_;
    emit contentChangedSpike();
}

void BlockModel::splitBlock(int row, int col) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return;
    const QString s = content_[row];
    col = std::clamp(col, 0, static_cast<int>(s.size()));
    const QString left = s.left(col), right = s.mid(col);
    const int at = row + 1;
    const QString newId = makeUlid();
    const QString newRank = rankBetween(ranks_[row],
        (at < static_cast<int>(ranks_.size())) ? ranks_[at] : QString());

    content_[row] = left;
    persistContent(row);

    beginInsertRows({}, at, at);
    Row r{}; r.type = Paragraph; r.param = 1;
    rows_.insert(rows_.begin() + at, r);
    content_.insert(content_.begin() + at, right);
    ids_.insert(ids_.begin() + at, newId);
    ranks_.insert(ranks_.begin() + at, newRank);
    fenwick_.insert(static_cast<size_t>(at), estimatedHeight(r));
    endInsertRows();

    if (doc_.isOpen())
        doc_.appendBlock(newId, newRank, 0, QString::fromLatin1(typeToString(r.type)), QString(), right);

    bumpLayout();
    ++contentRevision_;
    emit contentChangedSpike();
}

void BlockModel::insertBlock(int row) {
    row = std::clamp(row, 0, static_cast<int>(rows_.size()));
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
}

void BlockModel::removeBlock(int row) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return;
    if (doc_.isOpen()) doc_.deleteBlock(ids_[row]);
    beginRemoveRows({}, row, row);
    rows_.erase(rows_.begin() + row);
    content_.erase(content_.begin() + row);
    ids_.erase(ids_.begin() + row);
    ranks_.erase(ranks_.begin() + row);
    fenwick_.erase(static_cast<size_t>(row));
    endRemoveRows();
    bumpLayout();
}
