#include "BlockModel.h"
#include <QStringBuilder>
#include <QStandardPaths>
#include <QDir>
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
    if (s == QLatin1String("heading")) return Heading;
    if (s == QLatin1String("code"))    return Code;
    if (s == QLatin1String("media"))   return Media;
    return Paragraph;
}

const char* BlockModel::typeToString(uint8_t t) {
    switch (t) {
    case Heading: return "heading";
    case Code:    return "code";
    case Media:   return "media";
    default:      return "paragraph";
    }
}

void BlockModel::seedSyntheticStore(int n) {
    // One-time fill of an empty doc so there's something to edit. Sequential
    // zero-padded ranks keep visible order; fractional inserts come in Phase 1b.
    doc_.begin();
    for (int i = 0; i < n; ++i) {
        Row r{}; r.type = Paragraph; r.param = 1;
        const QString rank = QString::number(i).rightJustified(10, QLatin1Char('0'));
        doc_.appendBlock(makeUlid(), rank, 0, QString::fromLatin1(typeToString(r.type)),
                         QString(), genBase(i, r));
    }
    doc_.commit();
}

void BlockModel::loadFromStore() {
    beginResetModel();
    rows_.clear();
    ids_.clear();
    content_.clear();
    edits_.clear();

    const std::vector<Document::BlockMeta> metas = doc_.skinnyScan();
    rows_.reserve(metas.size());
    ids_.reserve(metas.size());
    content_.reserve(metas.size());
    std::vector<double> heights;
    heights.reserve(metas.size());

    for (const Document::BlockMeta& m : metas) {
        const QString text = doc_.contentFor(m.id);   // Phase 1a: load eagerly
        Row r{};
        r.type = typeFromString(m.type);
        r.param = static_cast<uint16_t>(std::max<int>(1, text.count(QLatin1Char('\n')) + 1));
        rows_.push_back(r);
        ids_.push_back(m.id);
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
    edits_.clear();
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
    case Code:
    case Paragraph:
    default:      return r.param * kLine + kPadV;
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
    if (const auto it = edits_.constFind(row); it != edits_.constEnd()) return it.value();
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
    edits_[row] = text;
    const QModelIndex idx = index(row);
    emit dataChanged(idx, idx, {ContentRole});   // ListView arm reacts to this
    ++contentRevision_;
    emit contentChangedSpike();                   // Flickable arm reacts to this
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
    edits_[loRow] = merged;
    emit dataChanged(index(loRow), index(loRow), {ContentRole});

    if (hiRow > loRow) {
        const int first = loRow + 1, last = hiRow, cnt = last - first + 1;
        beginRemoveRows({}, first, last);
        // Gather current (measured/estimated) heights of surviving rows.
        std::vector<double> hs;
        hs.reserve(rows_.size() - static_cast<size_t>(cnt));
        for (int i = 0; i < static_cast<int>(rows_.size()); ++i)
            if (i < first || i > last) hs.push_back(fenwick_.height(static_cast<size_t>(i)));
        rows_.erase(rows_.begin() + first, rows_.begin() + last + 1);
        content_.erase(content_.begin() + first, content_.begin() + last + 1);
        ids_.erase(ids_.begin() + first, ids_.begin() + last + 1);
        // Re-key sparse edits across the removed range.
        QHash<int, QString> shifted;
        for (auto it = edits_.constBegin(); it != edits_.constEnd(); ++it) {
            if (it.key() < first) shifted.insert(it.key(), it.value());
            else if (it.key() > last) shifted.insert(it.key() - cnt, it.value());
        }
        edits_ = std::move(shifted);
        fenwick_.reset(std::move(hs));
        endRemoveRows();
        bumpLayout();
    }
    ++contentRevision_;
    emit contentChangedSpike();
}

void BlockModel::insertText(int row, int col, const QString& text) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return;
    QString s = textAt(row);
    col = std::clamp(col, 0, static_cast<int>(s.size()));
    edits_[row] = s.left(col) + text + s.mid(col);
    emit dataChanged(index(row), index(row), {ContentRole});
    ++contentRevision_;
    emit contentChangedSpike();
}

void BlockModel::splitBlock(int row, int col) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return;
    QString s = textAt(row);
    col = std::clamp(col, 0, static_cast<int>(s.size()));
    edits_[row] = s.left(col);
    const int at = row + 1;
    beginInsertRows({}, at, at);
    Row r{}; r.type = Paragraph; r.param = 1;
    rows_.insert(rows_.begin() + at, r);
    content_.insert(content_.begin() + at, QString());   // base; edit overlay below holds the text
    ids_.insert(ids_.begin() + at, makeUlid());
    QHash<int, QString> shifted;
    for (auto it = edits_.constBegin(); it != edits_.constEnd(); ++it)
        shifted.insert(it.key() >= at ? it.key() + 1 : it.key(), it.value());
    edits_ = std::move(shifted);
    edits_[at] = s.mid(col);
    fenwick_.insert(static_cast<size_t>(at), estimatedHeight(r));
    endInsertRows();
    bumpLayout();
    ++contentRevision_;
    emit contentChangedSpike();
}

void BlockModel::insertBlock(int row) {
    row = std::clamp(row, 0, static_cast<int>(rows_.size()));
    beginInsertRows({}, row, row);
    Row r{}; r.type = Paragraph; r.param = 1;
    rows_.insert(rows_.begin() + row, r);
    content_.insert(content_.begin() + row, QString());
    ids_.insert(ids_.begin() + row, makeUlid());
    // Shift sparse edits at/after row.
    QHash<int, QString> shifted;
    for (auto it = edits_.constBegin(); it != edits_.constEnd(); ++it)
        shifted.insert(it.key() >= row ? it.key() + 1 : it.key(), it.value());
    edits_ = std::move(shifted);
    fenwick_.insert(static_cast<size_t>(row), estimatedHeight(r));
    endInsertRows();
    bumpLayout();
}

void BlockModel::removeBlock(int row) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return;
    beginRemoveRows({}, row, row);
    rows_.erase(rows_.begin() + row);
    content_.erase(content_.begin() + row);
    ids_.erase(ids_.begin() + row);
    QHash<int, QString> shifted;
    for (auto it = edits_.constBegin(); it != edits_.constEnd(); ++it) {
        if (it.key() == row) continue;
        shifted.insert(it.key() > row ? it.key() - 1 : it.key(), it.value());
    }
    edits_ = std::move(shifted);
    fenwick_.erase(static_cast<size_t>(row));
    endRemoveRows();
    bumpLayout();
}
