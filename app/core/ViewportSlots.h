#pragma once
#include <QObject>
#include <QHash>
#include <QList>
#include <QPointer>
#include <vector>
#include <climits>

class BlockModel;

namespace mn {

// Stable slot allocation for the editor's delegate pool (SR-2). slotRows[i] is
// the row slot i renders, -1 when idle. Rows still visible keep their slot (no
// re-render); rows that left free theirs; entering rows take free slots in
// ascending slot order, in `visible` order. `visible` beyond the slot count is
// dropped; a duplicate is shown once. Works for any visible set — contiguous
// today, gapped once split-row lanes are DFS-flat (SR-3). Returns true when
// any slot changed (including a resize).
bool assignSlots(std::vector<int>& slotRows, const std::vector<int>& visible);

} // namespace mn

// A1 (speed programme): one per pool slot. `rev` is the slot's own change counter —
// a delegate keys every row-scoped binding on it instead of the document-wide
// contentRevision, so a keystroke re-evaluates the ONE delegate whose row changed
// (plus whatever the model widened to: its split row, its table) and not the whole
// pool. Bumped by ViewportSlots from the model's row signals; never from QML.
class SlotRev : public QObject {
    Q_OBJECT
    Q_PROPERTY(int rev READ rev NOTIFY revChanged)
public:
    explicit SlotRev(QObject* parent = nullptr) : QObject(parent) {}
    int rev() const { return rev_; }
    void bump() { ++rev_; emit revChanged(); }
signals:
    void revChanged();
    // A3: the model's text op for the row this slot shows (routed by ViewportSlots::attach).
    void textInserted(int col, const QString& text);
    void textRemoved(int start, int end);
private:
    int rev_ = 0;
};

// QML face of assignSlots, exposed as the `viewSlots` context property.
// sync() is called from a root binding and RETURNS the revision: delegates read
// that binding before rowForSlot(), which forces the sync to run first — the
// ordering the old pure modulo binding had. Invokables are untracked, so no
// NOTIFY and no binding loop. Idempotent: an unchanged set keeps the revision.
//
// A1: slot(i) hands a delegate its SlotRev. attach(model) wires the model's row
// signals to slot bumps: rowsTouched(lo, hi) / dataChanged → the slots holding
// rows in [lo, hi]; rowsInserted / rowsRemoved(first) → every slot holding a row
// ≥ first (a slot keeps its row INDEX when rows shift under it, so the block it
// shows changed — the row-shift hazard, solved by construction); a structure
// change or reset → all (rare; it restores the old every-delegate guarantee for
// structural edits, which is where derived state — table membership, column
// geometry — moves under rows that weren't themselves touched).
// Bumps are DEFERRED: the row signals only accumulate a pending range, and the
// flush runs on contentChangedSpike / structureChangedSpike, i.e. at the end of
// the mutation with the model consistent (reactivity rule 2). A bump inside
// endInsertRows would have the delegates read a half-built record. sync() itself
// never bumps: a re-pointed slot's logicalRow changes, and the delegate's
// bindings already key on that.
class ViewportSlots : public QObject {
    Q_OBJECT
public:
    explicit ViewportSlots(QObject* parent = nullptr) : QObject(parent) {}

    Q_INVOKABLE int sync(const QList<int>& visibleRows, int slotCount);
    Q_INVOKABLE int rowForSlot(int slot) const;
    Q_INVOKABLE int slotForRow(int row) const;
    Q_INVOKABLE QObject* slot(int index);          // created on demand, parented here, never deleted
    Q_INVOKABLE void bumpRows(int lo, int hi);     // slots whose row ∈ [lo, hi] (hi == INT_MAX: to the end)
    Q_INVOKABLE void bumpFrom(int fromRow);        // slots whose row ≥ fromRow
    Q_INVOKABLE void bumpAll();
    void attach(BlockModel* model);                // follow this model's row signals (bumps all)
    int revision() const { return revision_; }

private:
    std::vector<int> slotRows_;
    QHash<int, int> slotByRow_;
    int revision_ = 0;
    std::vector<SlotRev*> slots_;
    QPointer<BlockModel> model_;
    std::vector<QMetaObject::Connection> conns_;
    // Pending (deferred) bumps — see the class comment.
    int pendLo_ = INT_MAX, pendHi_ = -1;
    bool pendAll_ = false;
    void note(int lo, int hi);
    void flush();
};
