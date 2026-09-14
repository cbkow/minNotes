#include "LayoutIndex.h"
#include <algorithm>

namespace mn {

bool LayoutIndex::reset(const std::vector<Entry>& entries, std::vector<double> heights) {
    *this = LayoutIndex{};
    if (entries.size() != heights.size()) return false;
    const std::size_t n = entries.size();

    std::vector<std::size_t> slotOf(n, 0), indexInCell(n, 0), flatOfSlot;
    std::vector<int> splitOfSlot;
    std::vector<bool> hidden;
    std::vector<Split> splits;
    std::vector<std::vector<std::vector<double>>> laneHeights;   // per split, per lane
    std::vector<double> outerH;
    int open = -1;   // the record whose children may follow

    auto closeOpen = [&](std::size_t end) {
        if (open < 0) return true;
        splits[std::size_t(open)].end = end;
        const bool ok = !splits[std::size_t(open)].cellStart.empty();   // no childless record
        open = -1;
        return ok;
    };

    for (std::size_t f = 0; f < n; ++f) {
        const Entry& e = entries[f];
        if (e.cell == kTop) {
            if (!closeOpen(f)) return false;
            slotOf[f] = flatOfSlot.size();
            flatOfSlot.push_back(f);
            hidden.push_back(e.split && e.hidden);   // only a record folds
            if (e.split) {
                splitOfSlot.push_back(int(splits.size()));
                splits.push_back(Split{f, 0, {}, {}});
                splits.back().padTop = std::max(0.0, e.padTop);
                splits.back().padBottom = std::max(0.0, e.padBottom);
                laneHeights.emplace_back();
                open = int(splits.size()) - 1;
                outerH.push_back(0.0);   // derived once the lanes are known
            } else {
                splitOfSlot.push_back(-1);
                outerH.push_back(heights[f]);
            }
            continue;
        }
        if (open < 0 || e.split || e.cell < 0) return false;
        Split& s = splits[std::size_t(open)];
        auto& lanes = laneHeights[std::size_t(open)];
        const std::size_t c = std::size_t(e.cell);
        if (c == s.cellStart.size()) {            // the next lane starts
            s.cellStart.push_back(f);
            lanes.emplace_back();
        } else if (c + 1 != s.cellStart.size()) {  // a gap, or a lane revisited
            return false;
        }
        slotOf[f] = flatOfSlot.size() - 1;
        indexInCell[f] = f - s.cellStart[c];
        lanes[c].push_back(heights[f]);
    }
    if (!closeOpen(n)) return false;

    for (std::size_t k = 0; k < splits.size(); ++k) {
        Split& s = splits[k];
        s.cells.resize(laneHeights[k].size());
        for (std::size_t c = 0; c < s.cells.size(); ++c)
            s.cells[c].reset(std::move(laneHeights[k][c]));
        const double extent = s.extent();
        outerH[slotOf[s.flat]] = hidden[slotOf[s.flat]] ? 0.0 : extent;
        heights[s.flat] = extent;
    }

    entries_ = entries;
    heights_ = std::move(heights);
    slotOf_ = std::move(slotOf);
    indexInCell_ = std::move(indexInCell);
    flatOfSlot_ = std::move(flatOfSlot);
    splitOfSlot_ = std::move(splitOfSlot);
    hidden_ = std::move(hidden);
    splits_ = std::move(splits);
    outer_.reset(std::move(outerH));
    return true;
}

const LayoutIndex::Split* LayoutIndex::splitAt(std::size_t flat) const {
    if (flat >= entries_.size() || !entries_[flat].split) return nullptr;
    return &splits_[std::size_t(splitOfSlot_[slotOf_[flat]])];
}

double LayoutIndex::y(std::size_t flat) const {
    if (flat >= entries_.size()) return total();
    const std::size_t slot = slotOf_[flat];
    const double top = outer_.prefix(slot);
    const int c = entries_[flat].cell;
    if (c == kTop) return top;
    const Split& s = splits_[std::size_t(splitOfSlot_[slot])];
    return top + s.padTop + s.cells[std::size_t(c)].prefix(indexInCell_[flat]);
}

double LayoutIndex::setHeight(std::size_t flat, double h) {
    if (flat >= entries_.size() || entries_[flat].split) return 0.0;
    const std::size_t slot = slotOf_[flat];
    heights_[flat] = h;
    const int c = entries_[flat].cell;
    if (c == kTop) return outer_.setHeight(slot, h);
    Split& s = splits_[std::size_t(splitOfSlot_[slot])];
    s.cells[std::size_t(c)].setHeight(indexInCell_[flat], h);
    const double extent = s.extent();
    heights_[s.flat] = extent;
    return hidden_[slot] ? 0.0 : outer_.setHeight(slot, extent);
}

double LayoutIndex::setHidden(std::size_t flat, bool hidden) {
    if (flat >= entries_.size()) return 0.0;
    const std::size_t slot = slotOf_[flat];
    if (splitOfSlot_[slot] < 0 || hidden_[slot] == hidden) return 0.0;
    hidden_[slot] = hidden;
    return outer_.setHeight(slot, hidden ? 0.0 : heights_[flatOfSlot_[slot]]);
}

std::size_t LayoutIndex::topAt(double y) const {
    if (flatOfSlot_.empty()) return 0;
    return flatOfSlot_[outer_.rowAtOffset(std::max(0.0, y))];
}

int LayoutIndex::cellCount(std::size_t split) const {
    const Split* s = splitAt(split);
    return s ? int(s->cells.size()) : 0;
}

std::size_t LayoutIndex::cellFirst(std::size_t split, int c) const {
    const Split* s = splitAt(split);
    if (!s || c < 0 || std::size_t(c) >= s->cellStart.size()) return split;
    return s->cellStart[std::size_t(c)];
}

std::size_t LayoutIndex::cellEnd(std::size_t split, int c) const {
    const Split* s = splitAt(split);
    if (!s || c < 0 || std::size_t(c) >= s->cellStart.size()) return split;
    return std::size_t(c) + 1 < s->cellStart.size() ? s->cellStart[std::size_t(c) + 1] : s->end;
}

double LayoutIndex::cellHeight(std::size_t split, int c) const {
    const Split* s = splitAt(split);
    if (!s || c < 0 || std::size_t(c) >= s->cells.size()) return 0.0;
    return s->cells[std::size_t(c)].total();
}

std::size_t LayoutIndex::blockInCellAt(std::size_t split, int c, double ly, bool* pastEnd) const {
    if (pastEnd) *pastEnd = false;
    const Split* s = splitAt(split);
    if (!s || s->cells.empty()) return split;
    const std::size_t lane = std::size_t(std::clamp(c, 0, int(s->cells.size()) - 1));
    const FenwickTree& t = s->cells[lane];
    ly -= s->padTop;                                  // local offsets are from the record's top
    if (ly >= t.total()) {
        if (pastEnd) *pastEnd = true;
        return s->cellStart[lane] + t.size() - 1;
    }
    return s->cellStart[lane] + t.rowAtOffset(std::max(0.0, ly));
}

std::vector<std::size_t> LayoutIndex::visible(double y0, double y1) const {
    std::vector<std::size_t> out;
    if (flatOfSlot_.empty() || y1 <= y0) return out;
    // An entry starting inside the window counts even at zero height.
    auto hits = [&](double top, double h) { return top < y1 && (top + h > y0 || top >= y0); };
    for (std::size_t slot = outer_.rowAtOffset(std::max(0.0, y0)); slot < flatOfSlot_.size(); ++slot) {
        const double top = outer_.prefix(slot);
        if (top >= y1) break;
        const std::size_t f = flatOfSlot_[slot];
        if (hidden_[slot] || !hits(top, outer_.height(slot))) continue;   // a folded record and its lanes
        out.push_back(f);
        if (splitOfSlot_[slot] < 0) continue;
        const Split& s = splits_[std::size_t(splitOfSlot_[slot])];
        const double ly0 = std::max(0.0, y0 - top - s.padTop);
        for (std::size_t c = 0; c < s.cells.size(); ++c) {
            const FenwickTree& lane = s.cells[c];
            if (lane.size() == 0 || (ly0 > 0.0 && ly0 >= lane.total())) continue;   // below a short lane
            for (std::size_t i = lane.rowAtOffset(ly0); i < lane.size(); ++i) {
                const double by = top + s.padTop + lane.prefix(i);
                if (by >= y1) break;
                if (hits(by, lane.height(i))) out.push_back(s.cellStart[c] + i);
            }
        }
    }
    return out;
}

} // namespace mn
