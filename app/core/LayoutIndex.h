#pragma once
#include "FenwickTree.h"
#include <cstddef>
#include <vector>

namespace mn {

// Two-level height index for a DFS-flat document (SR-3; PLAN-split-rows
// "Rendering, index, storage", rows_spike §A–D, F).
//
// Flat entries are in document order. A TOP entry is a plain block or a split
// row's record; a record is followed by its cells' blocks, cell numbers
// 0,1,…,C−1 in order with no gaps and no empty cell. Heights belong to blocks;
// a record's height is DERIVED — its tallest cell.
//
//   y(flat)             top of a block or record          O(log n + log k)
//   setHeight(block, h) returns the change in its TOP entry's height — the
//                       value a view compensates its scroll position by
//   topAt(y)            the top entry containing offset y  O(log n)
//   visible(y0, y1)     work bounded by what is visible (spike §B)
//
// Structural edits rebuild with reset() in O(n) — the class FenwickTree's
// insert/erase already has.
class LayoutIndex {
public:
    static constexpr int kTop = -1;
    struct Entry {
        bool split = false;   // a split row's record (top level only)
        int cell = kTop;      // >= 0: a block in that lane of the preceding record
    };

    // entries and heights are parallel; a record's height is ignored. A malformed
    // structure is rejected: returns false and leaves the index empty.
    bool reset(const std::vector<Entry>& entries, std::vector<double> heights);

    std::size_t size() const { return entries_.size(); }
    double total() const { return outer_.total(); }
    double y(std::size_t flat) const;
    double height(std::size_t flat) const { return flat < heights_.size() ? heights_[flat] : 0.0; }
    const std::vector<double>& heights() const { return heights_; }   // per flat entry
    double setHeight(std::size_t flat, double h);   // no-op (0) on a record

    const Entry& entry(std::size_t flat) const { return entries_[flat]; }
    std::size_t topOf(std::size_t flat) const { return flat < slotOf_.size() ? flatOfSlot_[slotOf_[flat]] : flat; }
    std::size_t topAt(double y) const;
    std::size_t topCount() const { return flatOfSlot_.size(); }
    std::size_t slotOf(std::size_t flat) const { return slotOf_[flat]; }
    std::size_t flatOfSlot(std::size_t slot) const { return flatOfSlot_[slot]; }

    // Lanes of the record at `split` (0 for anything that isn't a record).
    int cellCount(std::size_t split) const;
    std::size_t cellFirst(std::size_t split, int c) const;   // flat index of the lane's first block
    std::size_t cellEnd(std::size_t split, int c) const;     // one past its last block
    double cellHeight(std::size_t split, int c) const;
    // The lane's block containing local offset ly (from the record's top). Past
    // the lane's end → its last block, with *pastEnd set (the space below a short lane).
    std::size_t blockInCellAt(std::size_t split, int c, double ly, bool* pastEnd = nullptr) const;

    // Every entry intersecting [y0, y1) — top blocks, records, and each lane's
    // visible blocks — in flat order.
    std::vector<std::size_t> visible(double y0, double y1) const;

private:
    struct Split {
        std::size_t flat = 0, end = 0;          // the record, and one past its last child
        std::vector<std::size_t> cellStart;
        std::vector<FenwickTree> cells;
    };
    const Split* splitAt(std::size_t flat) const;

    std::vector<Entry> entries_;
    std::vector<double> heights_;             // per flat entry; records mirror their extent
    std::vector<std::size_t> slotOf_;         // flat → top slot
    std::vector<std::size_t> indexInCell_;    // flat → position within its lane (children)
    std::vector<std::size_t> flatOfSlot_;     // top slot → flat
    std::vector<int> splitOfSlot_;            // top slot → splits_ index, or -1
    std::vector<Split> splits_;
    FenwickTree outer_;                       // one entry per top slot
};

} // namespace mn
