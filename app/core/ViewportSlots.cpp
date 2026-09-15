#include "ViewportSlots.h"
#include <QSet>
#include <algorithm>
#include <unordered_set>

namespace mn {

bool assignSlots(std::vector<int>& slotRows, const std::vector<int>& visible) {
    const size_t n = slotRows.size();
    // The rows that fit, first-come, deduplicated.
    std::vector<int> shown;
    shown.reserve(std::min(n, visible.size()));
    std::unordered_set<int> want;
    for (int r : visible) {
        if (shown.size() == n) break;
        if (r >= 0 && want.insert(r).second) shown.push_back(r);
    }
    bool changed = false;
    std::unordered_set<int> kept;
    for (int& r : slotRows) {
        if (r < 0) continue;
        if (want.count(r) && kept.insert(r).second) continue;   // still visible: keep
        r = -1;
        changed = true;
    }
    size_t free = 0;
    for (int r : shown) {
        if (kept.count(r)) continue;
        while (slotRows[free] >= 0) ++free;   // shown.size() <= n guarantees a free slot
        slotRows[free] = r;
        changed = true;
    }
    return changed;
}

} // namespace mn

int ViewportSlots::sync(const QList<int>& visibleRows, int slotCount) {
    const size_t n = static_cast<size_t>(std::max(0, slotCount));
    bool changed = false;
    if (slotRows_.size() != n) {
        slotRows_.resize(n, -1);
        changed = true;
    }
    const std::vector<int> visible(visibleRows.cbegin(), visibleRows.cend());
    if (mn::assignSlots(slotRows_, visible)) changed = true;
    if (changed) {
        slotByRow_.clear();
        for (size_t i = 0; i < slotRows_.size(); ++i)
            if (slotRows_[i] >= 0) slotByRow_.insert(slotRows_[i], static_cast<int>(i));
        ++revision_;
    }
    return revision_;
}

void ViewportSlots::setPending(int n) {
    if (n == pending_) return;
    pending_ = n;
    emit pendingChanged();
}

int ViewportSlots::sync(const QList<int>& all, const QList<int>& inView, int slotCount, int budget) {
    const size_t n = static_cast<size_t>(std::max(0, slotCount));
    bool changed = false;
    if (slotRows_.size() != n) {
        slotRows_.resize(n, -1);
        changed = true;
    }
    QSet<int> allSet;
    for (int r : all) allSet.insert(r);
    QSet<int> viewSet;
    for (int r : inView) viewSet.insert(r);
    // Rows that left free their slot.
    QHash<int, int> byRow;
    for (size_t i = 0; i < slotRows_.size(); ++i) {
        const int r = slotRows_[i];
        if (r < 0) continue;
        if (!allSet.contains(r) || byRow.contains(r)) { slotRows_[i] = -1; changed = true; continue; }   // gone, or a duplicate
        byRow.insert(r, static_cast<int>(i));
    }
    // Entering rows, in `all` order: the viewport's first (uncapped), then the overscan's within budget.
    std::vector<int> entering;
    for (int r : all) if (!byRow.contains(r)) entering.push_back(r);
    size_t freeSlot = 0;
    auto takeSlot = [&](int row) {
        while (freeSlot < slotRows_.size() && slotRows_[freeSlot] >= 0) ++freeSlot;
        if (freeSlot >= slotRows_.size()) return false;
        slotRows_[freeSlot] = row; byRow.insert(row, static_cast<int>(freeSlot)); changed = true;
        return true;
    };
    int left = 0;
    for (int r : entering) if (viewSet.contains(r) && !byRow.contains(r) && !takeSlot(r)) ++left;
    int spent = 0;
    for (int r : entering) {
        if (viewSet.contains(r) || byRow.contains(r)) continue;
        if (spent >= budget || !takeSlot(r)) { ++left; continue; }
        ++spent;
    }
    if (changed) {
        slotByRow_ = byRow;
        ++revision_;
    }
    setPending(left);
    return revision_;
}

int ViewportSlots::rowForSlot(int slot) const {
    return slot >= 0 && slot < static_cast<int>(slotRows_.size()) ? slotRows_[size_t(slot)] : -1;
}

int ViewportSlots::slotForRow(int row) const {
    return slotByRow_.value(row, -1);
}
