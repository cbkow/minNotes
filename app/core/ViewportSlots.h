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
public:
    explicit ViewportSlots(QObject* parent = nullptr) : QObject(parent) {}

    Q_INVOKABLE int sync(const QList<int>& visibleRows, int slotCount);
    Q_INVOKABLE int rowForSlot(int slot) const;
    Q_INVOKABLE int slotForRow(int row) const;
    int revision() const { return revision_; }

private:
    std::vector<int> slotRows_;
    QHash<int, int> slotByRow_;
    int revision_ = 0;
};
