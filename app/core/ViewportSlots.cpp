#include "ViewportSlots.h"
#include "BlockModel.h"
#include "PerfCounters.h"
#include <algorithm>
#include <climits>
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

QObject* ViewportSlots::slot(int index) {
    if (index < 0) return nullptr;
    const size_t i = static_cast<size_t>(index);
    if (i >= slots_.size()) slots_.resize(i + 1, nullptr);
    if (!slots_[i]) slots_[i] = new SlotRev(this);
    return slots_[i];
}

void ViewportSlots::bumpRows(int lo, int hi) {
    if (hi < lo) return;
    auto& pc = mn::perf::counters();
    for (size_t i = 0; i < slotRows_.size() && i < slots_.size(); ++i) {
        const int r = slotRows_[i];
        if (r >= lo && r <= hi && slots_[i]) { slots_[i]->bump(); ++pc.slotBumps; }
    }
}

void ViewportSlots::bumpFrom(int fromRow) { bumpRows(fromRow, INT_MAX); }

void ViewportSlots::bumpAll() {
    // Idle slots (no row) skip: their delegate's bindings short-circuit on `active`, and a row
    // arriving later changes logicalRow, which re-evaluates them anyway.
    auto& pc = mn::perf::counters();
    for (size_t i = 0; i < slots_.size(); ++i)
        if (slots_[i] && (i >= slotRows_.size() || slotRows_[i] >= 0)) { slots_[i]->bump(); ++pc.slotBumps; }
}

void ViewportSlots::note(int lo, int hi) {
    if (hi < lo) return;
    pendLo_ = std::min(pendLo_, std::max(0, lo));
    pendHi_ = std::max(pendHi_, hi);
}

void ViewportSlots::flush() {
    if (pendAll_) bumpAll();
    else if (pendHi_ >= pendLo_) bumpRows(pendLo_, pendHi_);
    pendAll_ = false; pendLo_ = INT_MAX; pendHi_ = -1;
}

void ViewportSlots::attach(BlockModel* model) {
    if (model == model_) return;
    for (auto& c : conns_) QObject::disconnect(c);
    conns_.clear();
    model_ = model;
    pendAll_ = false; pendLo_ = INT_MAX; pendHi_ = -1;
    if (model) {
        // Accumulate…
        conns_.push_back(connect(model, &BlockModel::rowsTouched, this, &ViewportSlots::note));
        conns_.push_back(connect(model, &QAbstractItemModel::dataChanged, this,
                                 [this](const QModelIndex& tl, const QModelIndex& br) { note(tl.row(), br.row()); }));
        conns_.push_back(connect(model, &QAbstractItemModel::rowsInserted, this,
                                 [this](const QModelIndex&, int first, int) { note(first, INT_MAX); }));
        conns_.push_back(connect(model, &QAbstractItemModel::rowsRemoved, this,
                                 [this](const QModelIndex&, int first, int) { note(first, INT_MAX); }));
        conns_.push_back(connect(model, &QAbstractItemModel::modelReset, this, [this] { pendAll_ = true; }));
        // …and flush with the model consistent. A structure spike bumps every slot (and a reset
        // emits one even without a following content spike — loadFromStore).
        conns_.push_back(connect(model, &BlockModel::structureChangedSpike, this, [this] { pendAll_ = true; flush(); }));
        conns_.push_back(connect(model, &BlockModel::contentChangedSpike, this, &ViewportSlots::flush));
    }
    bumpAll();   // a different document behind every slot
}
