#pragma once
#include <QAbstractListModel>
#include <QHash>
#include <QString>
#include <vector>
#include <cstdint>
#include "FenwickTree.h"

// Synthetic, lazily-served block store for the virtualization spike.
// This is the SEAM SQLite plugs into later (DESIGN.md §3): the model serves
// {type, content, height} per row without ever materialising all N rows of
// content. For the spike, content is generated on demand from the row index
// and heights live in a Fenwick index — no DB, the seam is what we prove.
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
    Q_INVOKABLE QString contentForRow(int row) const;

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
        bool measured = false;
    };

    int clampRow(int row) const;
    double estimatedHeight(const Row& r) const;
    QString genBase(int row, const Row& r) const;   // synthesize a row's text (no edit overlay)
    QString textAt(int row) const;                  // edit override else pre-generated base
    void bumpLayout();

    std::vector<Row> rows_;
    std::vector<QString> content_;   // base text per row, generated ONCE at rebuild
                                     // (stands in for a cheap indexed SQLite fetch — keeps
                                     // string synthesis out of the scroll hot path)
    QHash<int, QString> edits_;      // sparse content overrides for edited rows
    FenwickTree fenwick_;
    int layoutRevision_ = 0;
    int contentRevision_ = 0;
};
