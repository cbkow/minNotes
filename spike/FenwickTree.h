#pragma once
#include <vector>
#include <cstddef>

// Cumulative-height index for the virtualization spike (DESIGN.md §7:
// "the most important structure in the design").
//
// 1-indexed Fenwick / binary-indexed tree over per-row heights:
//   - setHeight(row, h)   point update              O(log n)
//   - prefix(row)         sum of heights [0, row)   O(log n)   -> yForRow
//   - rowAtOffset(y)      block containing offset y O(log n)   -> rowForY
//   - total()            full document height       O(1)       -> scrollbar
//
// insert/erase rebuild in O(n): they are single user actions (insert/delete a
// block), not a hot path. If the real app needs O(log n) structural edits,
// swap in an order-statistics tree — the call sites don't change.
class FenwickTree {
public:
    void reset(std::vector<double> heights) {
        heights_ = std::move(heights);
        n_ = heights_.size();
        tree_.assign(n_ + 1, 0.0);
        total_ = 0.0;
        for (std::size_t i = 1; i <= n_; ++i) {
            tree_[i] += heights_[i - 1];
            total_ += heights_[i - 1];
            const std::size_t j = i + lowbit(i);
            if (j <= n_) tree_[j] += tree_[i];
        }
    }

    // Returns the delta applied (new - old), for contentY compensation.
    double setHeight(std::size_t row, double h) {
        if (row >= n_) return 0.0;
        const double delta = h - heights_[row];
        if (delta == 0.0) return 0.0;
        heights_[row] = h;
        total_ += delta;
        for (std::size_t i = row + 1; i <= n_; i += lowbit(i))
            tree_[i] += delta;
        return delta;
    }

    double height(std::size_t row) const { return row < n_ ? heights_[row] : 0.0; }

    // Sum of heights of rows [0, row).
    double prefix(std::size_t row) const {
        if (row > n_) row = n_;
        double s = 0.0;
        for (std::size_t i = row; i > 0; i -= lowbit(i)) s += tree_[i];
        return s;
    }

    double total() const { return total_; }
    std::size_t size() const { return n_; }

    // Largest row r in [0, n) with prefix(r) <= y — the block containing offset y.
    std::size_t rowAtOffset(double y) const {
        if (n_ == 0) return 0;
        std::size_t pos = 0;
        double remaining = y;
        for (std::size_t pw = msb(n_); pw > 0; pw >>= 1) {
            const std::size_t next = pos + pw;
            if (next <= n_ && tree_[next] <= remaining) {
                remaining -= tree_[next];
                pos = next;
            }
        }
        return pos >= n_ ? n_ - 1 : pos;
    }

    void insert(std::size_t row, double h) {
        if (row > n_) row = n_;
        heights_.insert(heights_.begin() + static_cast<std::ptrdiff_t>(row), h);
        reset(std::move(heights_));
    }
    void erase(std::size_t row) {
        if (row >= n_) return;
        heights_.erase(heights_.begin() + static_cast<std::ptrdiff_t>(row));
        reset(std::move(heights_));
    }

private:
    static std::size_t lowbit(std::size_t i) { return i & (~i + 1); }
    static std::size_t msb(std::size_t n) {
        std::size_t p = 1;
        while ((p << 1) <= n) p <<= 1;
        return p;
    }

    std::vector<double> tree_;     // 1-indexed BIT
    std::vector<double> heights_;  // mirror of current per-row heights
    std::size_t n_ = 0;
    double total_ = 0.0;
};
