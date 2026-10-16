#pragma once
// Dev-only performance counters (A0 of the speed programme, 2026-10-16).
//
// Header-only, process-wide atomics bumped at a handful of hot sites (highlighter
// re-runs, ink repaints, SQLite writes, slot bumps). Reading them is the job of
// BlockModel::probeStats(); the `--type-probe` harness prints them per phase so a
// keystroke's cost is attributed, not guessed. Always compiled in: an atomic
// increment is cheaper than a branch around one. `enabled()` gates the
// QElapsedTimer splits, which are not free.
#include <atomic>
#include <QtGlobal>

namespace mn::perf {

struct Counters {
    std::atomic<int> rehighlights{0};     // InlineMarkdownHighlighter: whole-document rehighlight() calls
    std::atomic<int> highlightBlocks{0};  // InlineMarkdownHighlighter: highlightBlock() entries
    std::atomic<int> inkPaints{0};        // DocInkCanvas::paint (a full viewport raster + upload)
    std::atomic<int> slotBumps{0};        // ViewportSlots: per-slot revision bumps (A1)
    std::atomic<int> dbWrites{0};         // Document: statements executed against the working copy
    void reset() {
        rehighlights = 0; highlightBlocks = 0; inkPaints = 0; slotBumps = 0; dbWrites = 0;
    }
};

inline Counters& counters() { static Counters c; return c; }

// MN_TYPE_PROBE=1 (or the --type-probe harness, which sets it) turns the timing
// splits on. Read once.
inline bool enabled() {
    static const bool on = qEnvironmentVariableIntValue("MN_TYPE_PROBE") > 0;
    return on;
}

} // namespace mn::perf
