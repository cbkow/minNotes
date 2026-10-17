#pragma once
#include <QString>

class CanvasDocument;

// The 1.0 → 2.0 converter (2.0 stage 2 step 4). Runs ON THE WORKING COPY of a 1.0 file that
// CanvasDocument just adopted (the canvas schema and ONE page element already exist, every block
// is on that page — CanvasDocument::ensurePages). It carries what the 1.0 schema stored outside
// the page into canvas ELEMENTS, in one transaction:
//   • px-space block_ink (margin ink pinned to a text block) → one `ink` element per anchor, placed
//     beside the text at the block's ESTIMATED y (the file never stored measured heights, so the
//     mark lands within a few lines of its block); its `texts[]` chips → `shape` text elements.
//   • frame-space block_ink on a MEDIA block → the block's own descriptor (`ink["0"]`), the home
//     PDF page ink always had (the page model's load path does this move; counted here).
//   • sketch blocks → an `ink` element the size of the sketch frame BESIDE the page (a column at
//     x = page right + gap, stacked in document order), its texts → `shape` text elements, its
//     images → `media` elements; the block row is deleted.
//   • PDF page ink stays in the PDF's descriptor wherever the PDF is (ruling: media ink lives in
//     the media block's JSON and renders through the GPU ink item).
//   • `DROP TABLE block_ink`; canvas_meta defaults; the working copy is stamped 2.0.
// The ORIGINAL file is never touched; the canvas opens read-only and Save As writes the 2.0 file.
namespace mn {

struct ConversionReport {
    bool converted = false;      // a conversion ran on this open
    int textInkElements = 0;     // px-space anchors → ink elements
    int textShapes = 0;          // texts[] chips (ink envelopes + sketches) → text shape elements
    int mediaInkMoved = 0;       // frame-space media ink → the media descriptor
    int sketches = 0;            // sketch blocks → elements beside the page
    int sketchImages = 0;        // images inside sketches → media elements
    int orphanInk = 0;           // block_ink rows with no usable anchor (dropped)
    int pdfInkInline = 0;        // PDFs whose page ink stayed in their descriptor (informational)
    QString summary() const;     // one line for the toast / release notes
};

// Convert the open working copy behind `canvas` (a 1.0 file already adopted onto `pageId`).
// Returns false only when the store is closed or the page is missing.
bool convertV1(CanvasDocument& canvas, const QString& pageId, ConversionReport& report);

} // namespace mn
