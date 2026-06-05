#pragma once
#include <QAbstractListModel>
#include <QHash>
#include <QString>
#include <vector>
#include <cstdint>
#include "FenwickTree.h"
#include "Document.h"

// The document model: a QAbstractListModel over the SQLite-canonical store
// (DESIGN.md §3–4). Eager skinny-scan seeds the layout (type/rank) + Fenwick
// height index; content is held per row (Phase 1a loads it all from the DB —
// lazy windowed fetch is a later optimization). Heights live in the Fenwick
// index. The editor talks only to the Q_INVOKABLE seam below.
class BlockModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ rowCountQml NOTIFY modelReset)
    Q_PROPERTY(qreal totalHeight READ totalHeight NOTIFY layoutChangedSpike)
    // Bump on any height change; QML bindings include it to force re-eval of
    // yForRow()/rowForY() (which are Q_INVOKABLE, not properties).
    Q_PROPERTY(int layoutRevision READ layoutRevision NOTIFY layoutChangedSpike)
    // Bump on any CONTENT change (edit/delete). The Flickable arm's text
    // binding includes it to re-read contentForRow() without repositioning.
    Q_PROPERTY(int contentRevision READ contentRevision NOTIFY contentChangedSpike)
public:
    enum Roles {
        TypeRole = Qt::UserRole + 1,
        ContentRole,
        HeightRole,
        MeasuredRole,
    };
    enum BlockType : uint8_t { Paragraph = 0, Heading = 1, Code = 2, Media = 3 };
    enum Distribution { Uniform = 0, Mixed = 1, Adversarial = 2 };

    explicit BlockModel(QObject* parent = nullptr);

    // QAbstractListModel
    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int rowCountQml() const { return static_cast<int>(rows_.size()); }
    qreal totalHeight() const { return fenwick_.total(); }
    int layoutRevision() const { return layoutRevision_; }
    int contentRevision() const { return contentRevision_; }

    // --- Build / reconfigure (driven by Main.qml controls) ---
    Q_INVOKABLE void rebuild(int n, int distribution);

    // --- Geometry, for the Flickable arm (and HUD) ---
    Q_INVOKABLE qreal yForRow(int row) const { return fenwick_.prefix(clampRow(row)); }
    Q_INVOKABLE qreal heightForRow(int row) const { return fenwick_.height(clampRow(row)); }
    Q_INVOKABLE int rowForY(qreal y) const { return static_cast<int>(fenwick_.rowAtOffset(y < 0 ? 0 : y)); }

    // --- Row data, for the Flickable arm (ListView uses roles) ---
    Q_INVOKABLE int typeForRow(int row) const;
    Q_INVOKABLE int levelForRow(int row) const;       // heading level 1–6, else 0
    Q_INVOKABLE QString contentForRow(int row) const;

    // type↔markdown autoformat: if content at `row` now starts with a recognised
    // prefix (e.g. "## "), consume it, set type/level, persist, and return the
    // number of chars stripped (so the editor can shift the caret). Else 0.
    Q_INVOKABLE int applyMarkdownTrigger(int row);

    // --- Feedback from delegates / edits ---
    // Delegate reports its laid-out height. Emits heightSettled(row, delta) so
    // the Flickable arm can compensate contentY when an off-screen block settles.
    Q_INVOKABLE void setMeasuredHeight(int row, qreal h);
    Q_INVOKABLE void setContent(int row, const QString& text);
    Q_INVOKABLE void insertBlock(int row);
    Q_INVOKABLE void removeBlock(int row);

    // P8: delete a logical selection spanning (aRow,aCol)..(fRow,fCol). Merges
    // the surviving head of the lo block with the tail of the hi block and
    // removes every block in between — a model op on a logical range, oblivious
    // to which of those blocks currently have delegates (DESIGN.md §7).
    Q_INVOKABLE void deleteRange(int aRow, int aCol, int fRow, int fCol);

    // Editing for the passive-surface arm: insert text at a caret, and split a
    // block at the caret (Enter). The model is the sole owner of content.
    Q_INVOKABLE void insertText(int row, int col, const QString& text);
    Q_INVOKABLE void splitBlock(int row, int col);

signals:
    void layoutChangedSpike();
    void contentChangedSpike();
    void modelReset();
    void heightSettled(int row, qreal delta);

private:
    struct Row {
        uint8_t type;
        uint16_t param;   // paragraph/code: line count; media: aspect*100; heading: 0
        uint8_t level = 0;  // heading level 1–6 (0 = not a heading)
        bool measured = false;
    };

    // Rule table: does `content` start with a markdown trigger? Fills type/
    // level and the prefix length to strip. The single source of truth that
    // (later) also drives import + export.
    static bool matchMarkdownPrefix(const QString& content, BlockType& type, int& level, int& strip);
    void persistMeta(int row);   // write type/attrs of content_[row]'s block

    int clampRow(int row) const;
    double estimatedHeight(const Row& r) const;
    QString genBase(int row, const Row& r) const;   // synthesize a row's text (no edit overlay)
    QString textAt(int row) const;                  // edit override else loaded content
    void bumpLayout();

    // --- SQLite store (Phase 1a/1b) ---
    void loadFromStore();             // skinny-scan → rows_/ids_/ranks_/content_/fenwick
    void seedSyntheticStore(int n);   // write N synthetic blocks if the DB is empty
    void persistContent(int row);     // write content_[row] back to the DB
    static BlockType typeFromString(const QString& s);
    static const char* typeToString(uint8_t t);
    // Lexicographic fractional rank strictly between a and b (DESIGN §4).
    // Empty a = "before all", empty b = "after all". O(shared-prefix length).
    static QString rankBetween(const QString& a, const QString& b);

    Document doc_;
    std::vector<Row> rows_;
    std::vector<QString> ids_;       // block id (ULID) per row, parallel to rows_
    std::vector<QString> ranks_;     // fractional rank per row, parallel to rows_
    std::vector<QString> content_;   // content per row (the in-memory truth; write-through to DB)
    FenwickTree fenwick_;
    int layoutRevision_ = 0;
    int contentRevision_ = 0;
};
