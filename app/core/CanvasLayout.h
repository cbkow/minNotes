#pragma once
// Canvas B2: the default card layout when no card has a stored position (and, in phase 1, always).
// A reading-order grid: cards of one width flow left to right, top to bottom, C columns wide; each
// grid row is as tall as its tallest card. Pure (heights in → rects out), so the test can pin it.
#include <QRectF>
#include <vector>

namespace mn {

// C = clamp(ceil(sqrt(N / 2)), 1, maxCols): a few cards sit in one or two columns, many in four.
int defaultCardColumns(int cards, int maxCols = 4);

// heights[i] = card i's full height (content + chrome). Rects start at (0, 0); x steps by cardW + gap,
// y by the row's tallest card + gap. Returns heights.size() rects in card order.
std::vector<QRectF> defaultCardLayout(const std::vector<double>& heights, double cardW, double gap, int maxCols = 4);

} // namespace mn
