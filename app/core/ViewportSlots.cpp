#include "ViewportSlots.h"
#include <algorithm>
#include <unordered_set>

namespace mn {

bool assignSlots(std::vector<int>& slotRows, const std::vector<int>& visible) {
    const size_t n = slotRows.size();
    // Every visible row wants a slot (deduplicated). A row that ALREADY holds a slot keeps it
    // whenever it is still visible — even when the visible set outgrows the pool for a moment
    // (a zoom-out on a dense table, before the pool has grown): the old first-N cap evicted the
    // rows past the cap and re-added them a beat later, and their images popped out and in
    // (2026-09-16). New rows take free slots first-come; the rest wait for the pool to grow.
    std::unordered_set<int> want;
    std::vector<int> order;
    order.reserve(visible.size());
    for (int r : visible)
        if (r >= 0 && want.insert(r).second) order.push_back(r);
    bool changed = false;
    std::unordered_set<int> kept;
    for (int& r : slotRows) {
        if (r < 0) continue;
        if (want.count(r) && kept.insert(r).second) continue;   // still visible: keep
        r = -1;
        changed = true;
    }
    size_t free = 0;
    for (int r : order) {
        if (kept.count(r)) continue;
        while (free < n && slotRows[free] >= 0) ++free;
        if (free == n) break;                 // the pool is full: the remaining rows wait
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
