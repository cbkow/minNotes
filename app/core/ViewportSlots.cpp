#include "ViewportSlots.h"
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

int ViewportSlots::rowForSlot(int slot) const {
    return slot >= 0 && slot < static_cast<int>(slotRows_.size()) ? slotRows_[size_t(slot)] : -1;
}

int ViewportSlots::slotForRow(int row) const {
    return slotByRow_.value(row, -1);
}
