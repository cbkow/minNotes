#include "CanvasLayout.h"
#include <algorithm>
#include <cmath>

namespace mn {

int defaultCardColumns(int cards, int maxCols) {
    if (cards <= 0) return 1;
    const int c = static_cast<int>(std::ceil(std::sqrt(cards / 2.0)));
    return std::clamp(c, 1, std::max(1, maxCols));
}

std::vector<QRectF> defaultCardLayout(const std::vector<double>& heights, double cardW, double gap, int maxCols) {
    std::vector<QRectF> out;
    out.reserve(heights.size());
    const int cols = defaultCardColumns(static_cast<int>(heights.size()), maxCols);
    double y = 0.0, rowH = 0.0;
    for (size_t i = 0; i < heights.size(); ++i) {
        const int col = static_cast<int>(i % cols);
        if (col == 0 && i > 0) { y += rowH + gap; rowH = 0.0; }
        const double h = std::max(0.0, heights[i]);
        out.emplace_back(col * (cardW + gap), y, cardW, h);
        rowH = std::max(rowH, h);
    }
    return out;
}

} // namespace mn
