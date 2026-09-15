#pragma once
#include <QObject>
#include <QHash>
#include <QList>
#include <vector>

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

// QML face of assignSlots, exposed as the `viewSlots` context property.
// sync() is called from a root binding and RETURNS the revision: delegates read
// that binding before rowForSlot(), which forces the sync to run first — the
// ordering the old pure modulo binding had. Invokables are untracked, so no
// NOTIFY and no binding loop. Idempotent: an unchanged set keeps the revision.
class ViewportSlots : public QObject {
    Q_OBJECT
    // Rows that still want a slot after the last sync (overscan rows past the budget, or rows
    // with no free slot yet). The editor re-syncs on a timer while this is > 0.
    Q_PROPERTY(int pending READ pending NOTIFY pendingChanged)
public:
    explicit ViewportSlots(QObject* parent = nullptr) : QObject(parent) {}

    Q_INVOKABLE int sync(const QList<int>& visibleRows, int slotCount);
    // Budgeted sync (2026-09-15 walk: a table entering the overscan band rebound ~100 delegates in
    // one frame): `all` = the rows to show (viewport + overscan, in order), `inView` = the ones
    // actually on screen. Rows leaving free their slot; in-view rows entering take a slot at once;
    // overscan rows entering take at most `budget` slots per call — the rest are `pending`.
    Q_INVOKABLE int sync(const QList<int>& all, const QList<int>& inView, int slotCount, int budget);
    Q_INVOKABLE int rowForSlot(int slot) const;
    Q_INVOKABLE int slotForRow(int row) const;
    int revision() const { return revision_; }
    int pending() const { return pending_; }

signals:
    void pendingChanged();

private:
    void setPending(int n);
    std::vector<int> slotRows_;
    QHash<int, int> slotByRow_;
    int revision_ = 0;
    int pending_ = 0;
};
